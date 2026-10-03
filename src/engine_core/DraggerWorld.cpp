#include "DraggerWorld.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Dragger.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "PhysicsObject.hpp"
#include "SceneService.hpp"
#include "UserInputService.hpp"

namespace engine_core {
namespace {

LuaSlot handle_slot(DraggerHandle handle) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &dragger_handle_enum();
    slot.number = static_cast<int>(handle);
    return slot;
}

LuaSlot vector_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

PVInstance* pv_of(DataModel& game, InstanceId id) { return dynamic_cast<PVInstance*>(game.instance(id)); }

}  // namespace

void DraggerWorld::dispatch(DataModel& game, std::vector<InputRecord>& records) {
    close_step(game);
    if (drag_ && !drag_holds(game)) {
        end(game);
    }
    DraggerView view;
    if (!view_of(game, view)) {
        if (drag_) {
            end(game);
        }
        return;
    }
    for (InputRecord& record : records) {
        const Vec2 point{record.position.x, record.position.y};
        if (record.type == UserInputService::kMouseMovement) {
            if (drag_) {
                move(game, view, point);
                record.processed = true;
            } else {
                hover(game, view, point);
            }
        } else if (record.type == UserInputService::kMouseButton1) {
            if (record.state == UserInputService::kBegin && !drag_) {
                record.processed = begin(game, view, point) || record.processed;
            } else if ((record.state == UserInputService::kEnd || record.state == UserInputService::kCancel) && drag_) {
                end(game);
                record.processed = true;
            }
        }
    }
}

bool DraggerWorld::view_of(DataModel& game, DraggerView& out) const {
    if (game.input().mouse_behavior() != UserInputService::kMouseBehaviorDefault) {
        return false;
    }
    const auto* workspace = dynamic_cast<const Workspace*>(game.instance(game.scene_service("Workspace")));
    const InstanceId id = workspace != nullptr ? workspace->current_camera() : 0;
    const auto* camera = dynamic_cast<const Camera*>(game.instance(id));
    if (camera == nullptr) {
        return false;
    }
    const Vec2 size = camera->viewport_size();
    if (size.x <= 0.f || size.y <= 0.f) {
        return false;
    }
    out.camera = camera->transform();
    out.fov_degrees = static_cast<float>(camera->field_of_view());
    out.size = size;
    return true;
}

Dragger* DraggerWorld::pick(DataModel& game, const DraggerView& view, Vec2 point, DraggerHandle& handle) const {
    std::vector<InstanceId> ids;
    game.draggers(ids);
    Dragger* best = nullptr;
    float best_depth = 0.f;
    handle = DraggerHandle::None;
    for (InstanceId id : ids) {
        auto* dragger = dynamic_cast<Dragger*>(game.instance(id));
        const InstanceId target = dragger != nullptr ? dragger->target() : 0;
        const PVInstance* pv = pv_of(game, target);
        if (pv == nullptr) {
            continue;
        }
        float depth = 0.f;
        const DraggerFrame frame = dragger_frame(pv->transform(), dragger->local_space());
        const DraggerHandle hit = pick_handle(frame, view, point, &depth);
        if (hit != DraggerHandle::None && (best == nullptr || depth < best_depth)) {
            best = dragger;
            best_depth = depth;
            handle = hit;
        }
    }
    return best;
}

void DraggerWorld::hover(DataModel& game, const DraggerView& view, Vec2 point) {
    DraggerHandle handle = DraggerHandle::None;
    Dragger* over = pick(game, view, point, handle);
    game.draggers(scratch_);
    for (InstanceId id : scratch_) {
        if (auto* dragger = dynamic_cast<Dragger*>(game.instance(id))) {
            dragger->set_hovered(dragger == over ? handle : DraggerHandle::None);
        }
    }
}

bool DraggerWorld::begin(DataModel& game, const DraggerView& view, Vec2 point) {
    DraggerHandle handle = DraggerHandle::None;
    Dragger* dragger = pick(game, view, point, handle);
    if (dragger == nullptr) {
        return false;
    }
    const InstanceId target = dragger->target();
    const PVInstance* pv = pv_of(game, target);
    Drag drag;
    drag.dragger = dragger->id();
    drag.target = target;
    drag.start_transform = pv->transform();
    if (!begin_drag(dragger_frame(drag.start_transform, dragger->local_space()), view, point, handle, drag.start)) {
        return false;
    }
    // Edit mode only: play writes are not edits.
    if (!game.simulation_running()) {
        if (std::optional<std::string> id = game.history().try_begin_recording("Move")) {
            drag.recording = std::move(*id);
        }
    }
    drag_ = drag;
    dragger->set_drag(true, handle);
    game.fire_event(dragger->id(), "DragBegan", {handle_slot(handle)});
    return true;
}

void DraggerWorld::move(DataModel& game, const DraggerView& view, Vec2 point) {
    auto* dragger = dynamic_cast<Dragger*>(game.instance(drag_->dragger));
    PVInstance* pv = pv_of(game, drag_->target);
    if (dragger == nullptr || pv == nullptr) {
        end(game);
        return;
    }
    const std::optional<Vec3> offset = drag_offset(drag_->start, view, point, dragger->increment());
    if (!offset) {
        return;
    }
    Matrix4 moved = drag_->start_transform;
    moved.m[12] += offset->x;
    moved.m[13] += offset->y;
    moved.m[14] += offset->z;
    if (pv->set_pv_transform(moved)) {
        end(game);
        return;
    }
    // Held by the hand: whatever speed it had would carry it off on release.
    if (game.simulation_running()) {
        if (auto* body = dynamic_cast<PhysicsObject*>(pv)) {
            body->set_velocity(Vec3{});
            body->set_angular_velocity(Vec3{});
        }
    }
    drag_->moved = drag_->moved || offset->x != 0.f || offset->y != 0.f || offset->z != 0.f;
    game.fire_event(dragger->id(), "Dragged", {handle_slot(drag_->start.handle), vector_slot(*offset)});
}

void DraggerWorld::end(DataModel& game) {
    const Drag drag = *drag_;
    drag_.reset();
    if (!drag.recording.empty()) {
        closing_ = Closing{drag.recording, drag.moved};
    }
    if (auto* dragger = dynamic_cast<Dragger*>(game.instance(drag.dragger))) {
        dragger->set_drag(false, DraggerHandle::None);
        game.fire_event(dragger->id(), "DragEnded", {handle_slot(drag.start.handle)});
    }
}

void DraggerWorld::close_step(DataModel& game) {
    if (!closing_) {
        return;
    }
    const Closing closing = *closing_;
    closing_.reset();
    // Play seals an edit recording when it starts; one sealed so is gone.
    ChangeHistoryService& history = game.history();
    if (history.is_recording_in_progress(closing.recording)) {
        history.finish_recording(closing.recording,
                                 closing.moved ? FinishRecordingOperation::Commit : FinishRecordingOperation::Cancel);
    }
}

bool DraggerWorld::drag_holds(DataModel& game) const {
    const auto* dragger = dynamic_cast<const Dragger*>(game.instance(drag_->dragger));
    return dragger != nullptr && dragger->target() == drag_->target;
}

}  // namespace engine_core
