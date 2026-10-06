// SkyMath: the DynamicSky's sun, moon, stars, light, and lighting refreshes,
// with no GL. procedural_sky.glsl copies the atmosphere's numbers.

#include "runner/SkyMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using namespace runner;

namespace {

SkyVector Apply(const float m[9], SkyVector v) {
    return {m[0] * v.x + m[3] * v.y + m[6] * v.z, m[1] * v.x + m[4] * v.y + m[7] * v.z,
            m[2] * v.x + m[5] * v.y + m[8] * v.z};
}

}  // namespace

TEST_CASE("SM1 at the equator the sun is overhead at noon and on the horizon at 6 and 18", "[sky_math]") {
    const SkyVector noon = SunDirection(12.0, 0.0);
    REQUIRE(noon.x == Approx(0.f).margin(1e-6));
    REQUIRE(noon.y == Approx(1.f));
    REQUIRE(noon.z == Approx(0.f).margin(1e-6));
    // It rises in the east (+X) and sets in the west (-X).
    const SkyVector rise = SunDirection(6.0, 0.0);
    REQUIRE(rise.x == Approx(1.f));
    REQUIRE(rise.y == Approx(0.f).margin(1e-6));
    const SkyVector set = SunDirection(18.0, 0.0);
    REQUIRE(set.x == Approx(-1.f));
    REQUIRE(set.y == Approx(0.f).margin(1e-6));
    // Midnight is straight down.
    REQUIRE(SunDirection(0.0, 0.0).y == Approx(-1.f));
}

TEST_CASE("SM2 Latitude sets the noon sun's height, south in the north and north in the south", "[sky_math]") {
    for (const double latitude : {-60.0, -35.0, 10.0, 35.0, 80.0}) {
        INFO(latitude);
        const SkyVector noon = SunDirection(12.0, latitude);
        REQUIRE(ElevationDegrees(noon) == Approx(90.0 - std::abs(latitude)).margin(1e-3));
        // South is +Z.
        REQUIRE((latitude > 0 ? noon.z > 0.f : noon.z < 0.f));
        // Sunrise is still due east at the equinox.
        const SkyVector rise = SunDirection(6.0, latitude);
        REQUIRE(rise.x == Approx(1.f));
        REQUIRE(ElevationDegrees(rise) == Approx(0.f).margin(1e-3));
    }
}

TEST_CASE("SM3 the stars' frame keeps the sun straight up and the pole at -Z", "[sky_math]") {
    float frame[9];
    for (const double latitude : {-45.0, 0.0, 35.0, 90.0}) {
        for (const double hours : {0.0, 3.5, 12.0, 20.25}) {
            INFO(latitude << " " << hours);
            StarFrame(hours, latitude, frame);
            const SkyVector sun = Apply(frame, SunDirection(hours, latitude));
            REQUIRE(sun.x == Approx(0.f).margin(1e-5));
            REQUIRE(sun.y == Approx(1.f).margin(1e-5));
            REQUIRE(sun.z == Approx(0.f).margin(1e-5));
            const float phi = static_cast<float>(latitude * 3.14159265358979 / 180.0);
            const SkyVector pole = Apply(frame, SkyVector{0.f, std::sin(phi), -std::cos(phi)});
            REQUIRE(pole.z == Approx(-1.f).margin(1e-5));
        }
    }
}

TEST_CASE("SM4 transmittance is white overhead and redder toward the horizon", "[sky_math]") {
    float overhead[3];
    float high[3];
    float low[3];
    Transmittance(90.f, overhead);
    Transmittance(60.f, high);
    Transmittance(5.f, low);
    for (float channel : overhead) {
        REQUIRE(channel == Approx(1.f).margin(1e-3));
    }
    REQUIRE(low[0] / low[2] > high[0] / high[2]);
    REQUIRE(low[0] < high[0]);
    // Below the horizon is the horizon.
    float below[3];
    float horizon[3];
    Transmittance(-10.f, below);
    Transmittance(0.f, horizon);
    REQUIRE(below[2] == horizon[2]);
}

TEST_CASE("SM5 fades, star visibility, and cloud dimming at their ends", "[sky_math]") {
    REQUIRE(BodyFade(-5.f) == 0.f);
    REQUIRE(BodyFade(0.f) == 0.f);
    REQUIRE(BodyFade(6.f) == 1.f);
    REQUIRE(BodyFade(3.f) == Approx(0.5f));
    REQUIRE(StarVisibility(10.f) == 0.f);
    REQUIRE(StarVisibility(-2.f) == 0.f);
    REQUIRE(StarVisibility(-12.f) == 1.f);
    REQUIRE(CloudDimming(0.f, 1.f) == 1.f);
    REQUIRE(CloudDimming(1.f, 1.f) == Approx(0.3f));
    REQUIRE(CloudDimming(0.5f, 0.5f) == Approx(1.f - 0.7f * 0.25f));
}

TEST_CASE("SM6 the light comes from the sun by day, the moon by night, and never jumps", "[sky_math]") {
    const SkyState noon = ComputeSky(12.0, 35.0, 3.0, 0.0, 0.0);
    REQUIRE(noon.light.fromSun);
    REQUIRE(noon.light.intensity == Approx(3.f));
    REQUIRE(noon.light.toward.y > 0.5f);
    const SkyState midnight = ComputeSky(0.0, 35.0, 3.0, 0.0, 0.0);
    REQUIRE_FALSE(midnight.light.fromSun);
    REQUIRE(midnight.light.intensity == Approx(3.f * kMoonBrightnessScale));
    REQUIRE(midnight.light.toward.y > 0.5f);
    REQUIRE(midnight.moon.y == Approx(-midnight.sun.y));
    REQUIRE(midnight.starVisibility == 1.f);
    REQUIRE(noon.starVisibility == 0.f);
    // A minute at a time through sunrise and sunset: small steps, and near
    // nothing at the crossing.
    for (const double around : {6.0, 18.0}) {
        float previous = ComputeSky(around - 1.0, 35.0, 3.0, 0.0, 0.0).light.intensity;
        for (double hours = around - 1.0; hours <= around + 1.0; hours += 1.0 / 60.0) {
            const float now = ComputeSky(hours, 35.0, 3.0, 0.0, 0.0).light.intensity;
            INFO(hours);
            REQUIRE(std::abs(now - previous) < 0.25f);
            previous = now;
        }
        REQUIRE(ComputeSky(around, 35.0, 3.0, 0.0, 0.0).light.intensity < 0.01f);
    }
    // Clouds dim it.
    REQUIRE(ComputeSky(12.0, 35.0, 3.0, 1.0, 1.0).light.intensity == Approx(3.f * 0.3f));
    // At a pole the sun stays on the horizon: no light, and nothing that is not a number.
    const SkyState pole = ComputeSky(12.0, 90.0, 3.0, 0.5, 0.5);
    REQUIRE(pole.light.intensity == Approx(0.f).margin(1e-4));
    REQUIRE(std::isfinite(pole.light.color[0]));
    REQUIRE(std::isfinite(pole.starFrame[0]));
}

TEST_CASE("SM7 the light reddens at a low sun, and the discs hide below the horizon", "[sky_math]") {
    const SkyState high = ComputeSky(12.0, 0.0, 3.0, 0.0, 0.0);
    // 7:00 at the equator: 15 degrees up.
    const SkyState low = ComputeSky(7.0, 0.0, 3.0, 0.0, 0.0);
    REQUIRE(low.light.color[0] / low.light.color[2] > high.light.color[0] / high.light.color[2]);
    REQUIRE(high.sunColor[0] == Approx(kSunDiscRadiance).margin(0.1f));
    const SkyState night = ComputeSky(0.0, 0.0, 3.0, 0.0, 0.0);
    REQUIRE(night.sunColor[0] == 0.f);
    REQUIRE(night.moonColor[2] > 0.f);
    REQUIRE(high.moonColor[2] == 0.f);
}

TEST_CASE("SM8 the lighting cube's sizes and when it is drawn again", "[sky_math]") {
    REQUIRE(EnvironmentSizesFor(0).environment == 128);
    REQUIRE(EnvironmentSizesFor(0).prefiltered == 128);
    REQUIRE(EnvironmentSizesFor(1).environment == 256);
    REQUIRE(EnvironmentSizesFor(2).environment == 512);
    REQUIRE(EnvironmentSizesFor(2).prefiltered == 512);
    REQUIRE(EnvironmentSizesFor(7).environment == 256);

    const SkyLightingKey key{14.f, 35.f, 0.5f, 0.5f, 1};
    // Never made: now.
    REQUIRE(LightingDue(key, key, 0.0, 0.0, false, false));
    // Unchanged and still: never.
    REQUIRE_FALSE(LightingDue(key, key, 1.0, 100.0, false, true));
    // Unchanged and windy: every quarter second.
    REQUIRE_FALSE(LightingDue(key, key, 1.0, 1.2, true, true));
    REQUIRE(LightingDue(key, key, 1.0, 1.25, true, true));
    // Changed: no sooner than 0.05 s after the last.
    SkyLightingKey later = key;
    later.timeOfDay = 14.01f;
    REQUIRE_FALSE(LightingDue(key, later, 1.0, 1.02, false, true));
    REQUIRE(LightingDue(key, later, 1.0, 1.05, false, true));
    // A new ReflectionQuality: at once.
    SkyLightingKey sharper = key;
    sharper.quality = 2;
    REQUIRE(LightingDue(key, sharper, 1.0, 1.0, false, true));
    // A clock that went back, as in a new view: now.
    REQUIRE(LightingDue(key, key, 5.0, 1.0, true, true));
}

TEST_CASE("SM9 a lighting cube redraw's slices draw each face and mip once, after what each reads", "[sky_math]") {
    int faces[6] = {};
    int levels[6] = {};
    int facesDrawn = 0;
    int diffuse = 0;
    for (int slice = 0; slice < kLightingSlices; ++slice) {
        const LightingSlice s = LightingSliceAt(slice);
        REQUIRE(0 <= s.faceBegin);
        REQUIRE(s.faceBegin <= s.faceEnd);
        REQUIRE(s.faceEnd <= 6);
        for (int face = s.faceBegin; face < s.faceEnd; ++face) {
            ++faces[face];
            ++facesDrawn;
        }
        // The mipmap and irradiance read every face; the mips read the mipmapped cube.
        if (s.diffuse) {
            REQUIRE(facesDrawn == 6);
            ++diffuse;
        }
        REQUIRE((s.levels >> 6) == 0u);
        for (int level = 0; level < 6; ++level) {
            if ((s.levels >> level) & 1u) {
                REQUIRE(diffuse == 1);
                ++levels[level];
            }
        }
    }
    REQUIRE(diffuse == 1);
    for (int i = 0; i < 6; ++i) {
        REQUIRE(faces[i] == 1);
        REQUIRE(levels[i] == 1);
    }
    // Past the last, nothing.
    const LightingSlice past = LightingSliceAt(kLightingSlices);
    REQUIRE(past.faceBegin == past.faceEnd);
    REQUIRE_FALSE(past.diffuse);
    REQUIRE(past.levels == 0u);
}
