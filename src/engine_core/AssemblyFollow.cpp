// The editor's RigidConstraint follow (spec §4): while stopped, each active
// RigidConstraint's sub (the end away from the root) is placed so its pivot
// sits on its parent's.

#include "Assemblies.hpp"

#include "Attachment.hpp"
#include "Constraint.hpp"
#include "DataModel.hpp"
#include "PhysicsBase.hpp"
#include "PlayerController.hpp"
#include "SceneService.hpp"
#include "SpatialObject.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace engine_core {
namespace {

// Bits of a node's follow mark.
constexpr std::uint8_t kMoved = 1u << 0;      // its subs must follow it
constexpr std::uint8_t kDisplaced = 1u << 1;  // it must go back onto its own parent
constexpr std::uint8_t kAlone = 1u << 2;      // moved by itself: as a sub, it goes back by itself
// A chain settles in one pass in follow order; more only when a carried
// child is itself a sub further up that order.
constexpr int kMaxPasses = 8;

// Whether a mover at frame is already at target: within 1e-5 of a unit,
// relative to how far from the origin it is, and 1e-5 in each rotation term.
bool settled(const Matrix4& frame, const Matrix4& target) {
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            if (std::fabs(frame.m[column * 4 + row] - target.m[column * 4 + row]) > 1e-5f) {
                return false;
            }
        }
    }
    for (int row = 0; row < 3; ++row) {
        const float tolerance = 1e-5f * std::max(1.f, std::fabs(target.m[12 + row]));
        if (std::fabs(frame.m[12 + row] - target.m[12 + row]) > tolerance) {
            return false;
        }
    }
    return true;
}

InstanceId view_camera(const DataModel& game) {
    const auto* workspace = dynamic_cast<const Workspace*>(game.instance(game.scene_service("Workspace")));
    return workspace != nullptr ? workspace->current_camera() : 0;
}

}  // namespace

void Assemblies::mark_follow(InstanceId node, std::uint8_t bits) {
    const std::uint32_t slot = id_slot(node);
    if (node == 0 || slot >= follow_mark_.size()) {
        return;
    }
    if (follow_mark_[slot] == 0) {
        follow_marked_.push_back(slot);
    }
    follow_mark_[slot] = static_cast<std::uint8_t>(follow_mark_[slot] | bits);
}

std::uint8_t Assemblies::follow_marks(InstanceId node) const {
    const std::uint32_t slot = id_slot(node);
    return node != 0 && slot < follow_mark_.size() ? follow_mark_[slot] : 0;
}

// Moves what keeps its own world Transform under mover by delta, as MoveTool
// carries what is under a moved thing: GameObjects, Cameras, Lights, and a
// PhysicsBase that moves no GameObject. Attachments and Bones derive from
// their parent, and a driven PhysicsBase follows its GameObject, so neither is
// written. A carried sub goes back on its own parent later in the pass.
void Assemblies::carry(DataModel& game, InstanceId mover, const Matrix4& delta) {
    carry_stack_.clear();
    for (InstanceId child = game.first_child(mover); child != 0; child = game.next_sibling(child)) {
        carry_stack_.push_back(child);
    }
    while (!carry_stack_.empty()) {
        const InstanceId at = carry_stack_.back();
        carry_stack_.pop_back();
        for (InstanceId child = game.first_child(at); child != 0; child = game.next_sibling(child)) {
            carry_stack_.push_back(child);
        }
        DataModel* object = game.instance(at);
        if (dynamic_cast<Attachment*>(object) != nullptr) {
            continue;
        }
        auto* body = dynamic_cast<PhysicsBase*>(object);
        if (body != nullptr && body->driven_game_object() != 0) {
            continue;
        }
        auto* placed = dynamic_cast<PVInstance*>(object);
        if (placed == nullptr || (body == nullptr && dynamic_cast<SpatialObject*>(object) == nullptr)) {
            continue;
        }
        placed->set_pv_transform(matrix4_multiply(delta, placed->transform()));
        if ((flags(at) & kRigid) != 0) {
            mark_follow(node_of(at), kMoved | kDisplaced);
        }
    }
}

bool Assemblies::sub_target(const DataModel& game, const RigidEdge& edge, Matrix4& frame, Matrix4& target) const {
    Matrix4 parent_pivot;
    Matrix4 sub_pivot;
    const int sub_end = 3 - edge.parent_end;
    if (!end_pivot(game, edge.constraint, edge.parent_end, parent_pivot) ||
        !end_pivot(game, edge.constraint, sub_end, sub_pivot)) {
        return false;
    }
    frame = node_frame(game, edge.sub_node);
    const auto* constraint = dynamic_cast<const Constraint*>(game.instance(edge.constraint));
    const InstanceId grip_id =
        constraint == nullptr ? 0 : sub_end == 1 ? constraint->pv_instance1_id() : constraint->pv_instance2_id();
    const auto* grip = dynamic_cast<const Attachment*>(game.instance(grip_id));
    if (grip != nullptr && grip->offset_space() == TransformSpace::World) {
        // A World-space Attachment follows only its holder's position.
        target = frame;
        for (int row = 0; row < 3; ++row) {
            target.m[12 + row] += parent_pivot.m[12 + row] - sub_pivot.m[12 + row];
        }
    } else {
        const Matrix4 local = matrix4_multiply(matrix4_inverse(frame), sub_pivot);
        target = matrix4_multiply(parent_pivot, matrix4_inverse(local));
    }
    if (dynamic_cast<const PlayerController*>(game.instance(edge.sub_node)) != nullptr) {
        target = upright_transform(target);
    } else {
        target = matrix4_orthonormalize(target);
    }
    return true;
}

void Assemblies::gather_side(const RigidEdge& edge, bool alone) {
    for (const InstanceId node : side_) {
        side_mark_[id_slot(node)] = 0;
    }
    side_.clear();
    side_.push_back(edge.sub_node);
    side_mark_[id_slot(edge.sub_node)] = 1;
    if (alone) {
        return;
    }
    for (std::size_t at = 0; at < side_.size(); ++at) {
        for (const Edge& other : edges_) {
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
                for (std::size_t i = 1; i < side_.size(); ++i) {
                    side_mark_[id_slot(side_[i])] = 0;
                }
                side_.resize(1);
                return;
            }
            side_mark_[id_slot(next)] = 1;
            side_.push_back(next);
        }
    }
}

// Puts edge's sub node where its pivot sits on the parent's, and its side
// (gather_side) with it, each keeping its offset. True when it wrote.
bool Assemblies::snap(DataModel& game, const RigidEdge& edge, InstanceId camera, bool alone) {
    const InstanceId mover = mover_of(game, edge.sub_node);
    if (mover == camera || (camera != 0 && mover_of(game, edge.parent_node) == camera)) {
        return false;
    }
    Matrix4 frame;
    Matrix4 target;
    if (!sub_target(game, edge, frame, target)) {
        return false;
    }
    if (settled(frame, target)) {
        return false;
    }
    if (dynamic_cast<PVInstance*>(game.instance(mover)) == nullptr) {
        return false;
    }
    const Matrix4 delta = matrix4_multiply(target, matrix4_inverse(frame));
    gather_side(edge, alone);
    // Each node's mover once. One under another mover in the tree is carried
    // by it, so it is not written itself.
    side_movers_.clear();
    for (const InstanceId node : side_) {
        const InstanceId placed = mover_of(game, node);
        if (placed == 0 || placed == camera || (side_mark_[id_slot(placed)] & 2u) != 0) {
            continue;
        }
        side_mark_[id_slot(placed)] = static_cast<std::uint8_t>(side_mark_[id_slot(placed)] | 2u);
        side_movers_.push_back(placed);
    }
    bool wrote = false;
    for (const InstanceId placed : side_movers_) {
        bool carried = false;
        int guard = 0;
        for (InstanceId above = game.parent(placed); above != DataModel::kNoParent && above != 0 &&
                                                     guard < static_cast<int>(DataModel::kMaxInstances);
             above = game.parent(above), ++guard) {
            if ((side_mark_[id_slot(above)] & 2u) != 0) {
                carried = true;
                break;
            }
        }
        auto* object = dynamic_cast<PVInstance*>(game.instance(placed));
        if (carried || object == nullptr) {
            continue;
        }
        // The sub goes exactly to its target; the rest keep their offsets from it.
        const Matrix4 next = placed == mover ? with_scale(target, object->transform())
                                             : matrix4_multiply(delta, object->transform());
        if (object->set_pv_transform(next)) {
            continue;
        }
        carry(game, placed, delta);
        wrote = true;
    }
    for (std::size_t i = 1; i < side_.size(); ++i) {
        mark_follow(side_[i], kMoved);
    }
    for (const InstanceId placed : side_movers_) {
        side_mark_[id_slot(placed)] = static_cast<std::uint8_t>(side_mark_[id_slot(placed)] & ~2u);
    }
    return wrote;
}

int Assemblies::follow(DataModel& game, InstanceId start, bool alone) {
    if (game.simulation_running() || busy_ > 0) {
        return 0;
    }
    refresh(game);
    if (rigid_.empty() && bone_.empty()) {
        return 0;
    }
    struct Busy {
        int& depth;
        explicit Busy(int& depth) : depth(depth) { ++depth; }
        ~Busy() { --depth; }
    } busy(busy_);
    for (const std::uint32_t slot : follow_marked_) {
        follow_mark_[slot] = 0;
    }
    follow_marked_.clear();
    if (start != 0) {
        mark_follow(start, static_cast<std::uint8_t>(kMoved | kDisplaced | (alone ? kAlone : 0u)));
    }
    const InstanceId camera = view_camera(game);
    int written = 0;
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        bool wrote = false;
        // Bone edges after the rest: a hand is where its GameObject is, and
        // what it holds may be a parent further on, for the next pass.
        for (const std::vector<RigidEdge>* edges : {&rigid_, &bone_}) {
            for (const RigidEdge& edge : *edges) {
                const bool parent_moved = (start == 0 && pass == 0) || (follow_marks(edge.parent_node) & kMoved) != 0;
                const std::uint8_t sub = follow_marks(edge.sub_node);
                const bool due = parent_moved || (sub & kDisplaced) != 0;
                // A sub moved by itself goes back by itself: what is welded to it did not move.
                const bool by_itself = !parent_moved && (sub & kAlone) != 0;
                if (due && snap(game, edge, camera, by_itself)) {
                    ++written;
                    wrote = true;
                    mark_follow(edge.sub_node, kMoved);
                }
            }
        }
        if (!wrote) {
            break;
        }
    }
    return written;
}

}  // namespace engine_core
