// The Dragger: translate handles that move a PVInstance, and the math under them.

#include "support.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "DraggerMath.hpp"
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
