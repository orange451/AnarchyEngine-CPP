// ReflectionMath: how much each traced reflection counts, and which level of
// the lit image it reads, with no GL context. ssr.glsl copies these.

#include "runner/ReflectionMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>

using Catch::Approx;
using namespace runner;

namespace {

// f rises (or falls, with falling) monotonically from a to b in 200 steps, with no jump over 0.05.
void RequireSmooth(const std::function<float(float)>& f, float a, float b, bool falling) {
    float previous = f(a);
    for (int i = 1; i <= 200; ++i) {
        const float x = a + (b - a) * static_cast<float>(i) / 200.f;
        const float y = f(x);
        INFO(x);
        REQUIRE((falling ? y <= previous + 1e-6f : y >= previous - 1e-6f));
        REQUIRE(std::abs(y - previous) <= 0.05f);
        previous = y;
    }
}

}  // namespace

TEST_CASE("RM1 the edge fade is 1 in the middle 80% and 0 at the edges", "[reflections]") {
    REQUIRE(EdgeFade(0.5f, 0.5f) == 1.f);
    REQUIRE(EdgeFade(0.1f, 0.9f) == Approx(1.f));
    REQUIRE(EdgeFade(0.f, 0.5f) == 0.f);
    REQUIRE(EdgeFade(0.5f, 1.f) == 0.f);
    REQUIRE(EdgeFade(-0.2f, 0.5f) == 0.f);
    RequireSmooth([](float u) { return EdgeFade(u, 0.5f); }, 0.f, 0.1f, false);
}

TEST_CASE("RM2 the distance fade ends at MaxDistance, from three quarters of it", "[reflections]") {
    REQUIRE(DistanceFade(10.f, 50.f) == 1.f);
    REQUIRE(DistanceFade(37.5f, 50.f) == Approx(1.f));
    REQUIRE(DistanceFade(50.f, 50.f) == 0.f);
    REQUIRE(DistanceFade(80.f, 50.f) == 0.f);
    REQUIRE(DistanceFade(1.f, 0.f) == 0.f);
    RequireSmooth([](float d) { return DistanceFade(d, 50.f); }, 37.5f, 50.f, true);
}

TEST_CASE("RM3 the roughness fade ends at MaxRoughness, from four fifths of it", "[reflections]") {
    REQUIRE(RoughnessFade(0.f, 0.5f) == 1.f);
    REQUIRE(RoughnessFade(0.4f, 0.5f) == Approx(1.f));
    REQUIRE(RoughnessFade(0.5f, 0.5f) == 0.f);
    REQUIRE(RoughnessFade(0.9f, 0.5f) == 0.f);
    REQUIRE(RoughnessFade(0.f, 0.f) == 0.f);
    RequireSmooth([](float r) { return RoughnessFade(r, 0.5f); }, 0.4f, 0.5f, true);
}

TEST_CASE("RM4 rays turning back toward the camera fade out", "[reflections]") {
    // View space looks down -Z: a ray leaving the camera has a negative z.
    REQUIRE(FacingFade(-1.f) == 1.f);
    REQUIRE(FacingFade(0.f) == 1.f);
    REQUIRE(FacingFade(0.5f) == 0.f);
    REQUIRE(FacingFade(1.f) == 0.f);
    RequireSmooth(FacingFade, 0.f, 0.5f, true);
}

TEST_CASE("RM5 the cone level grows with roughness and distance, from 0 for a mirror", "[reflections]") {
    REQUIRE(ConeLevel(0.f, 10.f, 50.f, 6) == 0.f);
    REQUIRE(ConeLevel(0.3f, 1.f, 50.f, 6) < ConeLevel(0.3f, 4.f, 50.f, 6));
    REQUIRE(ConeLevel(0.1f, 4.f, 50.f, 6) < ConeLevel(0.4f, 4.f, 50.f, 6));
    // A cone under a pixel reads level 0.
    REQUIRE(ConeLevel(0.01f, 0.5f, 50.f, 6) == 0.f);
    // Never past the last level.
    REQUIRE(ConeLevel(1.f, 1000.f, 500.f, 6) == 5.f);
}
