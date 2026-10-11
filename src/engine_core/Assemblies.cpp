#include "Assemblies.hpp"

#include "AssetInstances.hpp"
#include "Attachment.hpp"
#include "Bone.hpp"
#include "Brush.hpp"
#include "Constraint.hpp"
#include "DataModel.hpp"
#include "InstanceRef.hpp"
#include "LuaApi.hpp"
#include "GameObject.hpp"
#include "PhysicsBase.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "SpatialObject.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace engine_core {
namespace {

// Deep enough for any real nesting of Attachments; a deeper chain is not an end.
constexpr int kMaxWalk = 64;

bool has_parent(InstanceId id, InstanceId above) { return above != DataModel::kNoParent && above != id; }

// How big a body is, to rank roots of equal Mass: the box around its solid (a
// Brush's faces, a PhysicsObject's Size, or a Hull's or a Custom's Mesh),
// times its GameObject's Scale on each axis. 0 for a PlayerController.
double body_volume(const DataModel& game, const PhysicsBase& body) {
    Vec3 extent{};
    if (const auto* brush = dynamic_cast<const Brush*>(&body)) {
        const brush::Shape& shape = brush->shape();
        extent = Vec3{static_cast<float>(shape.max.x - shape.min.x), static_cast<float>(shape.max.y - shape.min.y),
                      static_cast<float>(shape.max.z - shape.min.z)};
    } else if (const auto* object = dynamic_cast<const PhysicsObject*>(&body)) {
        extent = object->size();
        const PhysicsObject::Shape shape = object->shape();
        if (shape == PhysicsObject::Shape::Hull || shape == PhysicsObject::Shape::Custom) {
            Vec3 low{};
            Vec3 high{};
            const auto* mesh = dynamic_cast<const Mesh*>(game.instance(object->mesh_id()));
            if (mesh != nullptr && mesh->bounds(low, high)) {
                extent = Vec3{high.x - low.x, high.y - low.y, high.z - low.z};
            }
        }
    } else {
        return 0.0;
    }
    double volume = static_cast<double>(extent.x) * extent.y * extent.z;
    const InstanceId driven = body.driven_game_object();
    if (const GameObject* object = driven != 0 ? game.game_object(driven) : nullptr) {
        volume *= object->scale() * object->scale() * object->scale();
    }
    return volume;
}

int depth_of(const DataModel& game, InstanceId id) {
    int depth = 0;
    InstanceId at = id;
    while (at != 0) {
        const InstanceId above = game.parent(at);
        if (!has_parent(at, above)) {
            break;
        }
        at = above;
        ++depth;
    }
    return depth;
}

InstanceId climb(const DataModel& game, InstanceId id, int steps) {
    for (int i = 0; i < steps; ++i) {
        id = game.parent(id);
    }
    return id;
}

// Whether a comes before b in a depth-first walk of the tree.
bool tree_before(const DataModel& game, InstanceId a, InstanceId b) {
    if (a == b) {
        return false;
    }
    const int da = depth_of(game, a);
    const int db = depth_of(game, b);
    InstanceId x = da > db ? climb(game, a, da - db) : a;
    InstanceId y = db > da ? climb(game, b, db - da) : b;
    if (x == y) {
        // One holds the other, and the holder comes first.
        return da < db;
    }
    while (game.parent(x) != game.parent(y)) {
        x = game.parent(x);
        y = game.parent(y);
    }
    for (InstanceId child = game.first_child(game.parent(x)); child != 0; child = game.next_sibling(child)) {
        if (child == x) {
            return true;
        }
        if (child == y) {
            return false;
        }
    }
    return x < y;
}

const PhysicsBase* body_of(const DataModel& game, InstanceId id) {
    return dynamic_cast<const PhysicsBase*>(game.instance(id));
}

// A Bone that poses a bone of the GameObject it is under: its frame moves
// with the pose, not only with its Offset.
const Bone* posing_bone(const DataModel& game, InstanceId id) {
    const auto* bone = dynamic_cast<const Bone*>(game.instance(id));
    return bone != nullptr && bone->bone_index() >= 0 ? bone : nullptr;
}

bool is_attachment(const DataModel& game, InstanceId id) {
    return dynamic_cast<const Attachment*>(game.instance(id)) != nullptr;
}

// The posing Bone an end moves with: the end itself, or the first one up its
// chain of Attachments (an Attachment put inside a hand's Bone). Else null.
const Bone* holding_bone(const DataModel& game, InstanceId id) {
    for (int depth = 0; depth < kMaxWalk && is_attachment(game, id); ++depth) {
        if (const Bone* bone = posing_bone(game, id)) {
            return bone;
        }
        id = game.parent(id);
    }
    return nullptr;
}

}  // namespace

Matrix4 Assemblies::with_scale(const Matrix4& frame, const Matrix4& scale_from) {
    Matrix4 out = frame;
    for (int column = 0; column < 3; ++column) {
        const float* axis = scale_from.m + column * 4;
        const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        for (int row = 0; row < 3; ++row) {
            out.m[column * 4 + row] *= length;
        }
    }
    return out;
}

InstanceId claim_winner(const DataModel& game, const InstanceId* candidates, std::size_t count) {
    InstanceId best = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (best == 0 || tree_before(game, candidates[i], best)) {
            best = candidates[i];
        }
    }
    return best;
}

Assemblies::Assemblies() {
    const std::size_t slots = DataModel::kMaxInstances;
    seen_.assign(slots, 0);
    node_.assign(slots, 0);
    group_.assign(slots, -1);
    flags_.assign(slots, 0);
    active_.assign(slots, 0);
    claimant_.assign(slots, 0);
    uf_.assign(slots, 0);
    rigid_uf_.assign(slots, 0);
    anchor_.assign(slots, 0);
    node_pos_.assign(slots, -1);
    rigid_node_.assign(slots, 0);
    scratch_index_.assign(slots, -1);
    touched_.reserve(slots);
    bodies_.reserve(slots);
    constraints_.reserve(slots);
    contested_.reserve(slots);
    candidates_.reserve(slots);
    edges_.reserve(slots);
    rigid_order_.reserve(slots);
    no_collide_.reserve(slots);
    joints_.reserve(slots);
    nodes_.reserve(slots);
    groups_.reserve(slots);
    group_nodes_.reserve(slots);
    rigid_.reserve(slots);
    bone_.reserve(slots);
    adj_start_.reserve(slots + 1);
    adj_cursor_.reserve(slots);
    adj_.reserve(2 * slots);
    hops_.reserve(slots);
    rigid_hops_.reserve(slots);
    queue_.reserve(slots);
    live_scratch_.reserve(slots);
    dangling_id_.assign(slots, 0);
    dangling_mask_.assign(slots, 0);
    dangling_epoch_.assign(slots, 0);
    follow_mark_.assign(slots, 0);
    play_.reserve(slots);
    play_members_.reserve(slots);
    play_losers_.reserve(slots);
    play_seen_.assign(slots, 0);
    play_index_.assign(slots, -1);
    play_member_index_.assign(slots, -1);
    play_touched_.reserve(slots);
    play_rigid_.reserve(slots);
    old_play_.reserve(slots);
    old_members_.reserve(slots);
    side_.reserve(slots);
    side_mark_.assign(slots, 0);
    side_movers_.reserve(slots);
    follow_marked_.reserve(slots);
    carry_stack_.reserve(slots);
}

void Assemblies::refresh(const DataModel& game) {
    const std::uint64_t revision_now = revision();
    const std::uint32_t generation = game.world_generation();
    const bool running = game.simulation_running();
    if (built_ && built_revision_ == revision_now && built_generation_ == generation && built_running_ == running) {
        return;
    }
    rebuild(game);
    built_ = true;
    built_revision_ = revision_now;
    built_generation_ = generation;
    built_running_ = running;
}

void Assemblies::resume(DataModel& game) {
    if (suspended_ > 0 && --suspended_ == 0 && follow_on_resume_) {
        follow_on_resume_ = false;
        if (!game.simulation_running()) {
            refresh(game);
            follow(game, 0);
        }
    }
}

bool Assemblies::relevant(const DataModel& game, InstanceId id) const {
    if ((flags(id) & kLinked) != 0) {
        return true;
    }
    const DataModel* object = game.instance(id);
    if (object == nullptr) {
        return false;
    }
    if (object->constraint()) {
        return true;
    }
    if (const auto* body = dynamic_cast<const PhysicsBase*>(object)) {
        const InstanceId driven = body->driven_game_object();
        return driven != 0 && (flags(driven) & kLinked) != 0;
    }
    return false;
}

void Assemblies::changed(DataModel& game) {
    bump();
    if (suspended_ > 0 || busy_ > 0 || game.simulation_running()) {
        return;
    }
    refresh(game);
    follow(game, 0);
}

void Assemblies::constraint_changed(DataModel& game, InstanceId) { changed(game); }

void Assemblies::tree_changed(DataModel& game, InstanceId) { changed(game); }

void Assemblies::guids_may_have_returned() {
    ++guid_epoch_;
    bump();
}

void Assemblies::transform_written(DataModel& game, InstanceId id) {
    if (suspended_ > 0 || busy_ > 0) {
        return;
    }
    refresh(game);
    const std::uint8_t flagged = flags(id);
    if ((flagged & kLinked) == 0) {
        return;
    }
    if (game.simulation_running()) {
        play_transform_written(game, id);
        return;
    }
    // Stopped, a driven PhysicsBase's own Transform moves nothing: its
    // GameObject decides, and the write is undone at the next sync.
    if (const auto* body = dynamic_cast<const PhysicsBase*>(game.instance(id))) {
        if (body->driven_game_object() != 0) {
            return;
        }
    }
    if ((flagged & kRigid) != 0) {
        follow(game, node_of(id), true);
    }
}

void Assemblies::attachment_changed(DataModel& game, InstanceId id) {
    if (suspended_ > 0 || busy_ > 0) {
        return;
    }
    refresh(game);
    const std::uint8_t flagged = flags(id);
    if ((flagged & kLinked) == 0) {
        return;
    }
    if (game.simulation_running()) {
        play_attachment_changed(game, id);
    } else if ((flagged & kRigid) != 0) {
        follow(game, node_of(id));
    }
}

void Assemblies::pose_changed(DataModel& game, InstanceId game_object) {
    if (suspended_ > 0 || busy_ > 0 || game.simulation_running()) {
        return;
    }
    refresh(game);
    if ((flags(game_object) & kRigid) != 0) {
        follow(game, node_of(game_object));
    }
}

InstanceId Assemblies::end_of(const DataModel& game, InstanceId constraint, const InstanceRef& ref, int end) {
    if (ref.guid().empty()) {
        return 0;
    }
    const std::uint32_t slot = id_slot(constraint);
    const std::uint8_t bit = end == 1 ? 1u : 2u;
    if (dangling_id_[slot] != constraint) {
        dangling_id_[slot] = constraint;
        dangling_mask_[slot] = 0;
    }
    if ((dangling_mask_[slot] & bit) != 0 && dangling_epoch_[slot] == guid_epoch_) {
        return 0;
    }
    const InstanceId target = ref.resolve(game);
    if (target == 0) {
        ++find_guid_calls_;
        dangling_mask_[slot] = static_cast<std::uint8_t>(dangling_mask_[slot] | bit);
        dangling_epoch_[slot] = guid_epoch_;
        return 0;
    }
    dangling_mask_[slot] = static_cast<std::uint8_t>(dangling_mask_[slot] & ~bit);
    const DataModel* object = game.instance(target);
    return object != nullptr && lua_class_inherits(object->class_name(), "PVInstance") ? target : 0;
}

AssemblySuspend::AssemblySuspend(DataModel& game) : game_(game) { game_.assemblies().suspend(); }

AssemblySuspend::~AssemblySuspend() { game_.assemblies().resume(game_); }

bool Assemblies::seen(InstanceId id) const {
    const std::uint32_t slot = id_slot(id);
    return id != 0 && slot < seen_.size() && seen_[slot] == id;
}

void Assemblies::touch(InstanceId id) {
    const std::uint32_t slot = id_slot(id);
    if (id == 0 || slot >= seen_.size() || seen_[slot] == id) {
        return;
    }
    if (seen_[slot] == 0) {
        touched_.push_back(slot);
    }
    seen_[slot] = id;
    node_[slot] = 0;
    group_[slot] = -1;
    flags_[slot] = 0;
    active_[slot] = 0;
    claimant_[slot] = 0;
    uf_[slot] = slot;
    rigid_uf_[slot] = slot;
    anchor_[slot] = 0;
    rigid_node_[slot] = 0;
    scratch_index_[slot] = -1;
}

void Assemblies::clear_touched() {
    for (const std::uint32_t slot : touched_) {
        seen_[slot] = 0;
    }
    touched_.clear();
}

std::uint32_t Assemblies::find(std::uint32_t slot) {
    while (uf_[slot] != slot) {
        uf_[slot] = uf_[uf_[slot]];
        slot = uf_[slot];
    }
    return slot;
}

std::uint32_t Assemblies::rigid_find(std::uint32_t slot) {
    while (rigid_uf_[slot] != slot) {
        rigid_uf_[slot] = rigid_uf_[rigid_uf_[slot]];
        slot = rigid_uf_[slot];
    }
    return slot;
}

void Assemblies::unite(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t ra = find(a);
    const std::uint32_t rb = find(b);
    if (ra != rb) {
        uf_[rb] = ra;
    }
}

InstanceId Assemblies::resolve_end(const DataModel& game, InstanceId end, bool mark) {
    if (end == 0) {
        return 0;
    }
    if (mark) {
        // An end is linked even when it does not resolve, so moving it into
        // Workspace, or under a holder, moves the revision.
        touch(end);
        flags_[id_slot(end)] |= kLinked;
    }
    if (!game.in_workspace(end)) {
        return 0;
    }
    InstanceId holder = end;
    int walked = 0;
    while (is_attachment(game, holder)) {
        const InstanceId above = game.parent(holder);
        if (!has_parent(holder, above) || ++walked > kMaxWalk) {
            return 0;
        }
        holder = above;
    }
    const DataModel* object = game.instance(holder);
    InstanceId node = 0;
    if (dynamic_cast<const PhysicsBase*>(object) != nullptr || dynamic_cast<const GameObject*>(object) != nullptr) {
        node = seen(holder) && node_[id_slot(holder)] != 0 ? node_[id_slot(holder)] : holder;
    } else if (dynamic_cast<const SpatialObject*>(object) != nullptr) {
        node = holder;
    } else {
        return 0;
    }
    if (mark) {
        touch(holder);
        if (node_[id_slot(holder)] == 0) {
            node_[id_slot(holder)] = node;
        }
        flags_[id_slot(holder)] |= kLinked;
        for (InstanceId at = end; at != holder; at = game.parent(at)) {
            touch(at);
            node_[id_slot(at)] = node;
            flags_[id_slot(at)] |= kLinked;
        }
    }
    return node;
}

InstanceId Assemblies::claimant_live(const DataModel& game, InstanceId game_object) const {
    game.physics_bodies(live_scratch_);
    std::size_t drivers = 0;
    for (const InstanceId id : live_scratch_) {
        const PhysicsBase* body = body_of(game, id);
        if (body != nullptr && body->driven_game_object() == game_object) {
            live_scratch_[drivers++] = id;
        }
    }
    return claim_winner(game, live_scratch_.data(), drivers);
}

InstanceId Assemblies::resolve_live(const DataModel& game, InstanceId end) const {
    if (end == 0 || !game.in_workspace(end)) {
        return 0;
    }
    InstanceId holder = end;
    int walked = 0;
    while (is_attachment(game, holder)) {
        const InstanceId above = game.parent(holder);
        if (!has_parent(holder, above) || ++walked > kMaxWalk) {
            return 0;
        }
        holder = above;
    }
    if (seen(holder) && node_[id_slot(holder)] != 0) {
        return node_[id_slot(holder)];
    }
    const DataModel* object = game.instance(holder);
    if (const auto* body = dynamic_cast<const PhysicsBase*>(object)) {
        const InstanceId target = body->driven_game_object();
        const InstanceId winner = target != 0 ? claimant_live(game, target) : 0;
        return winner != 0 ? winner : holder;
    }
    if (dynamic_cast<const GameObject*>(object) != nullptr) {
        const InstanceId winner = claimant_live(game, holder);
        return winner != 0 ? winner : holder;
    }
    return dynamic_cast<const SpatialObject*>(object) != nullptr ? holder : 0;
}

bool Assemblies::joint_holds(const DataModel& game, InstanceId node1, InstanceId node2) const {
    const std::int32_t group1 = group_[id_slot(node1)];
    const std::int32_t group2 = group_[id_slot(node2)];
    if (group1 >= 0 && group1 == group2) {
        return false;
    }
    // One end's assembly: it needs a body, and no PlayerController at the end or as its root.
    const auto side = [&](InstanceId node, std::int32_t group, bool& anchored) {
        const PhysicsBase* body = body_of(game, node);
        InstanceId root = node;
        bool physics = body != nullptr;
        anchored = body != nullptr && body->anchored();
        if (group >= 0) {
            const Group& assembly = groups_[static_cast<std::size_t>(group)];
            root = assembly.root;
            physics = assembly.has_physics;
            anchored = assembly.anchored;
        }
        return physics && enabled_controller(body) == nullptr && enabled_controller(body_of(game, root)) == nullptr;
    };
    bool anchored1 = false;
    bool anchored2 = false;
    if (!side(node1, group1, anchored1) || !side(node2, group2, anchored2)) {
        return false;
    }
    return !(anchored1 && anchored2);
}

// Whether a ranks above b as root: anchored, then a PlayerController, then a
// physics node, then higher Mass, then the larger body, then the lower GUID.
bool Assemblies::root_before(const DataModel& game, InstanceId a, InstanceId b) const {
    const PhysicsBase* pa = body_of(game, a);
    const PhysicsBase* pb = body_of(game, b);
    const bool anchored_a = pa != nullptr && pa->anchored();
    const bool anchored_b = pb != nullptr && pb->anchored();
    if (anchored_a != anchored_b) {
        return anchored_a;
    }
    const bool controller_a = enabled_controller(pa) != nullptr;
    const bool controller_b = enabled_controller(pb) != nullptr;
    if (controller_a != controller_b) {
        return controller_a;
    }
    if ((pa != nullptr) != (pb != nullptr)) {
        return pa != nullptr;
    }
    // A disabled PlayerController rides with no mass (PhysicsWorld): none here either.
    const auto ranked_mass = [](const PhysicsBase* body) {
        return dynamic_cast<const PlayerController*>(body) != nullptr && enabled_controller(body) == nullptr
                   ? 0.0
                   : body->mass();
    };
    if (pa != nullptr && ranked_mass(pa) != ranked_mass(pb)) {
        return ranked_mass(pa) > ranked_mass(pb);
    }
    if (pa != nullptr) {
        const double va = body_volume(game, *pa);
        const double vb = body_volume(game, *pb);
        if (va != vb) {
            return va > vb;
        }
    }
    return game.guid_view(a) < game.guid_view(b);
}

void Assemblies::rebuild(const DataModel& game) {
    ++builds_;
    clear_touched();
    edges_.clear();
    rigid_order_.clear();
    nodes_.clear();
    groups_.clear();
    group_nodes_.clear();
    rigid_.clear();
    bone_.clear();
    no_collide_.clear();
    joints_.clear();
    contested_.clear();

    // Claims: each body is a node, and so is the GameObject it drives, under
    // the body that wins it.
    game.physics_bodies(bodies_);
    for (const InstanceId id : bodies_) {
        const PhysicsBase* body = body_of(game, id);
        if (body == nullptr) {
            continue;
        }
        touch(id);
        const InstanceId target = body->driven_game_object();
        if (target == 0) {
            continue;
        }
        const std::uint32_t slot = id_slot(target);
        if (!seen(target)) {
            touch(target);
            claimant_[slot] = id;
        } else if (claimant_[slot] != id && (flags_[slot] & kLinked) == 0) {
            flags_[slot] |= kLinked;
            contested_.push_back(target);
        }
    }
    for (const InstanceId target : contested_) {
        candidates_.clear();
        for (const InstanceId id : bodies_) {
            const PhysicsBase* body = body_of(game, id);
            if (body != nullptr && body->driven_game_object() == target) {
                candidates_.push_back(id);
            }
        }
        claimant_[id_slot(target)] = claim_winner(game, candidates_.data(), candidates_.size());
    }
    for (const InstanceId id : bodies_) {
        const PhysicsBase* body = body_of(game, id);
        if (body == nullptr) {
            continue;
        }
        const InstanceId target = body->driven_game_object();
        const InstanceId owner = target != 0 ? claimant_[id_slot(target)] : id;
        node_[id_slot(id)] = owner;
        if (target != 0) {
            node_[id_slot(target)] = owner;
        }
    }

    // Edges: every enabled constraint in Workspace whose ends are two nodes.
    game.constraints(constraints_);
    for (const InstanceId id : constraints_) {
        const auto* constraint = dynamic_cast<const Constraint*>(game.instance(id));
        if (constraint == nullptr) {
            continue;
        }
        touch(id);
        const InstanceId end1 = end_of(game, id, constraint->pv_instance1_ref(), 1);
        const InstanceId end2 = end_of(game, id, constraint->pv_instance2_ref(), 2);
        const InstanceId node1 = resolve_end(game, end1, true);
        const InstanceId node2 = resolve_end(game, end2, true);
        if (!constraint->joins()) {
            if (constraint->joint()) {
                // A joint joins nothing; whether it holds needs the assemblies (below).
                active_[id_slot(id)] = 0;
                if (constraint->enabled() && node1 != 0 && node2 != 0 && node1 != node2) {
                    joints_.push_back(JointEdge{id, node1, node2});
                }
                continue;
            }
            // A NoCollisionConstraint joins nothing: it only keeps two bodies apart.
            const bool live = constraint->enabled() && node1 != 0 && node2 != 0 && node1 != node2 &&
                              body_of(game, node1) != nullptr && body_of(game, node2) != nullptr;
            active_[id_slot(id)] = live ? 1 : 0;
            if (live) {
                no_collide_.emplace_back(std::min(node1, node2), std::max(node1, node2));
            }
            continue;
        }
        if (!constraint->enabled() || node1 == 0 || node2 == 0 || node1 == node2) {
            continue;
        }
        Edge edge;
        edge.constraint = id;
        edge.node1 = node1;
        edge.node2 = node2;
        edge.rigid = constraint->rigid();
        if (edge.rigid) {
            // A held thing follows the hand, wherever the pose takes it: it is
            // not joined to the GameObject, so it is in no rigid tree.
            edge.bone_end = holding_bone(game, end1) != nullptr ? 1 : holding_bone(game, end2) != nullptr ? 2 : 0;
            if (edge.bone_end == 0) {
                rigid_order_.push_back(static_cast<std::uint32_t>(edges_.size()));
            }
        }
        edges_.push_back(edge);
    }

    std::sort(no_collide_.begin(), no_collide_.end());
    no_collide_.erase(std::unique(no_collide_.begin(), no_collide_.end()), no_collide_.end());

    // Rigid rule 1: the rigid edges make a forest. In GUID order, one whose
    // ends other rigid edges already join closes a loop, and is inactive.
    std::sort(rigid_order_.begin(), rigid_order_.end(), [&](std::uint32_t a, std::uint32_t b) {
        return game.guid_view(edges_[a].constraint) < game.guid_view(edges_[b].constraint);
    });
    for (const std::uint32_t index : rigid_order_) {
        Edge& edge = edges_[index];
        const std::uint32_t a = rigid_find(id_slot(edge.node1));
        const std::uint32_t b = rigid_find(id_slot(edge.node2));
        if (a == b) {
            edge.live = false;
            continue;
        }
        rigid_uf_[b] = a;
    }

    // Join: welds first; in play a rigid edge whose ends welds already join changes nothing.
    const bool running = game.simulation_running();
    for (const Edge& edge : edges_) {
        if (!edge.rigid && edge.live) {
            unite(id_slot(edge.node1), id_slot(edge.node2));
        }
    }
    for (const std::uint32_t index : rigid_order_) {
        Edge& edge = edges_[index];
        if (!edge.live) {
            continue;
        }
        if (running && find(id_slot(edge.node1)) == find(id_slot(edge.node2))) {
            edge.live = false;
            continue;
        }
        unite(id_slot(edge.node1), id_slot(edge.node2));
        rigid_node_[id_slot(edge.node1)] = 1;
        rigid_node_[id_slot(edge.node2)] = 1;
    }
    rigid_order_.erase(std::remove_if(rigid_order_.begin(), rigid_order_.end(),
                                      [&](std::uint32_t index) { return !edges_[index].live; }),
                       rigid_order_.end());
    for (const Edge& edge : edges_) {
        active_[id_slot(edge.constraint)] = edge.live ? 1 : 0;
    }

    // Groups: one per connected set of nodes that a live edge touches.
    for (const Edge& edge : edges_) {
        if (!edge.live || edge.bone_end != 0) {
            continue;
        }
        for (const InstanceId node : {edge.node1, edge.node2}) {
            if (group_[id_slot(node)] == -1) {
                group_[id_slot(node)] = -2;
                nodes_.push_back(node);
            }
        }
    }
    for (const InstanceId node : nodes_) {
        const std::uint32_t root_slot = find(id_slot(node));
        if (scratch_index_[root_slot] < 0) {
            scratch_index_[root_slot] = static_cast<std::int32_t>(groups_.size());
            groups_.push_back(Group{});
        }
        const std::int32_t group = scratch_index_[root_slot];
        group_[id_slot(node)] = group;
        ++groups_[group].count;
    }
    std::uint32_t first = 0;
    for (Group& group : groups_) {
        group.first = first;
        first += group.count;
        group.count = 0;
    }
    group_nodes_.resize(nodes_.size());
    for (const InstanceId node : nodes_) {
        Group& group = groups_[group_[id_slot(node)]];
        group_nodes_[group.first + group.count++] = node;
    }
    for (Group& group : groups_) {
        for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
            const InstanceId node = group_nodes_[i];
            const PhysicsBase* body = body_of(game, node);
            group.has_physics = group.has_physics || body != nullptr;
            group.anchored = group.anchored || (body != nullptr && body->anchored());
            if (group.root == 0 || root_before(game, node, group.root)) {
                group.root = node;
            }
        }
    }
    // Joints, now that assemblies are known (joint spec §2).
    joints_.erase(std::remove_if(joints_.begin(), joints_.end(),
                                 [&](const JointEdge& joint) { return !joint_holds(game, joint.node1, joint.node2); }),
                  joints_.end());
    std::sort(joints_.begin(), joints_.end(),
              [](const JointEdge& a, const JointEdge& b) { return a.constraint < b.constraint; });
    for (const JointEdge& joint : joints_) {
        active_[id_slot(joint.constraint)] = 1;
    }
    orient_rigid(game);

    // Bone edges: the Bone's side is the parent. Every Bone under the holder
    // poses the hand too (one higher up the arm, say), so each is linked: a
    // change to its Offset moves what the hand holds.
    for (const Edge& edge : edges_) {
        if (!edge.live || edge.bone_end == 0) {
            continue;
        }
        const auto* constraint = dynamic_cast<const Constraint*>(game.instance(edge.constraint));
        const InstanceId end = edge.bone_end == 1 ? constraint->pv_instance1_id() : constraint->pv_instance2_id();
        const Bone* posed = holding_bone(game, end);
        const InstanceId bone = posed != nullptr ? posed->id() : end;
        RigidEdge out;
        out.constraint = edge.constraint;
        out.parent_end = edge.bone_end;
        out.parent_node = edge.bone_end == 1 ? edge.node1 : edge.node2;
        out.sub_node = edge.bone_end == 1 ? edge.node2 : edge.node1;
        bone_.push_back(out);
        rigid_node_[id_slot(edge.node1)] = 1;
        rigid_node_[id_slot(edge.node2)] = 1;
        const InstanceId holder = game.parent(bone);
        for (InstanceId child = game.first_child(holder); child != 0; child = game.next_sibling(child)) {
            if (dynamic_cast<const Bone*>(game.instance(child)) == nullptr) {
                continue;
            }
            touch(child);
            if (node_[id_slot(child)] == 0) {
                node_[id_slot(child)] = out.parent_node;
            }
            flags_[id_slot(child)] |= kLinked;
        }
        // Held, with everything welded to it: in play, one kinematic body.
        const std::int32_t group = group_[id_slot(out.sub_node)];
        if (group < 0) {
            flags_[id_slot(out.sub_node)] |= kHeld;
        } else {
            const Group& held = groups_[static_cast<std::size_t>(group)];
            for (std::uint32_t i = held.first; i < held.first + held.count; ++i) {
                flags_[id_slot(group_nodes_[i])] |= kHeld;
            }
        }
    }

    // Flags for every instance that belongs to a node.
    const std::size_t count = touched_.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t slot = touched_[i];
        const InstanceId node = node_[slot];
        if (node == 0) {
            continue;
        }
        const std::uint32_t node_slot = id_slot(node);
        const std::int32_t group = group_[node_slot];
        if (group >= 0) {
            group_[slot] = group;
            if (!is_attachment(game, seen_[slot])) {
                flags_[slot] |= kMember | kLinked;
            }
        }
        if (rigid_node_[node_slot] != 0) {
            flags_[slot] |= kRigid;
        }
    }
}

// Rigid rule 2: the root never moves. Hops from each root over every live
// edge say how near each node is to it; in each rigid tree, the nearest node
// (root_before on a tie) is its anchor, which stays, and each edge points away
// from it, whichever end names which. Follow order: parents first, by hops
// from the anchor, then by GUID.
void Assemblies::orient_rigid(const DataModel& game) {
    if (rigid_order_.empty()) {
        return;
    }
    const std::size_t count = nodes_.size();
    for (std::size_t i = 0; i < count; ++i) {
        node_pos_[id_slot(nodes_[i])] = static_cast<std::int32_t>(i);
    }
    const auto pos = [&](InstanceId node) { return static_cast<std::uint32_t>(node_pos_[id_slot(node)]); };
    adj_start_.assign(count + 1, 0);
    for (const Edge& edge : edges_) {
        if (edge.live && edge.bone_end == 0) {
            ++adj_start_[pos(edge.node1) + 1];
            ++adj_start_[pos(edge.node2) + 1];
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        adj_start_[i + 1] += adj_start_[i];
    }
    adj_cursor_.assign(adj_start_.begin(), adj_start_.end() - 1);
    adj_.resize(adj_start_[count]);
    for (std::uint32_t index = 0; index < edges_.size(); ++index) {
        const Edge& edge = edges_[index];
        if (edge.live && edge.bone_end == 0) {
            adj_[adj_cursor_[pos(edge.node1)]++] = index;
            adj_[adj_cursor_[pos(edge.node2)]++] = index;
        }
    }
    const auto spread = [&](std::vector<std::int32_t>& hops, bool rigid_only) {
        for (std::size_t at = 0; at < queue_.size(); ++at) {
            const std::uint32_t from = queue_[at];
            for (std::uint32_t k = adj_start_[from]; k < adj_start_[from + 1]; ++k) {
                const Edge& edge = edges_[adj_[k]];
                if (rigid_only && !edge.rigid) {
                    continue;
                }
                const std::uint32_t to = pos(nodes_[from] == edge.node1 ? edge.node2 : edge.node1);
                if (hops[to] < 0) {
                    hops[to] = hops[from] + 1;
                    queue_.push_back(to);
                }
            }
        }
    };
    hops_.assign(count, -1);
    queue_.clear();
    for (const Group& group : groups_) {
        hops_[pos(group.root)] = 0;
        queue_.push_back(pos(group.root));
    }
    spread(hops_, false);
    for (const std::uint32_t index : rigid_order_) {
        for (const InstanceId node : {edges_[index].node1, edges_[index].node2}) {
            InstanceId& anchor = anchor_[rigid_find(id_slot(node))];
            if (anchor == 0 || hops_[pos(node)] < hops_[pos(anchor)] ||
                (hops_[pos(node)] == hops_[pos(anchor)] && root_before(game, node, anchor))) {
                anchor = node;
            }
        }
    }
    rigid_hops_.assign(count, -1);
    queue_.clear();
    for (const std::uint32_t index : rigid_order_) {
        for (const InstanceId node : {edges_[index].node1, edges_[index].node2}) {
            if (anchor_[rigid_find(id_slot(node))] == node && rigid_hops_[pos(node)] < 0) {
                rigid_hops_[pos(node)] = 0;
                queue_.push_back(pos(node));
            }
        }
    }
    spread(rigid_hops_, true);
    // rigid_order_ is in GUID order; a stable sort by depth keeps it within one.
    const auto depth = [&](std::uint32_t index) {
        return std::max(rigid_hops_[pos(edges_[index].node1)], rigid_hops_[pos(edges_[index].node2)]);
    };
    std::stable_sort(rigid_order_.begin(), rigid_order_.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return depth(a) < depth(b); });
    for (const std::uint32_t index : rigid_order_) {
        const Edge& edge = edges_[index];
        RigidEdge out;
        out.constraint = edge.constraint;
        out.parent_end = rigid_hops_[pos(edge.node1)] < rigid_hops_[pos(edge.node2)] ? 1 : 2;
        out.parent_node = out.parent_end == 1 ? edge.node1 : edge.node2;
        out.sub_node = out.parent_end == 1 ? edge.node2 : edge.node1;
        rigid_.push_back(out);
    }
}

std::uint8_t Assemblies::flags(InstanceId id) const { return seen(id) ? flags_[id_slot(id)] : 0; }

InstanceId Assemblies::node_of(InstanceId id) const { return seen(id) ? node_[id_slot(id)] : 0; }

bool Assemblies::active(InstanceId constraint) const {
    return seen(constraint) && active_[id_slot(constraint)] != 0;
}

InstanceId Assemblies::claimant_of(InstanceId game_object) const {
    return seen(game_object) ? claimant_[id_slot(game_object)] : 0;
}

int Assemblies::group_of(InstanceId node) const { return seen(node) ? group_[id_slot(node)] : -1; }

InstanceId Assemblies::root_of(const DataModel& game, InstanceId id) const {
    if (id == 0 || !game.in_workspace(id)) {
        return 0;
    }
    if (seen(id) && node_[id_slot(id)] != 0) {
        const std::int32_t group = group_[id_slot(id)];
        return group >= 0 ? groups_[group].root : node_[id_slot(id)];
    }
    return resolve_live(game, id);
}

Matrix4 Assemblies::node_frame(const DataModel& game, InstanceId node) const {
    const DataModel* object = game.instance(node);
    if (const auto* body = dynamic_cast<const PhysicsBase*>(object)) {
        const bool controller = enabled_controller(body) != nullptr;
        if (const GameObject* driven = game.game_object(body->driven_game_object())) {
            return controller ? upright_transform(driven->transform()) : matrix4_orthonormalize(driven->transform());
        }
        return controller ? upright_transform(body->transform()) : matrix4_orthonormalize(body->transform());
    }
    if (const auto* placed = dynamic_cast<const PVInstance*>(object)) {
        return matrix4_orthonormalize(placed->transform());
    }
    return matrix4_identity();
}

InstanceId Assemblies::mover_of(const DataModel& game, InstanceId node) const {
    if (const PhysicsBase* body = body_of(game, node)) {
        const InstanceId driven = body->driven_game_object();
        return driven != 0 ? driven : node;
    }
    return node;
}

// What an Attachment directly under holder measures its Offset from: the
// holder's Transform, except that a PhysicsBase's comes from its node's frame
// with its own scale, since its own Transform lags its GameObject while stopped.
Matrix4 Assemblies::holder_transform(const DataModel& game, InstanceId holder) const {
    const DataModel* object = game.instance(holder);
    if (const auto* body = dynamic_cast<const PhysicsBase*>(object)) {
        return with_scale(node_frame(game, holder), body->transform());
    }
    if (const auto* placed = dynamic_cast<const PVInstance*>(object)) {
        return placed->transform();
    }
    return matrix4_identity();
}

Matrix4 Assemblies::attachment_frame(const DataModel& game, InstanceId attachment, int depth) const {
    const auto* point = dynamic_cast<const Attachment*>(game.instance(attachment));
    if (point == nullptr) {
        return matrix4_identity();
    }
    // A Bone is where its posed bone is, which its Offset alone does not say.
    if (const Bone* bone = posing_bone(game, attachment)) {
        return bone->transform();
    }
    const InstanceId above = game.parent(attachment);
    const Matrix4 base = depth < kMaxWalk && is_attachment(game, above) ? attachment_frame(game, above, depth + 1)
                                                                         : holder_transform(game, above);
    if (point->offset_space() == TransformSpace::World) {
        const Vec3 position = matrix4_position(base);
        return matrix4_multiply(matrix4_translation(position.x, position.y, position.z), point->offset());
    }
    return matrix4_multiply(base, point->offset());
}

bool Assemblies::end_pivot(const DataModel& game, InstanceId constraint, int end, Matrix4& pivot) const {
    const auto* object = dynamic_cast<const Constraint*>(game.instance(constraint));
    if (object == nullptr) {
        return false;
    }
    const InstanceId named = end == 1 ? object->pv_instance1_id() : object->pv_instance2_id();
    const InstanceId node = resolve_live(game, named);
    if (node == 0) {
        return false;
    }
    pivot = is_attachment(game, named) ? matrix4_orthonormalize(attachment_frame(game, named, 0))
                                       : node_frame(game, node);
    return true;
}

}  // namespace engine_core
