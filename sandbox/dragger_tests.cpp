// The Dragger: translate handles that move a PVInstance, and the math under them.

#include "support.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "Dragger.hpp"
#include "DraggerMath.hpp"
#include "Folder.hpp"
#include "Light.hpp"
#include "PhysicsObject.hpp"
#include "SceneService.hpp"
#include "SelectionService.hpp"
#include "SnapshotPump.hpp"
#include "UserInputService.hpp"
#include "Matrix4.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace {

using engine_core::InstanceId;

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const auto& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

engine_core::DraggerView view_of(float width, float height) {
    engine_core::DraggerView view;
    view.camera = engine_core::matrix4_identity();
    view.fov_degrees = 90.f;
    view.size = engine_core::Vec2{width, height};
    return view;
}

engine_core::DraggerFrame world_frame(engine_core::Vec3 origin) {
    return engine_core::dragger_frame(engine_core::matrix4_translation(origin.x, origin.y, origin.z), false);
}

bool close(float a, float b, float tolerance = 1e-3f) { return std::abs(a - b) <= tolerance; }

// A camera at the origin looking down -Z with a 90 degree view, 200 x 200 points,
// a Dragger at (0, 0, -10), and a part beside it for listeners to move. The X
// arrow runs along screen y = 100 from x = 100 to 200, and 20 points drag 2 studs.
struct DragRig {
    ScriptRig rig;
    InstanceId camera = 0;
    InstanceId part = 0;
    InstanceId dragger = 0;
    // What Dragged reported, in order: the X of each offset.
    std::vector<float> offsets;

    DragRig() {
        engine_core::DataModel& game = rig.game;
        engine_core::Camera& made = game.create<engine_core::Camera>();
        camera = made.id();
        game.set_parent(camera, game.scene_service("Workspace"));
        made.set_field_of_view(90);
        made.set_viewport_size(engine_core::Vec2{200, 200});
        workspace().set_current_camera(camera);
        part = add_part(game.scene_service("Workspace"), engine_core::matrix4_translation(0, 5, -10));
        rig.game.set_name(part, "Part");
        dragger = add_dragger(game.scene_service("Workspace"), engine_core::matrix4_translation(0, 0, -10));
        game.history().end_gesture();
        game.history().reset_waypoints();
    }

    engine_core::Workspace& workspace() {
        return *dynamic_cast<engine_core::Workspace*>(rig.game.instance(rig.game.scene_service("Workspace")));
    }
    InstanceId add_part(InstanceId parent, const engine_core::Matrix4& transform) {
        engine_core::GameObject& made = rig.game.create_game_object();
        rig.game.set_parent(made.id(), parent);
        made.set_transform(transform);
        return made.id();
    }
    InstanceId add_dragger(InstanceId parent, const engine_core::Matrix4& transform) {
        engine_core::Dragger& made = rig.game.create<engine_core::Dragger>();
        rig.game.set_parent(made.id(), parent);
        REQUIRE_FALSE(made.set_transform(transform).has_value());
        return made.id();
    }
    // Records each Dragged offset's X into offsets.
    void listen(InstanceId id) {
        rig.game.event_signal(id, "Dragged").connect([this](InstanceId, engine_core::Field) {
            const engine_core::EventArgs* args = rig.game.events().current_args();
            offsets.push_back((*args)[1].vec.x);
        });
    }
    engine_core::Matrix4 transform(InstanceId id) {
        return dynamic_cast<engine_core::PVInstance*>(rig.game.instance(id))->transform();
    }
    engine_core::Dragger& handles() { return *dynamic_cast<engine_core::Dragger*>(rig.game.instance(dragger)); }

    // One step: in play, the input is dispatched before PreAnimation; in edit, by the tool step.
    void step() {
        if (rig.game.simulation_running()) {
            rig.scheduler.run_phase(engine_core::Phase::PreAnimation, 1.0 / 60.0);
            rig.game.events().drain();
        }
        rig.frames(1);
    }
    void press(float x, float y) {
        rig.game.input().post_mouse_button(0, true, x, y);
        step();
    }
    void move(float x, float y) {
        rig.game.input().post_mouse_move(x, y);
        step();
    }
    void release(float x, float y) {
        rig.game.input().post_mouse_button(0, false, x, y);
        step();
    }
};
}  // namespace

TEST_CASE("VP1 Camera.ViewportSize is read-only to scripts, unsaved, and not undone", "[VP1]") {
    ScriptRig rig;
    engine_core::Camera& camera = rig.game.create<engine_core::Camera>();
    rig.game.set_parent(camera.id(), rig.game.scene_service("Workspace"));
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    camera.set_viewport_size(engine_core::Vec2{800.f, 600.f});
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    rig.runtime.run_chunk(R"(
        local camera
        for _, child in workspace:GetChildren() do
            if child.ClassName == "Camera" then camera = child end
        end
        print("size", camera.ViewportSize.X, camera.ViewportSize.Y)
        print("refused", not pcall(function() camera.ViewportSize = Vector2.new(1, 1) end))
    )");
    rig.frames(1);
    const auto output = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output, "size\t800\t600\n"));
    REQUIRE(has_line(output, "refused\ttrue\n"));
}

TEST_CASE("DR1 the center ray is the look, and a corner ray is the frustum's corner", "[DR1]") {
    const auto view = view_of(200, 100);
    const auto center = engine_core::viewport_ray(view, {100, 50});
    REQUIRE((close(center.direction.x, 0) && close(center.direction.y, 0) && close(center.direction.z, -1)));
    // aspect 2, tan_half 1: the top-left corner is (-2, 1, -1), normalized.
    const auto corner = engine_core::viewport_ray(view, {0, 0});
    const float n = std::sqrt(6.f);
    REQUIRE((close(corner.direction.x, -2 / n) && close(corner.direction.y, 1 / n) && close(corner.direction.z, -1 / n)));
}

TEST_CASE("DR2 an arrow is the same number of pixels long near and far", "[DR2]") {
    const auto view = view_of(200, 200);
    for (float depth : {5.f, 500.f}) {
        const engine_core::Vec3 origin{0, 0, -depth};
        const float length = engine_core::kArrowPixels * engine_core::handle_scale(view, origin);
        engine_core::Vec2 a;
        engine_core::Vec2 b;
        REQUIRE(engine_core::project_point(view, origin, a));
        REQUIRE(engine_core::project_point(view, {length, 0, -depth}, b));
        REQUIRE(close(b.x - a.x, engine_core::kArrowPixels, 1e-2f));
    }
}

// At (0, 0, -10) in a 200 x 200 view, one pixel is 0.1 studs: the X arrow runs from
// screen (100, 100) to (200, 100), and the XY square covers x 125..140, y 60..75.
TEST_CASE("DR3 an arrow picks within 8 pixels, and a plane square picks inside itself", "[DR3]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    REQUIRE(engine_core::pick_handle(frame, view, {150, 108}, nullptr) == engine_core::DraggerHandle::X);
    REQUIRE(engine_core::pick_handle(frame, view, {150, 109}, nullptr) == engine_core::DraggerHandle::None);
    REQUIRE(engine_core::pick_handle(frame, view, {100, 50}, nullptr) == engine_core::DraggerHandle::Y);
    REQUIRE(engine_core::pick_handle(frame, view, {130, 70}, nullptr) == engine_core::DraggerHandle::XY);
}

TEST_CASE("DR4 an arrow at the camera and a plane seen edge-on are hidden", "[DR4]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    REQUIRE_FALSE(engine_core::handle_visible(frame, view, engine_core::DraggerHandle::Z));
    REQUIRE_FALSE(engine_core::handle_visible(frame, view, engine_core::DraggerHandle::YZ));
    REQUIRE_FALSE(engine_core::handle_visible(frame, view, engine_core::DraggerHandle::XZ));
    REQUIRE(engine_core::handle_visible(frame, view, engine_core::DraggerHandle::XY));
    REQUIRE(engine_core::pick_handle(frame, view, {100, 100}, nullptr) != engine_core::DraggerHandle::Z);
}

TEST_CASE("DR5 an axis drag stays on its axis, wherever the arrow was grabbed", "[DR5]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    engine_core::DragStart start;
    REQUIRE(engine_core::begin_drag(frame, view, {150, 100}, engine_core::DraggerHandle::X, start));
    auto offset = engine_core::drag_offset(start, view, {170, 100}, 0);
    REQUIRE(offset);
    REQUIRE((close(offset->x, 2) && close(offset->y, 0) && close(offset->z, 0)));
    // Off the arrow's line, the offset is still along the axis alone.
    offset = engine_core::drag_offset(start, view, {170, 90}, 0);
    REQUIRE(offset);
    REQUIRE((offset->x > 1.5f && close(offset->y, 0) && close(offset->z, 0)));
    REQUIRE(engine_core::begin_drag(frame, view, {120, 100}, engine_core::DraggerHandle::X, start));
    offset = engine_core::drag_offset(start, view, {140, 100}, 0);
    REQUIRE((offset && close(offset->x, 2)));
}

TEST_CASE("DR6 a plane drag stays in its plane", "[DR6]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    engine_core::DragStart start;
    REQUIRE(engine_core::begin_drag(frame, view, {130, 70}, engine_core::DraggerHandle::XY, start));
    const auto offset = engine_core::drag_offset(start, view, {150, 50}, 0);
    REQUIRE(offset);
    REQUIRE((close(offset->x, 2) && close(offset->y, 2) && close(offset->z, 0)));
}

TEST_CASE("DR7 snapping rounds each component along the frame's own axes", "[DR7]") {
    const auto view = view_of(200, 200);
    engine_core::DragStart start;
    REQUIRE(engine_core::begin_drag(world_frame({0, 0, -10}), view, {150, 100}, engine_core::DraggerHandle::X, start));
    auto offset = engine_core::drag_offset(start, view, {174, 100}, 1.0);
    REQUIRE((offset && close(offset->x, 2)));
    // Local: turned 45 degrees about Z. The offset is a whole number of studs along X', none along Y'.
    const engine_core::Matrix4 turned = engine_core::matrix4_multiply(
        engine_core::matrix4_translation(0, 0, -10), engine_core::matrix4_axis_angle({0, 0, 1}, 3.14159265 / 4));
    const auto frame = engine_core::dragger_frame(turned, true);
    engine_core::Vec2 tip;
    REQUIRE(engine_core::project_point(view, {frame.axes[0].x * 5, frame.axes[0].y * 5, -10}, tip));
    REQUIRE(engine_core::begin_drag(frame, view, tip, engine_core::DraggerHandle::X, start));
    offset = engine_core::drag_offset(start, view, {tip.x + 23, tip.y - 23}, 1.0);
    REQUIRE(offset);
    const float along = offset->x * frame.axes[0].x + offset->y * frame.axes[0].y;
    const float across = offset->x * frame.axes[1].x + offset->y * frame.axes[1].y;
    REQUIRE(close(along, std::round(along)));
    REQUIRE(std::abs(along) >= 1.f);
    REQUIRE(close(across, 0));
}

TEST_CASE("DR8 a ray along the axis gives no offset, and nothing is NaN", "[DR8]") {
    const auto view = view_of(200, 200);
    engine_core::DragStart start;
    start.frame = world_frame({0, 0, -10});
    start.handle = engine_core::DraggerHandle::Z;
    REQUIRE_FALSE(engine_core::drag_offset(start, view, {100, 100}, 0).has_value());
    start.handle = engine_core::DraggerHandle::YZ;
    start.hit = {0, 3, -10};
    REQUIRE_FALSE(engine_core::drag_offset(start, view, {100, 50}, 0).has_value());
}

TEST_CASE("DR9 a Dragger is a PVInstance, takes input anywhere under game, and none outside it", "[DR9]") {
    DragRig drag;
    REQUIRE(dynamic_cast<engine_core::PVInstance*>(drag.rig.game.instance(drag.dragger)) != nullptr);
    engine_core::Folder& folder = drag.rig.game.create<engine_core::Folder>();
    drag.rig.game.set_parent(folder.id(), drag.rig.game.scene_service("Storage"));
    drag.rig.game.set_parent(drag.dragger, folder.id());
    drag.press(150, 100);
    REQUIRE(drag.handles().dragging());
    drag.release(150, 100);
    drag.rig.game.set_parent(drag.dragger, engine_core::DataModel::kNoParent);
    drag.press(150, 100);
    REQUIRE_FALSE(drag.handles().dragging());
}

TEST_CASE("DR18 a script makes a Dragger, and Space, Increment, and Dragging refuse what they must", "[DR18]") {
    ScriptRig rig;
    add_script(rig.game, "Probe", R"(
        local dragger = Instance.new("Dragger")
        print("class", dragger.ClassName, dragger:IsA("PVInstance"), dragger.Space == Enum.DraggerSpace.World,
            dragger.Increment, dragger.Dragging)
        dragger.Space = Enum.DraggerSpace.Local
        dragger.Increment = 0.5
        dragger.Transform = Matrix4.new(Vector3.new(1, 2, 3))
        print("set", dragger.Space == Enum.DraggerSpace.Local, dragger.Increment, dragger.Transform.Position.Y)
        print("negative", pcall(function() dragger.Increment = -1 end))
        print("nan", pcall(function() dragger.Increment = 0 / 0 end))
        print("dragging", pcall(function() dragger.Dragging = true end))
        print("handle", Enum.DraggerHandle.XZ.Value)
    )");
    rig.game.start_simulation();
    rig.frames(1);
    const auto output = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output, "class\tDragger\ttrue\ttrue\t0\tfalse\n"));
    REQUIRE(has_line(output, "set\ttrue\t0.5\t2\n"));
    bool negative_refused = false;
    bool nan_refused = false;
    bool dragging_refused = false;
    for (const auto& line : output.lines) {
        negative_refused = negative_refused || line.text.rfind("negative\tfalse", 0) == 0;
        nan_refused = nan_refused || line.text.rfind("nan\tfalse", 0) == 0;
        dragging_refused = dragging_refused || line.text.rfind("dragging\tfalse", 0) == 0;
    }
    REQUIRE(negative_refused);
    REQUIRE(nan_refused);
    REQUIRE(dragging_refused);
    REQUIRE(has_line(output, "handle\t5\n"));
}

TEST_CASE("DR10 a drag fires its events in order, and moves nothing, not even the Dragger", "[DR10]") {
    DragRig drag;
    drag.rig.runtime.run_chunk(R"(
        local dragger = workspace:FindFirstChild("Dragger")
        dragger.DragBegan:Connect(function(handle) print("began", handle.Name, dragger.Dragging) end)
        dragger.Dragged:Connect(function(handle, offset) print("dragged", handle.Name, string.format("%.2f", offset.X), dragger.Dragging) end)
        dragger.DragEnded:Connect(function(handle) print("ended", handle.Name, dragger.Dragging) end)
    )");
    drag.rig.frames(1);
    drag.rig.runtime.drain_output();
    drag.press(150, 100);
    REQUIRE(drag.handles().dragging());
    drag.move(170, 100);
    drag.release(170, 100);
    REQUIRE_FALSE(drag.handles().dragging());
    REQUIRE(close(drag.transform(drag.dragger).m[12], 0));
    REQUIRE(close(drag.transform(drag.part).m[12], 0));
    std::vector<std::string> lines;
    for (const auto& line : drag.rig.runtime.drain_output().lines) {
        lines.push_back(line.text);
    }
    INFO(drag.rig.runtime.last_error());
    REQUIRE(lines == std::vector<std::string>{"began\tX\ttrue\n", "dragged\tX\t2.00\ttrue\n", "ended\tX\tfalse\n"});
}

TEST_CASE("DR11 the records a drag uses are processed, and a press that misses is not", "[DR11]") {
    DragRig drag;
    std::vector<bool> processed;
    drag.rig.game.input().signal(engine_core::UserInputService::Kind::Began)->connect(
        [&](InstanceId, engine_core::Field) {
            const engine_core::EventArgs* args = drag.rig.game.events().current_args();
            if (args != nullptr && args->size() == 2) {
                processed.push_back((*args)[1].flag);
            }
        });
    drag.press(10, 10);
    drag.release(10, 10);
    drag.press(150, 100);
    drag.release(150, 100);
    REQUIRE(processed == std::vector<bool>{false, true});
}

TEST_CASE("DR11b a press a game GUI took does not start a drag", "[DR11b]") {
    DragRig drag;
    drag.rig.game.input().post_mouse_button(0, true, 150, 100, true);
    drag.step();
    REQUIRE_FALSE(drag.handles().dragging());
}

TEST_CASE("DR14 a drag ends when its Dragger leaves game or is destroyed", "[DR14]") {
    DragRig drag;
    drag.rig.runtime.run_chunk(R"(
        workspace:FindFirstChild("Dragger").DragEnded:Connect(function(handle) print("ended", handle.Name) end)
    )");
    drag.rig.frames(1);
    drag.press(150, 100);
    drag.rig.game.set_parent(drag.dragger, engine_core::DataModel::kNoParent);
    drag.move(170, 100);
    REQUIRE_FALSE(drag.handles().dragging());
    REQUIRE(has_line(drag.rig.runtime.drain_output(), "ended\tX\n"));

    drag.rig.game.set_parent(drag.dragger, drag.rig.game.scene_service("Workspace"));
    drag.listen(drag.dragger);
    drag.press(150, 100);
    drag.rig.game.destroy(drag.dragger);
    drag.move(170, 100);
    drag.release(170, 100);
    REQUIRE(drag.offsets.empty());
}

TEST_CASE("DR14b Play or Stop ends a drag", "[DR14b]") {
    DragRig drag;
    drag.press(150, 100);
    REQUIRE(drag.handles().dragging());
    drag.rig.game.capture_place();
    drag.rig.game.start_simulation();
    drag.step();
    REQUIRE_FALSE(drag.handles().dragging());
    drag.press(150, 100);
    REQUIRE(drag.handles().dragging());
    drag.rig.game.stop_simulation();
    drag.step();
    REQUIRE_FALSE(drag.handles().dragging());
}

TEST_CASE("DR15 of two Draggers whose arrows overlap, the nearer gets the drag", "[DR15]") {
    DragRig drag;
    const InstanceId far = drag.add_dragger(drag.rig.game.scene_service("Workspace"), engine_core::matrix4_translation(0, 0, -20));
    drag.listen(drag.dragger);
    std::vector<float> far_offsets;
    drag.rig.game.event_signal(far, "Dragged").connect([&](InstanceId, engine_core::Field) { far_offsets.push_back(1); });
    drag.press(150, 100);
    drag.move(170, 100);
    drag.release(170, 100);
    REQUIRE(drag.offsets.size() == 1);
    REQUIRE(far_offsets.empty());
}

TEST_CASE("DR16 in Local space the drag runs along the Dragger's own turned axes", "[DR16]") {
    DragRig drag;
    REQUIRE_FALSE(drag.handles().set_space(1).has_value());
    // Turned 90 degrees about Y: its X axis points down -Z, away from the camera, hidden;
    // its Z axis points along +X, so the arrow right of the middle is Z.
    REQUIRE_FALSE(drag.handles()
                      .set_transform(engine_core::matrix4_multiply(engine_core::matrix4_translation(0, 0, -10),
                                                                   engine_core::matrix4_axis_angle({0, 1, 0}, 3.14159265 / 2)))
                      .has_value());
    std::vector<engine_core::Vec3> offsets;
    drag.rig.game.event_signal(drag.dragger, "Dragged").connect([&](InstanceId, engine_core::Field) {
        offsets.push_back((*drag.rig.game.events().current_args())[1].vec);
    });
    drag.press(150, 100);
    REQUIRE(drag.handles().active_handle() == engine_core::DraggerHandle::Z);
    drag.move(170, 100);
    drag.release(170, 100);
    REQUIRE(offsets.size() == 1);
    REQUIRE((close(offsets[0].x, 2) && close(offsets[0].y, 0) && close(offsets[0].z, 0)));
}

TEST_CASE("DR17 with no camera, no view size, or a locked pointer, input passes through", "[DR17]") {
    for (int way = 0; way < 3; ++way) {
        DragRig drag;
        if (way == 0) {
            drag.workspace().set_current_camera(0);
        } else if (way == 1) {
            dynamic_cast<engine_core::Camera*>(drag.rig.game.instance(drag.camera))->set_viewport_size({0, 0});
        } else {
            drag.rig.game.input().set_mouse_behavior(engine_core::UserInputService::kLockCurrentPosition);
        }
        drag.listen(drag.dragger);
        drag.press(150, 100);
        drag.move(170, 100);
        drag.release(170, 100);
        INFO(way);
        REQUIRE_FALSE(drag.handles().dragging());
        REQUIRE(drag.offsets.empty());
    }
}

TEST_CASE("DR19 the offset stays total since the drag began when Increment changes mid-drag", "[DR19]") {
    DragRig drag;
    drag.listen(drag.dragger);
    drag.press(150, 100);
    drag.move(174, 100);
    REQUIRE_FALSE(drag.handles().set_increment(1).has_value());
    drag.move(175, 100);
    drag.release(175, 100);
    REQUIRE(drag.offsets.size() == 2);
    REQUIRE(close(drag.offsets[0], 2.4f));
    REQUIRE(close(drag.offsets[1], 3));
}

TEST_CASE("DR22 a listener that moves the Dragger does not change what the drag is measured from", "[DR22]") {
    DragRig drag;
    drag.listen(drag.dragger);
    drag.rig.runtime.run_chunk(R"(
        local dragger = workspace:FindFirstChild("Dragger")
        local start
        dragger.DragBegan:Connect(function() start = dragger.Transform end)
        dragger.Dragged:Connect(function(_, offset) dragger.Transform = Matrix4.new(offset) * start end)
    )");
    drag.rig.frames(1);
    drag.press(150, 100);
    drag.move(160, 100);
    drag.move(170, 100);
    drag.release(170, 100);
    INFO(drag.rig.runtime.last_error());
    REQUIRE(drag.offsets.size() == 2);
    REQUIRE(close(drag.offsets[0], 1));
    REQUIRE(close(drag.offsets[1], 2));
    REQUIRE(close(drag.transform(drag.dragger).m[12], 2));
}

TEST_CASE("DR12 in edit mode a drag is one undo step for what its listeners move", "[DR12]") {
    DragRig drag;
    drag.press(150, 100);
    drag.release(150, 100);
    drag.rig.frames(1);
    REQUIRE_FALSE(drag.rig.game.history().can_undo().first);
    REQUIRE_FALSE(drag.rig.game.history().is_recording_in_progress());

    drag.rig.runtime.run_chunk(R"(
        workspace:FindFirstChild("Dragger").Dragged:Connect(function(_, offset)
            workspace.Part.Transform = Matrix4.new(Vector3.new(offset.X, 5, -10))
        end)
    )");
    drag.rig.frames(1);
    drag.press(150, 100);
    drag.move(160, 100);
    drag.move(170, 100);
    drag.release(170, 100);
    drag.rig.frames(1);
    INFO(drag.rig.runtime.last_error());
    REQUIRE(close(drag.transform(drag.part).m[12], 2));
    REQUIRE_FALSE(drag.rig.game.history().is_recording_in_progress());
    REQUIRE(drag.rig.game.history().can_undo().second == "Move");
    drag.rig.game.history().undo();
    REQUIRE(close(drag.transform(drag.part).m[12], 0));
    REQUIRE_FALSE(drag.rig.game.history().can_undo().first);
}

TEST_CASE("DR12b a click with no motion keeps what its handlers changed", "[DR12b]") {
    DragRig drag;
    drag.rig.runtime.run_chunk(R"(
        workspace:FindFirstChild("Dragger").DragBegan:Connect(function() workspace.Part.Name = "Clicked" end)
    )");
    drag.rig.frames(1);
    drag.press(150, 100);
    drag.release(150, 100);
    drag.rig.frames(1);
    REQUIRE(drag.rig.game.name(drag.part) == "Clicked");
}

TEST_CASE("DR13 in play a drag records no history", "[DR13]") {
    DragRig drag;
    drag.rig.runtime.run_chunk(R"(
        workspace:FindFirstChild("Dragger").Dragged:Connect(function(_, offset)
            workspace.Part.Transform = Matrix4.new(Vector3.new(offset.X, 5, -10))
        end)
    )");
    drag.rig.frames(1);
    drag.rig.game.capture_place();
    drag.rig.game.start_simulation();
    drag.press(150, 100);
    drag.move(170, 100);
    drag.release(170, 100);
    drag.rig.frames(1);
    REQUIRE(close(drag.transform(drag.part).m[12], 2));
    REQUIRE_FALSE(drag.rig.game.history().can_undo().first);
    drag.rig.game.stop_simulation();
    REQUIRE_FALSE(drag.rig.game.history().can_undo().first);
}

TEST_CASE("RD1 a Dragger under game has a snapshot row at its own Transform, one outside game none", "[RD1]") {
    DragRig drag;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&]() -> const engine_core::VisualSnapshot& {
        pump.prepare_copy(drag.rig.game);
        pump.publish();
        return pump.front();
    };
    drag.move(150, 100);
    {
        const engine_core::VisualSnapshot& shot = frame();
        REQUIRE(shot.draggers.size() == 1);
        REQUIRE(close(shot.draggers[0].frame.origin.z, -10));
        REQUIRE(shot.draggers[0].hovered == engine_core::DraggerHandle::X);
        REQUIRE(shot.draggers[0].active == engine_core::DraggerHandle::None);
    }
    drag.press(150, 100);
    REQUIRE(frame().draggers[0].active == engine_core::DraggerHandle::X);
    drag.release(150, 100);
    drag.rig.game.set_parent(drag.dragger, engine_core::DataModel::kNoParent);
    REQUIRE(frame().draggers.empty());
}

namespace {

// How many vertices of mesh are close to color (r, g, b), alpha ignored.
int count_color(const std::vector<engine_core::HandleVertex>& mesh, float r, float g, float b) {
    int count = 0;
    for (const engine_core::HandleVertex& vertex : mesh) {
        if (close(vertex.color[0], r, 0.01f) && close(vertex.color[1], g, 0.01f) && close(vertex.color[2], b, 0.01f)) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("RD3 handle_mesh draws what can be grabbed, colored by axis and state", "[RD3]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    std::vector<engine_core::HandleVertex> mesh;
    engine_core::handle_mesh(frame, view, engine_core::DraggerHandle::None, engine_core::DraggerHandle::None, mesh);
    REQUIRE(!mesh.empty());
    REQUIRE(mesh.size() % 3 == 0);
    REQUIRE(count_color(mesh, 0.90f, 0.20f, 0.20f) > 0);
    REQUIRE(count_color(mesh, 0.30f, 0.85f, 0.30f) > 0);
    // Z points at the camera: no arrow of its own; blue only on the XY square, which is see-through.
    for (const engine_core::HandleVertex& vertex : mesh) {
        if (close(vertex.color[0], 0.25f, 0.01f) && close(vertex.color[2], 0.95f, 0.01f)) {
            REQUIRE(vertex.color[3] < 0.5f);
        }
    }
    for (const engine_core::HandleVertex& vertex : mesh) {
        REQUIRE(std::isfinite(vertex.position[0]));
    }
    // Dragging X: X is yellow, Y fades.
    engine_core::handle_mesh(frame, view, engine_core::DraggerHandle::None, engine_core::DraggerHandle::X, mesh);
    REQUIRE(count_color(mesh, 1.0f, 0.85f, 0.2f) > 0);
    REQUIRE(count_color(mesh, 0.90f, 0.20f, 0.20f) == 0);
    for (const engine_core::HandleVertex& vertex : mesh) {
        if (close(vertex.color[1], 0.85f, 0.01f) && close(vertex.color[0], 0.30f, 0.01f)) {
            REQUIRE(vertex.color[3] < 0.5f);
        }
    }
    // Behind the camera: nothing.
    engine_core::handle_mesh(world_frame({0, 0, 10}), view, engine_core::DraggerHandle::None,
                             engine_core::DraggerHandle::None, mesh);
    REQUIRE(mesh.empty());
}

TEST_CASE("DR23 a drag after an edit that left its undo step open gets a step of its own", "[DR23]") {
    DragRig drag;
    drag.rig.runtime.run_chunk(R"(
        local part = workspace.Part
        workspace:FindFirstChild("Dragger").Dragged:Connect(function(_, offset)
            part.Transform = Matrix4.new(Vector3.new(offset.X, 5, -10))
        end)
    )");
    drag.rig.frames(1);
    // As a command line edit does: the place changes, and nothing closes the step.
    drag.rig.game.set_name(drag.part, "Edited");
    REQUIRE(drag.rig.game.history().is_recording_in_progress());
    drag.press(150, 100);
    drag.move(170, 100);
    drag.release(170, 100);
    drag.rig.frames(1);
    INFO(drag.rig.runtime.last_error());
    REQUIRE_FALSE(drag.rig.game.history().is_recording_in_progress());
    REQUIRE(drag.rig.game.history().can_undo().second == "Move");
    drag.rig.game.history().undo();
    REQUIRE(close(drag.transform(drag.part).m[12], 0));
    REQUIRE(drag.rig.game.name(drag.part) == "Edited");
    drag.rig.game.history().undo();
    REQUIRE(drag.rig.game.name(drag.part) == "Part");
}
