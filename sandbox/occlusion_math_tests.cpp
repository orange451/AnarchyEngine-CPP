// OcclusionMath: ambient occlusion's settings and formulas, with no GL.
// gtao.frag, ao_blur.frag, occlusion.glsl, and image_lighting.glsl copy them.

#include "runner/OcclusionMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using namespace runner;

namespace {
constexpr float kHalfPi = 1.57079632679f;
}

TEST_CASE("OM1 each Quality's resolution, slices, and blur", "[occlusion]") {
    const OcclusionQuality low = QualitySettings(0);
    const OcclusionQuality medium = QualitySettings(1);
    const OcclusionQuality high = QualitySettings(2);
    REQUIRE((low.scale == 2 && low.slices == 2 && low.blurRadius == 6));
    REQUIRE((medium.scale == 2 && medium.slices == 3 && medium.blurRadius == 4));
    REQUIRE((high.scale == 1 && high.slices == 3 && high.blurRadius == 4));
    // Anything else is Medium.
    REQUIRE(QualitySettings(9).slices == 3);
}

TEST_CASE("OM2 the pixel radius shrinks with depth and stops at a quarter of the buffer", "[occlusion]") {
    REQUIRE(PixelRadius(1.f, 10.f, 500.f, 1000.f) == Approx(50.f));
    REQUIRE(PixelRadius(1.f, 20.f, 500.f, 1000.f) == Approx(25.f));
    REQUIRE(PixelRadius(1.f, 0.01f, 500.f, 1000.f) == Approx(250.f));
}

TEST_CASE("OM3 the falloff is 1 within 40% of Radius, 0 at Radius, and smooth between", "[occlusion]") {
    REQUIRE(Falloff(0.f, 1.f) == 1.f);
    REQUIRE(Falloff(0.4f, 1.f) == Approx(1.f));
    REQUIRE(Falloff(1.f, 1.f) == Approx(0.f).margin(1e-6));
    REQUIRE(Falloff(2.f, 1.f) == 0.f);
    float previous = 1.f;
    for (float d = 0.4f; d <= 1.f; d += 0.01f) {
        const float f = Falloff(d, 1.f);
        REQUIRE(f <= previous + 1e-6f);
        previous = f;
    }
}

TEST_CASE("OM4 the arc integral: open is 1, a wall at the view direction half", "[occlusion]") {
    // A surface facing the camera, nothing above its tangent plane on either side.
    REQUIRE(ArcVisibility(0.f, -kHalfPi, kHalfPi, 1.f) == Approx(1.f));
    // One side closed straight up the view direction.
    REQUIRE(ArcVisibility(0.f, -kHalfPi, 0.f, 1.f) == Approx(0.5f));
    REQUIRE(ArcVisibility(0.f, 0.f, 0.f, 1.f) == Approx(0.f).margin(1e-6));
}

TEST_CASE("OM5 multi-bounce never darkens below the visibility, and white loses least", "[occlusion]") {
    REQUIRE(MultiBounce(1.f, 0.f) == 1.f);
    REQUIRE(MultiBounce(1.f, 1.f) == 1.f);
    for (float v = 0.f; v <= 1.f; v += 0.1f) {
        INFO(v);
        REQUIRE(MultiBounce(v, 0.f) >= v - 1e-6f);
        REQUIRE(MultiBounce(v, 1.f) >= MultiBounce(v, 0.f) - 1e-6f);
    }
}

TEST_CASE("OM6 specular occlusion is 1 when open, and falls faster for smooth surfaces", "[occlusion]") {
    REQUIRE(SpecularOcclusion(1.f, 0.5f, 0.2f) == 1.f);
    // NdotV + visibility must not be 1, where any exponent gives 1.
    REQUIRE(SpecularOcclusion(0.5f, 0.3f, 0.05f) < SpecularOcclusion(0.5f, 0.3f, 0.9f));
    REQUIRE(SpecularOcclusion(0.f, 0.f, 0.5f) == 0.f);
}
