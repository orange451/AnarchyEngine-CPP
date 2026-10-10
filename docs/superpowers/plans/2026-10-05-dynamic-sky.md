# Dynamic Sky Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `DynamicSky` under Lighting draws a procedural day–night sky (atmosphere, clouds, stars, sun, moon) and drives a built-in sun/moon directional light from its `TimeOfDay` and `Latitude`.

**Architecture:**

- `DynamicSky` (a `DataModel` in `engine_instances`) is carried as `VisualDynamicSky` in the render snapshot. The first Skybox *or* DynamicSky under Lighting is the sky.
- `SkyMath` (pure C++, `src/runner/`) turns the properties into sun/moon directions, the star frame, the light, and when the lighting cube is due.
- `GameView` fills `SceneLighting::dynamicSky` and puts the sky's light first in the light list (`id` 0).
- `Renderer` draws the visible sky with `dynamic_sky.frag` at full resolution. It draws a small environment cube with `dynamic_sky_cube.frag` through a new `EnvironmentMap::updateProcedural`, which reuses the existing irradiance and prefilter passes.
- Every shading pass keeps reading only `bindSky`'s cubes and uniforms, so SSR, AO, and the merge are unchanged.

**Tech Stack:** C++17 (MSVC 14.23 on Windows; Apple clang on macOS), OpenGL 3.3 core / GLSL 330, Catch2 (`sandbox`), `engine-tests`, `scene-render-check`.

**Spec:** `docs/superpowers/specs/2026-10-05-dynamic-sky-design.md`.

## Global Constraints

- **Branch:** work on branch `dynamic-sky` from `main`. Never commit on `main`.
- **Build (Git Bash):** `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target <target> --parallel`.
  - Run tests from the repository root, for example `build/Debug/sandbox.exe "[dynamic_sky]"`.
  - After restoring a file from a copy, `touch` it: MSBuild rebuilds by timestamp.
- **Compiler:** no `FLT_MAX` literal (use `std::numeric_limits<float>::max()`), and no `getenv`. Project code builds warning-free at /W4.
- **Properties:**

  | Property | Default | Rule |
  | --- | --- | --- |
  | TimeOfDay | 14 | wraps into [0, 24) |
  | Latitude | 35 | −90 to 90 |
  | Brightness | 3 | 0 to 20 |
  | Shadows | true | |
  | CloudCover | 0.5 | 0 to 1 |
  | CloudDensity | 0.5 | 0 to 1 |
  | WindDirection | (1, 0, 0.3) | |
  | SunTexture, MoonTexture | nil | `Texture?` |
  | SunSize, MoonSize | 2 | 0.1 to 20 |
  | ReflectionQuality | `Enum.EffectQuality.Medium` | |

- **Messages:**
  - "`<Property>` must be a finite number"
  - "WindDirection must be finite"
  - "SunTexture must be a Texture" / "MoonTexture must be a Texture"
  - "ReflectionQuality must be an Enum.EffectQuality"
  - "A DynamicSky must be in Lighting"
- **Axes:** +Y up, north −Z, east +X. Directions point *toward* the body.
- **Light:** the moon's is Brightness × 0.1. Each body's fade is a smoothstep of its elevation from 0° to 6°. Cloud dimming is `1 − 0.7 × CloudCover × CloudDensity`. ShadowDistance is 100.
- **ReflectionQuality → cube sizes** (environment/prefiltered): Low 128/128, Medium 256/256, High 512/512. Irradiance stays 32 and the prefiltered cube keeps 6 levels.
- **Lighting cube refresh:**
  - at once the first time, and on a ReflectionQuality change;
  - on another key change, once ≥ 0.05 s have passed since the last refresh;
  - while windy, every 0.25 s;
  - never otherwise.
- **No DynamicSky:** exactly today's pixels and cost.
- **Failure:** a DynamicSky program that fails to build, or a cube that can't be drawn yet, draws as no sky and never fails the frame.
- **The shading rule:** every pass reads only `uSkyEnabled`, `uViewToSky`, `uSkyColor`, `uSkyLightScale`, and the cubes. For a DynamicSky, `uSkyColor` = 1, `uSkyLightScale` = 1, and there is no rotation.

## Review Focus

1. **Latitude ±90°.** The sun circles on the horizon, so the light's intensity is 0 all day. The scene must still draw, with sky ambient and no NaN from the zero light. Covered by Task 1 test SM6 and the Task 4 render check "a polar sky draws".
2. **Switching sky kinds.** Skybox → DynamicSky → Skybox must re-make the cubes at the image sizes and show the image again, not the procedural cube. Covered by the Task 4 render check "the image sky comes back".
3. **ReflectionQuality changed at run time.** The cubes must be made again at the new size at once, without waiting for the refresh throttle. Covered by Task 1 SM8 and the Task 4 render check.
4. **A script animating TimeOfDay every frame.** At most one refresh per 0.05 s, and no frame skipped. Covered by Task 1 SM8.
5. **Looking straight down, and a sky with no clouds.** Directions at or below the horizon must not produce NaN. CloudCover 0 must draw no clouds. Covered by the Task 4 render check "straight down is dark ground, not NaN".

---

### Task 1: SkyMath

The pure math, with no GL, so it can be tested.

**Files:**
- Create: `src/runner/SkyMath.hpp`, `src/runner/SkyMath.cpp`, `sandbox/sky_math_tests.cpp`
- Modify: `CMakeLists.txt`
  - add `src/runner/SkyMath.cpp` after `src/runner/OcclusionMath.cpp` in `STUDIO_CORE_SOURCES`;
  - add `sandbox/sky_math_tests.cpp` after `sandbox/occlusion_math_tests.cpp`.

**Interfaces:**
- Produces (namespace `runner`):
  - `struct SkyVector { float x, y, z; }`
  - `SkyVector SunDirection(double timeOfDay, double latitudeDegrees)`
  - `void StarFrame(double timeOfDay, double latitudeDegrees, float out[9])`
  - `float ElevationDegrees(SkyVector)`
  - `float BodyFade(float)`
  - `float StarVisibility(float)`
  - `void Transmittance(float elevationDegrees, float out[3])`
  - `float CloudDimming(float cover, float density)`
  - `struct SkyLight`, `struct SkyState`, and `SkyState ComputeSky(double timeOfDay, double latitude, double brightness, double cloudCover, double cloudDensity)`
  - `struct EnvironmentSizes { int environment; int prefiltered; }` and `EnvironmentSizes EnvironmentSizesFor(int quality)`
  - `struct SkyLightingKey` with `==` and `!=`, and `bool LightingDue(const SkyLightingKey& made, const SkyLightingKey& now, double madeSeconds, double nowSeconds, bool windy, bool everMade)`
  - the constants below.

- [ ] **Step 1: Write the failing tests**

Create `sandbox/sky_math_tests.cpp`:

```cpp
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
```

- [ ] **Step 2: Add both files to CMake and confirm the test fails to build**

In `CMakeLists.txt`, add `    src/runner/SkyMath.cpp` after `    src/runner/OcclusionMath.cpp`, and `    sandbox/sky_math_tests.cpp` after `    sandbox/occlusion_math_tests.cpp`.

Run: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox --parallel`

Expected: FAIL. Either `runner/SkyMath.hpp` is not found, or CMake cannot find `SkyMath.cpp`.

- [ ] **Step 3: Write SkyMath.hpp**

```cpp
#pragma once

// The DynamicSky's sun, moon, stars, light, and lighting refreshes, worked
// out with no GL context so they can be tested. procedural_sky.glsl copies
// the atmosphere's numbers. World space: +Y up, north -Z, east +X; a
// direction points toward the body.
namespace runner {

// The moon's light is the sun's Brightness times this.
constexpr float kMoonBrightnessScale = 0.1f;
// Full, dense cloud takes this much of the light away.
constexpr float kCloudDimming = 0.7f;
// Each disc's radiance before the air reddens it, linear.
constexpr float kSunDiscRadiance = 40.f;
constexpr float kMoonDiscRadiance = 1.5f;
constexpr float kMoonTint[3] = {0.75f, 0.82f, 1.f};
// Studs from the camera the sky's light shadows, as a DirectionalLight's default.
constexpr float kSkyShadowDistance = 100.f;
// The lighting cube: no sooner after a change, and this often while clouds drift.
constexpr double kLightingChangeSeconds = 0.05;
constexpr double kLightingDriftSeconds = 0.25;
// Rayleigh (per meter, sea level) and Mie extinction, and their scale heights
// in meters, as procedural_sky.glsl has them.
constexpr float kRayleigh[3] = {5.5e-6f, 13.0e-6f, 22.4e-6f};
constexpr float kRayleighHeight = 8e3f;
constexpr float kMie = 21e-6f;
constexpr float kMieHeight = 1.2e3f;

struct SkyVector {
    float x = 0.f;
    float y = 1.f;
    float z = 0.f;
};

// Toward the sun at timeOfDay hours (0 to 24) and latitudeDegrees, at the
// equinox: east at 6, highest at 12 (90 - |latitude| up, south for a
// northern latitude), west at 18.
SkyVector SunDirection(double timeOfDay, double latitudeDegrees);
// Column-major: world directions into the stars' frame, which turns with the
// hour. The sun is always (0, 1, 0) in it and the celestial pole (0, 0, -1).
void StarFrame(double timeOfDay, double latitudeDegrees, float out[9]);
// Degrees above the horizon, -90 to 90.
float ElevationDegrees(SkyVector toward);
// How much a body above the horizon lights the scene: 0 at or below 0
// degrees, 1 from 6, smooth between.
float BodyFade(float elevationDegrees);
// How much the stars show: 0 with the sun above -2 degrees, 1 below -12.
float StarVisibility(float sunElevationDegrees);
// How much of a body's light reaches the ground through the air at this
// elevation, per channel, relative to straight overhead (1, 1, 1). Below the
// horizon is the horizon's.
void Transmittance(float elevationDegrees, float out[3]);
// 1 - kCloudDimming * cover * density.
float CloudDimming(float cover, float density);

// The sky's directional light.
struct SkyLight {
    SkyVector toward;
    // Linear, at most 1 per channel.
    float color[3] = {1.f, 1.f, 1.f};
    float intensity = 0.f;
    bool fromSun = true;
};

struct SkyState {
    SkyVector sun;
    SkyVector moon{0.f, -1.f, 0.f};
    float starFrame[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    float starVisibility = 0.f;
    // From the sun while it is above the horizon, the moon otherwise.
    SkyLight light;
    // Each disc's linear radiance, reddened by the air; 0 below the horizon.
    float sunColor[3] = {0.f, 0.f, 0.f};
    float moonColor[3] = {0.f, 0.f, 0.f};
};

SkyState ComputeSky(double timeOfDay, double latitudeDegrees, double brightness, double cloudCover,
                    double cloudDensity);

// The lighting cube's face widths, in texels, for Enum.EffectQuality's
// value: Low 0, Medium 1, High 2. Anything else is Medium.
struct EnvironmentSizes {
    int environment = 256;
    int prefiltered = 256;
};
EnvironmentSizes EnvironmentSizesFor(int quality);

// What the lighting cube is drawn from.
struct SkyLightingKey {
    float timeOfDay = 0.f;
    float latitude = 0.f;
    float cloudCover = 0.f;
    float cloudDensity = 0.f;
    int quality = 1;
};
bool operator==(const SkyLightingKey& a, const SkyLightingKey& b);
inline bool operator!=(const SkyLightingKey& a, const SkyLightingKey& b) { return !(a == b); }

// Whether to draw the lighting cube again now. made is what it was drawn
// from, at madeSeconds; everMade is false before the first.
bool LightingDue(const SkyLightingKey& made, const SkyLightingKey& now, double madeSeconds, double nowSeconds,
                 bool windy, bool everMade);

}  // namespace runner
```

- [ ] **Step 4: Write SkyMath.cpp**

```cpp
#include "SkyMath.hpp"

#include <algorithm>
#include <cmath>

namespace runner {
namespace {

constexpr double kDegrees = 3.14159265358979323846 / 180.0;

float Smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

}  // namespace

SkyVector SunDirection(double timeOfDay, double latitudeDegrees) {
    const double hour = (timeOfDay - 12.0) * 15.0 * kDegrees;
    const double phi = latitudeDegrees * kDegrees;
    // Around the celestial equator, then tilted about X by the latitude.
    return {static_cast<float>(-std::sin(hour)), static_cast<float>(std::cos(hour) * std::cos(phi)),
            static_cast<float>(std::cos(hour) * std::sin(phi))};
}

void StarFrame(double timeOfDay, double latitudeDegrees, float out[9]) {
    const double hour = (timeOfDay - 12.0) * 15.0 * kDegrees;
    const double phi = latitudeDegrees * kDegrees;
    const double ch = std::cos(hour);
    const double sh = std::sin(hour);
    const double cp = std::cos(phi);
    const double sp = std::sin(phi);
    // The latitude's tilt undone, then the hour's turn about the pole undone.
    const double rows[3][3] = {{ch, sh * cp, sh * sp}, {-sh, ch * cp, ch * sp}, {0.0, -sp, cp}};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            out[column * 3 + row] = static_cast<float>(rows[row][column]);
        }
    }
}

float ElevationDegrees(SkyVector toward) {
    const float length = std::sqrt(toward.x * toward.x + toward.y * toward.y + toward.z * toward.z);
    if (!(length > 0.f)) {
        return 0.f;
    }
    return static_cast<float>(std::asin(std::clamp(toward.y / length, -1.f, 1.f)) / kDegrees);
}

float BodyFade(float elevationDegrees) { return Smoothstep(0.f, 6.f, elevationDegrees); }

float StarVisibility(float sunElevationDegrees) { return Smoothstep(-2.f, -12.f, sunElevationDegrees); }

void Transmittance(float elevationDegrees, float out[3]) {
    // Kasten and Young's air mass (1989), relative to straight overhead.
    const double zenith = 90.0 - std::clamp(static_cast<double>(elevationDegrees), 0.0, 90.0);
    const double mass =
        1.0 / (std::cos(zenith * kDegrees) + 0.50572 * std::pow(96.07995 - zenith, -1.6364));
    const double overhead = 1.0 / (1.0 + 0.50572 * std::pow(96.07995, -1.6364));
    const double extra = mass - overhead;
    for (int channel = 0; channel < 3; ++channel) {
        const double depth = static_cast<double>(kRayleigh[channel]) * kRayleighHeight +
                             static_cast<double>(kMie) * 1.1 * kMieHeight;
        out[channel] = static_cast<float>(std::exp(-depth * extra));
    }
}

float CloudDimming(float cover, float density) {
    return 1.f - kCloudDimming * std::clamp(cover, 0.f, 1.f) * std::clamp(density, 0.f, 1.f);
}

SkyState ComputeSky(double timeOfDay, double latitudeDegrees, double brightness, double cloudCover,
                    double cloudDensity) {
    SkyState state;
    state.sun = SunDirection(timeOfDay, latitudeDegrees);
    state.moon = {-state.sun.x, -state.sun.y, -state.sun.z};
    StarFrame(timeOfDay, latitudeDegrees, state.starFrame);
    const float sunUp = ElevationDegrees(state.sun);
    const float moonUp = -sunUp;
    state.starVisibility = StarVisibility(sunUp);
    const float dimming = CloudDimming(static_cast<float>(cloudCover), static_cast<float>(cloudDensity));
    const float strength = static_cast<float>(std::max(brightness, 0.0));

    float sunAir[3];
    float moonAir[3];
    Transmittance(sunUp, sunAir);
    Transmittance(moonUp, moonAir);
    for (int channel = 0; channel < 3; ++channel) {
        state.sunColor[channel] = sunUp > -1.f ? sunAir[channel] * kSunDiscRadiance : 0.f;
        state.moonColor[channel] = moonUp > -1.f ? moonAir[channel] * kMoonTint[channel] * kMoonDiscRadiance : 0.f;
    }

    // Both fade to nothing as they cross the horizon together, so the light never jumps.
    SkyLight& light = state.light;
    if (sunUp > 0.f) {
        light.fromSun = true;
        light.toward = state.sun;
        std::copy(sunAir, sunAir + 3, light.color);
        light.intensity = strength * BodyFade(sunUp) * dimming;
    } else {
        light.fromSun = false;
        light.toward = state.moon;
        for (int channel = 0; channel < 3; ++channel) {
            light.color[channel] = moonAir[channel] * kMoonTint[channel];
        }
        light.intensity = strength * kMoonBrightnessScale * BodyFade(moonUp) * dimming;
    }
    return state;
}

EnvironmentSizes EnvironmentSizesFor(int quality) {
    if (quality == 0) {
        return {128, 128};
    }
    if (quality == 2) {
        return {512, 512};
    }
    return {256, 256};
}

bool operator==(const SkyLightingKey& a, const SkyLightingKey& b) {
    return a.timeOfDay == b.timeOfDay && a.latitude == b.latitude && a.cloudCover == b.cloudCover &&
           a.cloudDensity == b.cloudDensity && a.quality == b.quality;
}

bool LightingDue(const SkyLightingKey& made, const SkyLightingKey& now, double madeSeconds, double nowSeconds,
                 bool windy, bool everMade) {
    if (!everMade || made.quality != now.quality) {
        return true;
    }
    const double since = nowSeconds - madeSeconds;
    if (since < 0.0) {
        return true;
    }
    if (made != now) {
        return since >= kLightingChangeSeconds;
    }
    return windy && since >= kLightingDriftSeconds;
}

}  // namespace runner
```

- [ ] **Step 5: Build and run the tests**

Run: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox --parallel && build/Debug/sandbox.exe "[sky_math]"`

Expected: PASS. "All tests passed (… assertions in 8 test cases)".

- [ ] **Step 6: Commit**

```bash
git add src/runner/SkyMath.hpp src/runner/SkyMath.cpp sandbox/sky_math_tests.cpp CMakeLists.txt
git commit -m "Add SkyMath: the dynamic sky's sun, moon, stars, light, and lighting refreshes, with no GL"
```

---

### Task 2: The DynamicSky instance and its registration

**Files:**
- Create: `src/engine_instances/DynamicSky.hpp`, `src/engine_instances/DynamicSky.cpp`, `sandbox/dynamic_sky_tests.cpp`
- Modify:
  - `CMakeLists.txt`: `src/engine_instances/DynamicSky.cpp` before `src/engine_instances/Skybox.cpp`, and `sandbox/dynamic_sky_tests.cpp` after `sandbox/skybox_tests.cpp`
  - `src/engine_core/Containment.cpp:132-133`
  - `src/engine_core/Project.cpp`: include and `class_registry`
  - `src/engine_core/ScriptBindings.cpp`: include, `create_dynamic_sky`, and `register_lua_creatable`
  - `src/engine_core/LuaApi.cpp`: `build_docs`, after the Skybox lines
  - `src/ide/ClassOrder.hpp`
  - `src/ide/IdeIcons.cpp`
  - `tests/LuauCompleteTest.cpp:1201`
  - `README.md`, `src/engine_instances/README.md`

**Interfaces:**
- Consumes: `engine_core::EffectQuality`, `effect_quality_enum()`, `enum_item_name` (`Enum.hpp`); `set_instance_reference`, `instance_reference_slot`, and `note_property_change` (`DataModel`); `Vec3` (`Vector3.hpp`).
- Produces: `engine_core::DynamicSky`
  - getters: `double time_of_day()`, `latitude()`, `brightness()`, `cloud_cover()`, `cloud_density()`, `sun_size()`, `moon_size()`; `bool shadows()`; `Vec3 wind_direction()`; `LuaSlot sun_texture()`, `moon_texture()`; `EffectQuality reflection_quality()`
  - setters `set_*`, each returning `std::optional<std::string>`; `set_reflection_quality(int)`
  - constants: `kDefaultTimeOfDay` 14, `kDefaultLatitude` 35, `kMaxLatitude` 90, `kDefaultBrightness` 3, `kMaxBrightness` 20, `kDefaultShadows` true, `kDefaultCloudCover` 0.5, `kDefaultCloudDensity` 0.5, `kDefaultWindDirection` {1, 0, 0.3}, `kDefaultSunSize` 2, `kDefaultMoonSize` 2, `kMinBodySize` 0.1, `kMaxBodySize` 20, `kDefaultReflectionQuality` `EffectQuality::Medium`
  - Defaults changed in 2d00273 to Brightness 2, SunSize/MoonSize 4.

- [ ] **Step 1: Write the failing tests**

Create `sandbox/dynamic_sky_tests.cpp`:

```cpp
// DynamicSky: a sky drawn from a shader under Lighting, whose clock, place,
// clouds, sun, and moon the render snapshot carries.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "DynamicSky.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "Skybox.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace {

using engine_core::DynamicSky;
using engine_core::InstanceId;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

DynamicSky& add_dynamic_sky(engine_core::DataModel& game, InstanceId parent) {
    DynamicSky& sky = game.create<DynamicSky>();
    game.set_parent(sky.id(), parent);
    return sky;
}

engine_core::Texture& add_texture(engine_core::DataModel& game, const char* name, const char* path) {
    engine_core::Texture& texture = game.create<engine_core::Texture>();
    game.set_name(texture.id(), name);
    REQUIRE_FALSE(texture.set_path(path));
    game.set_parent(texture.id(), game.service("Textures"));
    return texture;
}

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

}  // namespace

TEST_CASE("DS1 a DynamicSky's properties are checked, undo, save, and come back at Stop", "[dynamic_sky]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("DynamicSky"));
    DynamicSky& sky = add_dynamic_sky(game, game.scene_service("Lighting"));
    REQUIRE(sky.time_of_day() == 14.0);
    REQUIRE(sky.latitude() == 35.0);
    REQUIRE(sky.brightness() == 3.0);
    REQUIRE(sky.shadows());
    REQUIRE(sky.cloud_cover() == 0.5);
    REQUIRE(sky.cloud_density() == 0.5);
    REQUIRE(sky.wind_direction().x == 1.f);
    REQUIRE(sky.wind_direction().z == 0.3f);
    REQUIRE(sky.sun_texture().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(sky.moon_texture().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(sky.sun_size() == 2.0);
    REQUIRE(sky.moon_size() == 2.0);
    REQUIRE(sky.reflection_quality() == engine_core::EffectQuality::Medium);

    // A default DynamicSky saves none of them.
    engine_core::PropertyBag saved;
    sky.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set TimeOfDay");
    REQUIRE_FALSE(sky.set_time_of_day(6.5));
    end_step(game);
    REQUIRE(sky.time_of_day() == 6.5);
    game.history().undo();
    REQUIRE(sky.time_of_day() == 14.0);
    game.history().redo();
    REQUIRE(sky.time_of_day() == 6.5);

    // TimeOfDay wraps into [0, 24).
    REQUIRE_FALSE(sky.set_time_of_day(25.0));
    REQUIRE(sky.time_of_day() == 1.0);
    REQUIRE_FALSE(sky.set_time_of_day(-1.0));
    REQUIRE(sky.time_of_day() == 23.0);
    REQUIRE_FALSE(sky.set_time_of_day(24.0));
    REQUIRE(sky.time_of_day() == 0.0);
    REQUIRE_FALSE(sky.set_time_of_day(-1e-300));
    REQUIRE(sky.time_of_day() < 24.0);
    // The rest clamp.
    REQUIRE_FALSE(sky.set_latitude(100.0));
    REQUIRE(sky.latitude() == 90.0);
    REQUIRE_FALSE(sky.set_latitude(-100.0));
    REQUIRE(sky.latitude() == -90.0);
    REQUIRE_FALSE(sky.set_brightness(50.0));
    REQUIRE(sky.brightness() == DynamicSky::kMaxBrightness);
    REQUIRE_FALSE(sky.set_brightness(-1.0));
    REQUIRE(sky.brightness() == 0.0);
    REQUIRE_FALSE(sky.set_cloud_cover(2.0));
    REQUIRE(sky.cloud_cover() == 1.0);
    REQUIRE_FALSE(sky.set_cloud_density(-2.0));
    REQUIRE(sky.cloud_density() == 0.0);
    REQUIRE_FALSE(sky.set_sun_size(0.0));
    REQUIRE(sky.sun_size() == DynamicSky::kMinBodySize);
    REQUIRE_FALSE(sky.set_moon_size(30.0));
    REQUIRE(sky.moon_size() == DynamicSky::kMaxBodySize);
    // Not a number is refused, and changes nothing.
    REQUIRE(reason(sky.set_time_of_day(std::nan(""))) == "TimeOfDay must be a finite number");
    REQUIRE(reason(sky.set_latitude(INFINITY)) == "Latitude must be a finite number");
    REQUIRE(reason(sky.set_brightness(std::nan(""))) == "Brightness must be a finite number");
    REQUIRE(reason(sky.set_cloud_cover(std::nan(""))) == "CloudCover must be a finite number");
    REQUIRE(reason(sky.set_cloud_density(std::nan(""))) == "CloudDensity must be a finite number");
    REQUIRE(reason(sky.set_sun_size(std::nan(""))) == "SunSize must be a finite number");
    REQUIRE(reason(sky.set_moon_size(std::nan(""))) == "MoonSize must be a finite number");
    REQUIRE(reason(sky.set_wind_direction(engine_core::Vec3{std::nanf(""), 0.f, 0.f})) ==
            "WindDirection must be finite");
    REQUIRE(reason(sky.set_reflection_quality(7)) == "ReflectionQuality must be an Enum.EffectQuality");
    REQUIRE(sky.reflection_quality() == engine_core::EffectQuality::Medium);

    // The textures take a Texture, and nothing else.
    engine_core::Texture& sun = add_texture(game, "Sun", "textures/sun.png");
    REQUIRE_FALSE(sky.set_sun_texture(instance_slot(sun.id())));
    REQUIRE(sky.sun_texture().id == sun.id());
    engine_core::GameObject& part = create_part(game);
    REQUIRE(reason(sky.set_sun_texture(instance_slot(part.id()))) == "SunTexture must be a Texture");
    REQUIRE(reason(sky.set_moon_texture(instance_slot(part.id()))) == "MoonTexture must be a Texture");
    REQUIRE_FALSE(sky.set_moon_texture(instance_slot(sun.id())));

    REQUIRE_FALSE(sky.set_time_of_day(20.0));
    REQUIRE_FALSE(sky.set_shadows(false));
    REQUIRE_FALSE(sky.set_wind_direction(engine_core::Vec3{0.f, 0.f, 5.f}));
    REQUIRE_FALSE(sky.set_reflection_quality(2));
    engine_core::PropertyBag changed;
    sky.save_properties(changed);
    for (const char* name : {"TimeOfDay", "Latitude", "Brightness", "Shadows", "CloudCover", "CloudDensity",
                             "WindDirection", "SunTexture", "MoonTexture", "SunSize", "MoonSize",
                             "ReflectionQuality"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(changed, name) != nullptr);
    }

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(sky.set_time_of_day(3.0));
    REQUIRE_FALSE(sky.set_shadows(true));
    REQUIRE_FALSE(sky.set_sun_texture(engine_core::LuaSlot{}));
    game.stop_simulation();
    REQUIRE(sky.time_of_day() == 20.0);
    REQUIRE_FALSE(sky.shadows());
    REQUIRE(sky.sun_texture().id == sun.id());
    REQUIRE(sky.wind_direction().z == 5.f);
    REQUIRE(sky.reflection_quality() == engine_core::EffectQuality::High);
}

TEST_CASE("DS2 a DynamicSky belongs under Lighting and nowhere else", "[dynamic_sky]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    REQUIRE_FALSE(placement_error("Lighting", "DynamicSky", "Sky"));
    REQUIRE(reason(placement_error("Workspace", "DynamicSky", "Sky")) == "A DynamicSky must be in Lighting");
    REQUIRE(reason(placement_error("Storage", "DynamicSky", "Sky")) == "A DynamicSky must be in Lighting");

    const InstanceId lighting = game.scene_service("Lighting");
    DynamicSky& sky = add_dynamic_sky(game, lighting);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(sky.id(), folder.id()));
    REQUIRE(reason(game.parent_error(sky.id(), workspace_of(game))) == "A DynamicSky must be in Lighting");

    REQUIRE(engine_core::parent_suits("Lighting", "DynamicSky"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "DynamicSky"));
}

TEST_CASE("DS4 scripts make a DynamicSky and set it", "[dynamic_sky]") {
    ScriptRig rig;
    engine_core::Texture& moon = rig.game.create<engine_core::Texture>();
    rig.game.set_name(moon.id(), "Moon");
    rig.game.set_parent(moon.id(), rig.game.service("Textures"));
    add_script(rig.game, "Sky", R"(
        local sky = Instance.new("DynamicSky", game.Lighting)
        _G.defaults = sky.TimeOfDay == 14 and sky.Latitude == 35 and sky.Brightness == 3 and sky.Shadows
            and sky.CloudCover == 0.5 and sky.CloudDensity == 0.5 and sky.SunTexture == nil
            and sky.MoonTexture == nil and sky.SunSize == 2 and sky.MoonSize == 2
            and sky.ReflectionQuality == Enum.EffectQuality.Medium
            and math.abs(sky.WindDirection.Z - 0.3) < 1e-6
        sky.TimeOfDay = 30
        sky.Latitude = -200
        sky.MoonTexture = game.Assets.Textures.Moon
        sky.WindDirection = Vector3.new(2, 0, 0)
        sky.ReflectionQuality = Enum.EffectQuality.Low
        _G.set = sky.TimeOfDay == 6 and sky.Latitude == -90 and sky.MoonTexture == game.Assets.Textures.Moon
            and sky.WindDirection == Vector3.new(2, 0, 0) and sky.ReflectionQuality == Enum.EffectQuality.Low
        _G.refused = not pcall(function() sky.SunTexture = workspace end)
            and not pcall(function() sky.TimeOfDay = 0 / 0 end)
            and not pcall(function() sky.ReflectionQuality = 2 end)
            and not pcall(function() Instance.new("DynamicSky", workspace) end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"defaults", "set", "refused"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}
```

(DS3 and DS5, the snapshot tests, come in Task 3.)

- [ ] **Step 2: Add to CMake and confirm it fails**

In `CMakeLists.txt`:
- add `    src/engine_instances/DynamicSky.cpp` before `    src/engine_instances/Skybox.cpp`;
- add `    sandbox/dynamic_sky_tests.cpp` after `    sandbox/skybox_tests.cpp`.

Run: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox --parallel`

Expected: FAIL, because `DynamicSky.hpp` is not found.

- [ ] **Step 3: Write DynamicSky.hpp**

```cpp
#pragma once

#include "DataModel.hpp"
#include "Enum.hpp"
#include "InstanceRef.hpp"
#include "Vector3.hpp"

#include <optional>
#include <string>

namespace engine_core {

// The sky drawn from a shader instead of an image: an atmosphere that is blue
// at noon, warm at sunset, and dark at night, with drifting clouds, the sun
// by day, and the moon and stars by night. It lights the place with its own
// directional light from the sun or the moon. Like a Skybox it belongs under
// Lighting, at any depth through Folders, and nowhere else; the first Skybox
// or DynamicSky in the tree is the sky.
//
// TimeOfDay          number              hours, wrapped into 0 up to 24. 14.
// Latitude           number              degrees, -90 to 90: how high the sun
//                                        climbs (90 - |Latitude| at noon). 35.
// Brightness         number              the sun light's intensity; the moon's
//                                        is a tenth of it. 3, 0 to kMaxBrightness.
// Shadows            boolean             whether the sun or moon light casts shadows. True.
// CloudCover         number              how much of the sky has cloud, 0 to 1. 0.5.
// CloudDensity       number              how thick the clouds are, 0 to 1. 0.5.
// WindDirection      Vector3             the clouds' drift, studs per second; Y is ignored.
// SunTexture         Texture?            drawn in place of the sun's disc. Nil.
// MoonTexture        Texture?            drawn in place of the moon's disc. Nil.
// SunSize, MoonSize  number              degrees across, kMinBodySize to kMaxBodySize. 2.
// ReflectionQuality  Enum.EffectQuality  the size of the cube the sky's light
//                                        and reflections are filtered from:
//                                        Low 128, Medium 256, High 512. Medium.
//
// Each is a saved registry property (lua_saved_property). The render
// snapshot reads them at every Prepare (VisualDynamicSky).
class DynamicSky : public DataModel {
public:
    static constexpr double kDefaultTimeOfDay = 14.0;
    static constexpr double kDefaultLatitude = 35.0;
    static constexpr double kMaxLatitude = 90.0;
    static constexpr double kDefaultBrightness = 3.0;
    static constexpr double kMaxBrightness = 20.0;
    static constexpr bool kDefaultShadows = true;
    static constexpr double kDefaultCloudCover = 0.5;
    static constexpr double kDefaultCloudDensity = 0.5;
    static constexpr Vec3 kDefaultWindDirection{1.f, 0.f, 0.3f};
    static constexpr double kDefaultSunSize = 2.0;
    static constexpr double kDefaultMoonSize = 2.0;
    static constexpr double kMinBodySize = 0.1;
    static constexpr double kMaxBodySize = 20.0;
    static constexpr EffectQuality kDefaultReflectionQuality = EffectQuality::Medium;

    DynamicSky(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "DynamicSky"; }

    double time_of_day() const { return time_of_day_; }
    double latitude() const { return latitude_; }
    double brightness() const { return brightness_; }
    bool shadows() const { return shadows_; }
    double cloud_cover() const { return cloud_cover_; }
    double cloud_density() const { return cloud_density_; }
    Vec3 wind_direction() const { return wind_direction_; }
    LuaSlot sun_texture() const;
    LuaSlot moon_texture() const;
    double sun_size() const { return sun_size_; }
    double moon_size() const { return moon_size_; }
    EffectQuality reflection_quality() const { return reflection_quality_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing. A number that is not finite is refused; TimeOfDay wraps and
    // the others clamp into their ranges. A ReflectionQuality that is not an
    // Enum.EffectQuality's value is refused, and so is a texture that is not a Texture.
    std::optional<std::string> set_time_of_day(double hours);
    std::optional<std::string> set_latitude(double degrees);
    std::optional<std::string> set_brightness(double value);
    std::optional<std::string> set_shadows(bool value);
    std::optional<std::string> set_cloud_cover(double value);
    std::optional<std::string> set_cloud_density(double value);
    std::optional<std::string> set_wind_direction(Vec3 value);
    std::optional<std::string> set_sun_texture(const LuaSlot& value);
    std::optional<std::string> set_moon_texture(const LuaSlot& value);
    std::optional<std::string> set_sun_size(double degrees);
    std::optional<std::string> set_moon_size(double degrees);
    std::optional<std::string> set_reflection_quality(int value);

protected:
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value, double min, double max);

    double time_of_day_ = kDefaultTimeOfDay;
    double latitude_ = kDefaultLatitude;
    double brightness_ = kDefaultBrightness;
    bool shadows_ = kDefaultShadows;
    double cloud_cover_ = kDefaultCloudCover;
    double cloud_density_ = kDefaultCloudDensity;
    Vec3 wind_direction_ = kDefaultWindDirection;
    InstanceRef sun_texture_ref_;
    InstanceRef moon_texture_ref_;
    double sun_size_ = kDefaultSunSize;
    double moon_size_ = kDefaultMoonSize;
    EffectQuality reflection_quality_ = kDefaultReflectionQuality;
};

}  // namespace engine_core
```

- [ ] **Step 4: Write DynamicSky.cpp**

```cpp
#include "DynamicSky.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot vec3_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

LuaSlot quality_slot(EffectQuality quality) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &effect_quality_enum();
    slot.number = static_cast<int>(quality);
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("DynamicSky setters run on SimulationThread");
    }
}

}  // namespace

LuaSlot DynamicSky::sun_texture() const { return instance_reference_slot(sun_texture_ref_, "Texture"); }

LuaSlot DynamicSky::moon_texture() const { return instance_reference_slot(moon_texture_ref_, "Texture"); }

std::optional<std::string> DynamicSky::set_number(const char* property, double& slot, double value, double min,
                                                  double max) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    value = std::clamp(value, min, max);
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> DynamicSky::set_time_of_day(double hours) {
    if (!std::isfinite(hours)) {
        require_thread(*this);
        return std::string("TimeOfDay must be a finite number");
    }
    hours = std::fmod(hours, 24.0);
    if (hours < 0) {
        hours += 24.0;
    }
    // fmod of a tiny negative number, plus 24, can round to 24 itself.
    if (hours >= 24.0) {
        hours = 0.0;
    }
    return set_number("TimeOfDay", time_of_day_, hours, 0.0, 24.0);
}

std::optional<std::string> DynamicSky::set_latitude(double degrees) {
    return set_number("Latitude", latitude_, degrees, -kMaxLatitude, kMaxLatitude);
}

std::optional<std::string> DynamicSky::set_brightness(double value) {
    return set_number("Brightness", brightness_, value, 0.0, kMaxBrightness);
}

std::optional<std::string> DynamicSky::set_shadows(bool value) {
    require_thread(*this);
    if (shadows_ == value) {
        return std::nullopt;
    }
    shadows_ = value;
    note_property_change("Shadows", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> DynamicSky::set_cloud_cover(double value) {
    return set_number("CloudCover", cloud_cover_, value, 0.0, 1.0);
}

std::optional<std::string> DynamicSky::set_cloud_density(double value) {
    return set_number("CloudDensity", cloud_density_, value, 0.0, 1.0);
}

std::optional<std::string> DynamicSky::set_wind_direction(Vec3 value) {
    require_thread(*this);
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
        return std::string("WindDirection must be finite");
    }
    if (value.x == wind_direction_.x && value.y == wind_direction_.y && value.z == wind_direction_.z) {
        return std::nullopt;
    }
    const Vec3 previous = wind_direction_;
    wind_direction_ = value;
    note_property_change("WindDirection", vec3_slot(previous), vec3_slot(value));
    return std::nullopt;
}

std::optional<std::string> DynamicSky::set_sun_texture(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("SunTexture", "Texture", sun_texture_ref_, value);
}

std::optional<std::string> DynamicSky::set_moon_texture(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("MoonTexture", "Texture", moon_texture_ref_, value);
}

std::optional<std::string> DynamicSky::set_sun_size(double degrees) {
    return set_number("SunSize", sun_size_, degrees, kMinBodySize, kMaxBodySize);
}

std::optional<std::string> DynamicSky::set_moon_size(double degrees) {
    return set_number("MoonSize", moon_size_, degrees, kMinBodySize, kMaxBodySize);
}

std::optional<std::string> DynamicSky::set_reflection_quality(int value) {
    require_thread(*this);
    if (enum_item_name(effect_quality_enum(), value) == nullptr) {
        return std::string("ReflectionQuality must be an Enum.EffectQuality");
    }
    const EffectQuality next = static_cast<EffectQuality>(value);
    if (next == reflection_quality_) {
        return std::nullopt;
    }
    const EffectQuality previous = reflection_quality_;
    reflection_quality_ = next;
    note_property_change("ReflectionQuality", quality_slot(previous), quality_slot(next));
    return std::nullopt;
}

void DynamicSky::on_reuse() {
    time_of_day_ = kDefaultTimeOfDay;
    latitude_ = kDefaultLatitude;
    brightness_ = kDefaultBrightness;
    shadows_ = kDefaultShadows;
    cloud_cover_ = kDefaultCloudCover;
    cloud_density_ = kDefaultCloudDensity;
    wind_direction_ = kDefaultWindDirection;
    sun_texture_ref_.set_guid(std::string());
    moon_texture_ref_.set_guid(std::string());
    sun_size_ = kDefaultSunSize;
    moon_size_ = kDefaultMoonSize;
    reflection_quality_ = kDefaultReflectionQuality;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

DynamicSky* sky_of(DataModel& object) { return dynamic_cast<DynamicSky*>(&object); }

template <double (DynamicSky::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = number_slot((sky->*Get)());
    return true;
}

template <std::optional<std::string> (DynamicSky::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    return sky != nullptr && refuse(in, (sky->*Set)(in.number));
}

template <LuaSlot (DynamicSky::*Get)() const>
bool read_texture(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = (sky->*Get)();
    return true;
}

template <std::optional<std::string> (DynamicSky::*Set)(const LuaSlot&)>
bool write_texture(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    return sky != nullptr && refuse(in, (sky->*Set)(in));
}

bool read_shadows(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = bool_slot(sky->shadows());
    return true;
}

bool write_shadows(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    return sky != nullptr && refuse(in, sky->set_shadows(in.flag));
}

bool read_wind(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = vec3_slot(sky->wind_direction());
    return true;
}

bool write_wind(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    return sky != nullptr && refuse(in, sky->set_wind_direction(in.vec));
}

bool read_quality(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = quality_slot(sky->reflection_quality());
    return true;
}

bool write_quality(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &effect_quality_enum()) {
        in.error = "ReflectionQuality must be an Enum.EffectQuality";
        return false;
    }
    return refuse(in, sky->set_reflection_quality(static_cast<int>(in.number)));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_dynamic_sky_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    using S = DynamicSky;
    static const std::string time_of_day = number_json(S::kDefaultTimeOfDay);
    static const std::string latitude = number_json(S::kDefaultLatitude);
    static const std::string brightness = number_json(S::kDefaultBrightness);
    static const std::string cloud_cover = number_json(S::kDefaultCloudCover);
    static const std::string cloud_density = number_json(S::kDefaultCloudDensity);
    static const std::string sun_size = number_json(S::kDefaultSunSize);
    static const std::string moon_size = number_json(S::kDefaultMoonSize);
    static const std::string wind = [] {
        const float axes[3] = {S::kDefaultWindDirection.x, S::kDefaultWindDirection.y, S::kDefaultWindDirection.z};
        return write_json(json_floats(axes, 3));
    }();
    const LuaField fields[] = {
        lua_slider(lua_saved_property("TimeOfDay", "number", read_number<&S::time_of_day>,
                                      write_number<&S::set_time_of_day>, time_of_day.c_str()),
                   0.0, 24.0),
        lua_slider(lua_saved_property("Latitude", "number", read_number<&S::latitude>,
                                      write_number<&S::set_latitude>, latitude.c_str()),
                   -S::kMaxLatitude, S::kMaxLatitude),
        lua_slider(lua_saved_property("Brightness", "number", read_number<&S::brightness>,
                                      write_number<&S::set_brightness>, brightness.c_str()),
                   0.0, S::kMaxBrightness),
        lua_saved_property("Shadows", "boolean", read_shadows, write_shadows, S::kDefaultShadows ? "true" : "false"),
        lua_slider(lua_saved_property("CloudCover", "number", read_number<&S::cloud_cover>,
                                      write_number<&S::set_cloud_cover>, cloud_cover.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("CloudDensity", "number", read_number<&S::cloud_density>,
                                      write_number<&S::set_cloud_density>, cloud_density.c_str()),
                   0.0, 1.0),
        lua_saved_property("WindDirection", "Vector3", read_wind, write_wind, wind.c_str()),
        lua_saved_property("SunTexture", "Texture?", read_texture<&S::sun_texture>,
                           write_texture<&S::set_sun_texture>, "null"),
        lua_saved_property("MoonTexture", "Texture?", read_texture<&S::moon_texture>,
                           write_texture<&S::set_moon_texture>, "null"),
        lua_slider(lua_saved_property("SunSize", "number", read_number<&S::sun_size>, write_number<&S::set_sun_size>,
                                      sun_size.c_str()),
                   S::kMinBodySize, S::kMaxBodySize),
        lua_slider(lua_saved_property("MoonSize", "number", read_number<&S::moon_size>,
                                      write_number<&S::set_moon_size>, moon_size.c_str()),
                   S::kMinBodySize, S::kMaxBodySize),
        lua_saved_enum("ReflectionQuality", effect_quality_enum(), read_quality, write_quality, "\"Medium\""),
    };
    register_lua_class("DynamicSky", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("DynamicSky", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
```

If `set_number`'s wrap-then-clamp leaves `TimeOfDay` 24 possible: the wrap above already maps it into [0, 24), and the clamp is a no-op.

- [ ] **Step 5: Register the class everywhere**

`src/engine_core/Containment.cpp`: add the new class to the Lighting-only list:

```cpp
    if ((child_class == "Skybox" || child_class == "DynamicSky" || child_class == "BloomEffect" ||
         child_class == "ScreenSpaceReflections" || child_class == "AmbientOcclusionEffect") &&
        holder_class != "Lighting") {
```

`src/engine_core/Project.cpp`: add `#include "DynamicSky.hpp"` in alphabetical order with the others. After the Skybox line in `class_registry` (line 144), add:

```cpp
        out.push_back({"DynamicSky", [](DataModel& world) -> DataModel& { return world.create<DynamicSky>(); }});
```

`src/engine_core/ScriptBindings.cpp`:
- add `#include "DynamicSky.hpp"`;
- after `create_skybox` (line 196), add `DataModel& create_dynamic_sky(DataModel& world) { return world.create<DynamicSky>(); }`;
- after `register_lua_creatable("Skybox", create_skybox);`, add `register_lua_creatable("DynamicSky", create_dynamic_sky);`.

`src/engine_core/LuaApi.cpp`: after the last `add("Skybox", ...)` (Tint), add:

```cpp
    add("DynamicSky", "TimeOfDay",
        "The hour, from 0 up to 24: sunrise in the east at 6, noon at 12, sunset in the west at 18. It moves "
        "only when set.",
        "number", false, {});
    add("DynamicSky", "Latitude",
        "Degrees from -90 to 90: at noon the sun stands 90 minus this many degrees up, to the south for a "
        "northern latitude.",
        "number", false, {});
    add("DynamicSky", "Brightness",
        "The sun light's intensity, from 0 to 20. The moon's light is a tenth of it.", "number", false, {});
    add("DynamicSky", "Shadows", "When true, the sun or moon light casts shadows.", "boolean", false, {});
    add("DynamicSky", "CloudCover", "How much of the sky has cloud, from 0 (clear) to 1 (overcast).", "number",
        false, {});
    add("DynamicSky", "CloudDensity",
        "How thick and opaque the clouds are, from 0 to 1. Thick cover dims the sun light.", "number", false, {});
    add("DynamicSky", "WindDirection",
        "Which way the clouds drift, across X and Z; its length is their speed in studs per second.", "Vector3",
        false, {});
    add("DynamicSky", "SunTexture", "An image drawn in place of the sun's disc. Nil draws the disc.", "Texture?",
        false, {});
    add("DynamicSky", "MoonTexture", "An image drawn in place of the moon's disc. Nil draws the disc.",
        "Texture?", false, {});
    add("DynamicSky", "SunSize", "How many degrees across the sun looks, from 0.1 to 20.", "number", false, {});
    add("DynamicSky", "MoonSize", "How many degrees across the moon looks, from 0.1 to 20.", "number", false, {});
    add("DynamicSky", "ReflectionQuality",
        "Enum.EffectQuality: how sharp the sky's light and reflections are (Low, Medium, High). The sky "
        "itself is always drawn at full resolution.",
        "Enum.EffectQuality", false, {});
```

`src/ide/ClassOrder.hpp`: after `{"Skybox", 4},`, add `        {"DynamicSky", 4},`.

`src/ide/IdeIcons.cpp`: in `IconFileOverride`, before the `PointLight` case, add:

```cpp
    if (class_name == "DynamicSky") {
        return "icon-sky.png";
    }
```

`tests/LuauCompleteTest.cpp:1201`: change the row to

```cpp
        "AmbientOcclusionEffect", "BloomEffect", "DynamicSky", "ScreenSpaceReflections", "Skybox",
```

`src/engine_instances/README.md:3`: add `` `DynamicSky`, `` after `` `Skybox`, ``.

`README.md`:
- line 16: change "and a `Skybox` in it is the sky, and a `BloomEffect` its bloom" to "and a `Skybox` or `DynamicSky` in it is the sky, and a `BloomEffect` its bloom".
- After the Skybox paragraph (line 33), add this paragraph:

```markdown
`DynamicSky` is a sky drawn from a shader instead of an image, and lives only under `Lighting` (through Folders too); anywhere else it is refused with "A DynamicSky must be in Lighting". It draws an atmosphere that is blue at noon, warm at sunset, and dark at night, drifting clouds, the sun by day, and the moon and stars by night, and lights the place with its own directional light from the sun or moon. `TimeOfDay` (0 up to 24, 14 by default) is the hour; it moves only when set, so a script animates it. `Latitude` (-90 to 90, 35 by default) sets how high the sun climbs: 90 minus its size at noon, to the south (+Z) for a northern latitude; the sun always rises due east (+X) at 6 and sets due west at 18. `Brightness` (0 to 20, 3 by default) is the sun light's intensity, and the moon's is a tenth of it; `Shadows` (true by default) makes that light cast shadows. `CloudCover` and `CloudDensity` (0 to 1, 0.5 each) shape the clouds, and thick cover dims the light; `WindDirection`, a `Vector3`, is their drift in studs per second across X and Z. `SunTexture` and `MoonTexture`, each a `Texture?`, replace the discs, which `SunSize` and `MoonSize` (degrees, 2 by default) size. `ReflectionQuality`, an `Enum.EffectQuality`, sets how sharp the sky's light and reflections are (`Medium` by default); the sky itself is always drawn at full resolution. When `Lighting` holds a Skybox and a DynamicSky, the first in the tree is the sky.
```

- [ ] **Step 6: Build and run the tests**

Run: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox engine-tests --parallel && build/Debug/sandbox.exe "[dynamic_sky],[skybox]" && build/Debug/engine-tests.exe`

Expected:
- the sandbox run passes DS1, DS2, DS4, and SKY1 to SKY4;
- engine-tests passes, including `testInsertFilter`.

- [ ] **Step 7: Commit**

```bash
git add src/engine_instances/DynamicSky.hpp src/engine_instances/DynamicSky.cpp sandbox/dynamic_sky_tests.cpp CMakeLists.txt src/engine_core/Containment.cpp src/engine_core/Project.cpp src/engine_core/ScriptBindings.cpp src/engine_core/LuaApi.cpp src/ide/ClassOrder.hpp src/ide/IdeIcons.cpp tests/LuauCompleteTest.cpp README.md src/engine_instances/README.md
git commit -m "Add DynamicSky, under Lighting, with TimeOfDay, Latitude, Brightness, Shadows, clouds, sun and moon, and ReflectionQuality"
```

---

### Task 3: Carry the first sky in the render snapshot

**Files:**
- Modify:
  - `src/engine_core/SnapshotPump.hpp`: the new struct after `VisualSky` (~line 102), the member after `VisualSnapshot::sky` (line 201), and `find_first_of` beside `find_first` (line 277)
  - `src/engine_core/SnapshotPump.cpp`: include, `find_first`, `resolve_lighting`, and `blit`
  - `src/runner/SceneFeed.cpp:54`
  - `tests/SceneFeedTest.cpp`
  - `sandbox/dynamic_sky_tests.cpp`: add DS3 and DS5

**Interfaces:**
- Consumes: `DynamicSky` (Task 2).
- Produces: `engine_core::VisualDynamicSky` with these fields:
  - `bool present`
  - `float time_of_day`, `latitude`, `brightness`
  - `bool shadows`
  - `float cloud_cover`, `cloud_density`
  - `Vec3 wind`
  - `std::string sun_texture`, `moon_texture`
  - `float sun_size`, `moon_size`
  - `int reflection_quality`

  `VisualSnapshot::dynamic_sky` holds it. `VisualSky::present` and `VisualDynamicSky::present` are never both true.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/dynamic_sky_tests.cpp`:

```cpp
TEST_CASE("DS3 the snapshot carries the first DynamicSky under Lighting", "[dynamic_sky][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    // With none, the class's defaults.
    {
        const engine_core::VisualDynamicSky& sky = pump.front().dynamic_sky;
        REQUIRE_FALSE(sky.present);
        REQUIRE(sky.time_of_day == static_cast<float>(DynamicSky::kDefaultTimeOfDay));
        REQUIRE(sky.latitude == static_cast<float>(DynamicSky::kDefaultLatitude));
        REQUIRE(sky.brightness == static_cast<float>(DynamicSky::kDefaultBrightness));
        REQUIRE(sky.shadows == DynamicSky::kDefaultShadows);
        REQUIRE(sky.cloud_cover == static_cast<float>(DynamicSky::kDefaultCloudCover));
        REQUIRE(sky.cloud_density == static_cast<float>(DynamicSky::kDefaultCloudDensity));
        REQUIRE(sky.wind.x == DynamicSky::kDefaultWindDirection.x);
        REQUIRE(sky.wind.z == DynamicSky::kDefaultWindDirection.z);
        REQUIRE(sky.sun_size == static_cast<float>(DynamicSky::kDefaultSunSize));
        REQUIRE(sky.moon_size == static_cast<float>(DynamicSky::kDefaultMoonSize));
        REQUIRE(sky.reflection_quality == static_cast<int>(DynamicSky::kDefaultReflectionQuality));
        REQUIRE(sky.sun_texture.empty());
    }

    const InstanceId lighting = game.scene_service("Lighting");
    engine_core::Texture& sun = add_texture(game, "Sun", "textures/sun.png");
    DynamicSky& sky = add_dynamic_sky(game, lighting);
    REQUIRE_FALSE(sky.set_time_of_day(18.5));
    REQUIRE_FALSE(sky.set_latitude(-20.0));
    REQUIRE_FALSE(sky.set_brightness(5.0));
    REQUIRE_FALSE(sky.set_shadows(false));
    REQUIRE_FALSE(sky.set_cloud_cover(0.25));
    REQUIRE_FALSE(sky.set_cloud_density(0.75));
    REQUIRE_FALSE(sky.set_wind_direction(engine_core::Vec3{3.f, 1.f, -2.f}));
    REQUIRE_FALSE(sky.set_sun_texture(instance_slot(sun.id())));
    REQUIRE_FALSE(sky.set_sun_size(4.0));
    REQUIRE_FALSE(sky.set_moon_size(6.0));
    REQUIRE_FALSE(sky.set_reflection_quality(0));
    frame();
    {
        const engine_core::VisualDynamicSky& seen = pump.front().dynamic_sky;
        REQUIRE(seen.present);
        REQUIRE_FALSE(pump.front().sky.present);
        REQUIRE(seen.time_of_day == 18.5f);
        REQUIRE(seen.latitude == -20.f);
        REQUIRE(seen.brightness == 5.f);
        REQUIRE_FALSE(seen.shadows);
        REQUIRE(seen.cloud_cover == 0.25f);
        REQUIRE(seen.cloud_density == 0.75f);
        REQUIRE(seen.wind.x == 3.f);
        REQUIRE(seen.wind.z == -2.f);
        REQUIRE(seen.sun_texture == "textures/sun.png");
        REQUIRE(seen.moon_texture.empty());
        REQUIRE(seen.sun_size == 4.f);
        REQUIRE(seen.moon_size == 6.f);
        REQUIRE(seen.reflection_quality == 0);
    }
    // A destroyed Texture reads as none; a destroyed sky as no sky.
    game.destroy(sun.id());
    frame();
    REQUIRE(pump.front().dynamic_sky.sun_texture.empty());
    game.destroy(sky.id());
    frame();
    REQUIRE_FALSE(pump.front().dynamic_sky.present);
    REQUIRE(pump.front().dynamic_sky.time_of_day == static_cast<float>(DynamicSky::kDefaultTimeOfDay));
}

TEST_CASE("DS5 the first Skybox or DynamicSky in the tree is the sky", "[dynamic_sky][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    const InstanceId lighting = game.scene_service("Lighting");
    DynamicSky& dynamic = add_dynamic_sky(game, lighting);
    engine_core::Skybox& image = game.create<engine_core::Skybox>();
    game.set_parent(image.id(), lighting);
    frame();
    REQUIRE(pump.front().dynamic_sky.present);
    REQUIRE_FALSE(pump.front().sky.present);

    // The DynamicSky into a Folder after the Skybox: the Skybox is first now.
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    game.set_parent(dynamic.id(), folder.id());
    frame();
    REQUIRE(pump.front().sky.present);
    REQUIRE_FALSE(pump.front().dynamic_sky.present);

    game.destroy(image.id());
    frame();
    REQUIRE(pump.front().dynamic_sky.present);
    REQUIRE_FALSE(pump.front().sky.present);
}
```

In `tests/SceneFeedTest.cpp`, inside `Frame()` after `snapshot.occlusion.radius = ...;`, add:

```cpp
    snapshot.dynamic_sky.present = true;
    snapshot.dynamic_sky.time_of_day = static_cast<float>(number);
    snapshot.dynamic_sky.sun_texture = "textures/" + std::to_string(number) + ".png";
```

In `Whole()`, after `snapshot.occlusion.present && snapshot.occlusion.radius == static_cast<float>(snapshot.frame) &&`, add:

```cpp
             snapshot.dynamic_sky.present && snapshot.dynamic_sky.time_of_day == static_cast<float>(snapshot.frame) &&
             snapshot.dynamic_sky.sun_texture == "textures/" + std::to_string(snapshot.frame) + ".png" &&
```

- [ ] **Step 2: Confirm the tests fail to build**

Run: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox engine-tests --parallel`

Expected: FAIL, because `VisualSnapshot` has no member `dynamic_sky`.

- [ ] **Step 3: Add VisualDynamicSky and find_first_of to SnapshotPump.hpp**

After `struct VisualSky {...};`, add:

```cpp
// The first DynamicSky under Lighting, in tree order, as the renderer reads
// it, when it comes before every Skybox; then VisualSky::present is false.
// present is false otherwise, and the values are DynamicSky's defaults.
struct VisualDynamicSky {
    bool present = false;
    float time_of_day = 14.f;
    float latitude = 35.f;
    float brightness = 3.f;
    bool shadows = true;
    float cloud_cover = 0.5f;
    float cloud_density = 0.5f;
    // Studs per second; Y is ignored.
    Vec3 wind{1.f, 0.f, 0.3f};
    // Texture Paths, relative to the resources folder. Empty for none.
    std::string sun_texture;
    std::string moon_texture;
    // Degrees across.
    float sun_size = 2.f;
    float moon_size = 2.f;
    // Enum.EffectQuality's value: Low 0, Medium 1, High 2.
    int reflection_quality = 1;
};
```

Change the comment above `struct VisualSky` to begin "The first Skybox under Lighting, in tree order, when it comes before every DynamicSky, as the renderer reads it." In `VisualSnapshot`, after `VisualSky sky;`, add `    VisualDynamicSky dynamic_sky;`.

Next to `find_first`'s declaration (line 277), add:

```cpp
    // The first instance under root, in tree order, that is any of T.
    template <class... T>
    const DataModel* find_first_of(const DataModel& game, InstanceId root);
```

- [ ] **Step 4: Fill it in SnapshotPump.cpp**

- Add `#include "DynamicSky.hpp"` after `#include "Dragger.hpp"`.
- Replace the body of `find_first` with `find_first_of` plus a one-line `find_first`:

```cpp
template <class... T>
const DataModel* SnapshotPump::find_first_of(const DataModel& game, InstanceId root) {
    // Children are pushed last first, so the first child comes off the walk first.
    lighting_walk_.clear();
    lighting_walk_.push_back(root);
    while (!lighting_walk_.empty()) {
        const InstanceId id = lighting_walk_.back();
        lighting_walk_.pop_back();
        if (id != root) {
            const DataModel* object = game.instance(id);
            if (object != nullptr && (... || (dynamic_cast<const T*>(object) != nullptr))) {
                return object;
            }
        }
        const std::size_t first = lighting_walk_.size();
        for (InstanceId child = game.first_child(id); child != 0; child = game.next_sibling(child)) {
            lighting_walk_.push_back(child);
        }
        std::reverse(lighting_walk_.begin() + static_cast<std::ptrdiff_t>(first), lighting_walk_.end());
    }
    return nullptr;
}

template <class T>
const T* SnapshotPump::find_first(const DataModel& game, InstanceId root) {
    return static_cast<const T*>(find_first_of<T>(game, root));
}
```

In `resolve_lighting`, replace

```cpp
    const Skybox* skybox = lighting != nullptr ? find_first<Skybox>(game, lighting->id()) : nullptr;
```

with

```cpp
    // The first Skybox or DynamicSky in tree order is the sky; the other kind is not drawn.
    const DataModel* first_sky =
        lighting != nullptr ? find_first_of<Skybox, DynamicSky>(game, lighting->id()) : nullptr;
    const auto* skybox = dynamic_cast<const Skybox*>(first_sky);
    const auto* dynamic = dynamic_cast<const DynamicSky*>(first_sky);
```

After the Skybox `if (skybox != nullptr) {...} else {...}` block, add:

```cpp
    VisualDynamicSky& procedural = base_.dynamic_sky;
    procedural.present = dynamic != nullptr;
    if (dynamic != nullptr) {
        procedural.time_of_day = static_cast<float>(dynamic->time_of_day());
        procedural.latitude = static_cast<float>(dynamic->latitude());
        procedural.brightness = static_cast<float>(dynamic->brightness());
        procedural.shadows = dynamic->shadows();
        procedural.cloud_cover = static_cast<float>(dynamic->cloud_cover());
        procedural.cloud_density = static_cast<float>(dynamic->cloud_density());
        procedural.wind = dynamic->wind_direction();
        texture_path(dynamic->sun_texture(), procedural.sun_texture);
        texture_path(dynamic->moon_texture(), procedural.moon_texture);
        procedural.sun_size = static_cast<float>(dynamic->sun_size());
        procedural.moon_size = static_cast<float>(dynamic->moon_size());
        procedural.reflection_quality = static_cast<int>(dynamic->reflection_quality());
    } else {
        // Field by field, so the strings keep their buffers.
        const VisualDynamicSky defaults;
        procedural.time_of_day = defaults.time_of_day;
        procedural.latitude = defaults.latitude;
        procedural.brightness = defaults.brightness;
        procedural.shadows = defaults.shadows;
        procedural.cloud_cover = defaults.cloud_cover;
        procedural.cloud_density = defaults.cloud_density;
        procedural.wind = defaults.wind;
        procedural.sun_texture.clear();
        procedural.moon_texture.clear();
        procedural.sun_size = defaults.sun_size;
        procedural.moon_size = defaults.moon_size;
        procedural.reflection_quality = defaults.reflection_quality;
    }
```

In `blit`, after `dst.sky = base_.sky;`, add `    dst.dynamic_sky = base_.dynamic_sky;`. Update the comment above `resolve_lighting` in the header (line 272) to list `base_.dynamic_sky` too.

In `src/runner/SceneFeed.cpp`, after `out->sky = front.sky;`, add `    out->dynamic_sky = front.dynamic_sky;`.

- [ ] **Step 5: Build and run**

Run: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox engine-tests --parallel && build/Debug/sandbox.exe "[dynamic_sky],[skybox],[bloom],[reflections]" && build/Debug/engine-tests.exe`

Expected: PASS. DS1 to DS5 pass, and SKY3 still passes because the walk is shared.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/SnapshotPump.hpp src/engine_core/SnapshotPump.cpp src/runner/SceneFeed.cpp tests/SceneFeedTest.cpp sandbox/dynamic_sky_tests.cpp
git commit -m "Carry the first Skybox or DynamicSky under Lighting in the render snapshot"
```

---

### Task 4: Draw the dynamic sky and its lighting cube

**Files:**
- Create: `resources/shaders/pipeline/procedural_sky.glsl`, `resources/shaders/pipeline/dynamic_sky.frag`, `resources/shaders/pipeline/dynamic_sky_cube.frag`
- Modify: `src/runner/EnvironmentMap.hpp`, `src/runner/EnvironmentMap.cpp`, `src/runner/Renderer.hpp`, `src/runner/Renderer.cpp`, `tests/SceneRenderCheck.cpp`

**Interfaces:**
- Consumes: `SkyMath` (Task 1).
- Produces:
  - `runner::SceneDynamicSky`, and `SceneLighting::dynamicSky`.
  - `void runner::SetSkyState(SceneDynamicSky& out, const SkyState& state)`, which copies the directions, star frame, visibility, light, and disc colours.
  - `LightDraw runner::SkyLightDraw(const SkyState& state, bool shadows)`, which gives the sky's directional light with `id` 0. It points the way the light shines.
  - `EnvironmentMap::updateProcedural(int environmentSize, int prefilteredSize, unsigned emptyVao, const std::function<bool(int face)>& drawFace)` and `bool EnvironmentMap::holdsProcedural() const`.

- [ ] **Step 1: Write the failing render check**

In `tests/SceneRenderCheck.cpp`, add `#include "SkyMath.hpp"` with the runner includes. Inside the Skybox block, just before the comment `// With no image, today's stand-in again, and the corner the clear color.`, add:

```cpp
            // A DynamicSky: drawn from its shader, and lighting what it surrounds.
            {
                const auto skyAt = [&](double hours, double latitude, int quality) {
                    runner::SceneLighting out;
                    out.ambient[0] = out.ambient[1] = out.ambient[2] = 0.f;
                    const runner::SkyState state = runner::ComputeSky(hours, latitude, 3.0, 0.0, 0.0);
                    runner::SceneDynamicSky& sky = out.dynamicSky;
                    sky.enabled = true;
                    runner::SetSkyState(sky, state);
                    sky.cloudCover = 0.f;
                    sky.cloudDensity = 0.f;
                    sky.reflectionQuality = static_cast<runner::SceneQuality>(quality);
                    sky.key = {static_cast<float>(hours), static_cast<float>(latitude), 0.f, 0.f, quality};
                    return std::make_pair(out, runner::SkyLightDraw(state, false));
                };
                const auto drawDynamic = [&](const std::pair<runner::SceneLighting, runner::LightDraw>& sky,
                                             const runner::MeshDraw* meshes, int count) {
                    renderer.setLighting(sky.first);
                    bool drawn = false;
                    for (int attempt = 0; attempt < 3 && !drawn; ++attempt) {
                        drawn = renderer.draw(0, 0, kSize, kSize, kSize, kSize, meshes, count, &sky.second, 1);
                    }
                    return drawn;
                };
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 0.f, 7.f}, {0.f, 0.f, 0.f}, up), 60.f);
                const auto noon = skyAt(12.0, 0.0, 1);
                Expect(drawDynamic(noon, nullptr, 0), "a DynamicSky with no meshes draws");
                const Pixel noonSky = ReadPixel(fbWidth / 2, fbHeight * 7 / 8);
                Expect(noonSky.b > noonSky.r && Sum(noonSky) > 60,
                       "the noon sky is bright and blue (" + Text(noonSky) + ")");
                const auto midnight = skyAt(0.0, 0.0, 1);
                drawDynamic(midnight, nullptr, 0);
                const Pixel midnightSky = ReadPixel(fbWidth / 2, fbHeight * 7 / 8);
                Expect(Sum(midnightSky) * 4 < Sum(noonSky),
                       "the midnight sky is dark (" + Text(midnightSky) + " against " + Text(noonSky) + ")");
                // Straight down is dark ground, not NaN (which tone maps to black or garbage).
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 5.f, 0.f}, {0.f, 0.f, 0.001f}, up), 60.f);
                drawDynamic(noon, nullptr, 0);
                const Pixel ground = ReadPixel(fbWidth / 2, fbHeight / 2);
                Expect(Sum(ground) > 3 && Sum(ground) < Sum(noonSky),
                       "straight down is dark ground, not NaN (" + Text(ground) + ")");

                // The sun lights a cube at noon far more than the moon at midnight.
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                                   runner::Renderer::kCameraFovYDegrees);
                runner::MeshDraw skyCube{cube, engine_core::matrix4_identity()};
                skyCube.roughness = 1.f;
                const int cubeTopY = fbHeight / 2 + fbHeight * 3 / 64;
                Expect(drawDynamic(noon, &skyCube, 1), "a cube under a DynamicSky draws");
                const Pixel noonTop = ReadPixel(fbWidth / 2, cubeTopY);
                drawDynamic(midnight, &skyCube, 1);
                const Pixel midnightTop = ReadPixel(fbWidth / 2, cubeTopY);
                Expect(Sum(noonTop) > 2 * Sum(midnightTop) + 10,
                       "noon lights the cube's top more than midnight (" + Text(noonTop) + " against " +
                           Text(midnightTop) + ")");
                // A new ReflectionQuality makes the cubes again, and draws at once.
                Expect(drawDynamic(skyAt(12.0, 0.0, 2), &skyCube, 1), "ReflectionQuality High draws");
                Expect(drawDynamic(skyAt(12.0, 0.0, 0), &skyCube, 1), "and Low");
                // At a pole the sun rides the horizon: no light, but the scene still draws.
                Expect(drawDynamic(skyAt(12.0, 90.0, 1), &skyCube, 1), "a polar sky draws");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the DynamicSky leaves no GL error");

                // Back to the image sky: its own cubes again, not the procedural ones.
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 0.f, 7.f}, {0.f, 0.f, 0.f}, up), 60.f);
                renderer.setLighting(lit);
                Expect(drawSky(nullptr, 0), "the image sky draws again after a DynamicSky");
                const Pixel back = ReadPixel(fbWidth / 4, fbHeight * 3 / 4);
                Expect(back.r > 150 && back.g < 30 && back.b < 30,
                       "the image sky comes back (" + Text(back) + ")");
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                                   runner::Renderer::kCameraFovYDegrees);
            }
```

(`lit`, `drawSky`, `cube`, and `up` are declared earlier in the same block. The inner names (`skyCube`, `cubeTopY`, `ground`, …) are chosen not to shadow the outer `matte` and `topY`, since /W4 warns on shadowing.)

- [ ] **Step 2: Confirm it fails to build**

Run: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target scene-render-check --parallel`

Expected: FAIL, because `SceneDynamicSky`, `SetSkyState`, and `SkyLightDraw` are undeclared.

- [ ] **Step 3: EnvironmentMap: sizes chosen at run time, and a procedural way in**

In `EnvironmentMap.hpp`:
- add `#include <functional>`;
- change the class comment's second paragraph to say a DynamicSky draws the environment cube itself through `updateProcedural`;
- replace the `ensureTextures` and `drawEnvironment` declarations with the ones below, and add the new members after `bool refused_ = false;`.

```cpp
    // An image's cube sizes. A procedural sky picks its own (updateProcedural).
    static constexpr int kEnvironmentSize = 512;
    static constexpr int kIrradianceSize = 32;
    // Mip 0 is roughness 0 and the last mip roughness 1.
    static constexpr int kPrefilteredSize = 256;
    static constexpr int kPrefilteredLevels = 6;
    static constexpr int kBrdfSize = 256;
```

(The constants keep their values; only the comment above `kEnvironmentSize` changes.)

Public, after `update`:

```cpp
    // Makes the cubes from a sky the caller draws: drawFace(face) draws face 0
    // to 5 (OpenGL's order) of the environment cube, environmentSize texels
    // wide, into the bound framebuffer with the viewport set, binding its own
    // program; false when it cannot draw yet. The cubes are made again at
    // these sizes when they differ. Always draws, so the caller decides when.
    // True when the cubes are ready; false as update.
    bool updateProcedural(int environmentSize, int prefilteredSize, unsigned emptyVao,
                          const std::function<bool(int face)>& drawFace);
    // Whether the cubes hold what updateProcedural last drew.
    bool holdsProcedural() const { return procedural_; }
```

Private: replace `bool ensureTextures();` and `bool drawEnvironment(unsigned image, unsigned emptyVao);` with:

```cpp
    // Makes the cubes at these sizes, and the lookup table and framebuffer the
    // first time. False when the driver will not render into them.
    bool ensureTextures(int environmentSize, int prefilteredSize);
    // The lookup table, once. False when it cannot draw yet.
    bool ensureBrdf(unsigned emptyVao);
    // The image into environment_, with its mips. False as drawFaces.
    bool drawEnvironment(unsigned image, unsigned emptyVao);
    // irradiance_ and prefiltered_ from environment_. False as drawFaces.
    bool filter(unsigned emptyVao);
```

Members:

```cpp
    // The cubes' sizes, 0 before they are made.
    int environmentSize_ = 0;
    int prefilteredSize_ = 0;
    // Whether the cubes hold a procedural sky rather than an image.
    bool procedural_ = false;
```

In `EnvironmentMap.cpp`, rewrite `ensureTextures`:

```cpp
bool EnvironmentMap::ensureTextures(int environmentSize, int prefilteredSize) {
    if (framebuffer_ != 0 && environmentSize == environmentSize_ && prefilteredSize == prefilteredSize_) {
        return true;
    }
    if (refused_) {
        return false;
    }
    // Made again at the new sizes: whatever they held is gone.
    for (unsigned* texture : {&environment_, &irradiance_, &prefiltered_}) {
        DeleteTexture(*texture);
    }
    imageRevision_ = 0;
    procedural_ = false;
    environmentSize_ = 0;
    prefilteredSize_ = 0;
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    environment_ = MakeCube(environmentSize, true);
    irradiance_ = MakeCube(kIrradianceSize, false);
    prefiltered_ = MakeCube(prefilteredSize, true);
    // Mips past the last roughness are never drawn or read.
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, prefiltered_);
    glTexParameteri(RT_GL_TEXTURE_CUBE_MAP, RT_GL_TEXTURE_MAX_LEVEL, kPrefilteredLevels - 1);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);

    if (brdf_ == 0) {
        glGenTextures(1, &brdf_);
        glBindTexture(GL_TEXTURE_2D, brdf_);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(RT_GL_RGBA16F), kBrdfSize, kBrdfSize, 0, GL_RGBA,
                     RT_GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
        glBindTexture(GL_TEXTURE_2D, 0);
        brdfDrawn_ = false;
    }
    if (framebuffer_ == 0) {
        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
        const GLenum buffer = RT_GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &buffer);
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
    bool complete = true;
    for (const unsigned cube : {environment_, irradiance_, prefiltered_}) {
        glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0, RT_GL_TEXTURE_CUBE_MAP_POSITIVE_X, cube, 0);
        complete = complete && glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE;
    }
    glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, brdf_, 0);
    complete = complete && glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE;
    if (!complete) {
        std::fprintf(stderr, "The sky's environment cubes are not supported; the sky is not drawn.\n");
        // Keeps the programs, but no textures: a later call does not try again.
        for (unsigned* texture : {&environment_, &irradiance_, &prefiltered_, &brdf_}) {
            DeleteTexture(*texture);
        }
        glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
        brdfDrawn_ = false;
        refused_ = true;
        return false;
    }
    environmentSize_ = environmentSize;
    prefilteredSize_ = prefilteredSize;
    return true;
}
```

In `drawEnvironment`, replace `kEnvironmentSize` with `environmentSize_` in both places, the uniform and the `drawFaces` size.

Replace `update` with the shared pieces plus the two entry points:

```cpp
bool EnvironmentMap::ensureBrdf(unsigned emptyVao) {
    if (brdfDrawn_) {
        return true;
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, brdf_, 0);
    glViewport(0, 0, kBrdfSize, kBrdfSize);
    glUseProgram(brdfProgram_.id);
    glBindVertexArray(emptyVao);
    if (!CanDraw(brdfProgram_.id)) {
        return false;
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    brdfDrawn_ = true;
    return true;
}

bool EnvironmentMap::filter(unsigned emptyVao) {
    // The diffuse light.
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, environment_);
    glUseProgram(irradianceProgram_.id);
    glUniform1f(irradianceProgram_.environmentSize, static_cast<float>(environmentSize_));
    if (!drawFaces(irradianceProgram_, irradiance_, 0, kIrradianceSize, emptyVao)) {
        return false;
    }
    // The reflections, from the same environment cube.
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, environment_);
    glUseProgram(prefilter_.id);
    glUniform1f(prefilter_.environmentSize, static_cast<float>(environmentSize_));
    for (int level = 0; level < kPrefilteredLevels; ++level) {
        glUseProgram(prefilter_.id);
        glUniform1f(prefilter_.roughness, static_cast<float>(level) / static_cast<float>(kPrefilteredLevels - 1));
        if (!drawFaces(prefilter_, prefiltered_, level, std::max(prefilteredSize_ >> level, 1), emptyVao)) {
            return false;
        }
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

namespace {

void BeginDrawing() {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(RT_GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    // Filtering reads across the edges of faces, so the cubes have no seams.
    glEnable(RT_GL_TEXTURE_CUBE_MAP_SEAMLESS);
}

}  // namespace

bool EnvironmentMap::update(unsigned image, std::uint64_t imageRevision, unsigned emptyVao) {
    if (image == 0 || equirect_.id == 0) {
        return false;
    }
    if (brdfDrawn_ && !procedural_ && imageRevision == imageRevision_ && environmentSize_ == kEnvironmentSize &&
        prefilteredSize_ == kPrefilteredSize) {
        return true;
    }
    if (!ensureTextures(kEnvironmentSize, kPrefilteredSize)) {
        return false;
    }
    BeginDrawing();
    // Whatever was made before is not the sky now, whether or not this finishes.
    imageRevision_ = 0;
    procedural_ = false;
    if (!ensureBrdf(emptyVao) || !drawEnvironment(image, emptyVao) || !filter(emptyVao)) {
        return false;
    }
    static_assert((kPrefilteredSize >> (kPrefilteredLevels - 1)) >= 1, "the prefiltered cube's mips run out at 1 by 1");
    imageRevision_ = imageRevision;
    return true;
}

bool EnvironmentMap::updateProcedural(int environmentSize, int prefilteredSize, unsigned emptyVao,
                                      const std::function<bool(int face)>& drawFace) {
    if (equirect_.id == 0 || (prefilteredSize >> (kPrefilteredLevels - 1)) < 1 ||
        !ensureTextures(environmentSize, prefilteredSize)) {
        return false;
    }
    BeginDrawing();
    imageRevision_ = 0;
    procedural_ = false;
    if (!ensureBrdf(emptyVao)) {
        return false;
    }
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    // Not bound while it is drawn into.
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, environmentSize_, environmentSize_);
    glBindVertexArray(emptyVao);
    for (int face = 0; face < 6; ++face) {
        glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0,
                               RT_GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), environment_, 0);
        if (!drawFace(face)) {
            return false;
        }
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, environment_);
    glGenerateMipmap(RT_GL_TEXTURE_CUBE_MAP);
    if (!filter(emptyVao)) {
        return false;
    }
    procedural_ = true;
    return true;
}
```

In `shutdown`, after `imageRevision_ = 0;`, add `environmentSize_ = 0; prefilteredSize_ = 0; procedural_ = false;`.

- [ ] **Step 4: Write the shaders**

Create `resources/shaders/pipeline/procedural_sky.glsl`:

```glsl
// The DynamicSky toward any world direction: a single-scattering atmosphere
// (Nishita 1993; the sun's glow after https://www.shadertoy.com/view/3djSzz)
// lit by the sun and, at night, the moon; clouds on a layer overhead (after
// https://www.shadertoy.com/view/wslyWs); stars; and the sun and moon. No
// #version: Renderer puts it in after the main file's, after environment.glsl
// (kPi, kMaxHalf, basis). SkyMath.hpp holds the CPU side of the same numbers.

// Toward each body, world space, unit length.
uniform vec3 uSunDirection;
uniform vec3 uMoonDirection;
// World directions into the stars' frame, which turns with the hour.
uniform mat3 uStarFrame;
// 0 by day, 1 at night.
uniform float uStarVisibility;
// The sky's light: toward it, and its color times its intensity, linear.
uniform vec3 uBodyLightDirection;
uniform vec3 uBodyLightColor;
// Each disc's radiance, linear, reddened by the air in front of it.
uniform vec3 uSunColor;
uniform vec3 uMoonColor;
uniform float uCloudCover;
uniform float uCloudDensity;
// How far the clouds have drifted, in studs across X and Z.
uniform vec2 uCloudOffset;
// The tangent of each disc's half angle.
uniform float uSunSize;
uniform float uMoonSize;
uniform float uSunTextureEnabled;
uniform float uMoonTextureEnabled;
uniform sampler2D uSunTexture;
uniform sampler2D uMoonTexture;

const float kPlanetRadius = 6371e3;
const float kAtmosphereRadius = 6471e3;
// Per meter at sea level, and the scale heights; SkyMath.hpp has the same.
const vec3 kRayleigh = vec3(5.5e-6, 13.0e-6, 22.4e-6);
const float kRayleighHeight = 8e3;
const float kMie = 21e-6;
const float kMieHeight = 1.2e3;
const float kMieG = 0.758;
const float kSunRadiance = 22.0;
// The moon lights the sky this much as the sun does: far more than nature, so a night is not black.
const float kMoonSkyRadiance = kSunRadiance * 0.03;
// A dark blue floor under everything at night.
const vec3 kNightSky = vec3(0.002, 0.003, 0.006);
const int kViewSteps = 12;
const int kLightSteps = 4;
// The cloud layer's height and a cloud's size, in studs.
const float kCloudHeight = 200.0;
const float kCloudFeature = 120.0;
const float kStarCells = 150.0;

// Where a ray from origin along unit dir enters (x) and leaves (y) a sphere
// about the planet's center; x > y when it misses.
vec2 sphereHits(vec3 origin, vec3 dir, float radius) {
    float b = dot(dir, origin);
    float c = dot(origin, origin) - radius * radius;
    float d = b * b - c;
    if (d < 0.0) {
        return vec2(1e9, -1e9);
    }
    d = sqrt(d);
    return vec2(-b - d, -b + d);
}

// The light the air scatters toward the eye along dir, from a body toward light.
vec3 atmosphere(vec3 dir, vec3 light, float radiance) {
    vec3 origin = vec3(0.0, kPlanetRadius + 2.0, 0.0);
    float span = sphereHits(origin, dir, kAtmosphereRadius).y;
    vec2 ground = sphereHits(origin, dir, kPlanetRadius);
    if (ground.x > 0.0 && ground.x < ground.y) {
        span = min(span, ground.x);
    }
    float stepLength = span / float(kViewSteps);
    float mu = dot(dir, light);
    float phaseRayleigh = 3.0 / (16.0 * kPi) * (1.0 + mu * mu);
    float g2 = kMieG * kMieG;
    float phaseMie = 3.0 / (8.0 * kPi) * ((1.0 - g2) * (1.0 + mu * mu)) /
                     ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * kMieG * mu, 1e-4), 1.5));
    vec3 rayleigh = vec3(0.0);
    vec3 mie = vec3(0.0);
    float depthRayleigh = 0.0;
    float depthMie = 0.0;
    for (int i = 0; i < kViewSteps; ++i) {
        vec3 at = origin + dir * (stepLength * (float(i) + 0.5));
        float height = length(at) - kPlanetRadius;
        float densityRayleigh = exp(-height / kRayleighHeight) * stepLength;
        float densityMie = exp(-height / kMieHeight) * stepLength;
        depthRayleigh += densityRayleigh;
        depthMie += densityMie;
        // In the planet's shadow, this point sees no light.
        vec2 blocked = sphereHits(at, light, kPlanetRadius);
        if (blocked.x > 0.0 && blocked.x < blocked.y) {
            continue;
        }
        float lightStep = sphereHits(at, light, kAtmosphereRadius).y / float(kLightSteps);
        float lightRayleigh = 0.0;
        float lightMie = 0.0;
        for (int j = 0; j < kLightSteps; ++j) {
            vec3 lit = at + light * (lightStep * (float(j) + 0.5));
            float litHeight = length(lit) - kPlanetRadius;
            lightRayleigh += exp(-litHeight / kRayleighHeight) * lightStep;
            lightMie += exp(-litHeight / kMieHeight) * lightStep;
        }
        vec3 attenuation =
            exp(-(kRayleigh * (depthRayleigh + lightRayleigh) + kMie * 1.1 * (depthMie + lightMie)));
        rayleigh += densityRayleigh * attenuation;
        mie += densityMie * attenuation;
    }
    return radiance * (phaseRayleigh * kRayleigh * rayleigh + phaseMie * kMie * mie);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float hash13(vec3 p) {
    vec3 p3 = fract(p * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
               mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

// 0 to 1, five octaves, each turned so their grids do not line up.
float fbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    mat2 turn = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 5; ++i) {
        sum += amplitude * valueNoise(p);
        p = turn * p;
        amplitude *= 0.5;
    }
    return sum / 0.96875;
}

// The clouds toward dir: their light in rgb, and how much of what is behind them they hide in a.
vec4 clouds(vec3 dir, vec3 skyAround) {
    if (dir.y < 0.01 || uCloudCover <= 0.0) {
        return vec4(0.0);
    }
    vec2 at = (dir.xz / dir.y * kCloudHeight + uCloudOffset) / kCloudFeature;
    float shape = fbm(at);
    float edge = 1.0 - uCloudCover;
    float coverage = smoothstep(edge, edge + 0.3, shape);
    if (coverage <= 0.0) {
        return vec4(0.0);
    }
    float opacity = coverage * mix(0.4, 1.0, uCloudDensity) * smoothstep(0.01, 0.12, dir.y);
    // Thicker toward the light is darker: the shape a little way toward it, against here.
    vec2 toward = uBodyLightDirection.xz;
    float lean = length(toward);
    toward = lean > 1e-4 ? toward / lean : vec2(0.0);
    float ahead = fbm(at + toward * 0.2);
    float shade = clamp(1.0 - (ahead - shape) * 4.0 * mix(0.5, 1.5, uCloudDensity), 0.2, 1.0);
    // Lit as a white matte surface, plus the sky around it; denser is grayer.
    vec3 light = uBodyLightColor / kPi * shade * mix(1.0, 0.55, uCloudDensity * coverage) + skyAround * 0.6;
    return vec4(light, opacity);
}

vec3 stars(vec3 dir) {
    vec3 s = uStarFrame * dir;
    vec3 cell = floor(s * kStarCells);
    float pick = hash13(cell);
    if (pick < 0.996) {
        return vec3(0.0);
    }
    vec3 jitter = vec3(hash13(cell + 1.7), hash13(cell + 3.1), hash13(cell + 5.9)) - 0.5;
    vec3 center = normalize((cell + 0.5 + jitter * 0.5) / kStarCells);
    float away = length(center - s) * kStarCells;
    float point = 1.0 - smoothstep(0.0, 0.35, away);
    float strength = (pick - 0.996) / 0.004;
    vec3 tint = mix(vec3(1.0, 0.85, 0.7), vec3(0.75, 0.85, 1.0), hash13(cell + 9.2));
    return tint * point * mix(0.3, 3.0, strength * strength);
}

// A sun or moon of half-angle tangent size toward toward, seen along dir: its
// shading in rgb and how much of dir it covers in a. With a texture, the
// texture's color (made linear) and alpha; without, a disc darkening toward
// its edge, mottled for the moon.
vec4 body(vec3 dir, vec3 toward, float size, float textured, sampler2D image, float mottled) {
    float c = dot(dir, toward);
    vec3 T;
    vec3 B;
    basis(toward, T, B);
    vec2 plane = vec2(dot(dir, T), dot(dir, B)) / (max(c, 1e-4) * size);
    float r = length(plane);
    float soft = max(fwidth(r), 1e-3);
    if (c <= 0.0) {
        return vec4(0.0);
    }
    if (textured > 0.5) {
        vec2 uv = plane * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            return vec4(0.0);
        }
        vec4 texel = textureLod(image, uv, 0.0);
        return vec4(pow(texel.rgb, vec3(2.2)), texel.a);
    }
    float cover = 1.0 - smoothstep(1.0 - soft, 1.0 + soft, r);
    float limb = sqrt(max(1.0 - r * r, 0.0));
    float shade = mix(0.4, 1.0, limb) * mix(1.0, 0.75 + 0.25 * valueNoise(plane * 4.0 + 7.0), mottled);
    return vec4(vec3(shade), cover);
}

// The sky's linear radiance toward dir (unit length, world space). detail
// adds the stars and the sun and moon discs, which the lighting cube leaves
// out: the sky's light already gives the sun's highlight.
vec3 proceduralSky(vec3 dir, bool detail) {
    // At and below the horizon, the horizon; the ground darkens it below.
    vec3 up = normalize(vec3(dir.x, max(dir.y, 0.0) + 1e-4, dir.z));
    vec3 air = atmosphere(up, uSunDirection, kSunRadiance);
    if (uStarVisibility > 0.0) {
        air += atmosphere(up, uMoonDirection, kMoonSkyRadiance);
    }
    air += kNightSky;
    vec3 color = air + uMoonColor * 0.02 * pow(max(dot(up, uMoonDirection), 0.0), 600.0);
    if (detail) {
        color += stars(up) * uStarVisibility;
        vec4 sun = body(up, uSunDirection, uSunSize, uSunTextureEnabled, uSunTexture, 0.0);
        color += sun.rgb * uSunColor * sun.a;
        vec4 moon = body(up, uMoonDirection, uMoonSize, uMoonTextureEnabled, uMoonTexture, 1.0);
        color = mix(color, air + moon.rgb * uMoonColor, moon.a);
    }
    vec4 cloud = clouds(up, air);
    color = mix(color, cloud.rgb, cloud.a);
    float ground = 1.0 - smoothstep(-0.25, 0.0, dir.y);
    color *= mix(1.0, 0.25, ground);
    return min(color, vec3(kMaxHalf));
}
```

Create `resources/shaders/pipeline/dynamic_sky.frag`:

```glsl
#version 330 core
// The DynamicSky behind everything, where no opaque surface was drawn, added
// into the accumulation buffer so it is tone mapped with the rest. Renderer
// puts lighting.glsl, environment.glsl, and procedural_sky.glsl in after the
// #version line.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uDepth;
// From view space to the world's: a DynamicSky does not turn.
uniform mat3 uViewToSky;

void main() {
    if (texture(uDepth, vUv).r < 1.0) {
        discard;
    }
    vec3 direction = normalize(uViewToSky * viewPositionAt(vUv, 1.0));
    outColor = vec4(proceduralSky(direction, true), 1.0);
}
```

Create `resources/shaders/pipeline/dynamic_sky_cube.frag`:

```glsl
#version 330 core
// One face of the DynamicSky's environment cube, which EnvironmentMap filters
// into the sky's light and reflections: no stars and no sun or moon disc.
// Renderer puts environment.glsl and procedural_sky.glsl in after the
// #version line.
in vec2 vUv;
out vec4 outColor;

uniform int uFace;

void main() {
    outColor = vec4(proceduralSky(normalize(cubeDirection(uFace, vUv)), false), 1.0);
}
```

- [ ] **Step 5: Renderer.hpp**

Add `#include "SkyMath.hpp"` after `#include "ShadowRenderer.hpp"`. After `struct SceneOcclusion {...};`, add:

```cpp
// The DynamicSky, as the renderer reads it, with SkyMath's sun, moon, and
// stars already worked out (SetSkyState). The defaults draw none.
struct SceneDynamicSky {
    bool enabled = false;
    // Toward each body, world space.
    float sunDirection[3] = {0.f, 1.f, 0.f};
    float moonDirection[3] = {0.f, -1.f, 0.f};
    // Column-major, world into the stars' frame.
    float starFrame[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    float starVisibility = 0.f;
    // The sky's light, toward it, and its color times its intensity, linear;
    // the clouds are lit by it.
    float lightDirection[3] = {0.f, 1.f, 0.f};
    float lightColor[3] = {0.f, 0.f, 0.f};
    // Each disc's linear radiance.
    float sunColor[3] = {0.f, 0.f, 0.f};
    float moonColor[3] = {0.f, 0.f, 0.f};
    float cloudCover = 0.5f;
    float cloudDensity = 0.5f;
    // Studs drifted across X and Z.
    float cloudOffset[2] = {0.f, 0.f};
    // Degrees across.
    float sunSizeDegrees = 2.f;
    float moonSizeDegrees = 2.f;
    // GL textures as TextureCache::get uploads them (sRGB), 0 for the discs.
    unsigned sunTexture = 0;
    unsigned moonTexture = 0;
    SceneQuality reflectionQuality = SceneQuality::Medium;
    // Whether the clouds drift, and the clock they drift by, in seconds.
    bool windy = false;
    double seconds = 0.0;
    // What the lighting cube is drawn from (LightingDue).
    SkyLightingKey key;
};

// Copies state's directions, star frame and visibility, light, and disc colors into out.
void SetSkyState(SceneDynamicSky& out, const SkyState& state);
```

The `LightDraw` struct is declared above `SceneSky`. After `LightDraw`, add:

```cpp
// The DynamicSky's light from state: directional, id 0, shining the way
// opposite state.light.toward, with kSkyShadowDistance.
LightDraw SkyLightDraw(const SkyState& state, bool shadows);
```

`SkyState` comes from `SkyMath.hpp`, which is included before `LightDraw`.

In `SceneLighting`, after `SceneSky sky;`, add `    SceneDynamicSky dynamicSky;`.

In `Program`, after `int occlusionIntensity = -1;`, add:

```cpp
        // DynamicSky (procedural_sky.glsl).
        int face = -1;
        int sunDirection = -1;
        int moonDirection = -1;
        int starFrame = -1;
        int starVisibility = -1;
        int bodyLightDirection = -1;
        int bodyLightColor = -1;
        int sunColor = -1;
        int moonColor = -1;
        int cloudCover = -1;
        int cloudDensity = -1;
        int cloudOffset = -1;
        int sunSize = -1;
        int moonSize = -1;
        int sunTextureEnabled = -1;
        int moonTextureEnabled = -1;
```

Private methods: after `void bindSky(const Program& program);`, add:

```cpp
    // The DynamicSky's uniforms and its two textures.
    void bindDynamicSky(const Program& program);
    // Draws the DynamicSky's lighting cube again when LightingDue says so.
    // True when the cubes hold the DynamicSky (if a little stale); false
    // before they ever have, and the sky is not drawn.
    bool updateDynamicSkyLighting();
    // Whether this draw's sky is the DynamicSky.
    bool dynamicSkyDrawn() const { return lighting_.dynamicSky.enabled && dynamicSkyBuilt_; }
```

Members: after `Program sky_;`, add:

```cpp
    // The DynamicSky's, built apart from the others: when they do not build,
    // only the DynamicSky goes undrawn.
    Program dynamicSky_;
    Program dynamicSkyCube_;
    bool dynamicSkyBuilt_ = false;
    // What the lighting cube was last drawn from, and when.
    SkyLightingKey skyLightingMade_;
    double skyLightingMadeAt_ = 0.0;
    bool skyLightingValid_ = false;
```

After `float skyColor_[3] = {1.f, 1.f, 1.f};`, add:

```cpp
    // The sky's LightScale and the 2D image bound for it: a DynamicSky's are 1 and white.
    float skyLightScale_ = 1.f;
    unsigned skyImage_ = 0;
```

- [ ] **Step 6: Renderer.cpp**

`buildProgram`: after `program.occlusionIntensity = at("uOcclusionIntensity");`, add:

```cpp
    program.face = at("uFace");
    program.sunDirection = at("uSunDirection");
    program.moonDirection = at("uMoonDirection");
    program.starFrame = at("uStarFrame");
    program.starVisibility = at("uStarVisibility");
    program.bodyLightDirection = at("uBodyLightDirection");
    program.bodyLightColor = at("uBodyLightColor");
    program.sunColor = at("uSunColor");
    program.moonColor = at("uMoonColor");
    program.cloudCover = at("uCloudCover");
    program.cloudDensity = at("uCloudDensity");
    program.cloudOffset = at("uCloudOffset");
    program.sunSize = at("uSunSize");
    program.moonSize = at("uMoonSize");
    program.sunTextureEnabled = at("uSunTextureEnabled");
    program.moonTextureEnabled = at("uMoonTextureEnabled");
```

After `sampler("uOcclusionSource", kUnitScene);`, add:

```cpp
    // The sky pass reads no Material, so the DynamicSky's textures take its units.
    sampler("uSunTexture", kUnitDiffuse);
    sampler("uMoonTexture", kUnitNormalMap);
```

`initialize`: after the `if (!built) { shutdown(); return false; }` block, add:

```cpp
    dynamicSkyBuilt_ =
        buildProgram(dynamicSky_, "Dynamic sky", "pipeline/fullscreen.vert", "pipeline/dynamic_sky.frag",
                     {"pipeline/lighting.glsl", "pipeline/environment.glsl", "pipeline/procedural_sky.glsl"}) &&
        buildProgram(dynamicSkyCube_, "Dynamic sky cube", "pipeline/fullscreen.vert",
                     "pipeline/dynamic_sky_cube.frag", {"pipeline/environment.glsl", "pipeline/procedural_sky.glsl"});
    if (!dynamicSkyBuilt_) {
        std::fprintf(stderr, "The DynamicSky's shaders did not build; a DynamicSky draws no sky.\n");
    }
```

`shutdown`: add `&dynamicSky_, &dynamicSkyCube_` to the list of programs deleted. After `skyReady_ = false;`, add `dynamicSkyBuilt_ = false; skyLightingValid_ = false;`.

Free functions, in `namespace runner` after the anonymous namespace that holds `DrawFullscreen`:

```cpp
void SetSkyState(SceneDynamicSky& out, const SkyState& state) {
    const auto copy = [](SkyVector v, float* to) {
        to[0] = v.x;
        to[1] = v.y;
        to[2] = v.z;
    };
    copy(state.sun, out.sunDirection);
    copy(state.moon, out.moonDirection);
    std::copy(state.starFrame, state.starFrame + 9, out.starFrame);
    out.starVisibility = state.starVisibility;
    copy(state.light.toward, out.lightDirection);
    for (int channel = 0; channel < 3; ++channel) {
        out.lightColor[channel] = state.light.color[channel] * state.light.intensity;
        out.sunColor[channel] = state.sunColor[channel];
        out.moonColor[channel] = state.moonColor[channel];
    }
}

LightDraw SkyLightDraw(const SkyState& state, bool shadows) {
    LightDraw light;
    light.kind = LightDraw::Kind::Directional;
    // Toward the body, so it shines the other way.
    light.direction[0] = -state.light.toward.x;
    light.direction[1] = -state.light.toward.y;
    light.direction[2] = -state.light.toward.z;
    std::copy(state.light.color, state.light.color + 3, light.color);
    light.intensity = state.light.intensity;
    light.id = 0;
    light.shadows = shadows;
    light.shadowDistance = kSkyShadowDistance;
    return light;
}
```

`draw`: replace

```cpp
    const bool hasSky = lighting_.sky.image != 0 && environment_.available();
```

with

```cpp
    const bool hasSky = environment_.available() && (dynamicSkyDrawn() || lighting_.sky.image != 0);
```

Replace the cube block:

```cpp
        // The sky's cubes: an image's made again only when it changes; a
        // DynamicSky's when LightingDue says, and never failing the frame.
        skyReady_ = false;
        bool cubesReady = true;
        if (hasSky && dynamicSkyDrawn()) {
            skyReady_ = updateDynamicSkyLighting();
            prepareSky();
        } else if (hasSky) {
            const SceneSky& sky = lighting_.sky;
            cubesReady = environment_.update(sky.image, sky.imageRevision, emptyVao_);
            skyReady_ = cubesReady;
            prepareSky();
        }
```

`prepareSky`: make rotation, color, scale, and image depend on the sky kind:

```cpp
    const bool dynamic = dynamicSkyDrawn();
    const float angle = dynamic ? 0.f : -lighting_.sky.rotationDegrees * 0.01745329252f;
```

(This replaces the existing `const float angle = ...` line.) At the end, replace the colour loop with:

```cpp
    // Tint is a color as picked, sRGB, made linear as surface.glsl makes a Material's.
    const float exposure = std::max(lighting_.sky.exposure, 0.f);
    for (int channel = 0; channel < 3; ++channel) {
        skyColor_[channel] =
            dynamic ? 1.f : exposure * std::pow(std::max(lighting_.sky.tint[channel], 0.f), 2.2f);
    }
    skyLightScale_ = dynamic ? 1.f : std::max(lighting_.sky.lightScale, 0.f);
    skyImage_ = dynamic ? whiteTexture_ : lighting_.sky.image;
```

`bindSky`: replace `std::max(lighting_.sky.lightScale, 0.f)` with `skyLightScale_`, and `BindTexture(kUnitSky, lighting_.sky.image);` with `BindTexture(kUnitSky, skyImage_);`.

Add after `bindSky`:

```cpp
void Renderer::bindDynamicSky(const Program& program) {
    const SceneDynamicSky& sky = lighting_.dynamicSky;
    constexpr float kHalfDegree = 0.5f * 0.01745329252f;
    glUniform3fv(program.sunDirection, 1, sky.sunDirection);
    glUniform3fv(program.moonDirection, 1, sky.moonDirection);
    glUniformMatrix3fv(program.starFrame, 1, GL_FALSE, sky.starFrame);
    glUniform1f(program.starVisibility, sky.starVisibility);
    glUniform3fv(program.bodyLightDirection, 1, sky.lightDirection);
    glUniform3fv(program.bodyLightColor, 1, sky.lightColor);
    glUniform3fv(program.sunColor, 1, sky.sunColor);
    glUniform3fv(program.moonColor, 1, sky.moonColor);
    glUniform1f(program.cloudCover, std::clamp(sky.cloudCover, 0.f, 1.f));
    glUniform1f(program.cloudDensity, std::clamp(sky.cloudDensity, 0.f, 1.f));
    glUniform2f(program.cloudOffset, sky.cloudOffset[0], sky.cloudOffset[1]);
    glUniform1f(program.sunSize, std::tan(std::clamp(sky.sunSizeDegrees, 0.1f, 20.f) * kHalfDegree));
    glUniform1f(program.moonSize, std::tan(std::clamp(sky.moonSizeDegrees, 0.1f, 20.f) * kHalfDegree));
    glUniform1f(program.sunTextureEnabled, sky.sunTexture != 0 ? 1.f : 0.f);
    glUniform1f(program.moonTextureEnabled, sky.moonTexture != 0 ? 1.f : 0.f);
    BindTexture(kUnitDiffuse, sky.sunTexture != 0 ? sky.sunTexture : whiteTexture_);
    BindTexture(kUnitNormalMap, sky.moonTexture != 0 ? sky.moonTexture : whiteTexture_);
}

bool Renderer::updateDynamicSkyLighting() {
    const SceneDynamicSky& sky = lighting_.dynamicSky;
    const bool made = skyLightingValid_ && environment_.holdsProcedural();
    if (!LightingDue(skyLightingMade_, sky.key, skyLightingMadeAt_, sky.seconds, sky.windy, made)) {
        return made;
    }
    RENDER_PASS("Sky lighting");
    const EnvironmentSizes sizes = EnvironmentSizesFor(sky.key.quality);
    const Program& program = dynamicSkyCube_;
    const bool drawn =
        environment_.updateProcedural(sizes.environment, sizes.prefiltered, emptyVao_, [&](int face) {
            if (face == 0) {
                glUseProgram(program.id);
                bindDynamicSky(program);
                glBindVertexArray(emptyVao_);
                if (!CanDraw(program.id)) {
                    return false;
                }
            }
            glUniform1i(program.face, face);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            return true;
        });
    if (drawn) {
        skyLightingMade_ = sky.key;
        skyLightingMadeAt_ = sky.seconds;
        skyLightingValid_ = true;
    }
    return environment_.holdsProcedural();
}
```

`skyPass`: draw the DynamicSky's program when it is the sky:

```cpp
bool Renderer::skyPass(const float* inverseProjection) {
    RENDER_PASS("Sky");
    if (!skyReady_) {
        return true;
    }
    const bool dynamic = dynamicSkyDrawn();
    const Program& program = dynamic ? dynamicSky_ : sky_;
    // Only where no opaque surface is, which no light pass wrote: no blending needed.
    glBindFramebuffer(RT_GL_FRAMEBUFFER, accumulationFbo_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(program.id);
    BindTexture(kUnitDepth, depthTexture_);
    glUniformMatrix4fv(program.inverseProjection, 1, GL_FALSE, inverseProjection);
    bindSky(program);
    if (dynamic) {
        bindDynamicSky(program);
    }
    glBindVertexArray(emptyVao_);
    if (!CanDraw(program.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);
    return true;
}
```

In the class comment above `class Renderer` (Renderer.hpp), change "then the Skybox behind every surface" to "then the Skybox or DynamicSky behind every surface". Change the closing sentence to "With a Skybox or DynamicSky, its image-based lighting (EnvironmentMap) is the sky light."

- [ ] **Step 7: Build and run the render check, then every suite**

Run:
```
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target scene-render-check sandbox engine-tests --parallel
build/Debug/scene-render-check.exe
```

Expected:
- every check passes, including the new DynamicSky ones and all the existing Skybox, SSR, AO, and bloom checks;
- it exits 0;
- stderr does not contain "The DynamicSky's shaders did not build".

If a colour check fails, print the pixel the message names, and adjust only the threshold the spec does not fix. Do not weaken "dark at midnight" or "noon lights more than midnight".

Then run `build/Debug/sandbox.exe && build/Debug/engine-tests.exe`. Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add resources/shaders/pipeline/procedural_sky.glsl resources/shaders/pipeline/dynamic_sky.frag resources/shaders/pipeline/dynamic_sky_cube.frag src/runner/EnvironmentMap.hpp src/runner/EnvironmentMap.cpp src/runner/Renderer.hpp src/runner/Renderer.cpp tests/SceneRenderCheck.cpp
git commit -m "Draw the DynamicSky from its shader, and filter its light from a cube drawn when LightingDue says"
```

---

### Task 5: Light the Scene View from the DynamicSky

**Files:**
- Modify: `src/runner/GameView.hpp`, `src/runner/GameView.cpp`

**Interfaces:**
- Consumes:
  - `VisualSnapshot::dynamic_sky` (Task 3);
  - `ComputeSky`, `SetSkyState`, `SkyLightDraw`, and `SceneDynamicSky` (Tasks 1 and 4).

- [ ] **Step 1: Add the clock**

In `GameView.hpp`, beside the other `std::chrono::steady_clock::time_point` members, add:

```cpp
    // The DynamicSky's clouds drift by this clock, from when the view was made.
    std::chrono::steady_clock::time_point skyClockStart_ = std::chrono::steady_clock::now();
```

- [ ] **Step 2: Fill the DynamicSky and its light**

In `GameView.cpp`, add `#include "SkyMath.hpp"` after `#include "SceneService.hpp"`. In the function that fills `lightDraws_`, after the `// The Skybox's images ...` block (the `if (sky.present) {...}`), add:

```cpp
    // The DynamicSky, if it is the sky: SkyMath's sun, moon, and stars, the
    // clouds' drift by the view's clock, and its light first among the suns,
    // so it takes the shadow cascades.
    const engine_core::VisualDynamicSky& dynamic = snapshot.dynamic_sky;
    if (dynamic.present) {
        const SkyState state = ComputeSky(dynamic.time_of_day, dynamic.latitude, dynamic.brightness,
                                          dynamic.cloud_cover, dynamic.cloud_density);
        SceneDynamicSky& out = lighting.dynamicSky;
        out.enabled = true;
        SetSkyState(out, state);
        out.cloudCover = dynamic.cloud_cover;
        out.cloudDensity = dynamic.cloud_density;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - skyClockStart_).count();
        // In double, wrapped, so the shader's floats stay precise: the clouds jump once in many hours.
        constexpr double kCloudOffsetWrap = 100000.0;
        out.cloudOffset[0] = static_cast<float>(std::fmod(static_cast<double>(dynamic.wind.x) * seconds, kCloudOffsetWrap));
        out.cloudOffset[1] = static_cast<float>(std::fmod(static_cast<double>(dynamic.wind.z) * seconds, kCloudOffsetWrap));
        out.windy = dynamic.wind.x != 0.f || dynamic.wind.z != 0.f;
        out.seconds = seconds;
        out.sunSizeDegrees = dynamic.sun_size;
        out.moonSizeDegrees = dynamic.moon_size;
        out.sunTexture = textures_.get(dynamic.sun_texture);
        out.moonTexture = textures_.get(dynamic.moon_texture);
        out.reflectionQuality = dynamic.reflection_quality == 0   ? SceneQuality::Low
                                : dynamic.reflection_quality == 2 ? SceneQuality::High
                                                                  : SceneQuality::Medium;
        out.key = {dynamic.time_of_day, dynamic.latitude, dynamic.cloud_cover, dynamic.cloud_density,
                   static_cast<int>(out.reflectionQuality)};
        if (state.light.intensity > 0.f) {
            lightDraws_.insert(lightDraws_.begin(), SkyLightDraw(state, dynamic.shadows));
        }
    }
```

- [ ] **Step 3: Build Debug and run every suite**

Run:
```
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --parallel
build/Debug/sandbox.exe && build/Debug/engine-tests.exe && build/Debug/scene-render-check.exe
```

Expected: everything builds warning-free, and all three suites pass.

- [ ] **Step 4: Look at it in the studio**

Build the Release studio:

```
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target AnarchyStudio --parallel
```

If it fails with LNK1104, the user's studio is open; ask them to close it. Do not kill it.

Then:
- Launch `build/Release/AnarchyStudio.exe` in the background with `ANARCHY_MCP_PORT=8765 ANARCHY_MCP_TOKEN=sky` and `APPDATA` pointed at a scratch folder.
- Over MCP (`POST http://127.0.0.1:8765/mcp`, `Authorization: Bearer sky`), call `run_lua` with:

```lua
local sky = Instance.new("DynamicSky", game.Lighting)
local part = Instance.new("GameObject", workspace)
```

- Call `screenshot` at each of these and look at each image (open it in the editor with `code -r`):
  - TimeOfDay 12;
  - TimeOfDay 18.2, an orange horizon;
  - TimeOfDay 0, stars and the moon;
  - CloudCover 1 with CloudDensity 1.
- Confirm:
  - the sky changes;
  - the part's shading and shadow move with TimeOfDay;
  - clouds drift between two screenshots taken 2 s apart;
  - `get_output` has no errors.

- [ ] **Step 5: Commit**

```bash
git add src/runner/GameView.hpp src/runner/GameView.cpp
git commit -m "Light the Scene View from the DynamicSky: its sky, its clouds' drift, and its sun or moon first among the lights"
```

---

### Task 6: Finish

**Files:**
- Modify: `docs/superpowers/specs/2026-10-05-dynamic-sky-design.md`, only where the build changed something.

- [ ] **Step 1: Fit the spec to what shipped**

Reread the spec against the code. Update any number that changed in the build, for example a render-check threshold or a cloud constant. Commit with "Fit the dynamic sky spec to what shipped".

- [ ] **Step 2: Run everything once more**

Run: `cd build && "C:/Program Files/CMake/bin/ctest.exe" -C Debug --output-on-failure && cd .. && build/Debug/scene-render-check.exe`

Expected: all tests pass.

- [ ] **Step 3: Refresh the release package**

Build `--config Release --target AnarchyStudio` and the player. Copy the Release exe, `AnarchyPlayer.exe`, and `resources/` into `build/package/AnarchyEngine/`, then re-zip `build/AnarchyEngine-release.zip`.

- [ ] **Step 4: Hand off**

Use superpowers:finishing-a-development-branch.
