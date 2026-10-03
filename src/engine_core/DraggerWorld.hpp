#pragma once

#include "DraggerMath.hpp"
#include "InputRecord.hpp"
#include "Matrix4.hpp"
#include "types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace engine_core {

class DataModel;
class Dragger;

// Turns mouse input into Dragger hovers and drags. UserInputService hands it
// every record of each dispatch before applying and firing them. It reads
// the view from Workspace.CurrentCamera (Transform, FieldOfView, and
// ViewportSize); with no camera, no size, or the pointer locked, input
// passes through. A press on a handle starts a drag of the nearest Dragger,
// each move reports how far the drag has gone, and the release ends it, each
// firing the Dragger's event. It moves nothing itself: the listeners do. The
// records a drag uses are marked processed. One drag runs at a
// time. In edit mode a drag is one undo step, "Move", which closes at the
// next dispatch after it ends: events are deferred, so what the ending
// step's handlers move joins it. SimulationThread only.
class DraggerWorld {
public:
    void dispatch(DataModel& game, std::vector<InputRecord>& records);

    // The Dragger whose drag runs, or 0.
    InstanceId dragging() const { return drag_ ? drag_->dragger : 0; }

private:
    struct Drag {
        InstanceId dragger = 0;
        DragStart start;
        // Whether the place was playing when the drag began. Play and Stop end it.
        bool playing = false;
        // The undo step this drag opened, or empty when it joined one already open.
        std::string recording;
    };

    // The view to pick and drag in, or false when input should pass through.
    bool view_of(DataModel& game, DraggerView& out) const;
    // The Dragger under point nearest the camera, with its handle.
    Dragger* pick(DataModel& game, const DraggerView& view, Vec2 point, DraggerHandle& handle) const;
    void hover(DataModel& game, const DraggerView& view, Vec2 point);
    bool begin(DataModel& game, const DraggerView& view, Vec2 point);
    void move(DataModel& game, const DraggerView& view, Vec2 point);
    void end(DataModel& game);
    void close_step(DataModel& game);
    // True while the drag's Dragger is alive and under game, and the place
    // has neither started nor stopped playing since it began.
    bool drag_holds(DataModel& game) const;

    std::optional<Drag> drag_;
    // The undo step of a drag that ended, which closes at the next dispatch.
    std::string closing_;
    std::vector<InstanceId> scratch_;
};

}  // namespace engine_core
