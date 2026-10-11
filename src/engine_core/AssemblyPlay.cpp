// Assemblies in play (spec §5): the frame and offsets each assembly keeps,
// and the writes that put its members where its body is.

#include "Assemblies.hpp"

#include "Brush.hpp"
#include "Constraint.hpp"
#include "DataModel.hpp"
#include "GameObject.hpp"
#include "PhysicsBase.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "SpatialObject.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace engine_core {

namespace {

bool is_controller(const DataModel& game, InstanceId id) {
    return dynamic_cast<const PlayerController*>(game.instance(id)) != nullptr;
}

}  // namespace

// The per-slot index of play_ and play_members_, and the PhysicsBases that
// lost their GameObject to a member, which are written with that member.
void Assemblies::index_play() {
    for (const std::uint32_t slot : play_touched_) {
        play_seen_[slot] = 0;
        play_index_[slot] = -1;
        play_member_index_[slot] = -1;
    }
    play_touched_.clear();
    for (std::size_t index = 0; index < play_.size(); ++index) {
        const PlayAssembly& assembly = play_[index];
        for (std::uint32_t i = assembly.first; i < assembly.first + assembly.count; ++i) {
            const std::uint32_t slot = id_slot(play_members_[i].node);
            if (play_seen_[slot] == 0) {
                play_touched_.push_back(slot);
            }
            play_seen_[slot] = play_members_[i].node;
            play_index_[slot] = static_cast<std::int32_t>(index);
            play_member_index_[slot] = static_cast<std::int32_t>(i);
        }
    }
    play_losers_.clear();
    for (const std::uint32_t slot : touched_) {
        const InstanceId id = seen_[slot];
        const InstanceId node = node_[slot];
        if (node == 0 || id == node) {
            continue;
        }
        const std::uint32_t node_slot = id_slot(node);
        if (play_seen_[node_slot] == node && play_member_index_[node_slot] >= 0) {
            PlayLoser loser;
            loser.instance = id;
            loser.member = static_cast<std::uint32_t>(play_member_index_[node_slot]);
            play_losers_.push_back(loser);
        }
    }
    // Only PhysicsBases ride as losers; a holder or an Attachment does not.
    play_losers_.erase(std::remove_if(play_losers_.begin(), play_losers_.end(),
                                      [&](const PlayLoser& loser) {
                                          return seen_[id_slot(loser.instance)] != loser.instance ||
                                                 (flags_[id_slot(loser.instance)] & kMember) == 0;
                                      }),
                       play_losers_.end());
}

void Assemblies::capture_play(DataModel& game) {
    refresh(game);
    play_.clear();
    play_members_.clear();
    for (const Group& group : groups_) {
        PlayAssembly assembly;
        assembly.root = group.root;
        assembly.anchored = group.anchored;
        assembly.has_physics = group.has_physics;
        assembly.frame = node_frame(game, group.root);
        assembly.first = static_cast<std::uint32_t>(play_members_.size());
        assembly.count = group.count;
        assembly.stamp = ++next_stamp_;
        const Matrix4 inverse = matrix4_inverse(assembly.frame);
        for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
            const InstanceId node = group_nodes_[i];
            PlayMember member;
            member.node = node;
            if (const auto* body = dynamic_cast<const PhysicsBase*>(game.instance(node))) {
                member.driven = body->driven_game_object();
            }
            member.offset = matrix4_multiply(inverse, node_frame(game, node));
            play_members_.push_back(member);
        }
        play_.push_back(assembly);
    }
    index_play();
    play_rigid_.clear();
    for (const RigidEdge& edge : rigid_) {
        play_rigid_.push_back(edge.constraint);
    }
    play_captured_ = true;
    play_generation_ = game.world_generation();
    play_builds_ = builds_;
}

void Assemblies::sync_play(DataModel& game) {
    if (!game.simulation_running()) {
        refresh(game);
        return;
    }
    if (!play_captured_ || play_generation_ != game.world_generation()) {
        capture_play(game);
        return;
    }
    refresh(game);
    if (play_builds_ != builds_) {
        play_rebuilt(game);
    }
}

int Assemblies::old_play_of(InstanceId node) const { return play_of(node); }

Matrix4 Assemblies::body_pose(const DataModel& game, const PlayAssembly& assembly,
                              const std::vector<PlayMember>& members) const {
    for (std::uint32_t i = assembly.first; i < assembly.first + assembly.count; ++i) {
        const PlayMember& member = members[i];
        if (game.instance(member.node) != nullptr && member.node != pivot_change_) {
            return matrix4_multiply(node_frame(game, member.node), matrix4_inverse(member.offset));
        }
    }
    return assembly.frame;
}

void Assemblies::snap_in_play(DataModel& game, const RigidEdge& edge) {
    Matrix4 parent_pivot;
    Matrix4 sub_pivot;
    if (!end_pivot(game, edge.constraint, edge.parent_end, parent_pivot) ||
        !end_pivot(game, edge.constraint, 3 - edge.parent_end, sub_pivot)) {
        return;
    }
    const Matrix4 delta = matrix4_multiply(parent_pivot, matrix4_inverse(sub_pivot));
    const int old = old_play_of(edge.sub_node);
    const auto move = [&](InstanceId node, InstanceId driven) {
        const Matrix4 frame = matrix4_orthonormalize(matrix4_multiply(delta, node_frame(game, node)));
        write_member(game, node, driven, frame, Vec3{}, Vec3{}, 0);
    };
    if (old < 0) {
        const auto* body = dynamic_cast<const PhysicsBase*>(game.instance(edge.sub_node));
        move(edge.sub_node, body != nullptr ? body->driven_game_object() : 0);
        return;
    }
    const PlayAssembly& side = play_[static_cast<std::size_t>(old)];
    for (std::uint32_t i = side.first; i < side.first + side.count; ++i) {
        move(play_members_[i].node, play_members_[i].driven);
    }
}

// The graph changed in play: assemblies whose members are the same keep their
// body, frame, and offsets; the rest get new ones, from where their members
// are now. A RigidConstraint that has just become active first snaps its sub's
// side onto its parent's pivot.
void Assemblies::play_rebuilt(DataModel& game) {
    play_builds_ = builds_;
    // Newly active rigid edges snap first, while the old assemblies still say
    // what each sub moves with.
    bool snapped = false;
    for (const RigidEdge& edge : rigid_) {
        if (std::find(play_rigid_.begin(), play_rigid_.end(), edge.constraint) == play_rigid_.end()) {
            snap_in_play(game, edge);
            snapped = true;
        }
    }
    play_rigid_.clear();
    for (const RigidEdge& edge : rigid_) {
        play_rigid_.push_back(edge.constraint);
    }
    // The old assemblies stay readable through the per-slot index until index_play.
    old_play_ = play_;
    old_members_ = play_members_;
    std::vector<PlayAssembly>& next = play_;
    std::vector<PlayMember>& members = play_members_;
    next.clear();
    members.clear();
    for (const Group& group : groups_) {
        // Which old assembly its nodes were in.
        int first_old = -2;
        bool same = true;
        int base = -1;
        for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
            const InstanceId node = group_nodes_[i];
            const int old = old_play_of(node);
            if (first_old == -2) {
                first_old = old;
            } else if (old != first_old) {
                same = false;
            }
            if (old >= 0 && base < 0 && old_play_[static_cast<std::size_t>(old)].root == node) {
                base = old;
            }
        }
        const bool unchanged =
            same && first_old >= 0 && old_play_[static_cast<std::size_t>(first_old)].count == group.count;
        // A PlayerController root keeps the body's origin at its feet: a root
        // change to or from one is a new body.
        const auto controller_change = [&](int old) {
            return old >= 0 && is_controller(game, old_play_[static_cast<std::size_t>(old)].root) !=
                                   is_controller(game, group.root);
        };
        PlayAssembly assembly;
        if (unchanged && !snapped && !controller_change(first_old)) {
            const PlayAssembly& kept = old_play_[static_cast<std::size_t>(first_old)];
            assembly = kept;
            assembly.root = group.root;
            assembly.anchored = group.anchored;
            assembly.has_physics = group.has_physics;
            assembly.first = static_cast<std::uint32_t>(members.size());
            for (std::uint32_t i = kept.first; i < kept.first + kept.count; ++i) {
                members.push_back(old_members_[i]);
            }
            next.push_back(assembly);
            continue;
        }
        if (snapped || controller_change(base)) {
            base = -1;
        }
        assembly.root = group.root;
        assembly.anchored = group.anchored;
        assembly.has_physics = group.has_physics;
        assembly.captured = false;
        assembly.stamp = ++next_stamp_;
        assembly.first = static_cast<std::uint32_t>(members.size());
        assembly.count = group.count;
        if (base >= 0) {
            // The body the old root was on stays, with its frame: the pose it has now.
            const PlayAssembly& kept = old_play_[static_cast<std::size_t>(base)];
            assembly.reuse = kept.stamp;
            assembly.frame = body_pose(game, kept, old_members_);
        } else {
            assembly.frame = node_frame(game, group.root);
        }
        const Matrix4 inverse = matrix4_inverse(assembly.frame);
        for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
            const InstanceId node = group_nodes_[i];
            PlayMember member;
            member.node = node;
            if (const auto* body = dynamic_cast<const PhysicsBase*>(game.instance(node))) {
                member.driven = body->driven_game_object();
            }
            const int old = old_play_of(node);
            if (base >= 0 && old == base) {
                const std::int32_t at = play_member_index_[id_slot(node)];
                member.offset = old_members_[static_cast<std::size_t>(at)].offset;
                member.revision = old_members_[static_cast<std::size_t>(at)].revision;
            } else {
                member.offset = matrix4_multiply(inverse, node_frame(game, node));
            }
            members.push_back(member);
        }
        next.push_back(assembly);
    }
    index_play();
}

void Assemblies::held_targets(const DataModel& game, std::vector<HeldTarget>& out) const {
    out.clear();
    for (const RigidEdge& edge : bone_) {
        Matrix4 frame;
        HeldTarget held;
        held.node = edge.sub_node;
        if (!sub_target(game, edge, frame, held.frame)) {
            continue;
        }
        held.assembly = play_of(edge.sub_node);
        if (held.assembly >= 0) {
            // The body's frame that puts this member on the hand.
            const std::int32_t member = play_member_index_[id_slot(edge.sub_node)];
            if (member < 0) {
                continue;
            }
            const Matrix4& offset = play_members_[static_cast<std::size_t>(member)].offset;
            held.frame = matrix4_orthonormalize(matrix4_multiply(held.frame, matrix4_inverse(offset)));
        }
        out.push_back(held);
    }
}

void Assemblies::place_held(DataModel& game) {
    if (!game.simulation_running()) {
        return;
    }
    sync_play(game);
    if (bone_.empty()) {
        return;
    }
    // In follow order, so a held thing that holds another (a hand on a
    // handle) is placed before what it holds is worked out from it.
    for (const RigidEdge& edge : bone_) {
        Matrix4 frame;
        Matrix4 target;
        if (!sub_target(game, edge, frame, target)) {
            continue;
        }
        const int assembly = play_of(edge.sub_node);
        if (assembly >= 0) {
            const std::int32_t member = play_member_index_[id_slot(edge.sub_node)];
            if (member < 0) {
                continue;
            }
            const Matrix4 body = matrix4_orthonormalize(
                matrix4_multiply(target, matrix4_inverse(play_members_[static_cast<std::size_t>(member)].offset)));
            pose_members(game, static_cast<std::size_t>(assembly), body, Vec3{}, Vec3{});
            continue;
        }
        const PhysicsBase* body = dynamic_cast<const PhysicsBase*>(game.instance(edge.sub_node));
        write_member(game, edge.sub_node, body != nullptr ? body->driven_game_object() : 0, target, Vec3{}, Vec3{}, 0);
    }
}

int Assemblies::play_of(InstanceId node) const {
    const std::uint32_t slot = id_slot(node);
    return node != 0 && slot < play_seen_.size() && play_seen_[slot] == node ? play_index_[slot] : -1;
}

void Assemblies::write_member(DataModel& game, InstanceId node, InstanceId driven, const Matrix4& frame,
                              Vec3 velocity, Vec3 angular, InstanceId skip) {
    DataModel* object = game.instance(node);
    if (auto* body = dynamic_cast<PhysicsBase*>(object)) {
        if (node != skip) {
            body->store_simulated(with_scale(frame, body->transform()), velocity);
            if (auto* rigid = dynamic_cast<PhysicsObject*>(body)) {
                rigid->store_angular_velocity(angular);
            } else if (auto* brush = dynamic_cast<Brush*>(body)) {
                brush->store_angular_velocity(angular);
            }
        }
        if (driven != 0 && driven != skip) {
            if (const GameObject* placed = game.game_object(driven)) {
                game.write_simulated_transform(driven, with_scale(frame, placed->transform()));
            }
        }
        return;
    }
    if (node != skip) {
        if (const auto* placed = dynamic_cast<const SpatialObject*>(object)) {
            game.write_simulated_transform(node, with_scale(frame, placed->transform()));
        }
    }
}

void Assemblies::pose_members(DataModel& game, std::size_t assembly, const Matrix4& body_pose, Vec3 velocity,
                              Vec3 angular, InstanceId skip) {
    if (assembly >= play_.size()) {
        return;
    }
    const PlayAssembly& placed = play_[assembly];
    for (std::uint32_t i = placed.first; i < placed.first + placed.count; ++i) {
        const PlayMember& member = play_members_[i];
        write_member(game, member.node, member.driven, matrix4_multiply(body_pose, member.offset), velocity, angular,
                     skip);
    }
    for (const PlayLoser& loser : play_losers_) {
        if (loser.member < placed.first || loser.member >= placed.first + placed.count || loser.instance == skip) {
            continue;
        }
        if (auto* body = dynamic_cast<PhysicsBase*>(game.instance(loser.instance))) {
            const Matrix4 frame = matrix4_multiply(body_pose, play_members_[loser.member].offset);
            body->store_simulated(with_scale(frame, body->transform()), velocity);
        }
    }
}

// A checked write to a member moves the whole assembly at once: the body
// frame that puts the written instance where it was written, upright when a
// PlayerController is root (and then the written member too is put back on
// that frame). The physics world moves the body there at its next sync.
void Assemblies::play_transform_written(DataModel& game, InstanceId id) {
    if ((flags(id) & kMember) == 0) {
        return;
    }
    const InstanceId node = node_of(id);
    const int index = play_of(node);
    const auto* placed = dynamic_cast<const PVInstance*>(game.instance(id));
    if (index < 0 || placed == nullptr) {
        return;
    }
    const std::int32_t member = play_member_index_[id_slot(node)];
    if (member < 0) {
        return;
    }
    PlayAssembly& assembly = play_[static_cast<std::size_t>(index)];
    if (id == pivot_change_ && node == id) {
        // A recentre: the member's pivot moved, not the member. Its offset
        // follows its new pivot; nothing else moves.
        PlayMember& moved = play_members_[static_cast<std::size_t>(member)];
        moved.offset = matrix4_multiply(matrix4_inverse(body_pose(game, assembly, play_members_)), node_frame(game, node));
        ++moved.revision;
        return;
    }
    const Matrix4 frame = matrix4_multiply(matrix4_orthonormalize(placed->transform()),
                                           matrix4_inverse(play_members_[static_cast<std::size_t>(member)].offset));
    const auto* root = dynamic_cast<const PhysicsBase*>(game.instance(assembly.root));
    const bool controller = dynamic_cast<const PlayerController*>(root) != nullptr;
    assembly.frame = controller ? upright_transform(frame) : matrix4_orthonormalize(frame);
    assembly.teleport = true;
    Vec3 velocity{};
    Vec3 angular{};
    if (root != nullptr) {
        velocity = root->velocity();
        if (const auto* rigid = dynamic_cast<const PhysicsObject*>(root)) {
            angular = rigid->angular_velocity();
        } else if (const auto* brush = dynamic_cast<const Brush*>(root)) {
            angular = brush->angular_velocity();
        }
    }
    pose_members(game, static_cast<std::size_t>(index), assembly.frame, velocity, angular, controller ? 0 : id);
}

void Assemblies::velocity_written(DataModel& game, InstanceId id, bool has_linear, Vec3 linear, bool has_angular,
                                  Vec3 angular) {
    if (suspended_ > 0 || busy_ > 0 || !game.simulation_running()) {
        return;
    }
    refresh(game);
    if ((flags(id) & kMember) == 0) {
        return;
    }
    const int index = play_of(node_of(id));
    if (index < 0) {
        return;
    }
    PlayAssembly& assembly = play_[static_cast<std::size_t>(index)];
    if (has_linear) {
        assembly.velocity_pending = true;
        assembly.velocity = linear;
    }
    if (has_angular) {
        assembly.angular_pending = true;
        assembly.angular = angular;
    }
    // Every member reads the assembly's velocity at once.
    const auto store = [&](PhysicsBase& body) {
        if (has_linear) {
            body.store_simulated(body.transform(), linear);
        }
        if (has_angular) {
            if (auto* rigid = dynamic_cast<PhysicsObject*>(&body)) {
                rigid->store_angular_velocity(angular);
            } else if (auto* brush = dynamic_cast<Brush*>(&body)) {
                brush->store_angular_velocity(angular);
            }
        }
    };
    for (std::uint32_t i = assembly.first; i < assembly.first + assembly.count; ++i) {
        if (auto* body = dynamic_cast<PhysicsBase*>(game.instance(play_members_[i].node))) {
            store(*body);
        }
    }
    for (const PlayLoser& loser : play_losers_) {
        if (loser.member >= assembly.first && loser.member < assembly.first + assembly.count) {
            if (auto* body = dynamic_cast<PhysicsBase*>(game.instance(loser.instance))) {
                store(*body);
            }
        }
    }
}

// An end Attachment of an active RigidConstraint moved in play: that edge is
// snapped again. Only the members on the sub's side of it move, in their
// assembly's body: their offsets change, and the physics world places only
// their shapes again.
void Assemblies::play_attachment_changed(DataModel& game, InstanceId id) {
    for (const RigidEdge& edge : rigid_) {
        const auto* constraint = dynamic_cast<const Constraint*>(game.instance(edge.constraint));
        if (constraint == nullptr || (constraint->pv_instance1_id() != id && constraint->pv_instance2_id() != id)) {
            continue;
        }
        const int index = play_of(edge.sub_node);
        if (index < 0 || index != play_of(edge.parent_node)) {
            continue;
        }
        Matrix4 parent_pivot;
        Matrix4 sub_pivot;
        if (!end_pivot(game, edge.constraint, edge.parent_end, parent_pivot) ||
            !end_pivot(game, edge.constraint, 3 - edge.parent_end, sub_pivot)) {
            continue;
        }
        // The sub's side: what the sub reaches through live edges but this one.
        for (const InstanceId node : side_) {
            side_mark_[id_slot(node)] = 0;
        }
        side_.clear();
        side_.push_back(edge.sub_node);
        side_mark_[id_slot(edge.sub_node)] = 1;
        bool loops = false;
        for (std::size_t at = 0; at < side_.size() && !loops; ++at) {
            for (const Edge& other : edges_) {
                // Holding joins no sides: a hand and what it holds are apart.
                if (!other.live || other.bone_end != 0 || other.constraint == edge.constraint) {
                    continue;
                }
                InstanceId next = 0;
                if (other.node1 == side_[at]) {
                    next = other.node2;
                } else if (other.node2 == side_[at]) {
                    next = other.node1;
                }
                if (next == 0 || side_mark_[id_slot(next)] != 0) {
                    continue;
                }
                if (next == edge.parent_node) {
                    loops = true;
                    break;
                }
                side_mark_[id_slot(next)] = 1;
                side_.push_back(next);
            }
        }
        if (loops) {
            continue;
        }
        PlayAssembly& assembly = play_[static_cast<std::size_t>(index)];
        const Matrix4 pose = body_pose(game, assembly, play_members_);
        const Matrix4 inverse = matrix4_inverse(pose);
        const Matrix4 delta = matrix4_multiply(parent_pivot, matrix4_inverse(sub_pivot));
        for (const InstanceId node : side_) {
            const std::int32_t at = play_member_index_[id_slot(node)];
            if (at < 0) {
                continue;
            }
            PlayMember& member = play_members_[static_cast<std::size_t>(at)];
            const Matrix4 frame = matrix4_orthonormalize(matrix4_multiply(delta, node_frame(game, node)));
            member.offset = matrix4_multiply(inverse, frame);
            ++member.revision;
            write_member(game, node, member.driven, frame, Vec3{}, Vec3{}, 0);
        }
    }
}

AssemblyPivotChange::AssemblyPivotChange(DataModel& game, InstanceId id) : game_(game) {
    game_.assemblies().begin_pivot_change(id);
}

AssemblyPivotChange::~AssemblyPivotChange() { game_.assemblies().end_pivot_change(); }

}  // namespace engine_core
