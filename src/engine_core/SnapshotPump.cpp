#include "SnapshotPump.hpp"

#include "GameObject.hpp"

#include <algorithm>

namespace engine_core {

void SnapshotPump::reserve(std::size_t instances) {
    base_.instances.reserve(instances);
    buffers_[0].instances.reserve(instances);
    buffers_[1].instances.reserve(instances);
    overrides_.reserve(instances);
    base_index_.assign(instances, -1);
}

void SnapshotPump::begin_prerender_window(DataModel& game) {
    window_open_ = true;
    game.set_prerender_window(true);
}

void SnapshotPump::end_prerender_window(DataModel& game) {
    game.set_prerender_window(false);
    window_open_ = false;
}

void SnapshotPump::override_visual(const SnapshotOverride& override) {
    if (!window_open_ || thread_role() != ThreadRole::Render) {
        contract_fail("SnapshotOverride is only valid inside RenderStepped or PreRender");
    }
    if (overrides_.size() == overrides_.capacity()) {
        contract_fail("snapshot override capacity exhausted");
    }
    overrides_.push_back(override);
}

void SnapshotPump::set_camera(const Transform& camera) {
    if (!window_open_ || thread_role() != ThreadRole::Render) {
        contract_fail("camera snapshot writes happen inside RenderStepped or PreRender");
    }
    pending_camera_ = camera;
    camera_pending_ = true;
}

VisualInstance* SnapshotPump::base_find(InstanceId id) {
    const std::uint32_t index = id_slot(id);
    if (index >= base_index_.size()) {
        return nullptr;
    }
    const int position = base_index_[index];
    if (position < 0 || static_cast<std::size_t>(position) >= base_.instances.size()) {
        return nullptr;
    }
    VisualInstance& inst = base_.instances[static_cast<std::size_t>(position)];
    if (inst.id != id) {
        return nullptr;
    }
    return &inst;
}

void SnapshotPump::remember(InstanceId id, int position) {
    const std::uint32_t index = id_slot(id);
    if (index < base_index_.size()) {
        base_index_[index] = position;
    }
}

void SnapshotPump::erase_base(InstanceId id) {
    const std::uint32_t index = id_slot(id);
    if (index >= base_index_.size()) {
        return;
    }
    const int position = base_index_[index];
    if (position < 0 || static_cast<std::size_t>(position) >= base_.instances.size()) {
        base_index_[index] = -1;
        return;
    }
    const InstanceId moved = base_.instances.back().id;
    base_.instances[static_cast<std::size_t>(position)] = base_.instances.back();
    base_.instances.pop_back();
    base_index_[index] = -1;
    if (static_cast<std::size_t>(position) < base_.instances.size()) {
        remember(moved, position);
    }
}

void SnapshotPump::apply_live(DataModel& game, const Invalidation& change) {
    if (any(change.fields, VisualField::Removed) || !game.alive(change.id)) {
        erase_base(change.id);
        return;
    }
    const GameObject* object = game.game_object(change.id);
    if (object == nullptr) {
        return;
    }
    VisualInstance* inst = base_find(change.id);
    if (inst == nullptr) {
        if (base_.instances.size() == base_.instances.capacity()) {
            contract_fail("snapshot instance capacity exhausted");
        }
        remember(change.id, static_cast<int>(base_.instances.size()));
        base_.instances.push_back(VisualInstance{});
        inst = &base_.instances.back();
        inst->id = change.id;
    }
    if (any(change.fields, VisualField::Transform)) {
        inst->world = object->transform();
        inst->transform_origin = change.origin;
    }
    if (any(change.fields, VisualField::Color)) {
        inst->color = object->color();
        inst->color_origin = change.origin;
    }
    if (any(change.fields, VisualField::Size)) {
        if (object->copy_size(inst->size)) {
            inst->size_origin = change.origin;
        }
    }
    inst->alive = true;
}

void SnapshotPump::resync(DataModel& game) {
    base_.instances.clear();
    std::fill(base_index_.begin(), base_index_.end(), -1);
    game.for_each_game_object([&](const GameObject& object) {
        VisualInstance inst;
        inst.id = object.id();
        inst.world = object.transform();
        inst.color = object.color();
        object.copy_size(inst.size);
        inst.alive = true;
        inst.transform_origin = WriteOrigin::Simulation;
        inst.color_origin = WriteOrigin::Simulation;
        inst.size_origin = WriteOrigin::Simulation;
        remember(object.id(), static_cast<int>(base_.instances.size()));
        base_.instances.push_back(inst);
    });
}

void SnapshotPump::blit(VisualSnapshot& dst) const {
    dst.camera = base_.camera;
    dst.instances.resize(base_.instances.size());
    std::copy(base_.instances.begin(), base_.instances.end(), dst.instances.begin());
}

void SnapshotPump::apply_overrides(VisualSnapshot& dst) {
    for (const SnapshotOverride& override : overrides_) {
        for (VisualInstance& inst : dst.instances) {
            if (inst.id != override.id || !inst.alive) {
                continue;
            }
            if (any(override.field, VisualField::Transform)) {
                inst.world = override.transform;
                inst.transform_origin = WriteOrigin::SnapshotOverride;
            }
            if (any(override.field, VisualField::Color)) {
                inst.color = override.color;
                inst.color_origin = WriteOrigin::SnapshotOverride;
            }
        }
    }
}

void SnapshotPump::prepare_copy(DataModel& game) {
    InvalidationQueue& queue = game.invalidations();
    if (queue.take_overflow() || game.consume_resync()) {
        resync(game);
    } else {
        queue.drain([&](const Invalidation& change) { apply_live(game, change); });
    }
    if (camera_pending_) {
        base_.camera = pending_camera_;
        camera_pending_ = false;
    }
    VisualSnapshot& back = buffers_[1 - front_];
    blit(back);
    apply_overrides(back);
    back.frame = next_frame_++;
    back.camera = base_.camera;
    overrides_.clear();
}

void SnapshotPump::publish() {
    front_ ^= 1;
    published_frame_.store(buffers_[front_].frame);
}

const VisualSnapshot& SnapshotPump::front() const { return buffers_[front_]; }

const VisualInstance* SnapshotPump::find(InstanceId id) const {
    for (const VisualInstance& inst : buffers_[front_].instances) {
        if (inst.alive && inst.id == id) {
            return &inst;
        }
    }
    return nullptr;
}

}  // namespace engine_core
