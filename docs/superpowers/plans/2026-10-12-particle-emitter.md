# ParticleEmitter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the `ParticleEmitter` instance of the spec: `NumberCurve`/`ColorCurve` value types, the emitter class and its Properties editors, a CPU `ParticleWorld`, and an instanced-quad particle pass in the renderer.

**Architecture:** Curves are immutable value types held in `LuaSlot` by `shared_ptr`, saved as JSON, baked into 64-sample tables. `ParticleEmitter` is a plain registry-property instance like `SoundEmitter`. `ParticleWorld` (engine_core, owned by `Engine`) keeps one `EmitterSim` per emitter (structure-of-arrays, no DataModel inside, unit-testable), steps once per frame inside `Engine::run_frame` (edit mode, play, and frozen while paused), and publishes immutable per-emitter particle blocks that `SnapshotPump` copies by pointer into `VisualSnapshot::particles`. `runner::ParticleRenderer` draws each block with one `glDrawElementsInstanced` after the transparent meshes, into the transparency target, reading a copy of scene depth for soft edges.

**Tech Stack:** C++20, Luau, flecs, Box3D (raycasts), OpenGL 3.3 / GLSL 330 (ES 3.00-safe), JadeFX (studio UI), Catch2 (`sandbox`), hand-rolled harnesses (`engine-tests`, `properties-tests`, `scene-render-check`).

**Spec:** `docs/superpowers/specs/2026-10-12-particle-emitter-design.md`

## Global Constraints

- Distances are "units". Never write "studs" in code, comments, docs, or UI text.
- Name APIs after the engine, not Roblox (`NumberCurve`, `ColorCurve`, `ParticleBudget`, `SoftEdge`…). No `ZOffset`, `LightEmission`, `NumberSequence`.
- GLSL stays within `#version 330 core` and GLSL ES 3.00: no compute, SSBOs, geometry shaders, transform feedback, `gl_BaseInstance`.
- Curves accept any number of keys; the simulation reads only 64-sample tables (`CurveTable::kSamples = 64`).
- `Workspace.ParticleBudget` default **20000**; `ParticleEmitter.MaxParticles` default **1000**.
- Particles never run at the 240 Hz physics rate: one step per engine frame.
- Emitters inside a `ViewportPane` are neither simulated nor drawn.
- Setters run on the engine thread (`require_thread(*this)`), refuse bad values with a reason string, and call `note_property_change` once per real change.
- Python edits to tracked files must keep LF (`newline=""`); check `git diff --stat` after scripted edits.
- On Windows run cmake with `MSYS_NO_PATHCONV=1` from Git Bash. "Rebuild" means both `build/Debug` and `build/Release` (`AnarchyStudio.exe`).
- Build: `cmake --build build --config Debug --target <target> --parallel`; run sandbox cases from the repo root: `build/Debug/sandbox.exe "[tag]"`.

## Review Focus

- **A curve property written with a malformed table from Lua** (empty keys, NaN, time outside 0–1, unknown interpolation name): refused with a message naming the property, no crash, value unchanged. Test in Task 3.
- **The parent changes or disappears while particles are alive** (reparent, destroy the parent GameObject, emitter moved into a ViewportPane): no dangling transforms; live particles of a World-space emitter keep flying, a destroyed emitter's particles vanish that frame. Test in Task 9.
- **Huge or tiny numbers** (`Rate = 1e9`, `Lifetime = 1e-6`, `MaxParticles = 0`, `ParticleBudget = 0`): spawn count is bounded by MaxParticles and budget in one frame; no unbounded loops or allocations. Test in Task 10.
- **Stop after editing mid-play**: particles spawned during play are gone at Stop; edit-mode particles resume; `Playing` restored to its pre-play value. Test in Task 9.
- **A Texture asset that is missing or still loading**: the built-in soft dot draws, nothing renders black or crashes. Test in Task 13.

---

## File structure

| File | Responsibility |
| --- | --- |
| `src/engine_datatypes/Curve.hpp/.cpp` | `NumberCurve`, `ColorCurve`, evaluation, validation, `CurveTable`/`ColorTable` baking. Pure C++. |
| `src/engine_datatypes/CurveLua.cpp` | `NumberCurve.*` and `ColorCurve.*` Lua libraries, userdata, analysis registration. |
| `src/engine_core/CurveJson.hpp/.cpp` | Curve ⇄ `JsonValue`. |
| `src/engine_core/LuaApi.hpp` | `LuaSlot` kinds `NumberCurve`, `ColorCurve`; `LuaField::enabled_when`; `lua_enabled_when`. |
| `src/engine_core/PropertyReflection.cpp`, `DataModel.cpp`, `ScriptBindings.cpp`, `ScriptRuntime.cpp`, `LuaApi.cpp` | Plumbing for the new kinds; docs. |
| `src/engine_datatypes/Enum.hpp/.cpp` | Seven particle enums. |
| `src/engine_instances/ParticleEmitter.hpp/.cpp` | The instance: properties, validation, dirty mask, quiet stores, registration. |
| `src/engine_core/ParticleBursts.hpp/.cpp` | `ParticleBurst` list ⇄ JSON text and ⇄ Lua table. |
| `src/engine_core/particles/ParticleRandom.hpp` | `Pcg32`. |
| `src/engine_core/particles/ParticleMath.hpp` | Small `Vec3` helpers (add, scale, dot, cross, normalize, rotate). |
| `src/engine_core/particles/ParticleShapes.hpp/.cpp` | Analytic shape sampling. |
| `src/engine_core/particles/EmitterSim.hpp/.cpp` | Per-emitter SoA simulation; spawn, integrate, collide via interface, sort, pack. |
| `src/engine_core/particles/ParticleColliders.hpp/.cpp` | `TerrainCollider`, `WorldCollider` (ray budget, plane cache). |
| `src/engine_core/ParticleWorld.hpp/.cpp` | DataModel glue: finds emitters, builds settings, budget, steps sims, publishes `ParticleDraw`s. |
| `src/engine_core/ParticleShapeLines.hpp/.cpp` | Wireframe lines for a selected emitter's shape. |
| `src/runner/ParticleRenderer.hpp/.cpp` | GL resources and the particle pass. |
| `resources/shaders/pipeline/particle.vert/.frag` | Particle shaders. |
| `src/ide/CurveEditModel.hpp/.cpp` | UI-free model for the curve/gradient popovers (hit tests, add/move/delete keys). |
| `src/ide/CurveEditor.hpp/.cpp` | JadeFX popover views for NumberCurve and ColorCurve rows. |
| `src/ide/BurstListRow.hpp/.cpp` | The Bursts table row. |
| Tests | `sandbox/curve_tests.cpp`, `sandbox/particle_emitter_tests.cpp`, `sandbox/particle_shape_tests.cpp`, `sandbox/particle_sim_tests.cpp`, `sandbox/particle_world_tests.cpp`, additions to `tests/LuaEngineTest.cpp`, `tests/PropertiesTest.cpp`, `tests/SceneRenderCheck.cpp`. |

Add every new `.cpp` to the engine source list in `CMakeLists.txt` (next to `src/engine_instances/SoundEmitter.cpp`, line ~611, or the engine_core / runner / ide lists as appropriate) and every new sandbox file to the `add_executable(sandbox ...)` list (line ~1032+).

---

### Task 1: Curve value types (C++)

**Files:**
- Create: `src/engine_datatypes/Curve.hpp`, `src/engine_datatypes/Curve.cpp`
- Modify: `CMakeLists.txt` (engine source list; sandbox list)
- Test: `sandbox/curve_tests.cpp`

**Interfaces:**
- Produces: `engine_core::CurveInterp`, `NumberKey`, `NumberCurveMode`, `NumberCurve`, `ColorKey`, `ColorCurveMode`, `ColorCurve`, `CurveTable`, `ColorTable`, `evaluate_keys`, `evaluate_color_keys` (exact declarations below).

- [ ] **Step 1: Write the failing tests**

```cpp
// sandbox/curve_tests.cpp
#include "Curve.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace engine_core;
using Catch::Approx;

TEST_CASE("CRV1 a constant and a range evaluate as they say", "[curve]") {
    const NumberCurve five = NumberCurve::constant(5.f);
    REQUIRE(five.mode() == NumberCurveMode::Constant);
    REQUIRE(five.evaluate(0.3f, 0.9f) == 5.f);
    const NumberCurve range = NumberCurve::range(1.f, 3.f);
    REQUIRE(range.evaluate(0.7f, 0.f) == 1.f);
    REQUIRE(range.evaluate(0.7f, 1.f) == 3.f);
    REQUIRE(range.evaluate(0.7f, 0.5f) == Approx(2.f));
}

TEST_CASE("CRV2 keys sort by time, hold their ends, and interpolate per segment", "[curve]") {
    std::string error;
    const auto curve = NumberCurve::keys({{1.f, 0.f}, {0.f, 0.f}, {0.5f, 4.f, CurveInterp::Step}}, error);
    REQUIRE(curve.has_value());
    REQUIRE(curve->keys_a()[0].time == 0.f);
    REQUIRE(curve->evaluate(0.25f, 0.f) == Approx(2.f));   // linear 0 -> 4
    REQUIRE(curve->evaluate(0.75f, 0.f) == 4.f);           // step holds 4 until 1
    REQUIRE(curve->evaluate(1.f, 0.f) == 0.f);
    REQUIRE(curve->evaluate(-1.f, 0.f) == 0.f);            // held
    REQUIRE(curve->evaluate(2.f, 0.f) == 0.f);
}

TEST_CASE("CRV3 smooth keys pass through their values and stay between them on a monotone run", "[curve]") {
    std::string error;
    const auto curve =
        NumberCurve::keys({{0.f, 0.f, CurveInterp::Smooth}, {0.5f, 1.f, CurveInterp::Smooth}, {1.f, 2.f}}, error);
    REQUIRE(curve.has_value());
    REQUIRE(curve->evaluate(0.5f, 0.f) == Approx(1.f));
    for (float t = 0.f; t <= 1.f; t += 0.05f) {
        const float v = curve->evaluate(t, 0.f);
        REQUIRE(v >= -0.001f);
        REQUIRE(v <= 2.001f);
    }
}

TEST_CASE("CRV4 a curve range blends its two curves by the particle's random", "[curve]") {
    std::string error;
    const auto curve = NumberCurve::keys_range({{0.f, 0.f}}, {{0.f, 10.f}}, error);
    REQUIRE(curve.has_value());
    REQUIRE(curve->evaluate(0.4f, 0.25f) == Approx(2.5f));
}

TEST_CASE("CRV5 bad keys are refused with a reason", "[curve]") {
    std::string error;
    REQUIRE_FALSE(NumberCurve::keys({}, error).has_value());
    REQUIRE(error.find("at least one key") != std::string::npos);
    REQUIRE_FALSE(NumberCurve::keys({{1.5f, 0.f}}, error).has_value());
    REQUIRE(error.find("between 0 and 1") != std::string::npos);
    REQUIRE_FALSE(NumberCurve::keys({{0.f, std::nanf("")}}, error).has_value());
    REQUIRE(error.find("finite") != std::string::npos);
}

TEST_CASE("CRV6 a baked table matches direct evaluation", "[curve]") {
    std::string error;
    const auto curve = NumberCurve::keys({{0.f, 0.f, CurveInterp::Smooth}, {0.3f, 5.f}, {1.f, -2.f}}, error);
    REQUIRE(curve.has_value());
    CurveTable table;
    table.bake(*curve);
    for (float t = 0.f; t <= 1.f; t += 0.01f) {
        REQUIRE(table.sample(t, 0.f) == Approx(curve->evaluate(t, 0.f)).margin(0.15f));
    }
    CurveTable constant;
    constant.bake(NumberCurve::range(2.f, 4.f));
    REQUIRE(constant.sample(0.9f, 0.5f) == Approx(3.f));
}

TEST_CASE("CRV7 a color curve carries transparency and blends gradients", "[curve]") {
    const ColorCurve white = ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, 0.25f);
    const ColorRgb c = white.evaluate(0.5f, 0.f);
    REQUIRE(c.a == Approx(0.75f));
    std::string error;
    const auto grad = ColorCurve::gradient(
        {{0.f, ColorRgb{1.f, 0.f, 0.f, 1.f}, 0.f}, {1.f, ColorRgb{0.f, 0.f, 1.f, 1.f}, 1.f}}, error);
    REQUIRE(grad.has_value());
    const ColorRgb mid = grad->evaluate(0.5f, 0.f);
    REQUIRE(mid.r == Approx(0.5f));
    REQUIRE(mid.b == Approx(0.5f));
    REQUIRE(mid.a == Approx(0.5f));
    const ColorCurve between =
        ColorCurve::between(ColorRgb{1.f, 0.f, 0.f, 1.f}, 0.f, ColorRgb{0.f, 1.f, 0.f, 1.f}, 0.f);
    REQUIRE(between.evaluate(0.9f, 1.f).g == Approx(1.f));
    ColorTable table;
    table.bake(*grad);
    REQUIRE(table.sample(0.5f, 0.f).r == Approx(0.5f).margin(0.02f));
}

TEST_CASE("CRV8 equal curves compare equal", "[curve]") {
    REQUIRE(NumberCurve::constant(1.f) == NumberCurve::constant(1.f));
    REQUIRE_FALSE(NumberCurve::constant(1.f) == NumberCurve::range(1.f, 1.f));
}
```

- [ ] **Step 2: Add `sandbox/curve_tests.cpp` to the sandbox list, build, see it fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Debug --target sandbox --parallel`
Expected: compile error `Curve.hpp: No such file`.

- [ ] **Step 3: Write `Curve.hpp`**

```cpp
#pragma once

#include "Color.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {

// How a key reaches the next one: straight, a cubic with automatic
// tangents, or held until the next key.
enum class CurveInterp : std::uint8_t { Linear = 0, Smooth = 1, Step = 2 };

struct NumberKey {
    float time = 0.f;
    float value = 0.f;
    CurveInterp interp = CurveInterp::Linear;
    bool operator==(const NumberKey&) const = default;
};

enum class NumberCurveMode : std::uint8_t { Constant = 0, Range = 1, Curve = 2, CurveRange = 3 };

// A number that is fixed, random within a range, keyed over 0..1, or a random
// blend of two keyed curves. Immutable. What its time means is the property's.
class NumberCurve {
public:
    static NumberCurve constant(float value);
    static NumberCurve range(float min, float max);
    // Keys sorted by time on the way in. Refuses no keys, a time outside
    // 0..1, or a time or value that is not finite, saying why in error.
    static std::optional<NumberCurve> keys(std::vector<NumberKey> keys, std::string& error);
    static std::optional<NumberCurve> keys_range(std::vector<NumberKey> a, std::vector<NumberKey> b,
                                                 std::string& error);

    NumberCurveMode mode() const { return mode_; }
    float min() const { return min_; }
    float max() const { return max_; }
    const std::vector<NumberKey>& keys_a() const { return a_; }
    const std::vector<NumberKey>& keys_b() const { return b_; }

    // blend, 0..1, picks within a Range or between a CurveRange's curves.
    float evaluate(float t, float blend) const;
    bool operator==(const NumberCurve&) const = default;

private:
    NumberCurveMode mode_ = NumberCurveMode::Constant;
    float min_ = 0.f;
    float max_ = 0.f;
    std::vector<NumberKey> a_;
    std::vector<NumberKey> b_;
};

// The keys' value at t, holding the end values outside them. keys is non-empty and sorted.
float evaluate_keys(const std::vector<NumberKey>& keys, float t);

struct ColorKey {
    float time = 0.f;
    ColorRgb color{1.f, 1.f, 1.f, 1.f};   // a is ignored
    float transparency = 0.f;
    bool operator==(const ColorKey&) const = default;
};

enum class ColorCurveMode : std::uint8_t { Constant = 0, RandomBetween = 1, Gradient = 2, GradientRange = 3 };

// A color and transparency that is fixed, one of two picked per particle,
// keyed over 0..1, or a random blend of two gradients. evaluate's alpha is
// 1 - transparency. Channels interpolate linearly as stored.
class ColorCurve {
public:
    static ColorCurve constant(ColorRgb color, float transparency);
    static ColorCurve between(ColorRgb a, float transparency_a, ColorRgb b, float transparency_b);
    static std::optional<ColorCurve> gradient(std::vector<ColorKey> keys, std::string& error);
    static std::optional<ColorCurve> gradient_range(std::vector<ColorKey> a, std::vector<ColorKey> b,
                                                    std::string& error);

    ColorCurveMode mode() const { return mode_; }
    const std::vector<ColorKey>& keys_a() const { return a_; }
    const std::vector<ColorKey>& keys_b() const { return b_; }

    ColorRgb evaluate(float t, float blend) const;
    bool operator==(const ColorCurve&) const = default;

private:
    ColorCurveMode mode_ = ColorCurveMode::Constant;
    std::vector<ColorKey> a_;   // Constant and RandomBetween keep their values as one key each
    std::vector<ColorKey> b_;
};

ColorRgb evaluate_color_keys(const std::vector<ColorKey>& keys, float t);

// A NumberCurve read in O(1): 64 samples per curve, linear between them.
struct CurveTable {
    static constexpr int kSamples = 64;
    NumberCurveMode mode = NumberCurveMode::Constant;
    float min = 0.f;
    float max = 0.f;
    float a[kSamples]{};
    float b[kSamples]{};
    void bake(const NumberCurve& curve);
    float sample(float t, float blend) const;
};

struct ColorTable {
    static constexpr int kSamples = 64;
    ColorCurveMode mode = ColorCurveMode::Constant;
    ColorRgb a[kSamples]{};
    ColorRgb b[kSamples]{};
    void bake(const ColorCurve& curve);
    ColorRgb sample(float t, float blend) const;
};

}  // namespace engine_core
```

Check `Color.hpp`'s namespace: if `ColorRgb` is not in `engine_core`, add the matching `using`. `ColorRgb` needs `operator==` for the defaulted comparisons; if it has none, add `bool operator==(const ColorRgb&) const = default;` to it.

- [ ] **Step 4: Write `Curve.cpp`**

```cpp
#include "Curve.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core {
namespace {

float clamp01(float t) { return std::clamp(t, 0.f, 1.f); }
float mix(float a, float b, float f) { return a + (b - a) * f; }

ColorRgb mix(const ColorRgb& a, const ColorRgb& b, float f) {
    return ColorRgb{mix(a.r, b.r, f), mix(a.g, b.g, f), mix(a.b, b.b, f), mix(a.a, b.a, f)};
}

bool check_number_keys(std::vector<NumberKey>& keys, std::string& error) {
    if (keys.empty()) {
        error = "a curve needs at least one key";
        return false;
    }
    for (const NumberKey& key : keys) {
        if (!std::isfinite(key.time) || !std::isfinite(key.value)) {
            error = "a key's time and value must be finite";
            return false;
        }
        if (key.time < 0.f || key.time > 1.f) {
            error = "a key's time must be between 0 and 1";
            return false;
        }
    }
    std::stable_sort(keys.begin(), keys.end(), [](const NumberKey& x, const NumberKey& y) { return x.time < y.time; });
    return true;
}

bool check_color_keys(std::vector<ColorKey>& keys, std::string& error) {
    if (keys.empty()) {
        error = "a gradient needs at least one key";
        return false;
    }
    for (const ColorKey& key : keys) {
        const float parts[] = {key.time, key.color.r, key.color.g, key.color.b, key.transparency};
        for (float part : parts) {
            if (!std::isfinite(part)) {
                error = "a key's time, color, and transparency must be finite";
                return false;
            }
        }
        if (key.time < 0.f || key.time > 1.f) {
            error = "a key's time must be between 0 and 1";
            return false;
        }
    }
    std::stable_sort(keys.begin(), keys.end(), [](const ColorKey& x, const ColorKey& y) { return x.time < y.time; });
    return true;
}

// The slope a Smooth key leaves with: the chord through its neighbours.
float tangent(const std::vector<NumberKey>& keys, std::size_t i) {
    const std::size_t last = keys.size() - 1;
    const std::size_t lo = i == 0 ? 0 : i - 1;
    const std::size_t hi = i == last ? last : i + 1;
    const float dt = keys[hi].time - keys[lo].time;
    return dt > 0.f ? (keys[hi].value - keys[lo].value) / dt : 0.f;
}

}  // namespace

float evaluate_keys(const std::vector<NumberKey>& keys, float t) {
    if (t <= keys.front().time) {
        return keys.front().value;
    }
    if (t >= keys.back().time) {
        return keys.back().value;
    }
    const auto next = std::upper_bound(keys.begin(), keys.end(), t,
                                       [](float value, const NumberKey& key) { return value < key.time; });
    const std::size_t i = static_cast<std::size_t>(next - keys.begin()) - 1;
    const NumberKey& k0 = keys[i];
    const NumberKey& k1 = keys[i + 1];
    const float span = k1.time - k0.time;
    if (span <= 0.f || k0.interp == CurveInterp::Step) {
        return k0.value;
    }
    const float f = (t - k0.time) / span;
    if (k0.interp == CurveInterp::Linear) {
        return mix(k0.value, k1.value, f);
    }
    // Cubic Hermite, tangents scaled to the segment.
    const float m0 = tangent(keys, i) * span;
    const float m1 = tangent(keys, i + 1) * span;
    const float f2 = f * f;
    const float f3 = f2 * f;
    return (2 * f3 - 3 * f2 + 1) * k0.value + (f3 - 2 * f2 + f) * m0 + (-2 * f3 + 3 * f2) * k1.value +
           (f3 - f2) * m1;
}

ColorRgb evaluate_color_keys(const std::vector<ColorKey>& keys, float t) {
    const auto as_rgba = [](const ColorKey& key) {
        return ColorRgb{key.color.r, key.color.g, key.color.b, 1.f - key.transparency};
    };
    if (t <= keys.front().time) {
        return as_rgba(keys.front());
    }
    if (t >= keys.back().time) {
        return as_rgba(keys.back());
    }
    const auto next = std::upper_bound(keys.begin(), keys.end(), t,
                                       [](float value, const ColorKey& key) { return value < key.time; });
    const std::size_t i = static_cast<std::size_t>(next - keys.begin()) - 1;
    const float span = keys[i + 1].time - keys[i].time;
    const float f = span > 0.f ? (t - keys[i].time) / span : 0.f;
    return mix(as_rgba(keys[i]), as_rgba(keys[i + 1]), f);
}

NumberCurve NumberCurve::constant(float value) {
    NumberCurve out;
    out.mode_ = NumberCurveMode::Constant;
    out.min_ = out.max_ = value;
    return out;
}

NumberCurve NumberCurve::range(float min, float max) {
    NumberCurve out;
    out.mode_ = NumberCurveMode::Range;
    out.min_ = min;
    out.max_ = max;
    return out;
}

std::optional<NumberCurve> NumberCurve::keys(std::vector<NumberKey> keys, std::string& error) {
    if (!check_number_keys(keys, error)) {
        return std::nullopt;
    }
    NumberCurve out;
    out.mode_ = NumberCurveMode::Curve;
    out.a_ = std::move(keys);
    return out;
}

std::optional<NumberCurve> NumberCurve::keys_range(std::vector<NumberKey> a, std::vector<NumberKey> b,
                                                   std::string& error) {
    if (!check_number_keys(a, error) || !check_number_keys(b, error)) {
        return std::nullopt;
    }
    NumberCurve out;
    out.mode_ = NumberCurveMode::CurveRange;
    out.a_ = std::move(a);
    out.b_ = std::move(b);
    return out;
}

float NumberCurve::evaluate(float t, float blend) const {
    switch (mode_) {
        case NumberCurveMode::Constant: return min_;
        case NumberCurveMode::Range: return mix(min_, max_, clamp01(blend));
        case NumberCurveMode::Curve: return evaluate_keys(a_, t);
        case NumberCurveMode::CurveRange: return mix(evaluate_keys(a_, t), evaluate_keys(b_, t), clamp01(blend));
    }
    return min_;
}

ColorCurve ColorCurve::constant(ColorRgb color, float transparency) {
    ColorCurve out;
    out.mode_ = ColorCurveMode::Constant;
    out.a_ = {ColorKey{0.f, color, transparency}};
    return out;
}

ColorCurve ColorCurve::between(ColorRgb a, float transparency_a, ColorRgb b, float transparency_b) {
    ColorCurve out;
    out.mode_ = ColorCurveMode::RandomBetween;
    out.a_ = {ColorKey{0.f, a, transparency_a}};
    out.b_ = {ColorKey{0.f, b, transparency_b}};
    return out;
}

std::optional<ColorCurve> ColorCurve::gradient(std::vector<ColorKey> keys, std::string& error) {
    if (!check_color_keys(keys, error)) {
        return std::nullopt;
    }
    ColorCurve out;
    out.mode_ = ColorCurveMode::Gradient;
    out.a_ = std::move(keys);
    return out;
}

std::optional<ColorCurve> ColorCurve::gradient_range(std::vector<ColorKey> a, std::vector<ColorKey> b,
                                                     std::string& error) {
    if (!check_color_keys(a, error) || !check_color_keys(b, error)) {
        return std::nullopt;
    }
    ColorCurve out;
    out.mode_ = ColorCurveMode::GradientRange;
    out.a_ = std::move(a);
    out.b_ = std::move(b);
    return out;
}

ColorRgb ColorCurve::evaluate(float t, float blend) const {
    switch (mode_) {
        case ColorCurveMode::Constant:
        case ColorCurveMode::Gradient: return evaluate_color_keys(a_, t);
        case ColorCurveMode::RandomBetween:
        case ColorCurveMode::GradientRange:
            return mix(evaluate_color_keys(a_, t), evaluate_color_keys(b_, t), clamp01(blend));
    }
    return ColorRgb{};
}

void CurveTable::bake(const NumberCurve& curve) {
    mode = curve.mode();
    min = curve.min();
    max = curve.max();
    if (mode != NumberCurveMode::Curve && mode != NumberCurveMode::CurveRange) {
        return;
    }
    for (int i = 0; i < kSamples; ++i) {
        const float t = static_cast<float>(i) / (kSamples - 1);
        a[i] = evaluate_keys(curve.keys_a(), t);
        b[i] = mode == NumberCurveMode::CurveRange ? evaluate_keys(curve.keys_b(), t) : a[i];
    }
}

float CurveTable::sample(float t, float blend) const {
    if (mode == NumberCurveMode::Constant) {
        return min;
    }
    if (mode == NumberCurveMode::Range) {
        return mix(min, max, clamp01(blend));
    }
    const float x = clamp01(t) * (kSamples - 1);
    const int i = std::min(static_cast<int>(x), kSamples - 2);
    const float f = x - static_cast<float>(i);
    const float va = mix(a[i], a[i + 1], f);
    if (mode == NumberCurveMode::Curve) {
        return va;
    }
    return mix(va, mix(b[i], b[i + 1], f), clamp01(blend));
}

void ColorTable::bake(const ColorCurve& curve) {
    mode = curve.mode();
    for (int i = 0; i < kSamples; ++i) {
        const float t = static_cast<float>(i) / (kSamples - 1);
        a[i] = evaluate_color_keys(curve.keys_a(), t);
        b[i] = curve.keys_b().empty() ? a[i] : evaluate_color_keys(curve.keys_b(), t);
    }
}

ColorRgb ColorTable::sample(float t, float blend) const {
    const float x = clamp01(t) * (kSamples - 1);
    const int i = std::min(static_cast<int>(x), kSamples - 2);
    const float f = x - static_cast<float>(i);
    const ColorRgb ca = mix(a[i], a[i + 1], f);
    if (mode == ColorCurveMode::Constant || mode == ColorCurveMode::Gradient) {
        return ca;
    }
    return mix(ca, mix(b[i], b[i + 1], f), clamp01(blend));
}

}  // namespace engine_core
```

- [ ] **Step 5: Add `src/engine_datatypes/Curve.cpp` to the engine source list; build and run**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Debug --target sandbox --parallel && build/Debug/sandbox.exe "[curve]"`
Expected: `All tests passed (… assertions in 8 test cases)`.

- [ ] **Step 6: Commit**

```bash
git add src/engine_datatypes/Curve.hpp src/engine_datatypes/Curve.cpp sandbox/curve_tests.cpp CMakeLists.txt
git commit -m "Curves: NumberCurve and ColorCurve values, baked to 64-sample tables"
```

---

### Task 2: Curves as saved properties (LuaSlot, JSON, undo)

**Files:**
- Create: `src/engine_core/CurveJson.hpp`, `src/engine_core/CurveJson.cpp`
- Modify: `src/engine_core/LuaApi.hpp:23-44` (LuaSlot), `src/engine_core/PropertyReflection.cpp` (`slot_to_json` 20-53, `slot_from_json` 55-135, `same_slot` 137-175), `src/engine_core/DataModel.cpp:1374-1412` (`type_fits`), `src/engine_core/LuaApi.hpp:146-149` (comment), `src/engine_instances/Gui.cpp:223` (exhaustive switch, if it warns), `CMakeLists.txt`
- Test: `sandbox/curve_tests.cpp`

**Interfaces:**
- Consumes: Task 1 types.
- Produces:
  - `LuaSlot::Kind::NumberCurve`, `LuaSlot::Kind::ColorCurve`; fields `std::shared_ptr<const NumberCurve> number_curve;` `std::shared_ptr<const ColorCurve> color_curve;`
  - `LuaSlot number_curve_slot(NumberCurve)`, `LuaSlot color_curve_slot(ColorCurve)` (in CurveJson.hpp)
  - `JsonValue number_curve_to_json(const NumberCurve&)`, `std::optional<NumberCurve> number_curve_from_json(const JsonValue&, std::string& error)`, and the `color_` equivalents.
  - Property type names `"NumberCurve"` and `"ColorCurve"` save, load, undo, and restore at Stop through `lua_saved_property`.

JSON forms (exactly):

```
NumberCurve:  {"mode":"Constant","value":5}
              {"mode":"Range","min":1,"max":3}
              {"mode":"Curve","keys":[[0,0,"Linear"],[0.2,3,"Smooth"],[1,0,"Linear"]]}
              {"mode":"CurveRange","keys":[...],"keys2":[...]}
ColorCurve:   {"mode":"Constant","color":[1,1,1],"transparency":0}
              {"mode":"RandomBetween","color":[1,0.5,0],"transparency":0,"color2":[1,1,0],"transparency2":0}
              {"mode":"Gradient","keys":[[0,[1,0.8,0.2],0],[1,[0.3,0.3,0.3],1]]}
              {"mode":"GradientRange","keys":[...],"keys2":[...]}
```

- [ ] **Step 1: Write the failing test** (append to `sandbox/curve_tests.cpp`)

```cpp
#include "CurveJson.hpp"
#include "Json.hpp"   // the header that declares JsonValue, parse_json, write_json; check PropertyReflection.cpp's includes for its real name

TEST_CASE("CRV9 curves round-trip through JSON and refuse bad JSON", "[curve]") {
    std::string error;
    const auto curve = NumberCurve::keys({{0.f, 0.f}, {0.2f, 3.f, CurveInterp::Smooth}, {1.f, 0.f}}, error);
    REQUIRE(curve.has_value());
    const JsonValue json = number_curve_to_json(*curve);
    const auto back = number_curve_from_json(json, error);
    REQUIRE(back.has_value());
    REQUIRE(*back == *curve);
    REQUIRE(write_json(number_curve_to_json(NumberCurve::constant(5.f))) == R"({"mode":"Constant","value":5})");

    const auto bad = number_curve_from_json(*parse_json(R"({"mode":"Curve","keys":[]})"), error);
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(error.find("at least one key") != std::string::npos);
    REQUIRE_FALSE(number_curve_from_json(*parse_json(R"({"mode":"Wavy"})"), error).has_value());

    const ColorCurve between =
        ColorCurve::between(ColorRgb{1.f, 0.5f, 0.f, 1.f}, 0.f, ColorRgb{1.f, 1.f, 0.f, 1.f}, 0.25f);
    const auto color_back = color_curve_from_json(color_curve_to_json(between), error);
    REQUIRE(color_back.has_value());
    REQUIRE(*color_back == between);
}
```

Then a property-level test, using a throwaway registry class is not possible without an instance; it is covered by Task 5's BLM-style test (`PE1`). Here, test the reflection functions directly:

```cpp
#include "PropertyReflection.hpp"

TEST_CASE("CRV10 slots of curves convert to JSON by type name and compare by value", "[curve]") {
    const LuaSlot a = number_curve_slot(NumberCurve::range(1.f, 2.f));
    const LuaSlot b = number_curve_slot(NumberCurve::range(1.f, 2.f));
    REQUIRE(same_slot(a, b));
    JsonValue json;
    REQUIRE(slot_to_json(a, "NumberCurve", json));
    LuaSlot back;
    std::string error;
    REQUIRE(slot_from_json(json, "NumberCurve", "Size", back, error, nullptr));
    REQUIRE(back.kind == LuaSlot::Kind::NumberCurve);
    REQUIRE(*back.number_curve == NumberCurve::range(1.f, 2.f));
    REQUIRE_FALSE(slot_from_json(*parse_json("5"), "NumberCurve", "Size", back, error, nullptr));
    REQUIRE(error.find("Size") != std::string::npos);
}
```

Match `slot_to_json` / `slot_from_json` / `same_slot`'s real parameter lists from `PropertyReflection.hpp:16-22` when writing the test.

- [ ] **Step 2: Build; expect compile errors for the missing names.**

- [ ] **Step 3: Extend `LuaSlot`** (`LuaApi.hpp`)

```cpp
    enum class Kind { Nil, Bool, Number, String, Instance, Vec3, Color, Matrix4, Signal, Enum, Vec2, InputObject,
                      TouchObject, NumberCurve, ColorCurve };
    ...
    // Kind::NumberCurve / Kind::ColorCurve. Shared and immutable, so a slot
    // copied into undo history copies a pointer.
    std::shared_ptr<const engine_core::NumberCurve> number_curve;
    std::shared_ptr<const engine_core::ColorCurve> color_curve;
```

Add `#include "Curve.hpp"` and `<memory>`. Update the `lua_saved_property` comment: "by its type: number, boolean, string, Color3, Vector3, NumberCurve, or ColorCurve".

- [ ] **Step 4: Write `CurveJson.hpp/.cpp`**

```cpp
// CurveJson.hpp
#pragma once
#include "Curve.hpp"
#include "Json.hpp"      // same header PropertyReflection.cpp uses for JsonValue
#include "LuaApi.hpp"

#include <optional>
#include <string>

namespace engine_core {

LuaSlot number_curve_slot(NumberCurve curve);
LuaSlot color_curve_slot(ColorCurve curve);

JsonValue number_curve_to_json(const NumberCurve& curve);
std::optional<NumberCurve> number_curve_from_json(const JsonValue& value, std::string& error);
JsonValue color_curve_to_json(const ColorCurve& curve);
std::optional<ColorCurve> color_curve_from_json(const JsonValue& value, std::string& error);

// "Linear", "Smooth", "Step"; nullopt for any other name.
std::optional<CurveInterp> curve_interp_from_name(std::string_view name);
const char* curve_interp_name(CurveInterp interp);

}  // namespace engine_core
```

In `CurveJson.cpp`, build objects and arrays with the `JsonValue` API PropertyReflection.cpp already uses (`JsonValue::number`, `JsonValue::string`, the object/array builders and readers — copy their exact spelling from `PropertyReflection.cpp` and `Json.hpp`). Logic:

```cpp
LuaSlot number_curve_slot(NumberCurve curve) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::NumberCurve;
    slot.number_curve = std::make_shared<const NumberCurve>(std::move(curve));
    return slot;
}

LuaSlot color_curve_slot(ColorCurve curve) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::ColorCurve;
    slot.color_curve = std::make_shared<const ColorCurve>(std::move(curve));
    return slot;
}

// number_curve_to_json:
//   Constant   -> {"mode":"Constant","value":min}
//   Range      -> {"mode":"Range","min":min,"max":max}
//   Curve      -> {"mode":"Curve","keys":[[t,v,"Interp"],...]}
//   CurveRange -> {"mode":"CurveRange","keys":[...],"keys2":[...]}
// number_curve_from_json: not an object -> error "must be a NumberCurve object";
//   unknown/missing mode -> error "has an unknown mode"; a key that is not [number, number, optional string]
//   -> error "has a malformed key"; an unknown interp name -> error "has an unknown interpolation";
//   then NumberCurve::keys / keys_range, passing their error through.
// color_curve_to_json / from_json: same shape with ColorKey [t,[r,g,b],transparency];
//   Constant uses keys_a()[0]; RandomBetween uses keys_a()[0] and keys_b()[0] as color/color2.
```

Write the full functions (about 150 lines) with those exact messages.

- [ ] **Step 5: Teach PropertyReflection the two types**

In `slot_to_json`, before the final `else`:

```cpp
    } else if (type == "NumberCurve" && slot.kind == LuaSlot::Kind::NumberCurve && slot.number_curve) {
        out = number_curve_to_json(*slot.number_curve);
    } else if (type == "ColorCurve" && slot.kind == LuaSlot::Kind::ColorCurve && slot.color_curve) {
        out = color_curve_to_json(*slot.color_curve);
```

In `slot_from_json`:

```cpp
    } else if (type == "NumberCurve") {
        std::string why;
        auto curve = number_curve_from_json(value, why);
        if (!curve) {
            error = label + " " + why;
            return false;
        }
        out = number_curve_slot(std::move(*curve));
    } else if (type == "ColorCurve") {
        std::string why;
        auto curve = color_curve_from_json(value, why);
        if (!curve) {
            error = label + " " + why;
            return false;
        }
        out = color_curve_slot(std::move(*curve));
```

In `same_slot`, add cases comparing by value:

```cpp
        case LuaSlot::Kind::NumberCurve:
            return a.number_curve && b.number_curve && *a.number_curve == *b.number_curve;
        case LuaSlot::Kind::ColorCurve:
            return a.color_curve && b.color_curve && *a.color_curve == *b.color_curve;
```

In `DataModel.cpp` `type_fits`, add `if (type == "NumberCurve") return slot.kind == LuaSlot::Kind::NumberCurve;` and the ColorCurve line, in the style of the Color3 line there. Fix any `switch (slot.kind)` that the compiler now warns is not exhaustive (`Gui.cpp:223`): curves are not GUI values, so they go to the same branch as `Kind::Matrix4`.

- [ ] **Step 6: Add CurveJson.cpp to CMake; build and run**

Run: `build/Debug/sandbox.exe "[curve]"` → all pass. Also run `build/Debug/sandbox.exe` fully once to catch regressions from the `LuaSlot` change.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/CurveJson.* src/engine_core/LuaApi.hpp src/engine_core/PropertyReflection.cpp src/engine_core/DataModel.cpp src/engine_instances/Gui.cpp sandbox/curve_tests.cpp CMakeLists.txt
git commit -m "Curves: saved properties hold NumberCurve and ColorCurve as JSON"
```

---

### Task 3: NumberCurve and ColorCurve in Lua

**Files:**
- Create: `src/engine_datatypes/CurveLua.cpp`, add declarations to `src/engine_datatypes/Curve.hpp`
- Modify: `src/engine_core/ScriptRuntime.cpp:780-784`, `src/engine_core/ScriptBindings.cpp` (`push_registered` ~396-450, write branch ~575), `src/engine_core/LuaApi.cpp` (datatype docs after Color3, ~821), `src/ide/LuauHighlight.cpp:47`, `src/engine_datatypes/README.md`, `CMakeLists.txt`
- Test: `tests/LuaEngineTest.cpp`

**Interfaces:**
- Consumes: Task 1, Task 2 (`number_curve_slot`, `curve_interp_from_name`).
- Produces: `void open_number_curve(lua_State*)`, `void open_color_curve(lua_State*)`, `void push_number_curve(lua_State*, std::shared_ptr<const NumberCurve>)`, `std::shared_ptr<const NumberCurve> to_number_curve(lua_State*, int)` (null if not one), and the ColorCurve equivalents. Lua API exactly:

```lua
NumberCurve.new(value)                 NumberCurve.range(min, max)
NumberCurve.keys({ {t, v}, {t, v, "Smooth"} })   NumberCurve.keysRange(keysA, keysB)
c.Mode ("Constant"|"Range"|"Curve"|"CurveRange")  c.Min  c.Max  c.Keys  c.Keys2   -- read-only; Keys are {Time=,Value=,Interpolation=} tables
c:Evaluate(t, blend?)                  typeof(c) == "NumberCurve"   ==   tostring
ColorCurve.new(color, transparency?)   ColorCurve.between(a, ta, b, tb)
ColorCurve.gradient({ {t, color, transparency?} })   ColorCurve.gradientRange(keysA, keysB)
c.Mode ("Constant"|"RandomBetween"|"Gradient"|"GradientRange")  c.Keys  c.Keys2  c:Evaluate(t, blend?) -> Color3, transparency
```

- [ ] **Step 1: Write the failing test** (in `tests/LuaEngineTest.cpp`, next to `testColor3`, and call it from `main()`)

```cpp
void testCurves() {
    runner::Runner runner;
    runner.start();
    expectPrinted(runner, "local c = NumberCurve.new(5) print(c.Mode, c:Evaluate(0.3), typeof(c))",
                  "Constant\t5\tNumberCurve", "NumberCurve.new is a constant");
    expectPrinted(runner, "print(NumberCurve.range(1, 3):Evaluate(0, 0.5))", "2", "a range blends");
    expectPrinted(runner,
                  "local c = NumberCurve.keys({{1, 0}, {0, 0}, {0.5, 4, 'Step'}}) "
                  "print(#c.Keys, c.Keys[1].Time, c:Evaluate(0.75), c.Keys[2].Interpolation)",
                  "3\t0\t4\tStep", "keys sort and step holds");
    expectPrinted(runner, "print(NumberCurve.new(1) == NumberCurve.new(1))", "true", "curves compare by value");
    expectChunkError(runner, "NumberCurve.keys({})", "at least one key", "no keys is refused");
    expectChunkError(runner, "NumberCurve.keys({{2, 0}})", "between 0 and 1", "time outside 0..1 is refused");
    expectChunkError(runner, "NumberCurve.keys({{0, 0, 'Wobbly'}})", "interpolation", "unknown interpolation");
    expectChunkError(runner, "NumberCurve.keys({{0, 0/0}})", "finite", "NaN is refused");
    expectChunkError(runner, "local c = NumberCurve.new(1) c.Mode = 'Range'", "cannot be assigned",
                     "NumberCurve is read-only");
    expectPrinted(runner,
                  "local g = ColorCurve.gradient({{0, Color3.new(1, 0, 0)}, {1, Color3.new(0, 0, 1), 1}}) "
                  "local c, t = g:Evaluate(0.5) print(c.R, c.B, t, typeof(g))",
                  "0.5\t0.5\t0.5\tColorCurve", "a gradient blends color and transparency");
    expectChunkError(runner, "ColorCurve.gradient({{0, 5}})", "Color3", "a key needs a Color3");
    runner.stop();
}
```

- [ ] **Step 2: Build `engine-tests`; expect failure (`NumberCurve` is nil).**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Debug --target engine-tests --parallel && build/Debug/engine-tests.exe`

- [ ] **Step 3: Write `CurveLua.cpp`** following `Color3.cpp`'s structure (metatable with `__index` closure over a read-only methods table, `__newindex` raising "%s cannot be assigned to", `__eq`, `__tostring`, `__type`, library table set read-only then `lua_setglobal`), but the userdata holds a `shared_ptr`, so allocate with `lua_newuserdatadtor` as `RaycastBindings.cpp:43` does:

```cpp
namespace {
const char* kNumberCurveMeta = "AE.NumberCurve";
const char* kColorCurveMeta = "AE.ColorCurve";

struct NumberCurveUd { std::shared_ptr<const NumberCurve> curve; };
struct ColorCurveUd { std::shared_ptr<const ColorCurve> curve; };

void destroy_number_curve(lua_State*, void* p) { static_cast<NumberCurveUd*>(p)->~NumberCurveUd(); }
void destroy_color_curve(lua_State*, void* p) { static_cast<ColorCurveUd*>(p)->~ColorCurveUd(); }

// Reads {t, v, interp?} rows. Raises a Lua error naming what is wrong.
std::vector<NumberKey> read_number_keys(lua_State* state, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    std::vector<NumberKey> keys;
    const int count = lua_objlen(state, index);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        if (!lua_istable(state, -1)) {
            luaL_error(state, "key %d must be a table {time, value, interpolation?}", i);
        }
        lua_rawgeti(state, -1, 1);
        lua_rawgeti(state, -2, 2);
        lua_rawgeti(state, -3, 3);
        if (!lua_isnumber(state, -3) || !lua_isnumber(state, -2)) {
            luaL_error(state, "key %d needs a number time and value", i);
        }
        NumberKey key;
        key.time = static_cast<float>(lua_tonumber(state, -3));
        key.value = static_cast<float>(lua_tonumber(state, -2));
        if (!lua_isnil(state, -1)) {
            const char* name = lua_tostring(state, -1);
            const auto interp = name != nullptr ? curve_interp_from_name(name) : std::nullopt;
            if (!interp) {
                luaL_error(state, "key %d has an unknown interpolation (Linear, Smooth, or Step)", i);
            }
            key.interp = *interp;
        }
        lua_pop(state, 4);
        keys.push_back(key);
    }
    return keys;
}
}  // namespace
```

`curve_interp_from_name` lives in engine_core's CurveJson; if engine_datatypes cannot depend on engine_core, move `curve_interp_from_name`/`curve_interp_name` into `Curve.hpp/.cpp` instead and have CurveJson use them from there.

Constructors: `new` (`luaL_checknumber`), `range` (two numbers), `keys` (`read_number_keys` then `NumberCurve::keys`; on failure `luaL_error(state, "%s", error.c_str())`), `keysRange`. Readers: `Mode` (name string), `Min`, `Max`, `Keys`/`Keys2` (new array of `{Time=, Value=, Interpolation=}` tables), method `Evaluate(t, blend=0)`. `__eq` compares `*a.curve == *b.curve`. `__tostring` returns e.g. `"NumberCurve(Range 1..3)"`. ColorCurve mirrors it; keys are `{t, Color3, transparency?}` read with `to_color3` (from `Color3.hpp`), raising `"key %d needs a Color3"`; `Evaluate` returns two values, a Color3 and the transparency (`1 - a`). Export `push_number_curve`, `to_number_curve`, `push_color_curve`, `to_color_curve`, `open_number_curve`, `open_color_curve`.

Register for analysis with `ANARCHY_LUA_REGISTER(register_curve_lua)` exactly as Color3.cpp:173-189 does, with fields built inside the function: properties `Mode` (string), `Min`, `Max` (number), `Keys`, `Keys2` (`{any}`), method `Evaluate`; `lua_note_result` for each constructor; an `__eq` operator.

- [ ] **Step 4: Wire into the runtime and bindings**

- `ScriptRuntime.cpp` `open_host_libraries`: add `open_number_curve(state); open_color_curve(state);` after `open_color3(state);`.
- `ScriptBindings.cpp` `push_registered`: add
  ```cpp
  case LuaSlot::Kind::NumberCurve: push_number_curve(state, slot.number_curve); return;
  case LuaSlot::Kind::ColorCurve: push_color_curve(state, slot.color_curve); return;
  ```
- Property write branch (`type == "Color3"` area):
  ```cpp
  } else if (type == "NumberCurve") {
      auto curve = to_number_curve(state, 3);
      if (!curve) {
          // A plain number is a Constant, so `emitter.Rate = 50` works.
          if (lua_isnumber(state, 3)) {
              curve = std::make_shared<const NumberCurve>(NumberCurve::constant(static_cast<float>(lua_tonumber(state, 3))));
          } else {
              luaL_error(state, "%s expects a NumberCurve", field->name);
          }
      }
      slot.kind = LuaSlot::Kind::NumberCurve;
      slot.number_curve = std::move(curve);
  } else if (type == "ColorCurve") {
      auto curve = to_color_curve(state, 3);
      if (!curve && read_color3(state, 3, slot.color)) {
          curve = std::make_shared<const ColorCurve>(ColorCurve::constant(slot.color, 0.f));   // a Color3 is a Constant
      }
      if (!curve) {
          luaL_error(state, "%s expects a ColorCurve", field->name);
      }
      slot.kind = LuaSlot::Kind::ColorCurve;
      slot.color_curve = std::move(curve);
  ```
  Add a test line to `testCurves` once Task 5 exists (`emitter.Rate = 50` then `print(emitter.Rate.Mode)` → `Constant`); for now the number/Color3 shortcut is exercised in Task 5's Lua test.
- `LuaApi.cpp` docs after the Color3 block: `add("", "NumberCurve", ...)`, each constructor with `P(...)` params, `Mode`, `Min`, `Max`, `Keys`, `Keys2`, `Evaluate`; the same for ColorCurve. One plain sentence each, e.g. `add("NumberCurve", "range", "A number picked at random between min and max, once per use.", "NumberCurve", false, {P("min", "number"), P("max", "number")});`.
- `LuauHighlight.cpp` `kWords`: add `"NumberCurve"`, `"ColorCurve"`.
- `engine_datatypes/README.md`: one paragraph describing both types, modes, immutability, and that a number or Color3 written to such a property becomes a Constant.

- [ ] **Step 5: Build `engine-tests` and run; expect all pass.**

- [ ] **Step 6: Commit**

```bash
git add src/engine_datatypes/CurveLua.cpp src/engine_datatypes/Curve.hpp src/engine_datatypes/README.md src/engine_core/ScriptRuntime.cpp src/engine_core/ScriptBindings.cpp src/engine_core/LuaApi.cpp src/ide/LuauHighlight.cpp tests/LuaEngineTest.cpp CMakeLists.txt
git commit -m "Curves: NumberCurve and ColorCurve in Lua"
```

---

### Task 4: Particle enums and the grey-out rule

**Files:**
- Modify: `src/engine_datatypes/Enum.hpp`, `src/engine_datatypes/Enum.cpp` (entries near 192, types near 250, `kTypes` 282-290, accessors near 368), `src/engine_core/LuaApi.hpp` (LuaField + helper), `src/ide/PropertySheet.hpp/.cpp` (`PropertyRow::enabled`, `read_sheet` ~397), `src/ide/PropertiesPanel.cpp` (`make_view` 1081-1240: `setDisable(!row.writable || !row.enabled)` on every editor)
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // Enum.hpp
  const EnumType& particle_shape_enum();            enum class ParticleShape { Point = 0, Box = 1, Sphere = 2, Hemisphere = 3, Cylinder = 4, Cone = 5, Disc = 6 };
  const EnumType& particle_emit_from_enum();        enum class ParticleEmitFrom { Volume = 0, Surface = 1, Edge = 2 };
  const EnumType& particle_collision_enum();        enum class ParticleCollision { None = 0, Terrain = 1, World = 2 };
  const EnumType& particle_collision_response_enum(); enum class ParticleCollisionResponse { Bounce = 0, Stick = 1, Die = 2 };
  const EnumType& particle_texture_mode_enum();     enum class ParticleTextureMode { Static = 0, Flipbook = 1 };
  const EnumType& particle_facing_enum();           enum class ParticleFacing { Camera = 0, CameraUpright = 1, Velocity = 2, World = 3 };
  const EnumType& particle_blend_mode_enum();       enum class ParticleBlendMode { Normal = 0, Additive = 1, Multiply = 2 };
  // LuaApi.hpp
  using LuaEnabled = bool (*)(DataModel& world, DataModel& object);
  LuaEnabled LuaField::enabled_when = nullptr;     // null: always enabled
  LuaField lua_enabled_when(LuaField field, LuaEnabled predicate);
  // PropertySheet.hpp
  bool PropertyRow::enabled = true;               // false: shown, not editable
  ```
  Lua names: `Enum.ParticleShape`, `Enum.ParticleEmitFrom`, `Enum.ParticleCollision`, `Enum.ParticleCollisionResponse`, `Enum.ParticleTextureMode`, `Enum.ParticleFacing`, `Enum.ParticleBlendMode`.

- [ ] **Step 1: Write the failing test.** No class uses the grey-out rule until Task 5, whose `TestParticleEmitterRows` covers it; this task tests the enum registration in Lua:

```cpp
// tests/LuaEngineTest.cpp, in testCurves or a new testParticleEnums called from main()
expectPrinted(runner, "print(Enum.ParticleShape.Cone.Value, Enum.ParticleBlendMode.Multiply.Name, #Enum.ParticleFacing:GetEnumItems())",
              "5\tMultiply\t4", "particle enums are registered");
```

Use whatever enum-listing method `Enum.cpp` exposes (`GetEnumItems` or similar); check an existing enum test for the exact spelling.

- [ ] **Step 2: Run; expect failure.**

- [ ] **Step 3: Add the seven enums** in `Enum.cpp`, each as `kTransformSpaces`/`kTransformSpaceType` is (entries array, `EnumType`, append to `kTypes`, accessor), and declare accessor + C++ enum class in `Enum.hpp` with a one-line comment naming the property that uses it.

- [ ] **Step 4: Add the grey-out rule**

`LuaApi.hpp`, in `LuaField` after `shown_when_items`:

```cpp
    // Properties shows the field disabled, visible but not editable, while
    // this returns false. Read under the lock each time the sheet is read.
    // A display rule like shown_when: scripts still write it. See lua_enabled_when.
    bool (*enabled_when)(DataModel& world, DataModel& object) = nullptr;
```

and after `lua_shown_when`:

```cpp
// field, shown disabled in Properties while predicate(world, object) is false.
inline LuaField lua_enabled_when(LuaField field, bool (*predicate)(DataModel& world, DataModel& object)) {
    field.enabled_when = predicate;
    return field;
}
```

`PropertySheet.hpp` `PropertyRow`: `bool enabled = true;`. In `read_sheet`, where a row is built for each selected object, AND it: `row.enabled = row.enabled && (own->enabled_when == nullptr || own->enabled_when(world, *object));` (a multi-selection row is enabled only if every object enables it). `PropertiesPanel.cpp` `make_view`: wherever an editor calls `setDisable(!row.writable)`, use `setDisable(!row.writable || !row.enabled)`; where an editor has no `setDisable` call (fields made by `make_field`, the slider), add one. When a sheet re-read flips `enabled`, the existing row-diff path must rebuild or re-disable the view: find where `writable` changes are applied on refresh and treat `enabled` the same way. `apply_edit` must refuse a write to a disabled row from the panel: in `PropertiesPanel` commit paths, return early if `!view.row.enabled`. MCP's `read_sheet(..., with_hidden=true)` reports `enabled` in its row JSON (`McpTools.cpp`, beside `writable`).

- [ ] **Step 5: Build `engine-tests` and `properties-tests`; run both; expect all pass.**

- [ ] **Step 6: Commit**

```bash
git add src/engine_datatypes/Enum.* src/engine_core/LuaApi.hpp src/ide/PropertySheet.* src/ide/PropertiesPanel.cpp src/ide/McpTools.cpp tests/LuaEngineTest.cpp
git commit -m "Particles: enums, and a Properties rule that greys a row out"
```

---

### Task 5: The ParticleEmitter class and Workspace.ParticleBudget

**Files:**
- Create: `src/engine_instances/ParticleEmitter.hpp`, `src/engine_instances/ParticleEmitter.cpp`
- Modify: `CMakeLists.txt:611`, `src/engine_core/Project.cpp:32,159`, `src/engine_core/ScriptBindings.cpp:36,206,269` (creatable; methods come in Task 9), `src/engine_core/LuaApi.cpp` (docs, after SoundEmitter ~1587), `src/ide/ClassOrder.hpp:45`, `src/ide/IdeIcons.cpp:53`, `src/engine_services/SceneService.hpp/.cpp` (ParticleBudget), `src/engine_instances/README.md`, `README.md`
- Test: `sandbox/particle_emitter_tests.cpp`, `tests/PropertiesTest.cpp`, `tests/LuaEngineTest.cpp`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces (`ParticleEmitter.hpp`):

```cpp
namespace engine_core {

struct ParticleBurst {
    float time = 0.f;                                   // seconds into the cycle, >= 0
    NumberCurve count = NumberCurve::constant(10.f);    // Constant or Range only
    bool operator==(const ParticleBurst&) const = default;
};

// Emits particles from its parent PVInstance (spec docs/superpowers/specs/2026-10-12-particle-emitter-design.md).
// ParticleWorld simulates it in Workspace, stopped or playing. Engine-side
// writes (Playing at a one-shot's end, ParticleCount) go through store_* and
// record nothing.
class ParticleEmitter : public DataModel {
public:
    enum Dirty : std::uint32_t {
        kDirtySettings = 1u << 0,   // any property the sim reads, rebake
        kDirtyRestart = 1u << 1,    // Playing went true, or Seed changed
        kDirtyAll = 0x3u,
    };

    ParticleEmitter(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "ParticleEmitter"; }
    bool particle_emitter() const override { return true; }

    // Emission
    bool playing() const;                      std::optional<std::string> set_playing(bool value);
    const NumberCurve& rate() const;           std::optional<std::string> set_rate(NumberCurve value);         // >= 0 everywhere
    const std::vector<ParticleBurst>& bursts() const; std::optional<std::string> set_bursts(std::vector<ParticleBurst> value);
    double duration() const;                   std::optional<std::string> set_duration(double value);          // > 0
    bool looping() const;                      std::optional<std::string> set_looping(bool value);
    bool prewarm() const;                      std::optional<std::string> set_prewarm(bool value);
    int max_particles() const;                 std::optional<std::string> set_max_particles(double value);     // integer >= 0
    int seed() const;                          std::optional<std::string> set_seed(double value);              // integer
    // Shape
    ParticleShape shape() const;               std::optional<std::string> set_shape(int value);
    ParticleEmitFrom emit_from() const;        std::optional<std::string> set_emit_from(int value);
    double arc() const;                        std::optional<std::string> set_arc(double value);               // 0..360
    Vec3 emit_direction() const;               std::optional<std::string> set_emit_direction(Vec3 value);      // normalised; zero refused
    TransformSpace emit_direction_space() const; std::optional<std::string> set_emit_direction_space(int value);
    Vec2 emit_spread() const;                  std::optional<std::string> set_emit_spread(Vec2 value);         // each 0..180
    // Particle
    const NumberCurve& lifetime() const;       std::optional<std::string> set_lifetime(NumberCurve value);     // > 0 everywhere
    const NumberCurve& emit_speed() const;     std::optional<std::string> set_emit_speed(NumberCurve value);
    const NumberCurve& emit_rotation() const;  std::optional<std::string> set_emit_rotation(NumberCurve value);
    const NumberCurve& rotation_speed() const; std::optional<std::string> set_rotation_speed(NumberCurve value);
    // Motion
    TransformSpace simulation_space() const;   std::optional<std::string> set_simulation_space(int value);
    double inherit_velocity() const;           std::optional<std::string> set_inherit_velocity(double value);  // 0..1
    Vec3 acceleration() const;                 std::optional<std::string> set_acceleration(Vec3 value);
    TransformSpace acceleration_space() const; std::optional<std::string> set_acceleration_space(int value);
    const NumberCurve& drag() const;           std::optional<std::string> set_drag(NumberCurve value);         // >= 0
    const NumberCurve& orbit_speed() const;    std::optional<std::string> set_orbit_speed(NumberCurve value);
    Vec3 orbit_axis() const;                   std::optional<std::string> set_orbit_axis(Vec3 value);          // normalised; zero refused
    // Collision
    ParticleCollision collision() const;       std::optional<std::string> set_collision(int value);
    ParticleCollisionResponse collision_response() const; std::optional<std::string> set_collision_response(int value);
    double bounciness() const;                 std::optional<std::string> set_bounciness(double value);        // 0..1
    double friction() const;                   std::optional<std::string> set_friction(double value);          // 0..1
    const std::string& collision_group() const; std::optional<std::string> set_collision_group(std::string value);
    // Appearance
    const std::string& texture_guid() const;   // Texture? reference, as SoundEmitter's Sound
    ParticleTextureMode texture_mode() const;  std::optional<std::string> set_texture_mode(int value);
    Vec2 flipbook_grid() const;                std::optional<std::string> set_flipbook_grid(Vec2 value);       // integers >= 1
    const ColorCurve& color() const;           std::optional<std::string> set_color(ColorCurve value);
    const NumberCurve& size() const;           std::optional<std::string> set_size(NumberCurve value);         // >= 0
    double brightness() const;                 std::optional<std::string> set_brightness(double value);        // >= 0
    const NumberCurve& stretch() const;        std::optional<std::string> set_stretch(NumberCurve value);
    // Rendering
    ParticleFacing facing() const;             std::optional<std::string> set_facing(int value);
    Vec3 facing_axis() const;                  std::optional<std::string> set_facing_axis(Vec3 value);         // normalised; zero refused
    TransformSpace facing_axis_space() const;  std::optional<std::string> set_facing_axis_space(int value);
    double velocity_stretch() const;           std::optional<std::string> set_velocity_stretch(double value);  // >= 0
    ParticleBlendMode blend_mode() const;      std::optional<std::string> set_blend_mode(int value);
    double soft_edge() const;                  std::optional<std::string> set_soft_edge(double value);         // >= 0
    double light_influence() const;            std::optional<std::string> set_light_influence(double value);   // 0..1
    int draw_order() const;                    std::optional<std::string> set_draw_order(double value);        // integer

    int particle_count() const { return particle_count_; }

    // ParticleWorld's side.
    std::uint32_t take_dirty() { const std::uint32_t out = dirty_; dirty_ = 0; return out; }
    // Emit(count) calls not yet spawned, and whether Clear() was called.
    int take_emit_requests() { const int out = emit_requests_; emit_requests_ = 0; return out; }
    bool take_clear() { const bool out = clear_requested_; clear_requested_ = false; return out; }
    void request_emit(int count);   // Emit; count clamped to >= 0
    void request_clear();           // Clear
    // A one-shot's end: Playing false, Changed fired, nothing recorded.
    void store_finished_cycle();
    // Fires Changed for ParticleCount only when the count moved.
    void store_particle_count(int count);

protected:
    void on_reuse() override;
    void read_place(const std::byte* data, std::size_t size) override;

private:
    // members with the spec's defaults (Rate 20, Duration 5, Looping true, Lifetime Range 1..2, EmitSpeed 5,
    // Shape Box, EmitDirection (0,1,0), Size 1, Color white, Brightness 1, MaxParticles 1000, SoftEdge 0.5,
    // LightInfluence 1, VelocityStretch 0.1, Bounciness 0.5, Friction 0.2, CollisionGroup "Default",
    // FlipbookGrid (1,1), Arc 360, OrbitAxis (0,1,0), FacingAxis (0,1,0), Playing true, ...)
    std::uint32_t dirty_ = kDirtyAll;
    int emit_requests_ = 0;
    bool clear_requested_ = false;
    int particle_count_ = 0;
    InstanceRef texture_ref_;
};

}  // namespace engine_core
```

Add `virtual bool particle_emitter() const { return false; }` to `DataModel.hpp` next to `sound_source()` (Task 9 adds the flecs tag that reads it). `Vec2` is the struct Vector2.hpp declares; use its real name.

`Workspace` (`SceneService.hpp`): `static constexpr int kDefaultParticleBudget = 20000; int particle_budget() const; std::optional<std::string> set_particle_budget(double value);` — refuses a non-integer or negative value; registered as `lua_saved_property("ParticleBudget", "number", read_particle_budget, write_particle_budget, "20000")` after Gravity, with the setter pattern of `set_gravity` (SceneService.cpp:57-71).

- [ ] **Step 1: Write the failing tests**

```cpp
// sandbox/particle_emitter_tests.cpp
#include "ParticleEmitter.hpp"
#include "SceneService.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace engine_core;

namespace {
ParticleEmitter& add_emitter(Game& game) {
    auto& emitter = game.create<ParticleEmitter>();
    emitter.set_parent(game.scene_service("Workspace"));   // use the parenting call bloom_tests' add_bloom uses
    return emitter;
}
}  // namespace

TEST_CASE("PE1 a new ParticleEmitter has the spec's defaults and saves nothing", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& emitter = add_emitter(game);
    PropertyBag saved;
    emitter.save_properties(saved);
    REQUIRE(saved.empty());
    REQUIRE(emitter.playing());
    REQUIRE(emitter.rate() == NumberCurve::constant(20.f));
    REQUIRE(emitter.lifetime() == NumberCurve::range(1.f, 2.f));
    REQUIRE(emitter.max_particles() == 1000);
    REQUIRE(emitter.shape() == ParticleShape::Box);
    REQUIRE(emitter.soft_edge() == 0.5);
    REQUIRE(emitter.collision_group() == "Default");
}

TEST_CASE("PE2 writes are checked, undo as one step, save, and come back at Stop", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& emitter = add_emitter(game);
    begin_step(game, "Set Rate");
    REQUIRE_FALSE(emitter.set_rate(NumberCurve::range(5.f, 10.f)));
    end_step(game);
    REQUIRE(emitter.rate() == NumberCurve::range(5.f, 10.f));
    game.history().undo();
    REQUIRE(emitter.rate() == NumberCurve::constant(20.f));
    game.history().redo();

    REQUIRE(emitter.set_duration(0.0).has_value());                 // > 0
    REQUIRE(emitter.set_rate(NumberCurve::constant(-1.f)).has_value());
    REQUIRE(emitter.set_emit_direction(Vec3{0, 0, 0}).has_value());
    REQUIRE(emitter.set_max_particles(1.5).has_value());
    REQUIRE(emitter.set_flipbook_grid(Vec2{0.f, 2.f}).has_value());
    REQUIRE(emitter.set_lifetime(NumberCurve::range(0.f, 1.f)).has_value());   // > 0 everywhere
    REQUIRE_FALSE(emitter.set_emit_direction(Vec3{0, 0, 5}));
    REQUIRE(emitter.emit_direction().z == 1.f);

    PropertyBag saved;
    emitter.save_properties(saved);
    REQUIRE(saved.count("Rate") == 1);
    // Stop restores: start, change, stop.
    game.start_simulation();
    REQUIRE_FALSE(emitter.set_rate(NumberCurve::constant(99.f)));
    game.stop_simulation();
    REQUIRE(emitter.rate() == NumberCurve::range(5.f, 10.f));
}

TEST_CASE("PE3 Workspace.ParticleBudget defaults to 20000 and refuses negatives", "[particles]") {
    SimRole role;
    Game game;
    auto* workspace = dynamic_cast<Workspace*>(game.instance(game.scene_service("Workspace").id()));
    REQUIRE(workspace->particle_budget() == 20000);
    REQUIRE(workspace->set_particle_budget(-1).has_value());
    REQUIRE_FALSE(workspace->set_particle_budget(0));
}
```

Match the calls (`create`, parenting, `scene_service`, `start_simulation`) to the ones `bloom_tests.cpp` and `audio_tests.cpp` use.

Properties (`tests/PropertiesTest.cpp`, called from `main()`):

```cpp
void TestParticleEmitterRows() {
    Rig rig;
    const InstanceId emitter = rig.add<engine_core::ParticleEmitter>("Emitter");
    rig.select({emitter});
    Expect(rig.hasRow("Rate"), "a NumberCurve row shows");
    Expect(rig.hasRow("Color"), "a ColorCurve row shows");
    Expect(!rig.hasRow("FlipbookGrid"), "a Static texture hides FlipbookGrid");
    Expect(!rig.hasRow("FacingAxis"), "Camera facing hides FacingAxis");
    const ide::PropertyRow* from = rig.panel.sheet().find("EmitFrom");
    Expect(from != nullptr && !from->enabled, "EmitFrom is greyed while the emitter emits from a point (Workspace parent)");
    const ide::PropertyRow* group = rig.panel.sheet().find("CollisionGroup");
    Expect(group != nullptr && !group->enabled, "CollisionGroup is greyed unless Collision is World");
    Expect(rig.hasRow("Bounciness"), "Bounciness shows for Bounce");
}
```

(`PropertyKind` must learn `NumberCurve` and `ColorCurve` for these rows to exist; this task adds the kinds with a read-only text view — "5", "1 – 3", "Curve (3 keys)", "Gradient (2 keys)" — and Task 15 replaces it with the popover editors.)

Lua (`tests/LuaEngineTest.cpp`):

```cpp
expectPrinted(runner,
              "local e = Instance.new('ParticleEmitter') e.Rate = 50 e.Color = Color3.new(1, 0, 0) "
              "print(e.Rate.Mode, e.Rate:Evaluate(0), e.Color.Mode, e.MaxParticles)",
              "Constant\t50\tConstant\t1000", "numbers and Color3s become Constant curves");
expectChunkError(runner, "Instance.new('ParticleEmitter').Duration = 0", "Duration", "Duration must be above 0");
```

- [ ] **Step 2: Build sandbox, properties-tests, engine-tests; expect failures.**

- [ ] **Step 3: Implement `ParticleEmitter.cpp`**

Follow `SoundEmitter.cpp` exactly: private generic setters (`set_number`, `set_bool`, `set_enum`, `set_vec3_dir`, `set_curve`, `set_color_curve`), each `require_thread(*this)`, validate, early-return on unchanged, store, `dirty_ |= kDirtySettings` (plus `kDirtyRestart` for Playing→true and Seed), `note_property_change(name, before_slot, after_slot)`. Curve validation helper:

```cpp
// Every value the curve can produce, for a range check: the constant, both
// range ends, or every key of both curves.
template <class Check>
bool curve_values_ok(const NumberCurve& curve, Check ok) {
    switch (curve.mode()) {
        case NumberCurveMode::Constant: return ok(curve.min());
        case NumberCurveMode::Range: return ok(curve.min()) && ok(curve.max());
        default: break;
    }
    for (const NumberKey& key : curve.keys_a()) if (!ok(key.value)) return false;
    for (const NumberKey& key : curve.keys_b()) if (!ok(key.value)) return false;
    return true;
}
```

Messages: `"Rate must not be negative"`, `"Lifetime must be above 0"`, `"Duration must be above 0"`, `"EmitDirection must not be zero"`, `"MaxParticles must be a whole number, 0 or more"`, `"FlipbookGrid must be whole numbers, 1 or more"`, `"Arc must be from 0 to 360"`, `"EmitSpread must be from 0 to 180 on each axis"`, `"Size must not be negative"`, `"Drag must not be negative"`, etc. Smooth keys can overshoot between keys; validation checks key values only, and the sim clamps sampled Lifetime to `>= 1e-3`, Size/Drag/Rate to `>= 0`.

`set_playing(true)` while already true is a no-op (no restart); false→true sets `kDirtyRestart`. `store_finished_cycle()`: `playing_ = false; emit_property("Playing");`. `store_particle_count(n)`: if changed, `particle_count_ = n; emit_property("ParticleCount");`. `read_place` calls the base then `emit_requests_ = 0; clear_requested_ = true; dirty_ = kDirtyAll;`. `on_reuse` resets every member to its default.

The Texture reference copies SoundEmitter's `Sound?` read/write thunks (`SoundEmitter.cpp:170-190`) with `"Texture?"`.

Bursts: saved through `ParticleBursts` (Task 6). In this task register `Bursts` as `lua_hidden(lua_json_text(lua_saved_property("Bursts", "string", ...)))` holding `"[]"`/`"null"` so the class compiles; Task 6 replaces it.

Registration block (order is the panel's):

```cpp
ANARCHY_LUA_REGISTER(register_particle_emitter_lua) {
    const LuaField fields[] = {
        lua_lead_group("Emission"),
        lua_saved_property("Playing", "boolean", read_playing, write_playing, "true"),
        lua_saved_property("Rate", "NumberCurve", read_rate, write_rate, R"({"mode":"Constant","value":20})"),
        /* Bursts (Task 6) */
        lua_saved_property("Duration", "number", read_duration, write_duration, "5"),
        lua_saved_property("Looping", "boolean", read_looping, write_looping, "true"),
        lua_saved_property("Prewarm", "boolean", read_prewarm, write_prewarm, "false"),
        lua_saved_property("MaxParticles", "number", read_max_particles, write_max_particles, "1000"),
        lua_saved_property("Seed", "number", read_seed, write_seed, "0"),
        lua_group("Shape"),
        lua_saved_enum("Shape", particle_shape_enum(), read_shape, write_shape, "\"Box\""),
        lua_enabled_when(lua_saved_enum("EmitFrom", particle_emit_from_enum(), read_emit_from, write_emit_from, "\"Volume\""),
                         emits_from_area),
        lua_enabled_when(lua_slider(lua_saved_property("Arc", "number", read_arc, write_arc, "360"), 0.0, 360.0),
                         arc_applies),
        lua_saved_property("EmitDirection", "Vector3", read_emit_direction, write_emit_direction, "[0,1,0]"),
        lua_saved_enum("EmitDirectionSpace", transform_space_enum(), read_emit_direction_space, write_emit_direction_space, "\"Local\""),
        lua_saved_property("EmitSpread", "Vector2", read_emit_spread, write_emit_spread, "[0,0]"),
        lua_group("Particle"),
        lua_saved_property("Lifetime", "NumberCurve", read_lifetime, write_lifetime, R"({"mode":"Range","min":1,"max":2})"),
        lua_saved_property("EmitSpeed", "NumberCurve", read_emit_speed, write_emit_speed, R"({"mode":"Constant","value":5})"),
        lua_saved_property("EmitRotation", "NumberCurve", read_emit_rotation, write_emit_rotation, R"({"mode":"Constant","value":0})"),
        lua_saved_property("RotationSpeed", "NumberCurve", read_rotation_speed, write_rotation_speed, R"({"mode":"Constant","value":0})"),
        lua_group("Motion"),
        lua_saved_enum("SimulationSpace", transform_space_enum(), read_simulation_space, write_simulation_space, "\"World\""),
        lua_enabled_when(lua_slider(lua_saved_property("InheritVelocity", "number", read_inherit_velocity, write_inherit_velocity, "0"), 0.0, 1.0),
                         simulates_in_world),
        lua_saved_property("Acceleration", "Vector3", read_acceleration, write_acceleration, "[0,0,0]"),
        lua_saved_enum("AccelerationSpace", transform_space_enum(), read_acceleration_space, write_acceleration_space, "\"World\""),
        lua_saved_property("Drag", "NumberCurve", read_drag, write_drag, R"({"mode":"Constant","value":0})"),
        lua_saved_property("OrbitSpeed", "NumberCurve", read_orbit_speed, write_orbit_speed, R"({"mode":"Constant","value":0})"),
        lua_saved_property("OrbitAxis", "Vector3", read_orbit_axis, write_orbit_axis, "[0,1,0]"),
        lua_group("Collision"),
        lua_saved_enum("Collision", particle_collision_enum(), read_collision, write_collision, "\"None\""),
        lua_enabled_when(lua_saved_enum("CollisionResponse", particle_collision_response_enum(), read_collision_response, write_collision_response, "\"Bounce\""),
                         collides),
        lua_shown_when(lua_slider(lua_saved_property("Bounciness", "number", read_bounciness, write_bounciness, "0.5"), 0.0, 1.0),
                       "CollisionResponse", {static_cast<int>(ParticleCollisionResponse::Bounce)}),
        lua_shown_when(lua_slider(lua_saved_property("Friction", "number", read_friction, write_friction, "0.2"), 0.0, 1.0),
                       "CollisionResponse", {static_cast<int>(ParticleCollisionResponse::Bounce)}),
        lua_enabled_when(lua_choices(lua_saved_property("CollisionGroup", "string", read_collision_group, write_collision_group, "\"Default\""),
                                     collision_group_choices, nullptr),
                         collides_with_world),
        lua_group("Appearance"),
        lua_saved_property("Texture", "Texture?", read_texture, write_texture, "null"),
        lua_saved_enum("TextureMode", particle_texture_mode_enum(), read_texture_mode, write_texture_mode, "\"Static\""),
        lua_shown_when(lua_saved_property("FlipbookGrid", "Vector2", read_flipbook_grid, write_flipbook_grid, "[1,1]"),
                       "TextureMode", {static_cast<int>(ParticleTextureMode::Flipbook)}),
        lua_saved_property("Color", "ColorCurve", read_color, write_color, R"({"mode":"Constant","color":[1,1,1],"transparency":0})"),
        lua_saved_property("Size", "NumberCurve", read_size, write_size, R"({"mode":"Constant","value":1})"),
        lua_saved_property("Brightness", "number", read_brightness, write_brightness, "1"),
        lua_saved_property("Stretch", "NumberCurve", read_stretch, write_stretch, R"({"mode":"Constant","value":0})"),
        lua_group("Rendering"),
        lua_saved_enum("Facing", particle_facing_enum(), read_facing, write_facing, "\"Camera\""),
        lua_shown_when(lua_saved_property("FacingAxis", "Vector3", read_facing_axis, write_facing_axis, "[0,1,0]"),
                       "Facing", {static_cast<int>(ParticleFacing::World)}),
        lua_shown_when(lua_saved_enum("FacingAxisSpace", transform_space_enum(), read_facing_axis_space, write_facing_axis_space, "\"Local\""),
                       "Facing", {static_cast<int>(ParticleFacing::World)}),
        lua_shown_when(lua_saved_property("VelocityStretch", "number", read_velocity_stretch, write_velocity_stretch, "0.1"),
                       "Facing", {static_cast<int>(ParticleFacing::Velocity)}),
        lua_saved_enum("BlendMode", particle_blend_mode_enum(), read_blend_mode, write_blend_mode, "\"Normal\""),
        lua_saved_property("SoftEdge", "number", read_soft_edge, write_soft_edge, "0.5"),
        lua_slider(lua_saved_property("LightInfluence", "number", read_light_influence, write_light_influence, "1"), 0.0, 1.0),
        lua_saved_property("DrawOrder", "number", read_draw_order, write_draw_order, "0"),
        lua_property("ParticleCount", "number", false, read_particle_count, nullptr),
        lua_event("Finished"),
    };
    register_lua_class("ParticleEmitter", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("ParticleEmitter", {"Workspace", "PVInstance"});
}
```

The predicates:

```cpp
// The parent gives the shape a size: a PhysicsObject, a Brush, or a GameObject drawing a Prefab.
bool parent_has_size(DataModel& world, DataModel& object);   // implemented in Task 7's emitter_extent; here: dynamic_cast checks
bool emits_from_area(DataModel& world, DataModel& object) {
    auto* e = dynamic_cast<ParticleEmitter*>(&object);
    return e != nullptr && e->shape() != ParticleShape::Point && parent_has_size(world, object);
}
bool arc_applies(DataModel& world, DataModel& object) {
    auto* e = dynamic_cast<ParticleEmitter*>(&object);
    return emits_from_area(world, object) && e->shape() != ParticleShape::Box;
}
bool simulates_in_world(DataModel&, DataModel& object) {
    auto* e = dynamic_cast<ParticleEmitter*>(&object);
    return e != nullptr && e->simulation_space() == TransformSpace::World;
}
bool collides(DataModel&, DataModel& object) {
    auto* e = dynamic_cast<ParticleEmitter*>(&object);
    return e != nullptr && e->collision() != ParticleCollision::None;
}
bool collides_with_world(DataModel&, DataModel& object) {
    auto* e = dynamic_cast<ParticleEmitter*>(&object);
    return e != nullptr && e->collision() == ParticleCollision::World;
}
```

`parent_has_size`: parent is a `PhysicsObject`, a `Brush`, or a `GameObject` whose `prefab()` slot names a live `Prefab`. `collision_group_choices` lists `world.collision_groups()` names (see how PhysicsBase's `CollisionGroup` row offers choices and reuse that function).

- [ ] **Step 4: Register the class** in every site from the spec's list: CMake source, `Project.cpp` factory (`{"ParticleEmitter", [](DataModel& world) -> DataModel& { return world.create<ParticleEmitter>(); }}`), `ScriptBindings.cpp` `create_particle_emitter` + `register_lua_creatable("ParticleEmitter", create_particle_emitter);`, `ClassOrder.hpp` (`{"ParticleEmitter", 9},`), `IdeIcons.cpp` (`if (class_name == "ParticleEmitter") { return "Sparkles.png"; }` — add a 16×16 `Sparkles.png` beside the other icons, or reuse the closest existing icon if adding art is out of reach, and say which), `LuaApi.cpp` docs (one `add("ParticleEmitter", ...)` line per property with a plain sentence and its time axis, e.g. `"Particles per second. A curve runs over one cycle (Duration)."`), README class lists.

Add `PropertyKind::NumberCurve` and `PropertyKind::ColorCurve` in `PropertySheet` (`property_kind_for`: `"NumberCurve"` / `"ColorCurve"`), shown in `make_view` as a read-only summary label for now (`curve_summary(const NumberCurve&)` → `"5"`, `"1 – 3"`, `"Curve (3 keys)"`, `"Curve range"`; `color_curve_summary` → `"Color"`, `"Random between"`, `"Gradient (2 keys)"`, `"Gradient range"`), and teach `McpTools.cpp:265-280` the two type names.

- [ ] **Step 5: Build and run all three suites; expect pass.**

Run: `build/Debug/sandbox.exe "[particles]"`, `build/Debug/properties-tests.exe`, `build/Debug/engine-tests.exe`.

- [ ] **Step 6: Commit**

```bash
git add src/engine_instances/ParticleEmitter.* src/engine_core/DataModel.hpp src/engine_core/Project.cpp src/engine_core/ScriptBindings.cpp src/engine_core/LuaApi.cpp src/ide/ClassOrder.hpp src/ide/IdeIcons.cpp src/ide/PropertySheet.* src/ide/PropertiesPanel.cpp src/ide/McpTools.cpp src/engine_services/SceneService.* src/engine_instances/README.md README.md sandbox/particle_emitter_tests.cpp tests/PropertiesTest.cpp tests/LuaEngineTest.cpp CMakeLists.txt resources
git commit -m "Particles: the ParticleEmitter class and Workspace.ParticleBudget"
```

---

### Task 6: Bursts

**Files:**
- Create: `src/engine_core/ParticleBursts.hpp`, `src/engine_core/ParticleBursts.cpp`
- Modify: `src/engine_instances/ParticleEmitter.cpp` (register `Bursts`), `src/engine_core/ScriptBindings.cpp` (read/write branch for type `"ParticleBursts"`), `src/engine_core/PropertyReflection.cpp` (type `"ParticleBursts"`), `src/engine_core/DataModel.cpp` (`type_fits`)
- Test: `sandbox/particle_emitter_tests.cpp`, `tests/LuaEngineTest.cpp`

**Interfaces:**
- Consumes: `ParticleBurst` (Task 5), CurveJson (Task 2).
- Produces:
  ```cpp
  JsonValue bursts_to_json(const std::vector<ParticleBurst>& bursts);   // [{"time":0,"count":{NumberCurve}}...]
  std::optional<std::vector<ParticleBurst>> bursts_from_json(const JsonValue& value, std::string& error);
  void push_bursts(lua_State* state, const std::vector<ParticleBurst>& bursts);       // array of {Time=, Count=} (Count a NumberCurve)
  std::optional<std::vector<ParticleBurst>> read_bursts(lua_State* state, int index, std::string& error);  // Count may be a number or NumberCurve
  ```
  The property type name is `"ParticleBursts"`; its `LuaSlot` is `Kind::String` holding the JSON text (so undo, same_slot, and history work unchanged); `slot_to_json` for `"ParticleBursts"` parses that text and writes the JSON array itself; `slot_from_json` validates with `bursts_from_json` and stores the canonical text. Script reads push a fresh Lua array; writes accept an array of tables.

Validation messages: `"a burst's Time must be 0 or more"`, `"a burst's Count must be a Constant or a Range, 0 or more"`.

- [ ] **Step 1: Failing tests**

```cpp
TEST_CASE("PE4 Bursts save as a JSON array, undo, and refuse bad rows", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& emitter = add_emitter(game);
    begin_step(game, "Set Bursts");
    REQUIRE_FALSE(emitter.set_bursts({ParticleBurst{0.f, NumberCurve::range(10.f, 20.f)}, ParticleBurst{1.5f, NumberCurve::constant(5.f)}}));
    end_step(game);
    PropertyBag saved;
    emitter.save_properties(saved);
    REQUIRE(write_json(saved.at("Bursts")) ==
            R"([{"time":0,"count":{"mode":"Range","min":10,"max":20}},{"time":1.5,"count":{"mode":"Constant","value":5}}])");
    game.history().undo();
    REQUIRE(emitter.bursts().empty());
    REQUIRE(emitter.set_bursts({ParticleBurst{-1.f, NumberCurve::constant(1.f)}}).has_value());
    std::string error;
    const auto keyed = NumberCurve::keys({{0.f, 1.f}}, error);
    REQUIRE(emitter.set_bursts({ParticleBurst{0.f, *keyed}}).has_value());
}
```

Use `PropertyBag`'s real accessor for a saved value (see `bloom_tests.cpp`).

```cpp
// tests/LuaEngineTest.cpp
expectPrinted(runner,
              "local e = Instance.new('ParticleEmitter') e.Bursts = {{Time = 0, Count = 50}, {Time = 1, Count = NumberCurve.range(2, 4)}} "
              "print(#e.Bursts, e.Bursts[1].Count:Evaluate(0), e.Bursts[2].Count.Mode)",
              "2\t50\tRange", "Bursts read and write as tables");
expectChunkError(runner, "Instance.new('ParticleEmitter').Bursts = {{Time = -1, Count = 1}}", "Time", "a negative Time is refused");
```

- [ ] **Step 2: Run; expect failure.**
- [ ] **Step 3: Implement** `ParticleBursts.cpp`; `set_bursts` validates each row, then `note_property_change("Bursts", bursts_slot(previous), bursts_slot(value))` where `bursts_slot` is `Kind::String` with `write_json(bursts_to_json(...))`. Register: `lua_saved_property("Bursts", "ParticleBursts", read_bursts_prop, write_bursts_prop, "[]")` right after `Rate`. In `ScriptBindings.cpp`, the read path pushes `push_bursts` when the field's `type_name` is `"ParticleBursts"` (find where `push_registered` is called with the field available; branch before it), and the write path calls `read_bursts` and stores the canonical JSON text in `slot.text`. `PropertySheet.cpp` `property_kind_for("ParticleBursts")` → `PropertyKind::Bursts` (shown read-only as `"2 bursts"` until Task 16).
- [ ] **Step 4: Run all three suites; pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: Bursts, saved as a JSON array and scripted as tables"`

---

### Task 7: Random numbers, vector helpers, and shape sampling

**Files:**
- Create: `src/engine_core/particles/ParticleRandom.hpp`, `src/engine_core/particles/ParticleMath.hpp`, `src/engine_core/particles/ParticleShapes.hpp`, `src/engine_core/particles/ParticleShapes.cpp`
- Modify: `CMakeLists.txt` (source + include dir if `particles/` is not already searched; follow how `terrain/` and `brush/` are included)
- Test: `sandbox/particle_shape_tests.cpp`

**Interfaces:**
- Produces:

```cpp
// ParticleRandom.hpp — PCG32 (O'Neill), small and deterministic across platforms.
namespace engine_core::particles {
class Pcg32 {
public:
    explicit Pcg32(std::uint64_t seed = 0x853c49e6748fea9bULL, std::uint64_t stream = 0xda3e39cb94b95bdbULL) { reseed(seed, stream); }
    void reseed(std::uint64_t seed, std::uint64_t stream = 0xda3e39cb94b95bdbULL) {
        state_ = 0u;
        inc_ = (stream << 1u) | 1u;
        next();
        state_ += seed;
        next();
    }
    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const std::uint32_t xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }
    // [0, 1)
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }
private:
    std::uint64_t state_ = 0;
    std::uint64_t inc_ = 0;
};
}  // namespace engine_core::particles
```

```cpp
// ParticleMath.hpp — float Vec3 helpers; Vec3 itself has no operators.
namespace engine_core::particles {
inline Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) { const float l = length(a); return l > 0.f ? scale(a, 1.f / l) : Vec3{0.f, 1.f, 0.f}; }
// a rotated by angle (radians) about the unit axis (Rodrigues).
inline Vec3 rotate(Vec3 a, Vec3 axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return add(add(scale(a, c), scale(cross(axis, a), s)), scale(axis, dot(axis, a) * (1.f - c)));
}
// Any unit vector perpendicular to the unit n.
inline Vec3 perpendicular(Vec3 n) {
    return normalize(std::fabs(n.y) < 0.9f ? cross(n, Vec3{0.f, 1.f, 0.f}) : cross(n, Vec3{1.f, 0.f, 0.f}));
}
}  // namespace engine_core::particles
```

```cpp
// ParticleShapes.hpp
namespace engine_core::particles {
struct ShapeSample {
    Vec3 position;   // shape-local, origin at the shape's centre
    Vec3 normal;     // outward unit normal there (up for Point and a volume's centre)
};
// half is half the box the shape fits in (all zero for a point). arc_degrees limits
// the angle about +Y for Sphere, Hemisphere, Cylinder, Cone, and Disc.
ShapeSample sample_shape(ParticleShape shape, ParticleEmitFrom from, Vec3 half, float arc_degrees, Pcg32& rng);
// A direction within spread (degrees about two axes) of the unit dir.
Vec3 spread_direction(Vec3 dir, float spread_x_deg, float spread_y_deg, Pcg32& rng);
}  // namespace engine_core::particles
```

- [ ] **Step 1: Failing tests**

```cpp
// sandbox/particle_shape_tests.cpp
#include "particles/ParticleShapes.hpp"
#include "particles/ParticleMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace engine_core;
using namespace engine_core::particles;

namespace {
const Vec3 kHalf{2.f, 1.f, 3.f};
bool inside_box(Vec3 p, float eps = 1e-4f) {
    return std::fabs(p.x) <= kHalf.x + eps && std::fabs(p.y) <= kHalf.y + eps && std::fabs(p.z) <= kHalf.z + eps;
}
float ellipsoid(Vec3 p) { return (p.x * p.x) / 4.f + (p.y * p.y) / 1.f + (p.z * p.z) / 9.f; }
}  // namespace

TEST_CASE("PS1 every shape and EmitFrom lands where it should", "[particle-shapes]") {
    Pcg32 rng(7);
    for (int i = 0; i < 2000; ++i) {
        REQUIRE(inside_box(sample_shape(ParticleShape::Box, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position));
        const Vec3 s = sample_shape(ParticleShape::Box, ParticleEmitFrom::Surface, kHalf, 360.f, rng).position;
        const bool on_face = std::fabs(std::fabs(s.x) - kHalf.x) < 1e-4f || std::fabs(std::fabs(s.y) - kHalf.y) < 1e-4f ||
                             std::fabs(std::fabs(s.z) - kHalf.z) < 1e-4f;
        REQUIRE(on_face);
        const Vec3 e = sample_shape(ParticleShape::Box, ParticleEmitFrom::Edge, kHalf, 360.f, rng).position;
        int on = 0;
        on += std::fabs(std::fabs(e.x) - kHalf.x) < 1e-4f;
        on += std::fabs(std::fabs(e.y) - kHalf.y) < 1e-4f;
        on += std::fabs(std::fabs(e.z) - kHalf.z) < 1e-4f;
        REQUIRE(on >= 2);
        REQUIRE(ellipsoid(sample_shape(ParticleShape::Sphere, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position) <= 1.0001f);
        REQUIRE(ellipsoid(sample_shape(ParticleShape::Sphere, ParticleEmitFrom::Surface, kHalf, 360.f, rng).position) ==
                Catch::Approx(1.f).margin(1e-3));
        REQUIRE(sample_shape(ParticleShape::Hemisphere, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position.y >= -1e-4f);
        const Vec3 d = sample_shape(ParticleShape::Disc, ParticleEmitFrom::Edge, kHalf, 360.f, rng).position;
        REQUIRE(d.y == 0.f);
        REQUIRE((d.x * d.x) / 4.f + (d.z * d.z) / 9.f == Catch::Approx(1.f).margin(1e-3));
        const Vec3 c = sample_shape(ParticleShape::Cone, ParticleEmitFrom::Volume, kHalf, 360.f, rng).position;
        REQUIRE(c.y >= -kHalf.y - 1e-4f);
        REQUIRE(c.y <= kHalf.y + 1e-4f);
        const ShapeSample point = sample_shape(ParticleShape::Point, ParticleEmitFrom::Surface, kHalf, 360.f, rng);
        REQUIRE(point.position.x == 0.f);
        REQUIRE(point.position.y == 0.f);
        REQUIRE(point.position.z == 0.f);
    }
}

TEST_CASE("PS2 Arc limits the angle about Y", "[particle-shapes]") {
    Pcg32 rng(3);
    for (int i = 0; i < 2000; ++i) {
        const Vec3 p = sample_shape(ParticleShape::Disc, ParticleEmitFrom::Volume, kHalf, 90.f, rng).position;
        const float angle = std::atan2(p.z, p.x);
        REQUIRE(angle >= -1e-4f);
        REQUIRE(angle <= 3.14159265f / 2.f + 1e-4f);
    }
}

TEST_CASE("PS3 a sphere's volume is even, not bunched at the centre", "[particle-shapes]") {
    Pcg32 rng(11);
    int inner = 0;
    const int n = 20000;
    for (int i = 0; i < n; ++i) {
        const Vec3 p = sample_shape(ParticleShape::Sphere, ParticleEmitFrom::Volume, Vec3{1.f, 1.f, 1.f}, 360.f, rng).position;
        inner += length(p) < 0.5f;
    }
    // A ball of half the radius holds 1/8 of the volume.
    REQUIRE(static_cast<float>(inner) / n == Catch::Approx(0.125f).margin(0.01f));
}

TEST_CASE("PS4 spread stays within its cone and zero spread is the direction", "[particle-shapes]") {
    Pcg32 rng(5);
    const Vec3 up{0.f, 1.f, 0.f};
    for (int i = 0; i < 1000; ++i) {
        const Vec3 d = spread_direction(up, 30.f, 30.f, rng);
        REQUIRE(length(d) == Catch::Approx(1.f).margin(1e-4));
        REQUIRE(dot(d, up) >= std::cos(30.f * 3.14159265f / 180.f) - 1e-3f);
        const Vec3 flat = spread_direction(up, 90.f, 0.f, rng);
        REQUIRE(std::fabs(flat.z) < 1e-4f);   // a fan in the X/Y plane
    }
    const Vec3 same = spread_direction(up, 0.f, 0.f, rng);
    REQUIRE(same.y == Catch::Approx(1.f));
}

TEST_CASE("PS5 the same seed gives the same samples", "[particle-shapes]") {
    Pcg32 a(42), b(42);
    for (int i = 0; i < 100; ++i) {
        const Vec3 pa = sample_shape(ParticleShape::Cone, ParticleEmitFrom::Surface, kHalf, 360.f, a).position;
        const Vec3 pb = sample_shape(ParticleShape::Cone, ParticleEmitFrom::Surface, kHalf, 360.f, b).position;
        REQUIRE(pa.x == pb.x);
        REQUIRE(pa.y == pb.y);
        REQUIRE(pa.z == pb.z);
    }
}
```

- [ ] **Step 2: Run; fail.**
- [ ] **Step 3: Implement `ParticleShapes.cpp`**

```cpp
#include "particles/ParticleShapes.hpp"
#include "particles/ParticleMath.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core::particles {
namespace {
constexpr float kPi = 3.14159265358979f;

float arc_angle(float arc_degrees, Pcg32& rng) {
    const float arc = std::clamp(arc_degrees, 0.f, 360.f) * kPi / 180.f;
    return rng.uniform() * arc;
}

// Unit-circle point at angle a in X/Z, scaled to the ellipse (hx, hz).
Vec3 ring(float a, float hx, float hz, float r) { return {std::cos(a) * hx * r, 0.f, std::sin(a) * hz * r}; }

// Outward normal of an axis-aligned ellipsoid at local p.
Vec3 ellipsoid_normal(Vec3 p, Vec3 h) {
    const auto inv2 = [](float v) { return v > 0.f ? 1.f / (v * v) : 0.f; };
    return normalize(Vec3{p.x * inv2(h.x), p.y * inv2(h.y), p.z * inv2(h.z)});
}

ShapeSample box(ParticleEmitFrom from, Vec3 h, Pcg32& rng) {
    if (from == ParticleEmitFrom::Volume) {
        return {{rng.range(-h.x, h.x), rng.range(-h.y, h.y), rng.range(-h.z, h.z)}, {0.f, 1.f, 0.f}};
    }
    if (from == ParticleEmitFrom::Surface) {
        // Pick a face by area: pairs X (y*z), Y (x*z), Z (x*y).
        const float ax = h.y * h.z, ay = h.x * h.z, az = h.x * h.y;
        float pick = rng.uniform() * (ax + ay + az);
        const float sign = rng.uniform() < 0.5f ? -1.f : 1.f;
        if ((pick -= ax) < 0.f) return {{sign * h.x, rng.range(-h.y, h.y), rng.range(-h.z, h.z)}, {sign, 0.f, 0.f}};
        if ((pick -= ay) < 0.f) return {{rng.range(-h.x, h.x), sign * h.y, rng.range(-h.z, h.z)}, {0.f, sign, 0.f}};
        return {{rng.range(-h.x, h.x), rng.range(-h.y, h.y), sign * h.z}, {0.f, 0.f, sign}};
    }
    // Edges: 4 along each axis, weighted by length.
    const float lx = h.x, ly = h.y, lz = h.z;
    float pick = rng.uniform() * (lx + ly + lz);
    const float s1 = rng.uniform() < 0.5f ? -1.f : 1.f;
    const float s2 = rng.uniform() < 0.5f ? -1.f : 1.f;
    Vec3 p;
    if ((pick -= lx) < 0.f) p = {rng.range(-h.x, h.x), s1 * h.y, s2 * h.z};
    else if ((pick -= ly) < 0.f) p = {s1 * h.x, rng.range(-h.y, h.y), s2 * h.z};
    else p = {s1 * h.x, s2 * h.y, rng.range(-h.z, h.z)};
    return {p, normalize(p)};
}

ShapeSample sphere(ParticleEmitFrom from, Vec3 h, float arc, bool hemi, Pcg32& rng) {
    // Uniform direction, longitude limited by arc, latitude by hemi.
    const float a = arc_angle(arc, rng);
    const float y = hemi ? rng.uniform() : rng.range(-1.f, 1.f);
    const float r_xz = std::sqrt(std::max(0.f, 1.f - y * y));
    const Vec3 dir{std::cos(a) * r_xz, y, std::sin(a) * r_xz};
    float r = 1.f;
    if (from == ParticleEmitFrom::Volume) {
        r = std::cbrt(rng.uniform());
    } else if (from == ParticleEmitFrom::Edge) {
        // The equator (and the rim of a hemisphere).
        const Vec3 p = ring(a, h.x, h.z, 1.f);
        return {p, normalize(Vec3{p.x, 0.f, p.z})};
    }
    const Vec3 p{dir.x * h.x * r, dir.y * h.y * r, dir.z * h.z * r};
    return {p, ellipsoid_normal(p.x == 0.f && p.y == 0.f && p.z == 0.f ? dir : p, h)};
}

ShapeSample cylinder(ParticleEmitFrom from, Vec3 h, float arc, Pcg32& rng) {
    const float a = arc_angle(arc, rng);
    if (from == ParticleEmitFrom::Volume) {
        const Vec3 p = ring(a, h.x, h.z, std::sqrt(rng.uniform()));
        return {{p.x, rng.range(-h.y, h.y), p.z}, {0.f, 1.f, 0.f}};
    }
    if (from == ParticleEmitFrom::Edge) {
        const Vec3 p = ring(a, h.x, h.z, 1.f);
        const float y = rng.uniform() < 0.5f ? -h.y : h.y;
        return {{p.x, y, p.z}, normalize(Vec3{p.x, y, p.z})};
    }
    // Surface: side by its area (approx. perimeter * height) against the two caps.
    const float side = 3.14159265f * (h.x + h.z) * 2.f * h.y;
    const float caps = 2.f * kPi * h.x * h.z;
    if (rng.uniform() * (side + caps) < side) {
        const Vec3 p = ring(a, h.x, h.z, 1.f);
        return {{p.x, rng.range(-h.y, h.y), p.z}, normalize(Vec3{p.x, 0.f, p.z})};
    }
    const Vec3 p = ring(a, h.x, h.z, std::sqrt(rng.uniform()));
    const float sign = rng.uniform() < 0.5f ? -1.f : 1.f;
    return {{p.x, sign * h.y, p.z}, {0.f, sign, 0.f}};
}

// The cone opens along +Y: apex at -h.y, wide base at +h.y.
ShapeSample cone(ParticleEmitFrom from, Vec3 h, float arc, Pcg32& rng) {
    const float a = arc_angle(arc, rng);
    if (from == ParticleEmitFrom::Edge) {
        const Vec3 p = ring(a, h.x, h.z, 1.f);
        return {{p.x, h.y, p.z}, normalize(Vec3{p.x, h.y, p.z})};
    }
    if (from == ParticleEmitFrom::Surface) {
        // Lateral surface: radius grows linearly with height; area density ~ height.
        const float k = std::sqrt(rng.uniform());   // 0 at apex, 1 at base
        const Vec3 p = ring(a, h.x, h.z, k);
        const float y = -h.y + 2.f * h.y * k;
        return {{p.x, y, p.z}, normalize(Vec3{std::cos(a), h.x > 0.f ? h.x / (2.f * h.y + 1e-6f) : 0.f, std::sin(a)})};
    }
    const float k = std::cbrt(rng.uniform());       // volume density ~ height^2
    const Vec3 p = ring(a, h.x, h.z, k * std::sqrt(rng.uniform()));
    const float y = -h.y + 2.f * h.y * k;
    return {{p.x, y, p.z}, normalize(Vec3{p.x, y + h.y, p.z})};
}

ShapeSample disc(ParticleEmitFrom from, Vec3 h, float arc, Pcg32& rng) {
    const float a = arc_angle(arc, rng);
    const float r = from == ParticleEmitFrom::Edge ? 1.f : std::sqrt(rng.uniform());
    const Vec3 p = ring(a, h.x, h.z, r);
    return {p, from == ParticleEmitFrom::Edge ? normalize(p) : Vec3{0.f, 1.f, 0.f}};
}
}  // namespace

ShapeSample sample_shape(ParticleShape shape, ParticleEmitFrom from, Vec3 half, float arc_degrees, Pcg32& rng) {
    if (half.x <= 0.f && half.y <= 0.f && half.z <= 0.f) {
        return {{0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
    }
    switch (shape) {
        case ParticleShape::Point: return {{0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
        case ParticleShape::Box: return box(from, half, rng);
        case ParticleShape::Sphere: return sphere(from, half, arc_degrees, false, rng);
        case ParticleShape::Hemisphere: return sphere(from, half, arc_degrees, true, rng);
        case ParticleShape::Cylinder: return cylinder(from, half, arc_degrees, rng);
        case ParticleShape::Cone: return cone(from, half, arc_degrees, rng);
        case ParticleShape::Disc: return disc(from, half, arc_degrees, rng);
    }
    return {{0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
}

Vec3 spread_direction(Vec3 dir, float spread_x_deg, float spread_y_deg, Pcg32& rng) {
    const Vec3 d = normalize(dir);
    if (spread_x_deg <= 0.f && spread_y_deg <= 0.f) {
        return d;
    }
    // For d = +Y the X spread tilts toward X (about Z) and the Y spread toward Z (about X).
    const Vec3 xr = std::fabs(d.y) > 0.9f ? Vec3{0.f, 0.f, 1.f} : normalize(cross(d, Vec3{0.f, 1.f, 0.f}));
    const Vec3 yr = normalize(cross(xr, d));
    const float ax = rng.range(-spread_x_deg, spread_x_deg) * kPi / 180.f;
    const float ay = rng.range(-spread_y_deg, spread_y_deg) * kPi / 180.f;
    return normalize(rotate(rotate(d, xr, ax), yr, ay));
}

}  // namespace engine_core::particles
```


- [ ] **Step 4: Run `[particle-shapes]`; pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: PCG32 and analytic shape sampling"`

---

### Task 8: EmitterSim — spawning and motion

**Files:**
- Create: `src/engine_core/particles/EmitterSim.hpp`, `src/engine_core/particles/EmitterSim.cpp`
- Test: `sandbox/particle_sim_tests.cpp`

**Interfaces:**
- Consumes: Tasks 1, 7.
- Produces:

```cpp
namespace engine_core::particles {

// Everything a sim reads from its emitter, curves already baked. Built by
// ParticleWorld whenever the emitter's settings are dirty.
struct ParticleSettings {
    // Emission
    CurveTable rate;                       // cycle
    struct Burst { float time; CurveTable count; };
    std::vector<Burst> bursts;             // sorted by time
    float duration = 5.f;
    bool looping = true;
    bool prewarm = false;
    int max_particles = 1000;
    std::uint32_t seed = 0;                // 0: fresh each restart
    // Shape
    ParticleShape shape = ParticleShape::Box;
    ParticleEmitFrom emit_from = ParticleEmitFrom::Volume;
    float arc = 360.f;
    Vec3 emit_direction{0.f, 1.f, 0.f};
    bool emit_direction_local = true;
    float spread_x = 0.f, spread_y = 0.f;
    // Particle (cycle)
    CurveTable lifetime, emit_speed, emit_rotation, rotation_speed;
    // Motion
    bool local_space = false;
    float inherit_velocity = 0.f;
    Vec3 acceleration{0.f, 0.f, 0.f};
    bool acceleration_local = false;
    CurveTable drag, orbit_speed;          // life
    Vec3 orbit_axis{0.f, 1.f, 0.f};
    // Collision
    ParticleCollision collision = ParticleCollision::None;
    ParticleCollisionResponse response = ParticleCollisionResponse::Bounce;
    float bounciness = 0.5f, friction = 0.2f;
    // Appearance (life)
    ColorTable color;
    CurveTable size, stretch;
    int frames = 1;                        // flipbook cells (grid x*y), 1 for Static
};

// Where the emitter is this frame.
struct EmitterFrame {
    Matrix4 previous;                      // parent world transform last step (== current on the first)
    Matrix4 current;
    Vec3 half{0.f, 0.f, 0.f};              // half the shape box; zero for a point
    Vec3 inherited_linear{0.f, 0.f, 0.f};  // assembly body velocity at the emitter (v + w x r), world
    float dt = 0.f;
};

struct ParticleHit {
    Vec3 point;
    Vec3 normal;                           // unit, toward where the particle came from
};

// Asked once per moving, unstuck particle. slot is stable for the step.
class ParticleCollider {
public:
    virtual ~ParticleCollider() = default;
    virtual void begin(std::size_t count) = 0;
    virtual std::optional<ParticleHit> test(std::size_t slot, Vec3 from, Vec3 to) = 0;
    // The sim swap-removes a dead particle: move slot `from` into `to`.
    virtual void moved(std::size_t from, std::size_t to) = 0;
};

// One drawn particle, world space, as the renderer reads it.
struct ParticleVertex {
    float position[3];
    float size;                            // units, after Size(life)
    float color[4];                        // straight rgba, rgb times nothing yet (brightness is per emitter)
    float velocity[3];                     // world, for Velocity facing
    float rotation;                        // radians
    float stretch;                         // Stretch(life)
    float frame;                           // flipbook cell, integer-valued
};

class EmitterSim {
public:
    void configure(const ParticleSettings& settings);   // keeps live particles
    void restart(std::uint32_t fresh_seed);              // cycle time 0, reseeds (settings.seed if non-zero)
    // How many particles this step would spawn (rate carry + bursts crossed + pending emits), without advancing.
    int wanted_spawns(float dt, bool playing, int pending_emits) const;
    // Advances one step. allowance caps spawns (budget and MaxParticles applied by the caller too).
    void step(const EmitterFrame& frame, bool playing, int pending_emits, int allowance, ParticleCollider* collider);
    void clear();
    void prewarm(const EmitterFrame& frame);             // one cycle at 1/30 s steps, no collision
    std::size_t count() const { return age_.size(); }
    float cycle_time() const { return cycle_time_; }
    // A non-looping cycle ran out this step (true once).
    bool take_cycle_ended();
    bool cycle_over() const { return cycle_over_; }
    // World-space, sorted back to front from eye when sort is true.
    void pack(std::vector<ParticleVertex>& out, const Matrix4& emitter_world, Vec3 eye, bool sort) const;
    // Tests.
    Vec3 position(std::size_t i) const { return pos_[i]; }
    Vec3 velocity(std::size_t i) const { return vel_[i]; }
    float age(std::size_t i) const { return age_[i]; }
private:
    void spawn(const EmitterFrame& frame, int count, float spread_start);
    void kill(std::size_t i, ParticleCollider* collider);
    ParticleSettings s_;
    Pcg32 rng_;
    float cycle_time_ = 0.f;
    float cycle_rate_blend_ = 0.f;        // Rate's Range pick for this cycle
    float rate_carry_ = 0.f;
    std::size_t next_burst_ = 0;
    bool cycle_over_ = false;
    bool cycle_ended_flag_ = false;
    // SoA. pos_ is world space, or emitter-local when s_.local_space.
    std::vector<Vec3> pos_, vel_;
    std::vector<float> age_, life_, random_, rot_, spin_;
    std::vector<std::uint8_t> stuck_;
};

}  // namespace engine_core::particles
```

- [ ] **Step 1: Failing tests**

```cpp
// sandbox/particle_sim_tests.cpp
#include "particles/EmitterSim.hpp"
#include "particles/ParticleMath.hpp"
#include "Matrix4.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace engine_core;
using namespace engine_core::particles;
using Catch::Approx;

namespace {
CurveTable table(const NumberCurve& curve) { CurveTable t; t.bake(curve); return t; }

ParticleSettings basic() {
    ParticleSettings s;
    s.rate = table(NumberCurve::constant(10.f));
    s.lifetime = table(NumberCurve::constant(100.f));
    s.emit_speed = table(NumberCurve::constant(0.f));
    s.emit_rotation = table(NumberCurve::constant(0.f));
    s.rotation_speed = table(NumberCurve::constant(0.f));
    s.drag = table(NumberCurve::constant(0.f));
    s.orbit_speed = table(NumberCurve::constant(0.f));
    s.size = table(NumberCurve::constant(1.f));
    s.stretch = table(NumberCurve::constant(0.f));
    s.color.bake(ColorCurve::constant(ColorRgb{1, 1, 1, 1}, 0.f));
    s.shape = ParticleShape::Point;
    s.seed = 1;
    return s;
}

EmitterFrame still(float dt) {
    EmitterFrame f;
    f.previous = f.current = matrix4_identity();
    f.dt = dt;
    return f;
}

void run(EmitterSim& sim, float seconds, float dt, bool playing = true) {
    for (float t = 0.f; t < seconds - 1e-6f; t += dt) sim.step(still(dt), playing, 0, 1 << 30, nullptr);
}
}  // namespace

TEST_CASE("SIM1 Rate spawns rate*time, carrying fractions across steps", "[particle-sim]") {
    EmitterSim sim;
    sim.configure(basic());
    sim.restart(0);
    run(sim, 1.f, 1.f / 60.f);
    REQUIRE(sim.count() == 10);
}

TEST_CASE("SIM2 a burst fires once at its time and adds to Rate", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.5f, table(NumberCurve::constant(25.f))}};
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    run(sim, 0.49f, 0.01f);
    REQUIRE(sim.count() == 0);
    run(sim, 0.02f, 0.01f);
    REQUIRE(sim.count() == 25);
    run(sim, 1.f, 0.01f);
    REQUIRE(sim.count() == 25);
}

TEST_CASE("SIM3 a one-shot ends its cycle once and stops spawning; a loop wraps and fires bursts again", "[particle-sim]") {
    ParticleSettings s = basic();
    s.duration = 1.f;
    s.looping = false;
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    run(sim, 1.05f, 0.05f);
    REQUIRE(sim.take_cycle_ended());
    REQUIRE_FALSE(sim.take_cycle_ended());
    const std::size_t n = sim.count();
    run(sim, 1.f, 0.05f);
    REQUIRE(sim.count() == n);

    s.looping = true;
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.f, table(NumberCurve::constant(3.f))}};
    EmitterSim loop;
    loop.configure(s);
    loop.restart(0);
    run(loop, 2.5f, 0.05f);
    REQUIRE(loop.count() == 9);   // t = 0, 1, 2
}

TEST_CASE("SIM4 particles die at their lifetime", "[particle-sim]") {
    ParticleSettings s = basic();
    s.lifetime = table(NumberCurve::constant(0.5f));
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.f, table(NumberCurve::constant(5.f))}};
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    run(sim, 0.4f, 0.1f);
    REQUIRE(sim.count() == 5);
    run(sim, 0.2f, 0.1f);
    REQUIRE(sim.count() == 0);
}

TEST_CASE("SIM5 acceleration, drag, and speed integrate as written", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.f, table(NumberCurve::constant(1.f))}};
    s.acceleration = Vec3{0.f, -10.f, 0.f};
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    run(sim, 1.f, 1.f / 240.f);
    REQUIRE(sim.velocity(0).y == Approx(-10.f).margin(0.1f));
    REQUIRE(sim.position(0).y == Approx(-5.f).margin(0.1f));

    s.acceleration = Vec3{0.f, 0.f, 0.f};
    s.emit_speed = table(NumberCurve::constant(10.f));
    s.drag = table(NumberCurve::constant(1.f));
    EmitterSim dragged;
    dragged.configure(s);
    dragged.restart(0);
    run(dragged, 1.f, 1.f / 60.f);
    REQUIRE(length(dragged.velocity(0)) == Approx(10.f * std::exp(-1.f)).margin(0.05f));
}

TEST_CASE("SIM6 drag is frame-rate independent", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.f, table(NumberCurve::constant(1.f))}};
    s.emit_speed = table(NumberCurve::constant(10.f));
    s.drag = table(NumberCurve::constant(2.f));
    EmitterSim a, b;
    a.configure(s); a.restart(0); run(a, 1.f, 1.f / 30.f);
    b.configure(s); b.restart(0); run(b, 1.f, 1.f / 240.f);
    REQUIRE(length(a.velocity(0)) == Approx(length(b.velocity(0))).margin(0.05f));
}

TEST_CASE("SIM7 orbit swings particles about the emitter and keeps their distance", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.f, table(NumberCurve::constant(1.f))}};
    s.shape = ParticleShape::Box;
    s.emit_from = ParticleEmitFrom::Surface;
    s.orbit_speed = table(NumberCurve::constant(90.f));
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    EmitterFrame f = still(0.01f);
    f.half = Vec3{1.f, 0.f, 1.f};
    sim.step(f, true, 0, 1 << 30, nullptr);
    const Vec3 start = sim.position(0);
    for (int i = 0; i < 100; ++i) sim.step(f, true, 0, 1 << 30, nullptr);
    const Vec3 end = sim.position(0);
    const float r0 = std::hypot(start.x, start.z), r1 = std::hypot(end.x, end.z);
    REQUIRE(r1 == Approx(r0).margin(1e-3));
    REQUIRE(std::atan2(end.z, end.x) != Approx(std::atan2(start.z, start.x)).margin(0.5f));
}

TEST_CASE("SIM8 a fast emitter spreads its spawns along its path", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(100.f));
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    EmitterFrame f;
    f.previous = matrix4_identity();
    f.current = matrix4_translation(Vec3{10.f, 0.f, 0.f});
    f.dt = 0.1f;   // 10 particles across x = 0..10
    sim.step(f, true, 0, 1 << 30, nullptr);
    REQUIRE(sim.count() == 10);
    float lo = 1e9f, hi = -1e9f;
    for (std::size_t i = 0; i < sim.count(); ++i) { lo = std::min(lo, sim.position(i).x); hi = std::max(hi, sim.position(i).x); }
    REQUIRE(lo < 1.5f);
    REQUIRE(hi > 8.5f);
}

TEST_CASE("SIM9 the same seed replays identically; Range picks per particle", "[particle-sim]") {
    ParticleSettings s = basic();
    s.shape = ParticleShape::Sphere;
    s.emit_speed = table(NumberCurve::range(1.f, 5.f));
    EmitterSim a, b;
    EmitterFrame f = still(1.f / 60.f);
    f.half = Vec3{1, 1, 1};
    a.configure(s); a.restart(0);
    b.configure(s); b.restart(0);
    for (int i = 0; i < 60; ++i) { a.step(f, true, 0, 1 << 30, nullptr); b.step(f, true, 0, 1 << 30, nullptr); }
    REQUIRE(a.count() == b.count());
    bool varied = false;
    for (std::size_t i = 0; i < a.count(); ++i) {
        REQUIRE(a.position(i).x == b.position(i).x);
        varied = varied || std::fabs(length(a.velocity(i)) - length(a.velocity(0))) > 0.1f;
    }
    REQUIRE(varied);
}

TEST_CASE("SIM10 allowance caps spawns and MaxParticles caps live count", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(1e9f));
    s.max_particles = 50;
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    sim.step(still(1.f), true, 0, 20, nullptr);
    REQUIRE(sim.count() == 20);
    sim.step(still(1.f), true, 0, 1 << 30, nullptr);
    REQUIRE(sim.count() == 50);
}

TEST_CASE("SIM11 local space particles follow the emitter; world space ones stay", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.f, table(NumberCurve::constant(1.f))}};
    std::vector<ParticleVertex> out;
    for (bool local : {false, true}) {
        s.local_space = local;
        EmitterSim sim;
        sim.configure(s);
        sim.restart(0);
        sim.step(still(0.01f), true, 0, 1 << 30, nullptr);
        const Matrix4 moved = matrix4_translation(Vec3{5.f, 0.f, 0.f});
        sim.pack(out, moved, Vec3{0, 0, 10}, false);
        REQUIRE(out[0].position[0] == Approx(local ? 5.f : 0.f));
    }
}

TEST_CASE("SIM12 pack sorts back to front from the eye", "[particle-sim]") {
    ParticleSettings s = basic();
    s.shape = ParticleShape::Box;
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.f, table(NumberCurve::constant(50.f))}};
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    EmitterFrame f = still(0.01f);
    f.half = Vec3{5, 5, 5};
    sim.step(f, true, 0, 1 << 30, nullptr);
    std::vector<ParticleVertex> out;
    const Vec3 eye{0, 0, 20};
    sim.pack(out, matrix4_identity(), eye, true);
    for (std::size_t i = 1; i < out.size(); ++i) {
        const Vec3 a{out[i - 1].position[0], out[i - 1].position[1], out[i - 1].position[2]};
        const Vec3 b{out[i].position[0], out[i].position[1], out[i].position[2]};
        REQUIRE(length(sub(a, eye)) >= length(sub(b, eye)) - 1e-4f);
    }
}

TEST_CASE("SIM13 Emit spawns now even when not playing", "[particle-sim]") {
    EmitterSim sim;
    sim.configure(basic());
    sim.restart(0);
    sim.step(still(0.01f), false, 7, 1 << 30, nullptr);
    REQUIRE(sim.count() == 7);
}
```

- [ ] **Step 2: Run; fail.**
- [ ] **Step 3: Implement `EmitterSim.cpp`.** Key parts, verbatim:

```cpp
namespace engine_core::particles {
namespace {
constexpr float kDegToRad = 3.14159265358979f / 180.f;
Vec3 transform_point(const Matrix4& m, Vec3 p) { return matrix4_point(m, p); }
Vec3 transform_dir(const Matrix4& m, Vec3 d) { return normalize(matrix4_vector(m, d)); }
// The rigid part of m (no scale) for directions.
}  // namespace

void EmitterSim::configure(const ParticleSettings& settings) {
    const bool space_changed = settings.local_space != s_.local_space;
    s_ = settings;
    if (space_changed) clear();   // positions mean something else now
}

void EmitterSim::restart(std::uint32_t fresh_seed) {
    rng_.reseed(s_.seed != 0 ? s_.seed : fresh_seed);
    cycle_time_ = 0.f;
    rate_carry_ = 0.f;
    next_burst_ = 0;
    cycle_over_ = false;
    cycle_rate_blend_ = rng_.uniform();
}

int EmitterSim::wanted_spawns(float dt, bool playing, int pending_emits) const {
    int n = pending_emits;
    if (playing && !cycle_over_ && dt > 0.f) {
        const float t = std::min(cycle_time_ / s_.duration, 1.f);
        n += static_cast<int>(rate_carry_ + std::max(0.f, s_.rate.sample(t, cycle_rate_blend_)) * dt);
        const float end = cycle_time_ + dt;
        for (std::size_t b = next_burst_; b < s_.bursts.size() && s_.bursts[b].time < end; ++b) {
            n += static_cast<int>(std::max(0.f, s_.bursts[b].count.sample(0.f, 0.5f)) + 0.5f);   // estimate; exact count drawn at spawn
        }
    }
    return n;
}

void EmitterSim::step(const EmitterFrame& frame, bool playing, int pending_emits, int allowance, ParticleCollider* collider) {
    const float dt = frame.dt;
    // 1. Spawn.
    int room = std::max(0, std::min(allowance, s_.max_particles - static_cast<int>(count())));
    const int emits = std::min(pending_emits, room);
    if (emits > 0) {
        spawn(frame, emits, 1.f);      // all at the end of the step: an Emit is "now"
        room -= emits;
    }
    if (playing && !cycle_over_ && dt > 0.f) {
        const float t = std::min(cycle_time_ / s_.duration, 1.f);
        rate_carry_ += std::max(0.f, s_.rate.sample(t, cycle_rate_blend_)) * dt;
        int from_rate = static_cast<int>(rate_carry_);
        rate_carry_ -= static_cast<float>(from_rate);
        const float end = cycle_time_ + dt;
        int from_bursts = 0;
        while (next_burst_ < s_.bursts.size() && s_.bursts[next_burst_].time < end) {
            from_bursts += static_cast<int>(std::max(0.f, s_.bursts[next_burst_].count.sample(0.f, rng_.uniform())) + 0.5f);
            ++next_burst_;
        }
        const int burst_n = std::min(from_bursts, room);
        if (burst_n > 0) { spawn(frame, burst_n, 1.f); room -= burst_n; }
        const int rate_n = std::min(from_rate, room);
        if (rate_n > 0) spawn(frame, rate_n, 0.f);   // spread across the step
        cycle_time_ = end;
        if (cycle_time_ >= s_.duration) {
            if (s_.looping) {
                cycle_time_ = std::fmod(cycle_time_, s_.duration);
                cycle_rate_blend_ = rng_.uniform();
                // Bursts of the new cycle that the wrapped time already passed fire now.
                next_burst_ = 0;
                int wrapped = 0;
                while (next_burst_ < s_.bursts.size() && s_.bursts[next_burst_].time < cycle_time_) {
                    wrapped += static_cast<int>(std::max(0.f, s_.bursts[next_burst_].count.sample(0.f, rng_.uniform())) + 0.5f);
                    ++next_burst_;
                }
                const int wrapped_n = std::min(wrapped, room);
                if (wrapped_n > 0) { spawn(frame, wrapped_n, 1.f); room -= wrapped_n; }
            } else {
                cycle_over_ = true;
                cycle_ended_flag_ = true;
            }
        }
    }
    // 2. Integrate, age, kill, collide.
    if (collider != nullptr) collider->begin(count());
    const Matrix4& m = frame.current;
    const Vec3 origin = matrix4_position(m);
    const Vec3 accel = s_.acceleration_local ? matrix4_vector(m, s_.acceleration) : s_.acceleration;
    const Vec3 orbit_axis_world = transform_dir(m, s_.orbit_axis);
    for (std::size_t i = 0; i < count();) {
        age_[i] += dt;
        if (age_[i] >= life_[i]) { kill(i, collider); continue; }
        if (stuck_[i]) { ++i; continue; }
        const float t = age_[i] / life_[i];
        Vec3 a = accel;
        if (s_.local_space) a = matrix4_vector(matrix4_inverse(m), accel);   // pos_/vel_ are emitter-local
        vel_[i] = add(vel_[i], scale(a, dt));
        const float drag = std::max(0.f, s_.drag.sample(t, random_[i]));
        if (drag > 0.f) vel_[i] = scale(vel_[i], std::exp(-drag * dt));
        const Vec3 from = pos_[i];
        Vec3 to = add(from, scale(vel_[i], dt));
        const float orbit = s_.orbit_speed.sample(t, random_[i]) * kDegToRad * dt;
        if (orbit != 0.f) {
            const Vec3 centre = s_.local_space ? Vec3{0, 0, 0} : origin;
            const Vec3 axis = s_.local_space ? normalize(s_.orbit_axis) : orbit_axis_world;
            to = add(centre, rotate(sub(to, centre), axis, orbit));
        }
        rot_[i] += spin_[i] * dt;
        if (collider != nullptr && s_.collision != ParticleCollision::None) {
            const Vec3 wf = s_.local_space ? transform_point(m, from) : from;
            const Vec3 wt = s_.local_space ? transform_point(m, to) : to;
            if (auto hit = collider->test(i, wf, wt)) {
                if (s_.response == ParticleCollisionResponse::Die) { kill(i, collider); continue; }
                const Vec3 p = s_.local_space ? transform_point(matrix4_inverse(m), hit->point) : hit->point;
                const Vec3 n = s_.local_space ? normalize(matrix4_vector(matrix4_inverse(m), hit->normal)) : hit->normal;
                if (s_.response == ParticleCollisionResponse::Stick) {
                    pos_[i] = p;
                    vel_[i] = Vec3{0, 0, 0};
                    stuck_[i] = 1;
                    ++i;
                    continue;
                }
                const float vn = dot(vel_[i], n);
                if (vn < 0.f) {
                    const Vec3 normal_part = scale(n, vn);
                    const Vec3 tangent_part = sub(vel_[i], normal_part);
                    vel_[i] = sub(scale(tangent_part, 1.f - s_.friction), scale(normal_part, s_.bounciness));
                }
                to = add(p, scale(n, 1e-3f));
            }
        }
        pos_[i] = to;
        ++i;
    }
}
```

`spawn(frame, n, spread_start)`:

```cpp
void EmitterSim::spawn(const EmitterFrame& frame, int count, float spread_start) {
    for (int k = 0; k < count; ++k) {
        // Sub-step birth time: spread_start 0 spreads births across the step (oldest first); 1 puts all at its end.
        const float f = spread_start >= 1.f ? 1.f : (static_cast<float>(k) + rng_.uniform()) / static_cast<float>(count);
        const Matrix4 at = f >= 1.f ? frame.current : matrix4_lerp(frame.previous, frame.current, f);
        const ShapeSample sample = sample_shape(s_.shape, s_.emit_from, frame.half, s_.arc, rng_);
        const float cycle_t = std::min(cycle_time_ / s_.duration, 1.f);
        const float r = rng_.uniform();
        Vec3 dir = s_.emit_direction;
        Vec3 dir_world = s_.emit_direction_local ? transform_dir(at, dir) : normalize(dir);
        dir_world = spread_direction(dir_world, s_.spread_x, s_.spread_y, rng_);
        const float speed = s_.emit_speed.sample(cycle_t, rng_.uniform());
        Vec3 p = transform_point(at, sample.position);
        Vec3 v = scale(dir_world, speed);
        if (!s_.local_space) v = add(v, scale(frame.inherited_linear, s_.inherit_velocity));
        const float pre_age = (1.f - f) * frame.dt;   // born mid-step: already this old
        p = add(p, scale(v, pre_age));
        if (s_.local_space) {
            const Matrix4 inv = matrix4_inverse(frame.current);
            p = transform_point(inv, p);
            v = matrix4_vector(inv, v);
        }
        pos_.push_back(p);
        vel_.push_back(v);
        age_.push_back(pre_age);
        life_.push_back(std::max(1e-3f, s_.lifetime.sample(cycle_t, rng_.uniform())));
        random_.push_back(r);
        rot_.push_back(s_.emit_rotation.sample(cycle_t, rng_.uniform()) * kDegToRad);
        spin_.push_back(s_.rotation_speed.sample(cycle_t, rng_.uniform()) * kDegToRad);
        stuck_.push_back(0);
    }
}

void EmitterSim::kill(std::size_t i, ParticleCollider* collider) {
    const std::size_t last = count() - 1;
    if (i != last) {
        pos_[i] = pos_[last]; vel_[i] = vel_[last]; age_[i] = age_[last]; life_[i] = life_[last];
        random_[i] = random_[last]; rot_[i] = rot_[last]; spin_[i] = spin_[last]; stuck_[i] = stuck_[last];
        if (collider != nullptr) collider->moved(last, i);
    }
    pos_.pop_back(); vel_.pop_back(); age_.pop_back(); life_.pop_back();
    random_.pop_back(); rot_.pop_back(); spin_.pop_back(); stuck_.pop_back();
}
```

`pack` writes `ParticleVertex` per particle: world position (`local_space` → `transform_point(emitter_world, pos)`), `size = max(0, s_.size.sample(t, random))`, `color = s_.color.sample(t, random)`, world velocity, rotation, stretch, `frame = min(frames - 1, floor(t * frames))`; then if `sort`, `std::sort` by descending squared distance to `eye`. `clear()` empties every array. `prewarm(frame)`: `for (float t = 0; t < s_.duration; t += 1/30.f) step(frame with dt 1/30, true, 0, INT_MAX, nullptr);` then `cycle_time_ = 0; next_burst_ = 0; cycle_over_ = false;` (a prewarmed loop starts its next cycle with a full cycle's particles alive). `take_cycle_ended()` returns and clears `cycle_ended_flag_`.

- [ ] **Step 4: Run `[particle-sim]`; pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: EmitterSim spawns, moves, and packs particles"`

---

### Task 9: ParticleWorld — emitters in the engine

**Files:**
- Create: `src/engine_core/ParticleWorld.hpp`, `src/engine_core/ParticleWorld.cpp`
- Modify: `src/engine_core/Ecs.hpp:43-58,78`, `src/engine_core/Ecs.cpp:25`, `src/engine_core/DataModel.cpp:93-100,586-588,1105-1114`, `src/engine_core/DataModelState.hpp:118-120`, `src/engine_core/DataModel.hpp:486-488`, `src/engine_core/Engine.hpp:142`, `src/engine_core/Engine.cpp` (`run_frame` 331-396, `run_step` stores its dt), `src/engine_core/ScriptBindings.hpp/.cpp` (Emit, Clear), `src/engine_core/LuaApi.cpp` (method docs)
- Test: `sandbox/particle_world_tests.cpp`

**Interfaces:**
- Consumes: Tasks 5, 7, 8.
- Produces:

```cpp
namespace engine_core {

// One emitter's drawable state for a frame. The particle list is immutable and shared.
struct ParticleDraw {
    InstanceId emitter = 0;
    std::shared_ptr<const std::vector<particles::ParticleVertex>> particles;
    std::string texture;          // resolved Texture path, empty for the built-in dot
    bool texture_flip_y = false;
    int grid_x = 1, grid_y = 1;
    ParticleBlendMode blend = ParticleBlendMode::Normal;
    ParticleFacing facing = ParticleFacing::Camera;
    Vec3 facing_axis{0.f, 1.f, 0.f};   // world, unit
    float velocity_stretch = 0.1f;
    float soft_edge = 0.5f;
    float light_influence = 1.f;
    float brightness = 1.f;
    int draw_order = 0;
    Vec3 bounds_min{}, bounds_max{};   // world AABB of the particles, padded by the largest size
};

// Simulates every ParticleEmitter in Workspace (not inside a ViewportPane),
// stopped or playing, once per engine frame. dt 0 freezes them (paused play).
class ParticleWorld {
public:
    void step(DataModel& game, double dt);
    const std::vector<ParticleDraw>& draws() const { return draws_; }
    std::size_t live_total() const { return live_total_; }
    // Tests.
    std::size_t emitter_count() const { return emitters_.size(); }
    const particles::EmitterSim* sim(InstanceId emitter) const;
private:
    struct Entry {
        particles::EmitterSim sim;
        Matrix4 last_parent = matrix4_identity();
        bool seen_parent = false;
        std::uint64_t pass = 0;
        bool finished_pending = false;   // one-shot ended; Finished fires when count reaches 0
    };
    std::unordered_map<InstanceId, Entry> emitters_;
    std::vector<ParticleDraw> draws_;
    std::vector<InstanceId> ids_;
    std::uint64_t pass_ = 0;
    std::uint64_t generation_ = ~0ull;
    std::size_t live_total_ = 0;
    std::uint32_t seed_counter_ = 0x9e3779b9u;
};

// The shape box for an emitter under parent: half extents in the parent's
// frame and that frame (centre and axes). false when the parent gives no
// size (a point at the parent's position).
bool emitter_extent(const DataModel& game, InstanceId parent, Matrix4& frame, Vec3& half);

}  // namespace engine_core
```

Wiring: tag `struct ParticleEmitterTag {};` in `Ecs.hpp`, id `particle_emitter` in `EcsIds` and `Ecs.cpp`, `tag_entity` adds it when `object.particle_emitter()`, a cached `particle_query` built `.with<ecs::Instance>().in().with<ecs::ParticleEmitterTag>().with<ecs::InWorkspace>().without<ecs::InViewport>()`, accessor `void particle_emitters(std::vector<InstanceId>& out) const;` beside `sound_sources`. `Engine.hpp`: `ParticleWorld particles_;` after `audio_`, plus `double last_step_dt_ = 0.0;` set at the end of `run_step` to `step_dt`, and a public `const ParticleWorld& particles() const { return particles_; }`.

In `Engine::run_frame`, inside the write lock, after the `if (paused_now) … else …` block and before `run_window`:

```cpp
        {
            // Stopped: edit-mode particles keep the frame's wall time. Paused play: frozen. Playing: the step's dt.
            PROFILE_SCOPE("Particles", profiler::Group::Engine);
            const double particle_dt = !game_.simulation_running() ? wall : (paused_now ? 0.0 : last_step_dt_);
            guarded_step([&] { particles_.step(game_, particle_dt); }, [&] { contract_count_.fetch_add(1); });
        }
```

- [ ] **Step 1: Failing tests** (`sandbox/particle_world_tests.cpp`; use `audio_tests.cpp`'s rig pattern for a Game with a Workspace)

```cpp
#include "ParticleEmitter.hpp"
#include "ParticleWorld.hpp"
#include "PhysicsObject.hpp"
#include "Attachment.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace engine_core;

namespace {
struct Rig {
    SimRole role;
    Game game;
    ParticleWorld world;
    void frames(int n, double dt = 1.0 / 60.0) { for (int i = 0; i < n; ++i) world.step(game, dt); }
};
ParticleEmitter& emitter_under(Game& game, DataModel& parent) {
    auto& e = game.create<ParticleEmitter>();
    e.set_parent(parent);   // same parenting call as Task 5's tests
    return e;
}
}  // namespace

TEST_CASE("PW1 an emitter in Workspace simulates stopped; one in a ViewportPane does not", "[particle-world]") {
    Rig rig;
    ParticleEmitter& e = emitter_under(rig.game, rig.game.scene_service("Workspace"));
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(60.f)));
    rig.frames(60);
    REQUIRE(rig.world.emitter_count() == 1);
    REQUIRE(e.particle_count() > 40);
    REQUIRE(rig.world.draws().size() == 1);
    // Move it into a ViewportPane (create one under a ScreenGui as viewport tests do).
    // ...then: rig.frames(1); REQUIRE(rig.world.emitter_count() == 0); REQUIRE(rig.world.draws().empty());
}

TEST_CASE("PW2 a one-shot sets Playing false at its cycle's end and fires Finished when its last particle dies", "[particle-world]") {
    Rig rig;
    ParticleEmitter& e = emitter_under(rig.game, rig.game.scene_service("Workspace"));
    REQUIRE_FALSE(e.set_looping(false));
    REQUIRE_FALSE(e.set_duration(0.5));
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(0.5f)));
    int finished = 0;
    // connect to e's Finished event with the same API Touched tests use (DataModel signal connect); count fires.
    rig.frames(31);   // ~0.52 s
    REQUIRE_FALSE(e.playing());
    REQUIRE(finished == 0);
    rig.frames(40);
    REQUIRE(e.particle_count() == 0);
    REQUIRE(finished == 1);
    REQUIRE_FALSE(e.set_playing(true));   // replays
    rig.frames(5);
    REQUIRE(e.particle_count() > 0);
}

TEST_CASE("PW3 Emit and Clear take effect on the next step", "[particle-world]") {
    Rig rig;
    ParticleEmitter& e = emitter_under(rig.game, rig.game.scene_service("Workspace"));
    REQUIRE_FALSE(e.set_playing(false));
    e.request_emit(25);
    rig.frames(1);
    REQUIRE(e.particle_count() == 25);
    e.request_clear();
    rig.frames(1);
    REQUIRE(e.particle_count() == 0);
}

TEST_CASE("PW4 particles spawn at the parent and follow it in Local space", "[particle-world]") {
    Rig rig;
    auto& attachment = rig.game.create<Attachment>();
    // parent the attachment to Workspace (or a GameObject) and set its Offset to (10, 0, 0) as attachment tests do
    ParticleEmitter& e = emitter_under(rig.game, attachment);
    REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)));
    rig.frames(30);
    const auto& draw = rig.world.draws().front();
    REQUIRE(draw.particles->front().position[0] == Catch::Approx(10.f).margin(1e-3));
}

TEST_CASE("PW5 InheritVelocity takes an assembly body's velocity and nothing without one", "[particle-world]") {
    Rig rig;
    // A PhysicsObject (body) under Workspace; store_simulated a velocity of (20,0,0) as AssemblyPlay does;
    // emitter under it with InheritVelocity 1, EmitSpeed 0; frames(1); a particle's velocity.x == 20.
    // Same emitter under a bare Attachment in Workspace: velocity.x == 0.
}

TEST_CASE("PW6 Stop clears particles spawned in play; destroying an emitter removes its draw", "[particle-world]") {
    Rig rig;
    ParticleEmitter& e = emitter_under(rig.game, rig.game.scene_service("Workspace"));
    rig.game.start_simulation();
    rig.frames(30);
    REQUIRE(e.particle_count() > 0);
    rig.game.stop_simulation();
    rig.frames(1);
    REQUIRE(e.particle_count() <= 1);   // cleared, then at most a frame's spawns
    e.destroy();                        // whatever DataModel's destroy call is
    rig.frames(1);
    REQUIRE(rig.world.draws().empty());
}

TEST_CASE("PW7 a destroyed parent leaves World-space particles flying", "[particle-world]") {
    Rig rig;
    auto& attachment = rig.game.create<Attachment>();
    // parent attachment to Workspace; emitter under it; frames(30); destroy the attachment (the emitter goes with it):
    // then the emitter is gone, so its draw is gone. Reparent case: move the emitter from the attachment to Workspace
    // mid-run: particle_count does not drop to 0 and no crash.
}
```

Fill the commented test bodies with the real helper calls from `audio_tests.cpp`, `attachment` tests, and `touched` tests (search `sandbox/` for `Attachment` and `connect`); every case must end in concrete `REQUIRE`s as sketched.

- [ ] **Step 2: Run; fail.**
- [ ] **Step 3: Implement `ParticleWorld.cpp`**

```cpp
void ParticleWorld::step(DataModel& game, double dt) {
    if (generation_ != game.world_generation()) {   // Stop happened: everything spawned in play goes
        emitters_.clear();
        generation_ = game.world_generation();
    }
    ++pass_;
    game.particle_emitters(ids_);
    const Vec3 eye = camera_position(game);          // AudioWorld.cpp:387-393 idiom
    // Pass 1: settings, transforms, and how many each wants (Task 10 adds the budget here).
    // Pass 2: step each sim, store counts, fire events, pack draws.
    draws_.clear();
    live_total_ = 0;
    for (InstanceId id : ids_) {
        auto* emitter = dynamic_cast<ParticleEmitter*>(game.instance(id));
        if (emitter == nullptr) continue;
        Entry& entry = emitters_[id];
        entry.pass = pass_;
        const std::uint32_t dirty = emitter->take_dirty();
        if (dirty & ParticleEmitter::kDirtySettings) entry.sim.configure(build_settings(game, *emitter));
        if (dirty & ParticleEmitter::kDirtyRestart) {
            entry.sim.restart(seed_counter_ += 0x6d2b79f5u);
            entry.finished_pending = false;
            if (emitter->prewarm() && emitter->looping()) { /* prewarm below, after the frame is known */ }
        }
        if (emitter->take_clear()) entry.sim.clear();
        const InstanceId parent = game.parent(id);
        Matrix4 shape_frame = matrix4_identity();
        Vec3 half{0, 0, 0};
        if (!emitter_extent(game, parent, shape_frame, half)) half = Vec3{0, 0, 0};
        particles::EmitterFrame frame;
        frame.current = shape_frame;
        frame.previous = entry.seen_parent ? entry.last_parent : shape_frame;
        frame.half = half;
        frame.inherited_linear = inherited_velocity(game, parent, matrix4_position(shape_frame));
        frame.dt = static_cast<float>(dt);
        if ((dirty & ParticleEmitter::kDirtyRestart) && emitter->prewarm() && emitter->looping()) entry.sim.prewarm(frame);
        const int emits = emitter->take_emit_requests();
        entry.sim.step(frame, emitter->playing(), emits, std::numeric_limits<int>::max(), collider_for(game, *emitter, entry));
        entry.last_parent = shape_frame;
        entry.seen_parent = true;
        if (entry.sim.take_cycle_ended()) { emitter->store_finished_cycle(); entry.finished_pending = true; }
        const int count = static_cast<int>(entry.sim.count());
        emitter->store_particle_count(count);
        if (entry.finished_pending && count == 0) {
            entry.finished_pending = false;
            game.fire_event(id, "Finished", {});
        }
        live_total_ += entry.sim.count();
        if (count > 0) draws_.push_back(make_draw(game, *emitter, entry, shape_frame, eye));
    }
    // Gone, moved into a ViewportPane, or out of Workspace: dropped now.
    for (auto it = emitters_.begin(); it != emitters_.end();) it = it->second.pass == pass_ ? std::next(it) : emitters_.erase(it);
}
```

`build_settings` bakes every curve from the emitter's getters into `ParticleSettings` (bursts sorted by time, `frames = texture_mode == Flipbook ? grid_x * grid_y : 1`, `emit_direction_local = emit_direction_space == Local`, etc.). `collider_for` returns `nullptr` until Task 11. `make_draw` packs with `entry.sim.pack(*vec, shape_frame, eye, blend == Normal)` into a fresh `std::make_shared<std::vector<ParticleVertex>>`, resolves the texture path from the emitter's `Texture?` reference the way the snapshot resolves Material textures (find `diffuse_texture` in `SnapshotPump.cpp` and reuse its helper; flip and always-loaded flags likewise), computes `facing_axis` world (`FacingAxisSpace == Local` → `normalize(matrix4_vector(shape_frame, axis))`), and the AABB padded by the largest particle size.

`emitter_extent`:

```cpp
bool emitter_extent(const DataModel& game, InstanceId parent, Matrix4& frame, Vec3& half) {
    const DataModel* object = parent != DataModel::kNoParent ? game.instance(parent) : nullptr;
    const auto* pv = dynamic_cast<const PVInstance*>(object);
    if (pv == nullptr) { frame = matrix4_identity(); half = {0, 0, 0}; return false; }
    frame = pv->transform();
    if (const auto* body = dynamic_cast<const PhysicsObject*>(object)) {
        const Vec3 size = body->size();
        half = {size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
        return true;   // transform() axes already carry any GameObject scale? If not, multiply by PhysicsWorld::shape_scale(game, *body).
    }
    if (const auto* brush = dynamic_cast<const Brush*>(object)) {
        const auto mesh = brush->mesh();
        if (mesh) {
            const Vec3 centre{(mesh->min[0] + mesh->max[0]) * 0.5f, (mesh->min[1] + mesh->max[1]) * 0.5f, (mesh->min[2] + mesh->max[2]) * 0.5f};
            half = {(mesh->max[0] - mesh->min[0]) * 0.5f, (mesh->max[1] - mesh->min[1]) * 0.5f, (mesh->max[2] - mesh->min[2]) * 0.5f};
            frame = matrix4_multiply(frame, matrix4_translation(centre));
            return true;
        }
    }
    if (const auto* go = dynamic_cast<const GameObject*>(object)) {
        const LuaSlot slot = go->prefab();
        const auto* prefab = slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Prefab*>(game.instance(slot.id)) : nullptr;
        Vec3 low, high;
        if (prefab != nullptr && prefab->bounds(low, high)) {
            const float s = static_cast<float>(go->scale());
            const Vec3 o = prefab->origin_offset();   // check whether bounds() is already relative to the origin
            const Vec3 centre{(low.x + high.x) * 0.5f * s, (low.y + high.y) * 0.5f * s, (low.z + high.z) * 0.5f * s};
            half = {(high.x - low.x) * 0.5f * s, (high.y - low.y) * 0.5f * s, (high.z - low.z) * 0.5f * s};
            (void)o;
            frame = matrix4_multiply(frame, matrix4_translation(centre));
            return true;
        }
    }
    half = {0, 0, 0};
    return false;
}
```

Verify the PhysicsObject/Brush scale question against `PhysicsWorld::shape_scale` (PhysicsWorld.hpp:236) while implementing, and add a PW test that a PhysicsObject of Size (4,2,6) gives particles only inside that box (sample 200 particles with EmitSpeed 0, `REQUIRE` each within ±(2,1,3) of the body centre in its frame). Replace Task 5's `parent_has_size` body with `Matrix4 f; Vec3 h; return emitter_extent(world, world.parent(object.id()), f, h);`.

`inherited_velocity(game, parent, point)`: `const InstanceId root = game.assemblies().root_of(game, parent); const auto* body = dynamic_cast<const PhysicsBase*>(game.instance(root)); if (!body) return {0,0,0};` linear `body->velocity()`, angular from `PhysicsObject::angular_velocity()` or `Brush::angular_velocity()`, `r = point - matrix4_position(body->transform())`, return `v + cross(w, r)`. `root_of` returns 0 outside an assembly → `{0,0,0}`.

Emit/Clear bindings in `ScriptBindings` exactly like `emitter_play` (a `particle_self(state)` resolving `ParticleEmitter`, `Emit` reads `luaL_checkinteger(state, 2)` and refuses negatives with `"Emit expects a count of 0 or more"`), registered as `lua_method("Emit", "nil", ...)` and `lua_method("Clear", "nil", ...)` in a `register_lua_class("ParticleEmitter", nullptr, ...)` block like SoundEmitter's at 1609-1614; docs `add("ParticleEmitter", "Emit", "Spawns count particles now, whether or not it is Playing. MaxParticles and Workspace.ParticleBudget still apply.", "nil", false, {P("count", "number")});`.

- [ ] **Step 4: Run `[particle-world]`, then the whole sandbox; pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: ParticleWorld steps emitters every frame, stopped or playing"`

---

### Task 10: The global budget

**Files:**
- Modify: `src/engine_core/ParticleWorld.cpp`
- Test: `sandbox/particle_world_tests.cpp`

**Interfaces:**
- Consumes: `Workspace::particle_budget()` (Task 5), `EmitterSim::wanted_spawns` (Task 8).
- Produces: allowance per emitter = its full want while `live_total + Σwant <= budget`; otherwise the overflow is taken from the farthest emitters first (sorted by squared distance from the camera to the emitter's shape frame position), each giving up to all of its want, until `live_total + Σallowance <= budget`. Live particles are never removed. `Emit` requests count against the want.

- [ ] **Step 1: Failing tests**

```cpp
TEST_CASE("PW8 over ParticleBudget, far emitters spawn fewer first and live particles are never culled", "[particle-world]") {
    Rig rig;
    auto* workspace = dynamic_cast<Workspace*>(rig.game.instance(rig.game.scene_service("Workspace").id()));
    REQUIRE_FALSE(workspace->set_particle_budget(100));
    // Two attachments in Workspace: near at (0,0,0), far at (0,0,500); the Workspace camera at (0,0,-10) looking +Z
    // (set CurrentCamera's Transform as other tests do). An emitter under each, Rate 1000, Lifetime 100.
    rig.frames(30);
    REQUIRE(rig.world.live_total() <= 100);
    REQUIRE(near_emitter.particle_count() > far_emitter.particle_count());
    REQUIRE_FALSE(workspace->set_particle_budget(10));
    const int before = near_emitter.particle_count() + far_emitter.particle_count();
    rig.frames(1);
    REQUIRE(near_emitter.particle_count() + far_emitter.particle_count() == before);   // nothing killed
}

TEST_CASE("PW9 huge rates, tiny lifetimes, and zero budgets stay bounded", "[particle-world]") {
    Rig rig;
    ParticleEmitter& e = emitter_under(rig.game, rig.game.scene_service("Workspace"));
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(1e9f)));
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(1e-6f)));   // clamped to 1e-3 in the sim
    rig.frames(3);
    REQUIRE(e.particle_count() <= 1000);
    REQUIRE_FALSE(e.set_max_particles(0));
    rig.frames(1);
    REQUIRE(e.particle_count() == 0);
    auto* workspace = dynamic_cast<Workspace*>(rig.game.instance(rig.game.scene_service("Workspace").id()));
    REQUIRE_FALSE(workspace->set_particle_budget(0));
    REQUIRE_FALSE(e.set_max_particles(1000));
    e.request_emit(500);
    rig.frames(1);
    REQUIRE(e.particle_count() == 0);
}
```

- [ ] **Step 2: Run; fail (budget ignored).**
- [ ] **Step 3: Implement**: split `step`'s loop into pass 1 (settings, frame, want = `sim.wanted_spawns(dt, playing, pending_emits)`, distance) storing a small `Pending` vector, then the allocation, then pass 2 (`sim.step(..., allowance, ...)`). Allocation:

```cpp
std::size_t live = 0;
for (const auto& [id, entry] : emitters_) live += entry.sim.count();
long long room = static_cast<long long>(budget) - static_cast<long long>(live);
long long want = 0;
for (const Pending& p : pending) want += p.want;
long long over = want - std::max(0LL, room);
std::sort(order.begin(), order.end(), [&](int a, int b) { return pending[a].distance2 > pending[b].distance2; });   // far first
for (int k : order) {
    if (over <= 0) break;
    const long long cut = std::min<long long>(over, pending[k].allowance);
    pending[k].allowance -= static_cast<int>(cut);
    over -= cut;
}
```

with `allowance` initialised to `want`. Pending emits that cannot fit are dropped (not carried), so a scripted burst under budget pressure spawns what fits.

- [ ] **Step 4: Run; pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: Workspace.ParticleBudget throttles the farthest emitters first"`

---

### Task 11: Collision

**Files:**
- Create: `src/engine_core/particles/ParticleColliders.hpp`, `src/engine_core/particles/ParticleColliders.cpp`
- Modify: `src/engine_core/ParticleWorld.hpp/.cpp` (`collider_for`, per-entry collider state)
- Test: `sandbox/particle_world_tests.cpp`, `sandbox/particle_sim_tests.cpp`

**Interfaces:**
- Consumes: `ParticleCollider` (Task 8), `VoxelSampler` (`terrain/VoxelSampler.hpp:32-47`), `PhysicsWorld::raycast` (`PhysicsWorld.hpp:123`), `DataModel::terrains`.
- Produces:

```cpp
namespace engine_core::particles {
// Hits Terrain in Workspace: a particle that ends a step inside solid voxels is
// pushed to the surface along the distance field's gradient.
class TerrainCollider : public ParticleCollider {
public:
    void prepare(DataModel& game);   // once per frame: inverse transforms and samplers of every collidable Terrain
    void begin(std::size_t) override {}
    std::optional<ParticleHit> test(std::size_t slot, Vec3 from, Vec3 to) override;
    void moved(std::size_t, std::size_t) override {}
};
// Terrain first, then up to kRaysPerFrame Box3D rays per frame shared by every
// World emitter; a particle whose turn has not come tests its cached hit plane.
class WorldCollider : public ParticleCollider {
public:
    static constexpr int kRaysPerFrame = 512;
    void prepare(DataModel& game, TerrainCollider& terrain, const std::string& collision_group, int* ray_budget);
    void begin(std::size_t count) override;     // resizes the plane cache, advances the round-robin cursor
    std::optional<ParticleHit> test(std::size_t slot, Vec3 from, Vec3 to) override;
    void moved(std::size_t from, std::size_t to) override;
private:
    struct Plane { Vec3 n; float d; bool valid; };
    std::vector<Plane> planes_;
    std::size_t cursor_ = 0;
    // ...
};
}  // namespace engine_core::particles
```

- [ ] **Step 1: Failing tests**

A sim-level test with a fake floor collider (no engine needed):

```cpp
namespace {
struct Floor : ParticleCollider {
    void begin(std::size_t) override {}
    void moved(std::size_t, std::size_t) override {}
    std::optional<ParticleHit> test(std::size_t, Vec3 from, Vec3 to) override {
        if (from.y >= 0.f && to.y < 0.f) {
            const float f = from.y / (from.y - to.y);
            return ParticleHit{{from.x + (to.x - from.x) * f, 0.f, from.z + (to.z - from.z) * f}, {0.f, 1.f, 0.f}};
        }
        return std::nullopt;
    }
};
}  // namespace

TEST_CASE("SIM14 bounce, stick, and die against a floor", "[particle-sim]") {
    for (auto response : {ParticleCollisionResponse::Bounce, ParticleCollisionResponse::Stick, ParticleCollisionResponse::Die}) {
        ParticleSettings s = basic();
        s.rate = table(NumberCurve::constant(0.f));
        s.bursts = {{0.f, table(NumberCurve::constant(1.f))}};
        s.acceleration = Vec3{0.f, -20.f, 0.f};
        s.collision = ParticleCollision::Terrain;
        s.response = response;
        s.bounciness = 0.5f;
        EmitterSim sim;
        sim.configure(s);
        sim.restart(0);
        Floor floor;
        EmitterFrame f;
        f.previous = f.current = matrix4_translation(Vec3{0.f, 5.f, 0.f});
        f.dt = 1.f / 120.f;
        for (int i = 0; i < 240; ++i) sim.step(f, true, 0, 1 << 30, &floor);
        if (response == ParticleCollisionResponse::Die) {
            REQUIRE(sim.count() == 0);
        } else {
            REQUIRE(sim.count() == 1);
            REQUIRE(sim.position(0).y >= -1e-3f);
            if (response == ParticleCollisionResponse::Stick) REQUIRE(length(sim.velocity(0)) == 0.f);
        }
    }
}
```

World-level tests (use the terrain rig other sandbox terrain tests use — search `sandbox/` for a test that fills voxels, e.g. a "fill a box of solid" helper — and the physics rig from `physics_rig.hpp` for a Brush floor):

```cpp
TEST_CASE("PW10 Terrain collision stops particles at voxel ground", "[particle-world]") {
    // Terrain with solid voxels below y = 0 in Workspace; an emitter at (0,10,0) with Acceleration (0,-50,0),
    // Collision Terrain, CollisionResponse Stick; frames(120); every particle has y >= -1 (one voxel tolerance).
}
TEST_CASE("PW11 World collision hits a Brush, obeys CollisionGroup, and keeps to the ray budget", "[particle-world]") {
    // A 20x1x20 anchored Brush at y = 0; emitter above, Collision World, Stick: particles end with y >= 0.4.
    // Put the Brush in a group that does not collide with "Particles" and set the emitter's CollisionGroup "Particles":
    // particles fall through (some y < -5 after 2 s).
    // Ray budget: 5000 particles with Collision World in one frame -> the test collider counts raycasts <= 512.
    // (Expose `int ParticleWorld::rays_last_frame() const` for this test.)
}
```

Fill these with real rig code; each ends in the stated `REQUIRE`s.

- [ ] **Step 2: Run; fail.**
- [ ] **Step 3: Implement.** `TerrainCollider::prepare`: `game.terrains(ids)`; for each `Terrain` with `CanCollide`, keep `inverse = matrix4_inverse(terrain->transform())`, `forward = terrain->transform()`, and a `VoxelSampler(terrain->volume().chunks(), voxel_size)` (one per terrain; not thread-safe, used on the engine thread only). `test`: for each terrain, `local = matrix4_point(inverse, to)`; `d = sampler.distance(local)`; if `d < 0`: `n_local = normalize(sampler.gradient(local))`; `surface = local - n_local * d` (d negative → pushes outward); return `{matrix4_point(forward, surface), normalize(matrix4_vector(forward, n_local))}`.

`WorldCollider::test`: terrain first (`terrain_->test`), then: if `*ray_budget_ > 0` and this slot is within the round-robin window (`(slot + cursor_) % stride == 0` where `stride = max(1, count / kRaysPerFrame)`), cast `game.physics().raycast(game, from, to - from, filter)` with `filter.include = false` (exclude nothing), `filter.collision_group = group_`; decrement the shared budget; on hit store `planes_[slot] = {normal, dot(normal, point), true}` and return the hit; on miss `planes_[slot].valid = false`. Otherwise, if `planes_[slot].valid`: if `dot(n, from) - d >= 0 && dot(n, to) - d < 0`, return the intersection on the plane. `moved(from, to)`: `planes_[to] = planes_[from]`. Raycast cost: `raycast` syncs physics first; that is a no-op when nothing changed, but profile it in Task 17 and, if it shows, add a `PhysicsWorld::raycast_synced` that skips `sync` and call `sync` once per frame from ParticleWorld before the first World emitter.

`collider_for`: `None` → `nullptr`; `Terrain` → the world's shared `TerrainCollider` (prepared once per frame, lazily, when any emitter needs it); `World` → that entry's `WorldCollider` prepared with the shared terrain collider and the shared per-frame ray budget (reset to `kRaysPerFrame` at the top of `step`).

- [ ] **Step 4: Run; pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: collision with terrain voxels and, on a ray budget, the world"`

---

### Task 12: Particles in the render snapshot

**Files:**
- Modify: `src/engine_core/SnapshotPump.hpp:317-349,373,445`, `src/engine_core/SnapshotPump.cpp` (`take_changes` 794-815, `blit` 653-673, new `resolve_particles`), `src/engine_core/Engine.cpp:80,103` (`pump_.set_particle_world(&particles_)` / `nullptr`)
- Test: `sandbox/particle_world_tests.cpp` (or the existing snapshot test file — search `sandbox/` for `take_changes`)

**Interfaces:**
- Consumes: `ParticleWorld::draws()` (Task 9).
- Produces: `std::vector<ParticleDraw> VisualSnapshot::particles;` rebuilt at every `take_changes` ("Rebuilt at every take_changes, like draggers. Pointers only: each particle list is immutable and shared with the simulation."), `void SnapshotPump::set_particle_world(const ParticleWorld*)`.

- [ ] **Step 1: Failing test**

```cpp
TEST_CASE("PW12 the snapshot carries each drawn emitter's particles by pointer", "[particle-world]") {
    // Build a SnapshotPump the way the existing snapshot tests do, set_particle_world(&rig.world),
    // add an emitter, rig.frames(10), pump.take_changes(game); pump.finish_copy(); pump.publish();
    const VisualSnapshot& snap = pump.front();
    REQUIRE(snap.particles.size() == 1);
    REQUIRE(snap.particles[0].particles.get() == rig.world.draws()[0].particles.get());   // shared, not copied
}
```

- [ ] **Step 2: Run; fail.**
- [ ] **Step 3: Implement** `resolve_particles` exactly like `resolve_terrains` (`base_.particles.assign(world->draws().begin(), world->draws().end())` or clear when null), call it after `resolve_terrains(game)`, add `dst.particles = base_.particles;` in `blit`, wire the pointer in `Engine.cpp` beside `set_terrain_world`.
- [ ] **Step 4: Run; pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: the render snapshot carries particle blocks"`

---

### Task 13: The particle pass in the renderer

**Files:**
- Create: `src/runner/ParticleRenderer.hpp`, `src/runner/ParticleRenderer.cpp`, `resources/shaders/pipeline/particle.vert`, `resources/shaders/pipeline/particle.frag`
- Modify: `src/runner/Renderer.hpp/.cpp` (own a `ParticleRenderer`; build its program in the `buildProgram` chain 405-453; call it at the end of the transparency pass ~2626, before `glDisable(GL_BLEND)`; a depth copy texture created beside `depthTexture_` ~695), `src/runner/GameView.cpp` / `SceneFeed` (pass `snapshot.particles` and the camera through to the renderer the way `terrains` are passed), `CMakeLists.txt`
- Test: `tests/SceneRenderCheck.cpp`

**Interfaces:**
- Consumes: `VisualSnapshot::particles` (Task 12), `TextureCache::get`, the forward pass's light uniforms.
- Produces: `class ParticleRenderer { bool create(); void destroy(); void draw(const std::vector<engine_core::ParticleDraw>& draws, const Frame& frame, TextureCache& textures); };` where `Frame` holds view, projection, inverse projection, camera position/right/up, viewport size, the depth-copy texture, ambient, sun direction/colour, and the forward light arrays (copy the exact set the forward pass uploads).

Per-instance vertex layout (one `ParticleVertex`, 64 bytes, divisor 1) on its own VAO with a static unit quad (4 vertices, 6 indices) at location 0:

| loc | attribute | from |
| --- | --- | --- |
| 0 | `aCorner` vec2 | quad (-0.5..0.5) |
| 1 | `iPosition` vec3 | position |
| 2 | `iSize` float | size |
| 3 | `iColor` vec4 | color |
| 4 | `iVelocity` vec3 | velocity |
| 5 | `iRotation` float | rotation |
| 6 | `iStretch` float | stretch |
| 7 | `iFrame` float | frame |

- [ ] **Step 1: Failing render checks** (add to `tests/SceneRenderCheck.cpp`, following an existing transparency case such as `clearCube` at ~2290; each builds a `VisualSnapshot` by hand with one `ParticleDraw` and a `std::make_shared` particle list):

```cpp
// P1: one white Normal particle of size 2 at the view centre draws non-black there and nothing at the corners.
// P2: Additive over a mid-grey quad brightens it; Multiply with colour (0.5,0.5,0.5) darkens it (ReadPixel compare).
// P3: Brightness 4 on an Additive particle produces a pixel above 1.0 before tone mapping (read the merge target,
//     or compare: the bloom neighbourhood around it is brighter than with Brightness 1).
// P4: SoftEdge: a particle centred exactly on an opaque floor plane, SoftEdge 1: the pixel just above the
//     intersection line is fainter than the same pixel with SoftEdge 0.
// P5: a ParticleDraw whose texture path does not exist draws the soft dot (centre pixel non-black, corner of the
//     quad black/transparent), and an empty path does the same.
// P6: Facing World with FacingAxis (0,1,0) seen from straight above draws a full square; seen edge-on draws nearly nothing.
// P7: Flipbook grid 2x1 with a texture whose left half is red and right half blue: frame 0 samples red, frame 1 blue.
```

Write each with `Expect(...)` on `ReadPixel`/`Sum` exactly as neighbouring cases do. Build `scene-render-check` and run it by hand: `build/Debug/scene-render-check.exe` → the new checks fail (no pass yet).

- [ ] **Step 2: Write the shaders**

```glsl
// resources/shaders/pipeline/particle.vert
#version 330 core
layout (location = 0) in vec2 aCorner;
layout (location = 1) in vec3 iPosition;
layout (location = 2) in float iSize;
layout (location = 3) in vec4 iColor;
layout (location = 4) in vec3 iVelocity;
layout (location = 5) in float iRotation;
layout (location = 6) in float iStretch;
layout (location = 7) in float iFrame;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraPosition;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;
uniform int uFacing;            // 0 Camera, 1 CameraUpright, 2 Velocity, 3 World
uniform vec3 uFacingAxis;       // world, unit
uniform float uVelocityStretch;
uniform vec2 uGrid;             // columns, rows

out vec2 vUv;
out vec4 vColor;
out float vViewDepth;
out vec3 vWorld;

void main() {
    // Stretch keeps area: x * e^(-s/2), y * e^(s/2) with s clamped.
    float s = clamp(iStretch, -4.0, 4.0);
    vec2 corner = aCorner * vec2(exp(-0.5 * s), exp(0.5 * s)) * iSize;
    float c = cos(iRotation), sn = sin(iRotation);
    vec3 right, up;
    if (uFacing == 2) {
        vec3 v = iVelocity;
        float speed = length(v);
        vec3 dir = speed > 1e-4 ? v / speed : uCameraUp;
        vec3 toCam = normalize(uCameraPosition - iPosition);
        right = normalize(cross(dir, toCam));
        up = dir * (1.0 + speed * uVelocityStretch);
    } else if (uFacing == 3) {
        vec3 n = uFacingAxis;
        vec3 t = abs(n.y) < 0.9 ? normalize(cross(n, vec3(0.0, 1.0, 0.0))) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
        right = t;
        up = cross(n, t);
    } else if (uFacing == 1) {
        vec3 toCam = uCameraPosition - iPosition;
        toCam.y = 0.0;
        vec3 f = length(toCam) > 1e-4 ? normalize(toCam) : vec3(0.0, 0.0, 1.0);
        right = normalize(cross(vec3(0.0, 1.0, 0.0), f));
        up = vec3(0.0, 1.0, 0.0);
    } else {
        right = uCameraRight;
        up = uCameraUp;
    }
    vec2 r = (uFacing == 2) ? corner : vec2(c * corner.x - sn * corner.y, sn * corner.x + c * corner.y);
    vec3 world = iPosition + right * r.x + up * r.y;
    vec4 view = uView * vec4(world, 1.0);
    gl_Position = uProjection * view;
    vViewDepth = -view.z;
    vWorld = world;
    vColor = iColor;
    float frame = iFrame;
    float col = mod(frame, uGrid.x);
    float row = floor(frame / uGrid.x);
    vUv = (vec2(col, row) + (aCorner + 0.5)) / uGrid;
}
```

```glsl
// resources/shaders/pipeline/particle.frag
#version 330 core
in vec2 vUv;
in vec4 vColor;
in float vViewDepth;
in vec3 vWorld;

uniform sampler2D uTexture;
uniform bool uHasTexture;
uniform sampler2D uSceneDepth;   // a copy of scene depth, not the attached one
uniform vec2 uViewport;
uniform mat4 uInverseProjection;
uniform float uSoftEdge;
uniform float uBrightness;
uniform float uLightInfluence;
uniform int uBlend;              // 0 Normal, 1 Additive, 2 Multiply
uniform vec3 uAmbient;
uniform vec3 uSunDirection;      // toward the sun, world
uniform vec3 uSunColor;
uniform int uLightCount;
uniform vec4 uLightPositionRadius[32];
uniform vec4 uLightColorIntensity[32];

out vec4 outColor;

float sceneViewDepth(vec2 uv) {
    float d = texture(uSceneDepth, uv).r;
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 view = uInverseProjection * clip;
    return -view.z / view.w;
}

void main() {
    vec4 tex;
    if (uHasTexture) {
        tex = texture(uTexture, vUv);
    } else {
        vec2 p = (fract(vUv * 1.0) - 0.5) * 2.0;   // grid cell UVs are not used without a texture
        float r = length(p);
        tex = vec4(1.0, 1.0, 1.0, smoothstep(1.0, 0.0, r));
    }
    vec4 c = tex * vColor;
    float fade = 1.0;
    if (uSoftEdge > 0.0) {
        vec2 uv = gl_FragCoord.xy / uViewport;
        fade = clamp((sceneViewDepth(uv) - vViewDepth) / uSoftEdge, 0.0, 1.0);
    }
    c.a *= fade;
    // Lighting at the fragment, unshadowed: ambient + sun (half-Lambert facing the camera is not known; use full) + point lights by distance.
    vec3 light = uAmbient + uSunColor;
    for (int i = 0; i < uLightCount; ++i) {
        vec3 d = uLightPositionRadius[i].xyz - vWorld;
        float dist = length(d);
        float radius = uLightPositionRadius[i].w;
        float att = clamp(1.0 - dist / max(radius, 1e-4), 0.0, 1.0);
        light += uLightColorIntensity[i].rgb * uLightColorIntensity[i].a * att * att;
    }
    vec3 lit = mix(vec3(1.0), light, uLightInfluence);
    vec3 rgb = c.rgb * lit * uBrightness;
    if (uBlend == 2) {
        // Multiply: blended with (DST_COLOR, ZERO); fade toward white where transparent.
        outColor = vec4(mix(vec3(1.0), rgb, c.a), 1.0);
    } else if (uBlend == 1) {
        outColor = vec4(rgb * c.a, 0.0);        // premultiplied, adds light, covers nothing
    } else {
        outColor = vec4(rgb * c.a, c.a);        // premultiplied over
    }
}
```

Check the sun/ambient uniform names and values the forward pass computes (`uAmbient`, the sun from the lighting snapshot) and pass the same values; spot cones may be ignored for particles (point falloff only) — note this in the shader comment.

- [ ] **Step 3: Write `ParticleRenderer`**

```cpp
bool ParticleRenderer::create() {
    // VAO with a static quad (location 0) and a streamed instance VBO (locations 1-7, divisor 1).
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    const float corners[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
    const unsigned indices[] = {0, 1, 2, 0, 2, 3};
    glGenBuffers(1, &quad_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glGenBuffers(1, &index_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glGenBuffers(1, &instances_);
    glBindBuffer(GL_ARRAY_BUFFER, instances_);
    const GLsizei stride = sizeof(engine_core::particles::ParticleVertex);
    const auto attr = [&](GLuint loc, GLint size, std::size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, size, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offset));
        glVertexAttribDivisor(loc, 1);
    };
    using V = engine_core::particles::ParticleVertex;
    attr(1, 3, offsetof(V, position));
    attr(2, 1, offsetof(V, size));
    attr(3, 4, offsetof(V, color));
    attr(4, 3, offsetof(V, velocity));
    attr(5, 1, offsetof(V, rotation));
    attr(6, 1, offsetof(V, stretch));
    attr(7, 1, offsetof(V, frame));
    glBindVertexArray(0);
    return true;
}

void ParticleRenderer::draw(const std::vector<engine_core::ParticleDraw>& draws, const Frame& frame, TextureCache& textures) {
    if (draws.empty()) return;
    // Order: DrawOrder ascending, then far to near by AABB centre.
    order_.resize(draws.size());
    std::iota(order_.begin(), order_.end(), 0);
    std::stable_sort(order_.begin(), order_.end(), [&](int a, int b) {
        if (draws[a].draw_order != draws[b].draw_order) return draws[a].draw_order < draws[b].draw_order;
        return distance2(draws[a], frame.camera) > distance2(draws[b], frame.camera);
    });
    glUseProgram(program_);
    glBindVertexArray(vao_);
    // set view/projection/camera/lights/ambient/sun/viewport/inverse projection uniforms once
    glActiveTexture(GL_TEXTURE0 + kUnitParticleDepth);
    glBindTexture(GL_TEXTURE_2D, frame.depth_copy);
    for (int k : order_) {
        const auto& d = draws[k];
        if (!frame.frustum_contains(d.bounds_min, d.bounds_max) || !d.particles || d.particles->empty()) continue;
        glBindBuffer(GL_ARRAY_BUFFER, instances_);
        glBufferData(GL_ARRAY_BUFFER, d.particles->size() * sizeof(engine_core::particles::ParticleVertex), d.particles->data(), GL_STREAM_DRAW);
        const unsigned texture = d.texture.empty() ? 0u : textures.get(d.texture, engine_core::texture::Usage::Color, d.texture_flip_y);
        // A grey loading placeholder is fine to draw; 0 means missing: use the dot.
        // set per-draw uniforms: uFacing, uFacingAxis, uVelocityStretch, uGrid, uHasTexture, uSoftEdge, uBrightness, uLightInfluence, uBlend
        if (d.blend == engine_core::ParticleBlendMode::Multiply) {
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
        } else {
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(d.particles->size()));
    }
    glBindVertexArray(0);
}
```

In `Renderer`, right before the particle draw inside the transparency pass, copy depth once per frame when `!particles.empty()`: a `particleDepthTexture_` (`GL_DEPTH_COMPONENT24`, same size as `depthTexture_`, recreated with it) attached to `particleDepthFbo_`; `glBindFramebuffer(GL_READ_FRAMEBUFFER, <the FBO that has depthTexture_ attached>)`, `glBindFramebuffer(GL_DRAW_FRAMEBUFFER, particleDepthFbo_)`, `glBlitFramebuffer(0,0,w,h,0,0,w,h, GL_DEPTH_BUFFER_BIT, GL_NEAREST)`, then rebind `transparencyFbo_`. The forward pass's existing early return when there are no transparent runs (`if (batches_.opaqueRuns == runs.size()) return true;`) must not skip particles: restructure so the meshes part is skipped but the particle draw still runs. Keep depth test on, depth writes off. Restore the forward blend state after particles. `frame.frustum_contains` uses the view-projection matrix against the AABB's 8 corners (a conservative test is fine).

- [ ] **Step 4: Build `scene-render-check` and `AnarchyStudio`; run the check; P1–P7 pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: an instanced particle pass with soft edges, blend modes, and flipbooks"`

---

### Task 14: The selected emitter's shape in the studio

**Files:**
- Create: `src/engine_core/ParticleShapeLines.hpp`, `src/engine_core/ParticleShapeLines.cpp`
- Modify: `src/runner/GameView.cpp` (`readSelectedBodies` 718-754, `collectOutlines` 917-929), `CMakeLists.txt`
- Test: `sandbox/particle_shape_tests.cpp`

**Interfaces:**
- Consumes: `emitter_extent` (Task 9).
- Produces: `void particle_shape_lines(ParticleShape shape, float arc_degrees, const Matrix4& frame, Vec3 half, std::vector<float>& out);` appending 7 floats per point (x y z r g b a), two points per line, in the same format as `constraint_lines`. Colour: `(1.0, 0.75, 0.2, 1.0)`. Point (or zero half): a small 3-axis cross of half-length 0.25 units. Box: 12 edges. Sphere: three ellipses (XY, YZ, XZ), 32 segments each, XZ limited by `arc`. Hemisphere: the XZ rim plus two half-ellipses. Cylinder: top and bottom ellipses plus 4 verticals. Cone: base ellipse at +half.y plus 4 lines to the apex at -half.y. Disc: one ellipse.

- [ ] **Step 1: Failing test**

```cpp
TEST_CASE("PS6 shape lines: a box has 12 edges and every point lies on the shape", "[particle-shapes]") {
    std::vector<float> out;
    particle_shape_lines(ParticleShape::Box, 360.f, matrix4_identity(), Vec3{1, 2, 3}, out);
    REQUIRE(out.size() == 12 * 2 * 7);
    for (std::size_t i = 0; i < out.size(); i += 7) {
        REQUIRE(std::fabs(out[i]) <= 1.0001f);
        REQUIRE(std::fabs(out[i + 1]) <= 2.0001f);
        REQUIRE(std::fabs(out[i + 2]) <= 3.0001f);
    }
    out.clear();
    particle_shape_lines(ParticleShape::Disc, 360.f, matrix4_translation(Vec3{0, 5, 0}), Vec3{1, 0, 1}, out);
    for (std::size_t i = 0; i < out.size(); i += 7) REQUIRE(out[i + 1] == Catch::Approx(5.f));
}
```

- [ ] **Step 2: Run; fail.** **Step 3: Implement**, then in `GameView::readSelectedBodies` collect selected `ParticleEmitter`s into `emitterShapes_` (shape, arc, and `emitter_extent(game, parent, frame, half)`), and in `collectOutlines` append `particle_shape_lines(...)` for each into `toolLines_` after the constraint lines. **Step 4: Run; pass.** Launch the studio over MCP (memory: drive studio over MCP; the screenshot tool omits overlays, so check visually or via `read_page`/selection state as the memory note says) and confirm the outline follows a selected emitter in a Brush.
- [ ] **Step 5: Commit** — `git commit -m "Particles: the studio outlines a selected emitter's shape"`

---

### Task 15: Curve and gradient editors in Properties

**Files:**
- Create: `src/ide/CurveEditModel.hpp`, `src/ide/CurveEditModel.cpp`, `src/ide/CurveEditor.hpp`, `src/ide/CurveEditor.cpp`
- Modify: `src/ide/PropertiesPanel.cpp` (`make_view` cases for `PropertyKind::NumberCurve` / `ColorCurve`; live/finish paths like `live_color`/`finish_color` 1751-1777), `src/ide/PropertySheet.hpp/.cpp` (`PropertyEdit` carries a curve), `CMakeLists.txt`
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Consumes: Task 1 types, Task 2 slots, `ZoomPopover` pattern (`ZoomPopover.cpp:161-181`), `PropertyColor` (`PropertiesPanel.cpp:244-300`).
- Produces:

```cpp
namespace ide {
// UI-free editing of a NumberCurve's keys inside a graph rect. Graph space: x = time 0..1, y = value.
class CurveEditModel {
public:
    explicit CurveEditModel(engine_core::NumberCurve curve);
    const engine_core::NumberCurve& curve() const { return curve_; }
    void set_mode(engine_core::NumberCurveMode mode);        // converts: Constant <- first value; Range <- min/max of values; Curve <- one key per old value
    // Vertical range that fits every key with 10% margin (at least 1 unit tall).
    void fit(float& low, float& high) const;
    // Key nearest to (t, v) within radius (graph units), or -1. which: 0 = first curve, 1 = second.
    int hit(float t, float v, float radius_t, float radius_v, int which) const;
    int add_key(float t, float v, int which);               // returns the new key's index after sorting
    void move_key(int index, float t, float v, int which);  // clamps t to 0..1; keeps the key selected through re-sorting (returns via selected())
    void delete_key(int index, int which);                  // never deletes the last key
    void set_interp(int index, engine_core::CurveInterp interp, int which);
    int selected() const { return selected_; }
private:
    engine_core::NumberCurve curve_;
    int selected_ = -1;
};
// Same for a ColorCurve's gradient stops.
class GradientEditModel { /* set_mode, hit(t, radius), add_stop(t) (colour = evaluate at t), move_stop, delete_stop, set_color, set_transparency */ };
}  // namespace ide
```

- [ ] **Step 1: Failing tests** (`tests/PropertiesTest.cpp`)

```cpp
void TestCurveEditModel() {
    std::string error;
    ide::CurveEditModel model(*engine_core::NumberCurve::keys({{0.f, 0.f}, {1.f, 1.f}}, error));
    const int added = model.add_key(0.5f, 3.f, 0);
    Expect(added == 1 && model.curve().keys_a().size() == 3, "add_key inserts in time order");
    model.move_key(1, 0.9f, 2.f, 0);
    Expect(model.curve().keys_a()[1].time == 0.9f, "move_key moves");
    model.move_key(1, 1.5f, 2.f, 0);
    Expect(model.curve().keys_a()[1].time <= 1.f, "move_key clamps time");
    model.delete_key(0, 0);
    model.delete_key(0, 0);
    model.delete_key(0, 0);
    Expect(model.curve().keys_a().size() == 1, "the last key is never deleted");
    model.set_mode(engine_core::NumberCurveMode::Range);
    Expect(model.curve().mode() == engine_core::NumberCurveMode::Range, "mode converts");
    float lo = 0, hi = 0;
    model.fit(lo, hi);
    Expect(hi - lo >= 1.f, "fit is at least one unit tall");
}

void TestCurveRowEditsAsOneUndoStep() {
    Rig rig;
    const InstanceId emitter = rig.add<engine_core::ParticleEmitter>("Emitter");
    rig.select({emitter});
    // Open the Size row's popover (panel.editor("Size") gives the row view; click it as TestColorPicker opens the picker),
    // switch the mode dropdown to Curve, drag a key (simulate the model edits through the popover's API: live_* then finish),
    // then: history can_undo().second == "Set Size", one undo restores Constant 1, Changed fired once.
}
```

Model `TestCurveRowEditsAsOneUndoStep` on `TestColorPicker` (PropertiesTest.cpp:807): it shows how the test drives a popover and checks undo labels.

- [ ] **Step 2: Run; fail.**
- [ ] **Step 3: Implement** `CurveEditModel`/`GradientEditModel` (pure logic as declared), then `CurveEditor` views: a row button (`PropertyCurveButton`) showing the summary text and a small sparkline/gradient strip drawn as a JadeFX canvas or a row of thin panes (match how `MaterialBall`/`ThumbnailLoader` draw custom pixels, or use a `Pane` per sample bar — 32 bars is enough); clicking opens a `CurvePopover : jadefx::VBox` via `scene->showPopupNear(..., jadefx::Side::Top, options)` with `autoHide`, containing the mode `ComboBox`, a graph pane handling mouse down/drag/up/double-click/Delete through the model, numeric `TextField`s for the selected key's time and value, and an interpolation `ComboBox`. Every model change calls `on_live(curve)`; popup hidden calls `on_done(changed)`. In `PropertiesPanel`, `on_live` → `preview(view.ids, curve_edit(view))` and `on_done` → `finish_live(view.ids, curve_edit(view), changed)`, exactly like `live_color`/`finish_color`, so one popover session is one "Set <Property>" step and one Changed. `PropertyEdit` gains `std::shared_ptr<const NumberCurve> number_curve; std::shared_ptr<const ColorCurve> color_curve;`, and `apply_edit`/`preview_edit` build the slot with `number_curve_slot(*edit.number_curve)`. The gradient popover: a bar with stop handles (drag to move, double-click to add, Delete to remove), a `ColorPicker` and a transparency `TextField` for the selected stop, and the mode `ComboBox`.
- [ ] **Step 4: Run `properties-tests`; pass. Launch the studio and use the editor on a real emitter for a minute** (add keys, switch modes, undo) to catch anything the tests miss.
- [ ] **Step 5: Commit** — `git commit -m "Particles: curve and gradient editors in Properties"`

---

### Task 16: The Bursts row

**Files:**
- Create: `src/ide/BurstListRow.hpp`, `src/ide/BurstListRow.cpp`
- Modify: `src/ide/PropertiesPanel.cpp` (`PropertyKind::Bursts` view), `src/ide/PropertySheet.*` (`PropertyEdit` carries bursts JSON text), `CMakeLists.txt`
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Consumes: Task 6 (`bursts_to_json`, `bursts_from_json`).
- Produces: a `BurstListRow : jadefx::VBox` showing one line per burst — a `Time` field and a `Count` field accepting `"10"` or `"10-20"` — plus "+" (adds `{last time + 0.5, 10}`) and per-line "×" buttons. Any committed change writes the whole list as one "Set Bursts" step.

- [ ] **Step 1: Failing test**

```cpp
void TestBurstsRow() {
    Rig rig;
    const InstanceId emitter = rig.add<engine_core::ParticleEmitter>("Emitter");
    rig.select({emitter});
    // click the Bursts row's "+" (find it as other tests find buttons), rig.frame();
    // type "0.5" into the first line's Time and "10-20" into its Count, enter;
    // then read the emitter: bursts().size() == 1, time 0.5, count == NumberCurve::range(10, 20);
    // typing "abc" into Count leaves the value unchanged and shows the field as invalid (as Number fields do).
    // undo label == "Set Bursts".
}
```

- [ ] **Step 2: Run; fail.** **Step 3: Implement.** Parse Count with: trim; if it contains `-` after the first character, split into min/max floats → `NumberCurve::range`; else one float → `NumberCurve::constant`; refuse negatives and non-numbers. **Step 4: Run; pass.**
- [ ] **Step 5: Commit** — `git commit -m "Particles: a Bursts table in Properties"`

---

### Task 17: Docs, the real app, and a Release profile

**Files:**
- Modify: `src/engine_instances/README.md`, `src/engine_core/README.md` (ParticleWorld paragraph: where it steps, edit mode, budget, snapshot), `README.md` (feature list), `docs/superpowers/specs/2026-10-12-particle-emitter-design.md` (record any deviation made during implementation, e.g. particles drawn after transparent meshes rather than interleaved; colour interpolation per channel as stored)

- [ ] **Step 1: Write the README paragraphs** in the house style (plain sentences, "units").
- [ ] **Step 2: Rebuild Debug and Release** (`AnarchyStudio`, `AnarchyPlayer`, all test targets) and run every suite: `cd build && ctest -C Release --output-on-failure` and `build/Release/scene-render-check.exe`. Note known flakes from memory (explorer-tests on 2026-10-12) rather than "fixing" them.
- [ ] **Step 3: Drive the studio over MCP** (launch with the MCP port/token as the memory note describes). Build a scene: a Brush floor, a fire emitter (Cone, Additive, Brightness 3, Size curve 0.5→2, Color gradient yellow→red→transparent grey, Acceleration (0,4,0)), a smoke emitter (Normal, SoftEdge 1, flipbook if a sheet is available), sparks (Velocity facing, Acceleration (0,-30,0), Collision World, Bounce), a shockwave ring (Facing World, Disc Edge). Take a few screenshots (soft edges, blend modes, glow into bloom, flipbook) and open them with `Start-Process` per the memory note; send the best two to the user with SendUserFile.
- [ ] **Step 4: Profile Release, warmed up** (memory: compare at equal warm-up): set `Workspace.ParticleBudget` 20000 with enough emitters to hit it, let it run a minute, read the profiler's `Particles` scope and the frame time. Record the numbers in the spec's Testing section. If `World` collision's raycasts show up, implement the `raycast_synced` split noted in Task 11.
- [ ] **Step 5: Commit** — `git commit -m "Particles: docs and measured cost"`

---

## Self-review notes

- Spec coverage: value types (T1–3), grey-out rule (T4), every property and Workspace.ParticleBudget (T5–6), shapes and size from the parent (T7, T9), sim and motion (T8), lifecycle, Emit/Clear/Finished/ParticleCount, edit mode, Stop, ViewportPane exclusion (T9), budget (T10), collision (T11), snapshot (T12), rendering incl. soft edges, blend modes, flipbook, lighting, glow, DrawOrder (T13), studio outline (T14), curve/gradient editors (T15), bursts row (T16), docs and real-app checks (T17).
- Deliberate deviations to record in the spec (T17): particles draw after all transparent meshes, sorted among themselves by DrawOrder then distance, instead of interleaving with transparent meshes; particle lighting uses point-light falloff without spot cones or shadows; ColorCurve interpolates per channel as stored.
