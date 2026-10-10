#include "DraggerWorld.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Dragger.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
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

LuaSlot matrix_slot(const Matrix4& value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Matrix4;
    slot.transform = value;
    return slot;
}

LuaSlot vector_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

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
            } else if (!record.processed) {
                hover(game, view, point);
            }
        } else if (record.type == UserInputService::kMouseButton1) {
            // A press a game GUI took is not the handles'.
            if (record.state == UserInputService::kBegin && !drag_ && !record.processed) {
                record.processed = begin(game, view, point);
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
        if (dragger == nullptr) {
            continue;
        }
        float depth = 0.f;
        const DraggerFrame frame = dragger_frame(dragger->transform(), dragger->local_space());
        const DraggerHandle hit = pick_handle(frame, view, point, &depth, dragger->transform_mode());
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
    Drag drag;
    drag.dragger = dragger->id();
    drag.playing = game.simulation_running();
    // The drag is measured from where the handles sat at the press, wherever
    // the listeners move the Dragger after.
    if (!begin_drag(dragger_frame(dragger->transform(), dragger->local_space()), view, point, handle, drag.start,
                    dragger->transform_mode())) {
        return false;
    }
    // Edit mode only: play writes are not edits.
    if (!game.simulation_running()) {
        const char* step = dragger->transform_mode() == DraggerMode::Rotation ? "Rotate" : "Move";
        if (std::optional<std::string> id = game.history().try_begin_recording(step)) {
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
    if (dragger == nullptr) {
        end(game);
        return;
    }
    if (drag_->start.mode == DraggerMode::Rotation) {
        const std::optional<float> angle = drag_angle(drag_->start, view, point, dragger->increment());
        if (!angle) {
            return;
        }
        // The turn about where the handles sat at the press.
        const DraggerFrame& frame = drag_->start.frame;
        const Vec3 axis = frame.axes[static_cast<int>(drag_->start.handle)];
        const Vec3 at = frame.origin;
        const Matrix4 turn = matrix4_multiply(
            matrix4_multiply(matrix4_translation(at.x, at.y, at.z), matrix4_axis_angle(axis, *angle)),
            matrix4_translation(-at.x, -at.y, -at.z));
        game.fire_event(dragger->id(), "Dragged", {handle_slot(drag_->start.handle), matrix_slot(turn)});
        return;
    }
    const std::optional<Vec3> offset = drag_offset(drag_->start, view, point, dragger->increment());
    if (!offset) {
        return;
    }
    game.fire_event(dragger->id(), "Dragged", {handle_slot(drag_->start.handle), vector_slot(*offset)});
}

void DraggerWorld::end(DataModel& game) {
    const Drag drag = *drag_;
    drag_.reset();
    closing_ = drag.recording;
    if (auto* dragger = dynamic_cast<Dragger*>(game.instance(drag.dragger))) {
        dragger->set_drag(false, DraggerHandle::None);
        game.fire_event(dragger->id(), "DragEnded", {handle_slot(drag.start.handle)});
    }
}

void DraggerWorld::close_step(DataModel& game) {
    if (closing_.empty()) {
        return;
    }
    const std::string closing = std::move(closing_);
    closing_.clear();
    // Play seals an edit recording when it starts; one sealed so is gone.
    ChangeHistoryService& history = game.history();
    if (history.is_recording_in_progress(closing)) {
        // Commit even when nothing moved: an empty step records nothing, and
        // what the drag's handlers changed stays changed.
        history.finish_recording(closing, FinishRecordingOperation::Commit);
    }
}

bool DraggerWorld::drag_holds(DataModel& game) const {
    return game.alive(drag_->dragger) && game.in_game(drag_->dragger) && game.simulation_running() == drag_->playing;
}

}  // namespace engine_core
