// RenderMath: the Scene View's and the shadow maps' projections and views,
// which need no GL context.

#include "runner/RenderMath.hpp"

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
