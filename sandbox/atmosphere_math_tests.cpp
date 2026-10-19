// AtmosphereMath: how much air lies along a ray, its sun glow, and its sky
// mip, with no GL context. atmosphere.glsl is AtmosphereMath, line for line.

#include "runner/AtmosphereMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using namespace runner;

namespace {

// The same transmittance by brute force: the density summed along the ray in
// small steps, in double.
double Integrated(double sigma0, double falloff, double height, double dy, double distance, int steps = 1000) {
    double depth = 0.0;
    for (int i = 0; i < steps; ++i) {
        const double t = (i + 0.5) / steps * distance;
        depth += sigma0 * std::exp(-falloff * (height + dy * t)) * distance / steps;
    }
    return std::exp(-depth);
}

}  // namespace

TEST_CASE("ATMM1 air of one density fades as exp(-sigma d)", "[atmosphere-math]") {
    for (float dy : {-1.f, 0.f, 1.f}) {
        INFO(dy);
        REQUIRE(AtmosphereTransmittance(0.01f, 0.f, 25.f, dy, 500.f) == Approx(std::exp(-0.01f * 500.f)).margin(1e-5));
    }
}

TEST_CASE("ATMM2 near the horizon the series matches the closed form", "[atmosphere-math]") {
    for (float dy : {1e-7f, 2e-6f, 1e-3f, 0.5f, -0.2f}) {
        INFO(dy);
        const double expected = Integrated(0.01, 0.05, 10.0, dy, 1000.0);
        REQUIRE(AtmosphereTransmittance(0.01f, 0.05f, 10.f, dy, 1000.f) == Approx(expected).margin(1e-4));
    }
    // Either side of the switch to the series (|f dy d| = 1e-4) agree.
    const float below = AtmosphereTransmittance(0.01f, 0.05f, 10.f, 0.99e-4f / 50.f, 1000.f);
    const float above = AtmosphereTransmittance(0.01f, 0.05f, 10.f, 1.01e-4f / 50.f, 1000.f);
    REQUIRE(std::fabs(below - above) < 1e-5f);
}

TEST_CASE("ATMM3 a camera far below the base height stays finite", "[atmosphere-math]") {
    const float deep = AtmosphereTransmittance(0.01f, 0.2f, -1e5f, 0.f, 100.f);
    REQUIRE(std::isfinite(deep));
    REQUIRE(deep >= 0.f);
    REQUIRE(deep <= 1.f);
    const float far = AtmosphereTransmittance(0.01f, 0.2f, -1e5f, 0.f, 1e9f);
    REQUIRE(std::isfinite(far));
    REQUIRE(far >= 0.f);
    REQUIRE(far <= 1.f);
    // Looking down from far below, the air ahead only thickens.
    const float down = AtmosphereTransmittance(0.01f, 0.2f, -1e5f, -1.f, 1e9f);
    REQUIRE(std::isfinite(down));
    REQUIRE(down >= 0.f);
    REQUIRE(AtmosphereTransmittance(0.01f, 0.05f, 10.f, 0.3f, 0.f) == 1.f);
}

TEST_CASE("ATMM6 a camera far above the base height still sees the air below", "[atmosphere-math]") {
    // Straight down from 9000 units through air that thins fast: the whole column is sigma0 / falloff deep.
    for (float falloff : {0.01f, 0.2f}) {
        INFO(falloff);
        const double expected = Integrated(0.004, falloff, 9000.0, -1.0, 9000.0, 1000000);
        REQUIRE(expected < 0.99);
        REQUIRE(AtmosphereTransmittance(0.004f, falloff, 9000.f, -1.f, 9000.f) == Approx(expected).margin(1e-3));
    }
    // Down at a slant from high up, as a camera over a misty valley.
    const double slant = Integrated(0.01, 0.2, 500.0, -0.5, 1000.0, 1000000);
    REQUIRE(AtmosphereTransmittance(0.01f, 0.2f, 500.f, -0.5f, 1000.f) == Approx(slant).margin(1e-3));
}

TEST_CASE("ATMM4 air thins with height", "[atmosphere-math]") {
    const float low = AtmosphereTransmittance(0.01f, 0.01f, 0.f, 0.f, 300.f);
    const float high = AtmosphereTransmittance(0.01f, 0.01f, 200.f, 0.f, 300.f);
    REQUIRE(high > low);
    const float up = AtmosphereTransmittance(0.01f, 0.01f, 0.f, 1.f, 300.f);
    REQUIRE(up > low);
}

TEST_CASE("ATMM5 the sun glow and the sky mip", "[atmosphere-math]") {
    REQUIRE(AtmosphereSunLobe(1.f) == 1.f);
    REQUIRE(AtmosphereSunLobe(0.f) == 0.f);
    REQUIRE(AtmosphereSunLobe(-0.5f) == 0.f);
    REQUIRE(AtmosphereSunLobe(0.9f) == Approx(std::pow(0.9f, 8.f)));
    REQUIRE(AtmosphereSkyLod(1.f, 5.f) == 5.f);
    REQUIRE(AtmosphereSkyLod(0.f, 5.f) == 0.f);
    REQUIRE(AtmosphereSkyLod(0.5f, 6.f) == 3.f);
}
