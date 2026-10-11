// Assemblies in play (spec §5): the frame and offsets each assembly keeps,
// and the writes that put its members where its body is.

#include "Assemblies.hpp"

#include "Brush.hpp"
#include "DataModel.hpp"
#include "GameObject.hpp"
#include "PhysicsBase.hpp"
#include "PhysicsObject.hpp"
#include "SpatialObject.hpp"

namespace engine_core {

void Assemblies::capture_play(DataModel& game) {
    refresh(game);
    for (const std::uint32_t slot : play_touched_) {
        play_seen_[slot] = 0;
        play_index_[slot] = -1;
        play_member_index_[slot] = -1;
    }
    play_touched_.clear();
    play_.clear();
    play_members_.clear();
    play_losers_.clear();
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
        const auto index = static_cast<std::int32_t>(play_.size());
        for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
            const InstanceId node = group_nodes_[i];
            PlayMember member;
            member.node = node;
            if (const auto* body = dynamic_cast<const PhysicsBase*>(game.instance(node))) {
                member.driven = body->driven_game_object();
            }
            member.offset = matrix4_multiply(inverse, node_frame(game, node));
            const std::uint32_t slot = id_slot(node);
            if (play_seen_[slot] == 0) {
                play_touched_.push_back(slot);
            }
            play_seen_[slot] = node;
            play_index_[slot] = index;
            play_member_index_[slot] = static_cast<std::int32_t>(play_members_.size());
            play_members_.push_back(member);
        }
        play_.push_back(assembly);
    }
    // A PhysicsBase that lost its GameObject to a member is written with that member.
    for (const std::uint32_t slot : touched_) {
        const InstanceId id = seen_[slot];
        const InstanceId node = node_[slot];
        if (node == 0 || id == node || dynamic_cast<const PhysicsBase*>(game.instance(id)) == nullptr) {
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
    play_captured_ = true;
    play_generation_ = game.world_generation();
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

// Task 7 fills it: a member moved in play moves the whole assembly.
void Assemblies::play_transform_written(DataModel&, InstanceId) {}

// Task 8 fills it: an end Attachment moved in play re-snaps its edge.
void Assemblies::play_attachment_changed(DataModel&, InstanceId) {}

}  // namespace engine_core
