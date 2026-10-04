// RenderMath: the Scene View's and the shadow maps' projections and views,
// which need no GL context.

#include "runner/RenderMath.hpp"

#include "runner/BillboardMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using engine_core::Vec3;
using engine_core::matrix4_point;

TEST_CASE("RM1 Perspective puts the near plane at depth -1 and the far plane at 1", "[RM][shadow]") {
    const engine_core::Matrix4 p = runner::Perspective(60.f, 1.5f, 0.1f, 1000.f);
    REQUIRE(matrix4_point(p, {0.f, 0.f, -0.1f}).z == Approx(-1.f));
    REQUIRE(matrix4_point(p, {0.f, 0.f, -1000.f}).z == Approx(1.f));
    // 30 degrees up, at any depth, is the top edge.
    REQUIRE(matrix4_point(p, {0.f, std::tan(30.f * runner::kDegree) * 4.f, -4.f}).y == Approx(1.f));
}

TEST_CASE("RM2 Orthographic maps its box onto the clip cube", "[RM][shadow]") {
    const engine_core::Matrix4 o = runner::Orthographic(-2.f, 4.f, -1.f, 3.f, 0.5f, 10.f);
    const Vec3 low = matrix4_point(o, {-2.f, -1.f, -0.5f});
    const Vec3 high = matrix4_point(o, {4.f, 3.f, -10.f});
    REQUIRE(low.x == Approx(-1.f));
    REQUIRE(low.y == Approx(-1.f));
    REQUIRE(low.z == Approx(-1.f));
    REQUIRE(high.x == Approx(1.f));
    REQUIRE(high.y == Approx(1.f));
    REQUIRE(high.z == Approx(1.f));
}

TEST_CASE("RM3 LookAtView puts the eye at the origin and the target down -Z, even looking straight down",
          "[RM][shadow]") {
    const engine_core::Matrix4 v = runner::LookAtView({1.f, 2.f, 3.f}, {1.f, 2.f, -2.f}, {0.f, 1.f, 0.f});
    const Vec3 eye = matrix4_point(v, {1.f, 2.f, 3.f});
    const Vec3 target = matrix4_point(v, {1.f, 2.f, -2.f});
    REQUIRE(eye.x == Approx(0.f).margin(1e-5));
    REQUIRE(eye.y == Approx(0.f).margin(1e-5));
    REQUIRE(eye.z == Approx(0.f).margin(1e-5));
    REQUIRE(target.x == Approx(0.f).margin(1e-5));
    REQUIRE(target.z == Approx(-5.f));
    // Up along the look: another axis is used, and nothing is NaN.
    const engine_core::Matrix4 down = runner::LookAtView({0.f, 5.f, 0.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    for (const float value : down.m) {
        REQUIRE(std::isfinite(value));
    }
    REQUIRE(matrix4_point(down, {0.f, 0.f, 0.f}).z == Approx(-5.f));
}

TEST_CASE("RM10 a billboard ahead of the camera is centred, sized by distance, at the renderer's depth", "[RM][billboard]") {
    // The default camera: at the origin, looking down -Z.
    const engine_core::Matrix4 view = engine_core::matrix4_identity();
    const runner::BillboardPlacement at10 = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, -10.f});
    REQUIRE(at10.visible);
    REQUIRE(at10.x == Approx(400.f));
    REQUIRE(at10.y == Approx(300.f));
    // tan(45) is 1: one unit at ten units away is 600 / 20 points.
    REQUIRE(at10.pixelsPerUnit == Approx(30.f));
    REQUIRE(at10.distance == Approx(10.f));
    const engine_core::Matrix4 projection =
        runner::Perspective(90.f, 800.f / 600.f, runner::kSceneNear, runner::kSceneFar);
    const float ndc = matrix4_point(projection, {0.f, 0.f, -10.f}).z;
    REQUIRE(at10.depth == Approx(ndc * 0.5f + 0.5f));
    const runner::BillboardPlacement at20 = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, -20.f});
    REQUIRE(at20.pixelsPerUnit == Approx(15.f));
    REQUIRE(at20.depth > at10.depth);
    // Up and to the right on screen is +X and +Y in view space.
    const runner::BillboardPlacement offset = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {5.f, 5.f, -10.f});
    REQUIRE(offset.x == Approx(400.f + 5.f * 30.f));
    REQUIRE(offset.y == Approx(300.f - 5.f * 30.f));
}

TEST_CASE("RM11 a billboard at or behind the near plane is hidden", "[RM][billboard]") {
    const engine_core::Matrix4 view = engine_core::matrix4_identity();
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, 10.f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, 0.f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, -0.05f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 180.f, 800.f, 600.f, {0.f, 0.f, -10.f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 0.f, 600.f, {0.f, 0.f, -10.f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {std::nanf(""), 0.f, -10.f}).visible);
}

TEST_CASE("RM12 a far billboard stays finite and visible", "[RM][billboard]") {
    const engine_core::Matrix4 view = engine_core::matrix4_identity();
    const runner::BillboardPlacement far = runner::PlaceBillboard(view, 60.f, 800.f, 600.f, {0.f, 0.f, -5000.f});
    REQUIRE(far.visible);
    REQUIRE(std::isfinite(far.pixelsPerUnit));
    REQUIRE(far.pixelsPerUnit > 0.f);
    REQUIRE(far.pixelsPerUnit < 1.f);
    // Past the far plane nothing is drawn to hide it; its depth stays at most 1.
    REQUIRE(far.depth <= 1.f);
}

TEST_CASE("RM13 the camera's own turn moves the billboard on screen", "[RM][billboard]") {
    // Turned 90 degrees left about Y, the camera looks down -X.
    const engine_core::Matrix4 world = engine_core::matrix4_axis_angle({0.f, 1.f, 0.f}, 3.14159265 / 2.0);
    const engine_core::Matrix4 view = engine_core::matrix4_inverse(world);
    const runner::BillboardPlacement ahead = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {-10.f, 0.f, 0.f});
    REQUIRE(ahead.visible);
    REQUIRE(ahead.x == Approx(400.f).margin(0.01));
    // What was straight ahead of the unturned camera is now off to the side, or hidden.
    const runner::BillboardPlacement old = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, -10.f});
    REQUIRE((!old.visible || std::abs(old.x - 400.f) > 100.f));
}
