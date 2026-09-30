#include "SnapshotPump.hpp"

#include "GameObject.hpp"

#include <algorithm>

namespace engine_core {

void SnapshotPump::reserve(std::size_t instances) {
    base_.instances.reserve(instances);
    buffers_[0].instances.reserve(instances);
    buffers_[1].instances.reserve(instances);
    overrides_.reserve(instances);
    // Keyed by slot, so it covers every slot whatever the row capacity.
    base_ids_.reserve(DataModel::kMaxInstances);
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
    const int position = base_ids_.position(id);
    return position < 0 ? nullptr : &base_.instances[static_cast<std::size_t>(position)];
}

void SnapshotPump::erase_base(InstanceId id) {
    const int position = base_ids_.erase(id);
    if (position < 0) {
        return;
    }
    // base_ids_ swapped its last id into position; mirror that on the rows.
    base_.instances[static_cast<std::size_t>(position)] = base_.instances.back();
    base_.instances.pop_back();
}

void SnapshotPump::apply_live(DataModel& game, const Invalidation& change) {
    if (any(change.fields, VisualField::Removed) || !game.alive(change.id)) {
        erase_base(change.id);
        return;
    }
    // Only GameObjects under Workspace have rows. This drops the row of one
    // that left, and ignores a change to one that was never in.
    if (!game.in_workspace(change.id)) {
        erase_base(change.id);
        return;
    }
    const GameObject* object = game.game_object(change.id);
    if (object == nullptr) {
        return;
    }
    // A row that joins reads every field: none was kept while it was out.
    bool whole = any(change.fields, VisualField::Ancestry);
    VisualInstance* inst = base_find(change.id);
    if (inst == nullptr) {
        if (base_.instances.size() == base_.instances.capacity()) {
            contract_fail("snapshot instance capacity exhausted");
        }
        base_ids_.insert(change.id);
        base_.instances.push_back(VisualInstance{});
        inst = &base_.instances.back();
        inst->id = change.id;
        whole = true;
    }
    if (whole || any(change.fields, VisualField::Transform)) {
        inst->world = object->transform();
        inst->transform_origin = change.origin;
    }
    if (whole || any(change.fields, VisualField::Color)) {
        inst->color = object->color();
        inst->color_origin = change.origin;
    }
    if (whole || any(change.fields, VisualField::Size)) {
        if (object->copy_size(inst->size)) {
            inst->size_origin = change.origin;
        }
    }
    inst->alive = true;
}

void SnapshotPump::resync(DataModel& game) {
    base_.instances.clear();
    base_ids_.clear();
    game.for_each_rendered([&](const GameObject& object) {
        VisualInstance inst;
        inst.id = object.id();
        inst.world = object.transform();
        inst.color = object.color();
        object.copy_size(inst.size);
        inst.alive = true;
        inst.transform_origin = WriteOrigin::Simulation;
        inst.color_origin = WriteOrigin::Simulation;
        inst.size_origin = WriteOrigin::Simulation;
        base_ids_.insert(object.id());
        base_.instances.push_back(inst);
    });
}

void SnapshotPump::blit(VisualSnapshot& dst) const {
    dst.camera = base_.camera;
    dst.instances.resize(base_.instances.size());
    std::copy(base_.instances.begin(), base_.instances.end(), dst.instances.begin());
}

void SnapshotPump::apply_overrides(VisualSnapshot& dst) {
    // dst is a copy of base_, so base_ids_ gives each instance's position.
    for (const SnapshotOverride& override : overrides_) {
        const int position = base_ids_.position(override.id);
        if (position < 0 || static_cast<std::size_t>(position) >= dst.instances.size()) {
            continue;
        }
        VisualInstance& inst = dst.instances[static_cast<std::size_t>(position)];
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

void SnapshotPump::prepare_copy(DataModel& game) {
    take_changes(game);
    finish_copy();
}

void SnapshotPump::take_changes(DataModel& game) {
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
}

void SnapshotPump::finish_copy() {
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
