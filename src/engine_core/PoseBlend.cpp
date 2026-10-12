#include "PoseBlend.hpp"

#include "DataModel.hpp"
#include "PhysicsBase.hpp"

#include <algorithm>

namespace engine_core {

void PoseBlend::begin_capture() {
    entries_.clear();
    if (++stamp_ == 0) {
        // Wrapped: no slot may still hold a stamp the next captures reuse.
        std::fill(slot_stamp_.begin(), slot_stamp_.end(), 0u);
        stamp_ = 1;
    }
    capturing_ = true;
}

void PoseBlend::record(InstanceId id, Kind kind, const Matrix4& before, const Matrix4& after) {
    if (!capturing_) {
        return;
    }
    const std::uint32_t slot = id_slot(id);
    if (slot >= slot_stamp_.size()) {
        const std::size_t size = std::max<std::size_t>(1024, std::size_t{slot} * 2);
        slot_stamp_.resize(size, 0u);
        slot_entry_.resize(size, 0u);
    }
    if (slot_stamp_[slot] == stamp_ && entries_[slot_entry_[slot]].id == id) {
        entries_[slot_entry_[slot]].after = after;
        return;
    }
    slot_stamp_[slot] = stamp_;
    slot_entry_[slot] = static_cast<std::uint32_t>(entries_.size());
    entries_.push_back(Entry{id, kind, before, after});
}

void PoseBlend::clear() {
    entries_.clear();
    swaps_.clear();
    if (++stamp_ == 0) {
        std::fill(slot_stamp_.begin(), slot_stamp_.end(), 0u);
        stamp_ = 1;
    }
    armed_ = false;
    capturing_ = false;
}

Matrix4 PoseBlend::blended(const Entry& entry, const Matrix4& current, double alpha) {
    if (alpha >= 1.0) {
        return current;
    }
    const Matrix4 drawn = matrix4_lerp(entry.before, entry.after, alpha < 0.0 ? 0.0 : alpha);
    return matrix4_multiply(matrix4_multiply(drawn, matrix4_inverse(entry.after)), current);
}

void swap_in_blend(DataModel& game, double alpha) {
    PoseBlend& blend = game.pose_blend();
    blend.swaps_.clear();
    for (const PoseBlend::Entry& entry : blend.entries_) {
        Matrix4 truth;
        if (entry.kind == PoseBlend::Kind::Body) {
            auto* body = dynamic_cast<PhysicsBase*>(game.instance(entry.id));
            if (body == nullptr) {
                continue;
            }
            truth = body->transform();
            const Matrix4 shown = PoseBlend::blended(entry, truth, alpha);
            body->store_pose(shown);
            blend.swaps_.push_back({entry.id, entry.kind, truth, shown});
        } else if (game.spatial_transform(entry.id, truth)) {
            const Matrix4 shown = PoseBlend::blended(entry, truth, alpha);
            game.store_visual_transform(entry.id, shown);
            blend.swaps_.push_back({entry.id, entry.kind, truth, shown});
        }
    }
}

void swap_out_blend(DataModel& game) {
    PoseBlend& blend = game.pose_blend();
    for (const PoseBlend::Swap& swap : blend.swaps_) {
        if (swap.kind == PoseBlend::Kind::Body) {
            auto* body = dynamic_cast<PhysicsBase*>(game.instance(swap.id));
            if (body != nullptr && same_matrix4(body->transform(), swap.shown)) {
                body->store_pose(swap.truth);
            }
            continue;
        }
        Matrix4 now;
        if (game.spatial_transform(swap.id, now) && same_matrix4(now, swap.shown)) {
            game.store_visual_transform(swap.id, swap.truth);
        }
    }
    blend.swaps_.clear();
}

}  // namespace engine_core
