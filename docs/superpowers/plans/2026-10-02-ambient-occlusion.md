# Ambient Occlusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surfaces in the Scene View are shaded where nearby geometry hides the sky and ambient light from them (in creases, under objects, where a wall meets a floor), using ground-truth ambient occlusion (GTAO). This replaces the legacy engine's HBAO shader, `ssao.fs` / `ssaoBlur.fs`, and fixes its bugs instead of porting them.

**Architecture:**

- A new full-screen pass, `gtao.frag`, runs after the G-buffer pass. It reads depth and normals and writes one visibility value per pixel (1 open, 0 shut in) into an R8 target.
- A separable, depth- and normal-aware blur (`aoBlur.frag`, run across and then down) removes the per-pixel noise.
- `ibl.frag` reads the result and scales only the sky and ambient light by it. Direct lights are untouched, as they should be: AO approximates occlusion of *indirect* light.
- Three new Lighting properties (`AmbientOcclusion`, `OcclusionRadius`, `OcclusionStrength`) flow through `VisualLighting` and `SceneLighting`, as `Exposure` does.

**Tech Stack:** C++17 (MSVC on Windows), OpenGL 3.3 core through the repo's own loader (`src/runner/gl.hpp`), GLSL 330 fragment shaders (no compute), Catch2 (`sandbox` target), and the hand-run `scene-render-check` for pixels.

**Spec:** none separate. This plan carries its design (§Design below). Background:

- `docs/research/2026-09-30-legacy-renderer-port.md`, feature P1 and improvement I13 ("GTAO … instead of noisy ray-marched AO").
- Jimenez et al. 2016, *Practical Real-Time Strategies for Accurate Indirect Occlusion*.
- Intel's XeGTAO (MIT), the reference implementation this shader follows.

## Design

### Why GTAO, and why this shape

GTAO is the direct successor of the legacy HBAO. Both march along screen-space slices and track the highest horizon. GTAO differs in three ways:

- It finds *two* horizons per slice, one on each side of the pixel.
- It integrates the visible arc in closed form, cosine-weighted and in angle space.
- It projects the normal into each slice, so tilted surfaces are handled correctly.

A flat surface comes out at exactly 1, so no hand-tuned bias is needed.

Modern engines run 1–2 slices a pixel and rely on temporal accumulation (TAA) to remove noise. **This renderer has no TAA and no compute shaders** (GL 3.3 core). So this plan:

- uses 3 slices × 2 sides × 6 steps = 36 depth taps a pixel, at full resolution;
- jitters with per-pixel spatial noise: interleaved gradient noise for the slice angle, and the R2 sequence for the step offset, so the two are decorrelated;
- removes that noise with a 9-tap separable bilateral blur.

Half resolution and temporal accumulation are deferred (see §Deferred).

### What the legacy shader got wrong, and where this plan fixes it

| Legacy issue (`ssao.fs` / `ssaoBlur.fs`) | Fix | Task |
| --- | --- | --- |
| `x * inversesqrt(x*x+1)` is `sin(atan x)`, then mixed with radians and passed through `sin()` again | GTAO keeps horizons as cosines and integrates in angle space with `acos` once per side; no mixed units | 2 |
| `normalCheck = 1 - dot(tangentAngle, horizonAngle)` multiplies two floats and means nothing | Removed. The normal enters through its projection into the slice (`n`, `projectedLength`) | 2 |
| Adds `sin h − sin t` at every step and averages, so the nearest occluder counts up to 6× | Each side keeps one running maximum horizon; the arc is integrated once per slice | 2 |
| `CONST_RADIUS` in far-plane units (effectively infinite), and the real falloff `(4/screenWidth)/|dz|` depends on resolution and far plane | `OcclusionRadius` is in world units. The pixel radius comes from the projection, and the falloff is a linear fade over the last 60% of the world radius | 1, 2 |
| `texture()` on depth, at UVs that are not texel centers | `texelFetch`, and positions rebuilt at the fetched texel's center | 2 |
| `viewNorm` not normalized | Normalized | 2 |
| One noise value drives both rotation and step jitter | Interleaved gradient noise for rotation, R2 for step: decorrelated | 2 |
| Blur depth weight is an absolute depth difference × 700, and depth is stored in the AO target's `.b` channel (8-bit if RGBA8) | The blur reads the 24-bit depth buffer directly and weights by distance from the center pixel's tangent plane, relative to its depth. It also adds a normal weight | 3 |
| `pow(ao, 4)` contrast hack to cover weak values | Gone. `OcclusionStrength` is an explicit exponent, 1 by default | 2 |
| AO darkens everything equally | Multi-bounce fit (bright surfaces lose less) and specular occlusion for reflections | 4 |

### The algorithm in `gtao.frag`

For each pixel with depth below 1:

1. Rebuild `P` (view space) from depth, with `V = normalize(-P)` and `N` from the G-buffer, normalized.
2. Pixel radius = `OcclusionRadius × uProjectionScale / −P.z`, capped at 25% of the buffer's height. Below 1 pixel, the output is 1.
3. For each of 3 slices at angle `φ = (slice + noise) × π / 3`:
   1. Project `N` into the slice plane. Its length is `projectedLength` and its signed angle from `V` is `n`.
   2. Start both horizon cosines at the tangent plane, `cos(n ± π/2)`.
   3. March 6 steps each way. Step distances grow quadratically (`t²`) with an R2 jitter, at least one pixel apart.
   4. For each sample, rebuild its position at the texel center, take the cosine between `V` and the direction to it, fade that toward the tangent-plane value by the distance falloff, and keep the maximum. A sample off screen or on the sky is the tangent-plane value, meaning nothing occludes.
   5. Clamp the horizon angles `h0`, `h1` to within ±π/2 of `n`, then integrate the visible arc in closed form:
      `projectedLength × ¼ × Σ(cos n + 2h·sin n − cos(2h − n))`.
4. Average the slices.

### Parameters

| Name | Where | Value |
| --- | --- | --- |
| `Lighting.AmbientOcclusion` | property | boolean, default `true` |
| `Lighting.OcclusionRadius` | property | world units, default `1`, not below 0, slider 0–10 |
| `Lighting.OcclusionStrength` | property | exponent on visibility, default `1`, not below 0, slider 0–4 |
| `kSlices` | `gtao.frag` | 3 |
| `kStepsPerSide` | `gtao.frag` | 6 |
| `kMaxRadiusFraction` | `gtao.frag` | 0.25 of buffer height |
| `kFalloffRange` | `gtao.frag` | 0.6 of the radius |
| `kBlurRadius` | `aoBlur.frag` | 4 (9 taps per direction) |
| `kPlaneTolerance` | `aoBlur.frag` | 0.02 of the center's view depth |
| `kNormalPower` | `aoBlur.frag` | 8 |

The pass is skipped, and a 1×1 white texture bound in its place, when `AmbientOcclusion` is false, or `OcclusionRadius` or `OcclusionStrength` is 0.

## Global Constraints

- **GL:** 3.3 core only, with no compute shaders. Every new GL constant or entry point goes in `src/runner/gl.hpp`, as a `constexpr GLenum RT_GL_…` next to `RT_GL_RGBA16F` (L191). Entry points also go in `src/runner/gl.cpp` (`LOAD`). This plan needs only two new constants: `RT_GL_R8 = 0x8229` and `RT_GL_RED = 0x1903`.
- **Texture units:** all 16 are named in `Renderer.cpp` (L18–35).
  - `uOcclusion` gets `constexpr int kUnitOcclusion = kUnitMetalnessMap;` (unit 3). It is free in the light pass, which reads no Material, and in the AO passes.
  - The shadows plan (`2026-10-01-shadows.md`) takes units 0, 1 and 2 in the light pass, so the two plans can land in either order. If shadows has landed and moved things, keep `uOcclusion` on a unit the light pass does not otherwise bind.
- **Samplers:** every sampler a program declares always has a texture of its own type bound. With AO off, `whiteTexture_` is bound to `kUnitOcclusion`. `ibl.frag` reads it with `texture(uOcclusion, vUv)`, not `texelFetch`, because `texelFetch` outside a 1×1 texture is undefined.
- **CanDraw:** each new program checks `CanDraw` before its first draw. A false return propagates up, and the pane gets only its clear for that frame (macOS readies programs a frame late).
- **No GLSL `pow(0, 0)`:** the occlusion pass is skipped when Strength is 0, so `ibl.frag` raises 1 (white) to 0, which is defined.
- **Code style:** match the surrounding files' prose comments and density.
  - Engine code (`engine_services`, `engine_core`) uses `snake_case`.
  - `src/runner/` uses `camelCase` methods and `PascalCase` free functions.
  - Shaders open with a comment saying what the pass does and which libraries Renderer splices in.
- **Branch:** start from a clean `main` and create `ambient-occlusion`.

Build and test commands, from the repo root. In Git Bash, prefix cmake with `MSYS_NO_PATHCONV=1`.

- Sandbox: `cmake --build build --config Debug --target sandbox`, then `build/Debug/sandbox.exe "[light]"`.
- Pixels: `cmake --build build --config Debug --target scene-render-check`, then `build/Debug/scene-render-check.exe`. Run it from the repo root, by hand; it opens a hidden GL window.
- Everything: `ctest --test-dir build -C Debug --output-on-failure`.
- Finish: also `cmake --build build --config Release`. "Rebuilt" means `build/Release/AnarchyEngine-CPP.exe` too.

## Review Focus

1. **Open floor at a grazing angle** must not darken from depth-precision self-occlusion. Task 2 pins it with the "open floor" and "far open floor" checks.
2. **Halos at silhouettes:** the floor seen just past an object's edge, far behind it, must not be shaded by that object. Task 2, "past the top edge" (the falloff); Task 3 re-runs it after the blur.
3. **A camera right at a surface** must not produce NaN (black) pixels or unbounded sample spans. Task 2, "camera close" (the `kMaxRadiusFraction` cap and `safeAcos`).
4. **Turning AO off, or Radius or Strength 0,** must look exactly like no AO, and resizing the pane must rebuild the AO targets. Task 2, the "off", "radius 0", "strength 0" and "resized pane" checks.
5. **A smooth metal floor under a Skybox** must have its reflection dimmed where something sits on it, not just its (zero) diffuse. Task 4, "metal floor contact".

---

### Task 1: Lighting's occlusion properties, through to SceneLighting

**Files:**
- Modify: `src/engine_services/Lighting.hpp` (header comment, constants, accessors, members)
- Modify: `src/engine_services/Lighting.cpp` (`bool_slot`, setters, `read_flag`/`write_flag`, registration)
- Modify: `src/engine_core/SnapshotPump.hpp:79-84` (`VisualLighting`)
- Modify: `src/engine_core/SnapshotPump.cpp:16-19` (`static_assert`) and `~L427-431` (copy from Lighting)
- Modify: `src/runner/Renderer.hpp` (`SceneLighting`)
- Modify: `src/runner/GameView.cpp:~294-300` (copy into `SceneLighting`)
- Test: `sandbox/light_tests.cpp`

**Interfaces:**
- Produces:
  - `engine_core::Lighting`:
    - `static constexpr bool kDefaultAmbientOcclusion = true;`
    - `static constexpr double kDefaultOcclusionRadius = 1.0;`
    - `static constexpr double kDefaultOcclusionStrength = 1.0;`
    - `bool ambient_occlusion() const;` and `void set_ambient_occlusion(bool);`
    - `double occlusion_radius() const;` and `std::optional<std::string> set_occlusion_radius(double);`
    - `double occlusion_strength() const;` and `std::optional<std::string> set_occlusion_strength(double);`
  - `engine_core::VisualLighting`: `bool ambient_occlusion = true; float occlusion_radius = 1.f; float occlusion_strength = 1.f;`
  - `runner::SceneLighting`: `bool occlusion = true; float occlusionRadius = 1.f; float occlusionStrength = 1.f;`

- [ ] **Step 1: Write the failing test**

Append to `sandbox/light_tests.cpp`, after LIT8:

```cpp
TEST_CASE("LIT9 the snapshot carries Lighting's AmbientOcclusion, OcclusionRadius, and OcclusionStrength",
          "[light][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    auto* lighting = dynamic_cast<engine_core::Lighting*>(game.instance(game.scene_service("Lighting")));
    REQUIRE(lighting != nullptr);
    frame();
    REQUIRE(pump.front().lighting.ambient_occlusion);
    REQUIRE(pump.front().lighting.occlusion_radius == 1.f);
    REQUIRE(pump.front().lighting.occlusion_strength == 1.f);

    lighting->set_ambient_occlusion(false);
    REQUIRE_FALSE(lighting->set_occlusion_radius(2.5));
    REQUIRE_FALSE(lighting->set_occlusion_strength(2.0));
    frame();
    REQUIRE_FALSE(pump.front().lighting.ambient_occlusion);
    REQUIRE(pump.front().lighting.occlusion_radius == 2.5f);
    REQUIRE(pump.front().lighting.occlusion_strength == 2.f);

    // Below 0 is 0, and a value that is not finite is refused.
    REQUIRE_FALSE(lighting->set_occlusion_radius(-1.0));
    REQUIRE(lighting->occlusion_radius() == 0.0);
    const auto refused = lighting->set_occlusion_strength(std::nan(""));
    REQUIRE(refused.has_value());
    REQUIRE(*refused == "OcclusionStrength must be a finite number");
    REQUIRE(lighting->occlusion_strength() == 2.0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Debug --target sandbox`
Expected: compile error, `'set_ambient_occlusion': is not a member of 'engine_core::Lighting'`.

- [ ] **Step 3: Add the properties to Lighting**

In `src/engine_services/Lighting.hpp`, extend the class comment's property list. Replace the sentence "The render snapshot carries Ambient, Exposure, Saturation, and Gamma (VisualLighting)" with "The render snapshot carries Ambient, Exposure, Saturation, Gamma, and the three occlusion properties (VisualLighting)", then add after the `Gamma` line:

```cpp
// AmbientOcclusion   boolean whether nearby surfaces shade the sky and ambient light a surface gets.
// OcclusionRadius    number  how far, in world units, a surface looks for what shades it. Not below 0.
// OcclusionStrength  number  how dark that shade is: 0 none, 1 as measured, above 1 darker. Not below 0.
```

After `kDefaultGamma`:

```cpp
    static constexpr bool kDefaultAmbientOcclusion = true;
    static constexpr double kDefaultOcclusionRadius = 1.0;
    static constexpr double kDefaultOcclusionStrength = 1.0;
```

After `set_gamma`:

```cpp
    bool ambient_occlusion() const { return ambient_occlusion_; }
    void set_ambient_occlusion(bool enabled);
    double occlusion_radius() const { return occlusion_radius_; }
    std::optional<std::string> set_occlusion_radius(double value);
    double occlusion_strength() const { return occlusion_strength_; }
    std::optional<std::string> set_occlusion_strength(double value);
```

After `gamma_`:

```cpp
    bool ambient_occlusion_ = kDefaultAmbientOcclusion;
    double occlusion_radius_ = kDefaultOcclusionRadius;
    double occlusion_strength_ = kDefaultOcclusionStrength;
```

In `src/engine_services/Lighting.cpp`, add to the first anonymous namespace, after `color_slot`:

```cpp
LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}
```

After `Lighting::set_gamma`:

```cpp
void Lighting::set_ambient_occlusion(bool enabled) {
    if (!on_gameplay_thread()) {
        contract_fail("Lighting setters run on SimulationThread");
    }
    if (ambient_occlusion_ == enabled) {
        return;
    }
    ambient_occlusion_ = enabled;
    note_property_change("AmbientOcclusion", bool_slot(!enabled), bool_slot(enabled));
}

std::optional<std::string> Lighting::set_occlusion_radius(double value) {
    return set_number("OcclusionRadius", occlusion_radius_, value);
}

std::optional<std::string> Lighting::set_occlusion_strength(double value) {
    return set_number("OcclusionStrength", occlusion_strength_, value);
}
```

In the second anonymous namespace, after `write_number`:

```cpp
template <bool (Lighting::*Get)() const>
bool read_flag(DataModel&, DataModel& object, LuaSlot& out) {
    Lighting* lighting = lighting_of(object);
    if (lighting == nullptr) {
        return false;
    }
    out = bool_slot((lighting->*Get)());
    return true;
}

template <void (Lighting::*Set)(bool)>
bool write_flag(DataModel&, DataModel& object, LuaSlot& in) {
    Lighting* lighting = lighting_of(object);
    if (lighting == nullptr) {
        return false;
    }
    (lighting->*Set)(in.flag);
    return true;
}
```

In `register_lighting_lua`, after the `gamma` default string:

```cpp
    static const std::string occlusion_radius = number_json(Lighting::kDefaultOcclusionRadius);
    static const std::string occlusion_strength = number_json(Lighting::kDefaultOcclusionStrength);
```

Append to `fields[]`, after the Gamma slider:

```cpp
        lua_saved_property("AmbientOcclusion", "boolean", read_flag<&Lighting::ambient_occlusion>,
                           write_flag<&Lighting::set_ambient_occlusion>,
                           Lighting::kDefaultAmbientOcclusion ? "true" : "false"),
        lua_slider(lua_saved_property("OcclusionRadius", "number", read_number<&Lighting::occlusion_radius>,
                                      write_number<&Lighting::set_occlusion_radius>, occlusion_radius.c_str()),
                   0.0, 10.0),
        lua_slider(lua_saved_property("OcclusionStrength", "number", read_number<&Lighting::occlusion_strength>,
                                      write_number<&Lighting::set_occlusion_strength>, occlusion_strength.c_str()),
                   0.0, 4.0),
```

- [ ] **Step 4: Carry them through the snapshot and into the renderer**

`src/engine_core/SnapshotPump.hpp`, in `VisualLighting` after `gamma`:

```cpp
    bool ambient_occlusion = true;
    // World units.
    float occlusion_radius = 1.f;
    float occlusion_strength = 1.f;
```

`src/engine_core/SnapshotPump.cpp`: extend the `static_assert` condition with

```cpp
                  VisualLighting{}.ambient_occlusion == Lighting::kDefaultAmbientOcclusion &&
                  VisualLighting{}.occlusion_radius == static_cast<float>(Lighting::kDefaultOcclusionRadius) &&
                  VisualLighting{}.occlusion_strength == static_cast<float>(Lighting::kDefaultOcclusionStrength) &&
```

and after `base_.lighting.gamma = …`:

```cpp
    base_.lighting.ambient_occlusion = lighting->ambient_occlusion();
    base_.lighting.occlusion_radius = static_cast<float>(lighting->occlusion_radius());
    base_.lighting.occlusion_strength = static_cast<float>(lighting->occlusion_strength());
```

`src/runner/Renderer.hpp`, in `SceneLighting` after `gamma`:

```cpp
    // Ambient occlusion: whether nearby surfaces shade the sky and ambient
    // light, how far each surface looks in world units, and the exponent on
    // what it finds. Off, or either number 0, draws none.
    bool occlusion = true;
    float occlusionRadius = 1.f;
    float occlusionStrength = 1.f;
```

`src/runner/GameView.cpp`, after `lighting.gamma = snapshot.lighting.gamma;`:

```cpp
    lighting.occlusion = snapshot.lighting.ambient_occlusion;
    lighting.occlusionRadius = snapshot.lighting.occlusion_radius;
    lighting.occlusionStrength = snapshot.lighting.occlusion_strength;
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Debug --target sandbox`, then `build/Debug/sandbox.exe "[light]"`
Expected: all `[light]` cases pass, including LIT9.

- [ ] **Step 6: Commit**

```bash
git add src/engine_services/Lighting.hpp src/engine_services/Lighting.cpp src/engine_core/SnapshotPump.hpp src/engine_core/SnapshotPump.cpp src/runner/Renderer.hpp src/runner/GameView.cpp sandbox/light_tests.cpp
git commit -m "Add Lighting's AmbientOcclusion, OcclusionRadius, and OcclusionStrength"
```

---

### Task 2: The GTAO pass, applied to the sky and ambient light

**Files:**
- Create: `resources/shaders/pipeline/gtao.frag`
- Modify: `src/runner/gl.hpp:~191` (`RT_GL_R8`, `RT_GL_RED`)
- Modify: `src/runner/Renderer.hpp` (`Program` fields, `gtao_`, the occlusion target, `occlusionPass`, `occlusionReady_`)
- Modify: `src/runner/Renderer.cpp`: `kUnitOcclusion` (L35), `buildProgram` (uniforms and sampler), `initialize` (L174–188), `ensureTargets` / `destroyTargets` (L303–360), `draw` (L655), new `occlusionPass`, `lightPass` (IBL draw, L832–841)
- Modify: `resources/shaders/pipeline/ibl.frag`, `resources/shaders/pipeline/image_lighting.glsl`, `resources/shaders/pipeline/forward.frag:37`
- Test: `tests/SceneRenderCheck.cpp` (new block before `renderer.shutdown();`, L680)

**Interfaces:**
- Consumes: `SceneLighting::occlusion`, `occlusionRadius`, `occlusionStrength` (Task 1). `lighting.glsl`'s `viewPositionAt(vec2 uv, float depth)` and `uInverseProjection`.
- Produces:
  - `Renderer::occlusionPass(const float* projection, const float* inverseProjection) -> bool`
  - `occlusionTexture_` (R8, the buffers' size, final visibility) and `occlusionFbo_`
  - `bool occlusionReady_`
  - `Program` fields: `occlusionRadius` (`uOcclusionRadius`), `projectionScale` (`uProjectionScale`), `occlusionStrength` (`uOcclusionStrength`)
  - GLSL: `vec3 skyLight(…, vec3 skyRadiance, float occlusion)` in `image_lighting.glsl`
  - Test helpers in the new block: `at(x, y, z)` (window pixel of a world point under the default camera) and `drawWith(lighting)`

- [ ] **Step 1: Write the failing pixel checks**

In `tests/SceneRenderCheck.cpp`, insert this block immediately before `renderer.shutdown();`:

```cpp
        // Ambient occlusion: a cube resting on a wide floor, under ambient light alone.
        {
            renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                               runner::Renderer::kCameraFovYDegrees);
            // The floor: the cube stretched to 20 by 0.1 by 20, its top at y = -0.5, where the cube stands.
            engine_core::Matrix4 floorModel = engine_core::matrix4_translation(0.f, -0.55f, 0.f);
            floorModel.m[0] = 20.f;
            floorModel.m[5] = 0.1f;
            floorModel.m[10] = 20.f;
            const runner::MeshDraw scene[2] = {runner::MeshDraw{cube, engine_core::matrix4_identity()},
                                               runner::MeshDraw{cube, floorModel}};
            // The window pixel a world point lands on, from that camera. The window is square.
            const engine_core::Matrix4 view =
                engine_core::matrix4_inverse(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up));
            const float focal = 1.f / std::tan(0.5f * runner::Renderer::kCameraFovYDegrees * 0.01745329252f);
            const auto at = [&](float x, float y, float z) {
                const engine_core::Vec3 p = engine_core::matrix4_point(view, {x, y, z});
                const float ndcX = focal * p.x / -p.z;
                const float ndcY = focal * p.y / -p.z;
                return ReadPixel(static_cast<int>((ndcX * 0.5f + 0.5f) * static_cast<float>(fbWidth)),
                                 static_cast<int>((ndcY * 0.5f + 0.5f) * static_cast<float>(fbHeight)));
            };
            runner::SceneLighting off;
            off.ambient[0] = off.ambient[1] = off.ambient[2] = 1.f;
            off.occlusion = false;
            runner::SceneLighting on = off;
            on.occlusion = true;
            const auto drawWith = [&](const runner::SceneLighting& lighting) {
                renderer.setLighting(lighting);
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2);
            };

            drawWith(off);
            const Pixel contactOff = at(0.f, -0.5f, 0.56f);
            const Pixel openOff = at(2.5f, -0.5f, 1.5f);
            const Pixel farOpenOff = at(-3.f, -0.5f, -8.f);
            const Pixel pastEdgeOff = at(0.f, -0.5f, -4.5f);
            const Pixel topOff = at(0.f, 0.5f, 0.f);
            drawWith(on);
            const Pixel contactOn = at(0.f, -0.5f, 0.56f);
            Expect(Sum(contactOn) + 15 < Sum(contactOff), "the floor where the cube stands on it is shaded (" +
                                                              Text(contactOn) + " under " + Text(contactOff) + ")");
            Expect(std::abs(Sum(at(2.5f, -0.5f, 1.5f)) - Sum(openOff)) <= 3,
                   "open floor, farther than the radius from anything, is not");
            Expect(std::abs(Sum(at(-3.f, -0.5f, -8.f)) - Sum(farOpenOff)) <= 3,
                   "nor is open floor far off, seen at a grazing angle");
            Expect(std::abs(Sum(at(0.f, -0.5f, -4.5f)) - Sum(pastEdgeOff)) <= 3,
                   "nor the floor seen just past the cube's top edge, far behind it");
            Expect(std::abs(Sum(at(0.f, 0.5f, 0.f)) - Sum(topOff)) <= 3, "nor the cube's open top");

            // Off, Radius 0, and Strength 0 all look like no occlusion; Strength 2 is darker than 1.
            runner::SceneLighting noRadius = on;
            noRadius.occlusionRadius = 0.f;
            drawWith(noRadius);
            Expect(std::abs(Sum(at(0.f, -0.5f, 0.56f)) - Sum(contactOff)) <= 3, "OcclusionRadius 0 shades nothing");
            runner::SceneLighting noStrength = on;
            noStrength.occlusionStrength = 0.f;
            drawWith(noStrength);
            Expect(std::abs(Sum(at(0.f, -0.5f, 0.56f)) - Sum(contactOff)) <= 3, "OcclusionStrength 0 shades nothing");
            runner::SceneLighting strong = on;
            strong.occlusionStrength = 2.f;
            drawWith(strong);
            Expect(Sum(at(0.f, -0.5f, 0.56f)) + 5 < Sum(contactOn), "OcclusionStrength 2 shades darker than 1");

            // A smaller pane rebuilds the buffers, and the full one again still shades.
            renderer.setLighting(on);
            renderer.draw(0, 0, kSize / 2, kSize / 2, kSize, kSize, scene, 2);
            drawWith(on);
            Expect(std::abs(Sum(at(0.f, -0.5f, 0.56f)) - Sum(contactOn)) <= 3, "a resized pane shades the same");

            // A camera right at the floor: no pixel goes black, as a NaN would.
            renderer.setCamera(engine_core::matrix4_look_at({0.f, -0.45f, 0.9f}, {0.f, -0.5f, 0.f}, up), 60.f);
            drawWith(on);
            bool anyBlack = false;
            for (int y = 8; y < fbHeight; y += fbHeight / 8) {
                for (int x = 8; x < fbWidth; x += fbWidth / 8) {
                    anyBlack = anyBlack || Sum(ReadPixel(x, y)) == 0;
                }
            }
            Expect(!anyBlack, "a camera right at a surface leaves no black pixels");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "ambient occlusion leaves no GL error");

            renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                               runner::Renderer::kCameraFovYDegrees);
            renderer.setLighting(runner::SceneLighting{});
        }
```

- [ ] **Step 2: Run the checks to verify they fail**

Run: `cmake --build build --config Debug --target scene-render-check`, then `build/Debug/scene-render-check.exe`
Expected: `FAIL the floor where the cube stands on it is shaded (…)` and `FAIL OcclusionStrength 2 shades darker than 1`. Every check that existed before still passes.

- [ ] **Step 3: Write `gtao.frag`**

Create `resources/shaders/pipeline/gtao.frag`:

```glsl
#version 330 core
// Ground-truth ambient occlusion (Jimenez et al. 2016, "Practical Real-Time
// Strategies for Accurate Indirect Occlusion", after Intel's XeGTAO): how
// much of the sky above each opaque surface its neighbors leave open, 1 open
// and 0 shut in. Drawn with fullscreen.vert into an R8 buffer; aoBlur.frag
// smooths its noise and ibl.frag scales the sky and ambient light by it.
// Everything is in view space. Renderer puts lighting.glsl in after the
// #version line.
in vec2 vUv;
out vec4 outOcclusion;

uniform sampler2D uDepth;
uniform sampler2D uNormal;
// 1 over the buffers' size in pixels.
uniform vec2 uTexel;
// Lighting.OcclusionRadius, in world units.
uniform float uOcclusionRadius;
// Pixels per world unit at view depth 1: half the buffers' height times the projection's [1][1].
uniform float uProjectionScale;

const float kPi = 3.14159265359;
const float kHalfPi = 1.57079632679;
const int kSlices = 3;
const int kStepsPerSide = 6;
// No sample reaches farther than this part of the buffers' height, so a
// surface right at the camera stays cheap.
const float kMaxRadiusFraction = 0.25;
// Samples fade out over this last part of the radius instead of cutting off.
const float kFalloffRange = 0.6;

// Interleaved gradient noise (Jimenez 2014): a different angle at each pixel, with no texture.
float sliceNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// The R2 sequence (Roberts 2018), uncorrelated with the slice noise.
float stepNoise(vec2 pixel) {
    return fract(dot(pixel, vec2(0.7548776662, 0.5698402910)));
}

// A cosine a hair past 1 from rounding would make acos NaN.
float safeAcos(float x) {
    return acos(clamp(x, -1.0, 1.0));
}

// The cosine between V and the way from P to the surface at uv, faded toward
// low as it nears the radius. Off the buffers, or on the sky, it is low:
// nothing there occludes.
float horizonCosAt(vec2 uv, vec3 P, vec3 V, float low, float falloffMul, float falloffAdd) {
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
        return low;
    }
    ivec2 texel = ivec2(uv / uTexel);
    float depth = texelFetch(uDepth, texel, 0).r;
    if (depth >= 1.0) {
        return low;
    }
    // At the texel's center, which is where its depth was written.
    vec3 delta = viewPositionAt((vec2(texel) + 0.5) * uTexel, depth) - P;
    float distance = length(delta);
    float cosine = dot(delta, V) / max(distance, 1e-6);
    float weight = clamp(distance * falloffMul + falloffAdd, 0.0, 1.0);
    return mix(low, cosine, weight);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(uDepth, pixel, 0).r;
    if (depth >= 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    vec3 P = viewPositionAt(vUv, depth);
    vec3 V = normalize(-P);
    vec3 N = normalize(texelFetch(uNormal, pixel, 0).xyz);

    float radiusPixels = min(uOcclusionRadius * uProjectionScale / -P.z, kMaxRadiusFraction / uTexel.y);
    if (radiusPixels < 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    // Full weight out to falloffFrom, none at the radius.
    float falloffRange = kFalloffRange * uOcclusionRadius;
    float falloffFrom = uOcclusionRadius - falloffRange;
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;

    float angleNoise = sliceNoise(gl_FragCoord.xy);
    float offsetNoise = stepNoise(gl_FragCoord.xy);
    float visibility = 0.0;
    for (int slice = 0; slice < kSlices; ++slice) {
        float phi = (float(slice) + angleNoise) * (kPi / float(kSlices));
        // Screen y and view y both point up, so one direction serves both.
        vec2 omega = vec2(cos(phi), sin(phi));
        vec3 direction = vec3(omega, 0.0);
        vec3 orthoDirection = direction - dot(direction, V) * V;
        vec3 axis = normalize(cross(orthoDirection, V));
        vec3 projectedNormal = N - axis * dot(N, axis);
        float projectedLength = length(projectedNormal);
        float signNormal = sign(dot(orthoDirection, projectedNormal));
        float cosNormal = clamp(dot(projectedNormal, V) / max(projectedLength, 1e-6), 0.0, 1.0);
        float n = signNormal * safeAcos(cosNormal);

        // Each horizon starts at the surface's own tangent plane: nothing below it counts.
        float low0 = cos(n + kHalfPi);
        float low1 = cos(n - kHalfPi);
        float horizonCos0 = low0;
        float horizonCos1 = low1;
        for (int s = 0; s < kStepsPerSide; ++s) {
            // Steps bunch up near the pixel, where occluders matter most, at least a pixel apart.
            float t = (float(s) + offsetNoise) / float(kStepsPerSide);
            vec2 offset = omega * max(t * t * radiusPixels, float(s) + 1.0) * uTexel;
            horizonCos0 = max(horizonCos0, horizonCosAt(vUv + offset, P, V, low0, falloffMul, falloffAdd));
            horizonCos1 = max(horizonCos1, horizonCosAt(vUv - offset, P, V, low1, falloffMul, falloffAdd));
        }

        // The visible arc between the two horizons, cosine weighted, in closed form.
        float h0 = -safeAcos(horizonCos1);
        float h1 = safeAcos(horizonCos0);
        h0 = n + clamp(h0 - n, -kHalfPi, kHalfPi);
        h1 = n + clamp(h1 - n, -kHalfPi, kHalfPi);
        float arc0 = cosNormal + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n);
        float arc1 = cosNormal + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n);
        visibility += projectedLength * 0.25 * (arc0 + arc1);
    }
    outOcclusion = vec4(clamp(visibility / float(kSlices), 0.0, 1.0));
}
```

- [ ] **Step 4: Scale the sky and ambient light by it**

`resources/shaders/pipeline/image_lighting.glsl`: give `skyLight` a trailing `float occlusion` and apply it to the diffuse terms only. Specular occlusion comes in Task 4.

```glsl
// viewDirection is the unit vector from the camera to the surface. occlusion
// is gtao.frag's, 1 open and 0 shut in; forward.frag passes 1.
vec3 skyLight(vec3 viewDirection, vec3 N, vec3 albedo, float metallic, float roughness, float reflectivity,
              vec3 ambient, vec3 skyRadiance, float occlusion) {
    if (uSkyEnabled < 0.5) {
        return ambientLight(viewDirection, N, albedo, metallic, roughness, reflectivity, ambient, skyRadiance) *
               occlusion;
    }
```

Then, in the same function, multiply the irradiance term and the flat ambient term by `occlusion`:

```glsl
    vec3 sky = (kD * albedo * irradiance * occlusion + reflected * (F0 * brdf.x + brdf.y)) * uSkyColor;
    // Lighting.Ambient lights every surface alike, as light from no direction.
    return sky + albedo * (1.0 - metallic) * ambient / kPi * occlusion;
```

`resources/shaders/pipeline/forward.frag:37`: the call becomes `skyLight(viewDirection, s.normal, s.albedo, s.metalness, s.roughness, s.reflectivity, uAmbient, uSkyRadiance, 1.0)`. See-through surfaces get no occlusion; they are not in the G-buffer.

`resources/shaders/pipeline/ibl.frag`: after `uniform vec3 uSkyRadiance;` add

```glsl
// gtao.frag's visibility, or 1 by 1 white with Lighting.AmbientOcclusion off.
uniform sampler2D uOcclusion;
// Lighting.OcclusionStrength: the visibility is raised to it.
uniform float uOcclusionStrength;
```

and replace the `outColor` line with

```glsl
    // texture, not texelFetch: with occlusion off this is the 1 by 1 white.
    float occlusion = pow(texture(uOcclusion, vUv).r, uOcclusionStrength);
    outColor = vec4(skyLight(viewDirection, N, albedo, material.x, material.y, material.z, uAmbient, uSkyRadiance,
                             occlusion),
                    1.0);
```

- [ ] **Step 5: Wire the pass into Renderer**

`src/runner/gl.hpp`, next to `RT_GL_RGBA16F`:

```cpp
constexpr GLenum RT_GL_R8 = 0x8229;
constexpr GLenum RT_GL_RED = 0x1903;
```

`src/runner/Renderer.cpp`, after `kUnitCount`:

```cpp
// Ambient occlusion shares the metalness map's unit: neither the light pass
// nor the occlusion passes read a Material.
constexpr int kUnitOcclusion = kUnitMetalnessMap;
```

In `buildProgram`, with the other `at(…)` lines:

```cpp
    program.occlusionRadius = at("uOcclusionRadius");
    program.projectionScale = at("uProjectionScale");
    program.occlusionStrength = at("uOcclusionStrength");
```

and with the other samplers (the material programs have no `uOcclusion`, so unit 3 stays the metalness map there):

```cpp
    sampler("uOcclusion", kUnitOcclusion);
```

`src/runner/Renderer.hpp`, in `Program` after `saturation`:

```cpp
        // Ambient occlusion (gtao.frag, aoBlur.frag, ibl.frag).
        int occlusionRadius = -1;
        int projectionScale = -1;
        int occlusionStrength = -1;
```

After `bool mergePass();`:

```cpp
    // Ambient occlusion into occlusionTexture_, from the G-buffer's depth and
    // normals. True, having drawn nothing, when Lighting turns it off.
    bool occlusionPass(const float* projection, const float* inverseProjection);
```

After `Program grid_;`: `Program gtao_;`. After `mergeTexture_`:

```cpp
    // Ambient occlusion, one channel: 1 open, 0 shut in.
    unsigned occlusionFbo_ = 0;
    unsigned occlusionTexture_ = 0;
    // Whether this draw made occlusionTexture_; when not, the light pass reads white.
    bool occlusionReady_ = false;
```

`initialize`, appended to the `buildProgram` chain after `grid_`:

```cpp
        buildProgram(gtao_, "Ambient occlusion", "pipeline/fullscreen.vert", "pipeline/gtao.frag",
                     {"pipeline/lighting.glsl"}) &&
```

`shutdown`: delete `gtao_.id` wherever the other programs are deleted, the same way they are.

`ensureTargets`, after `mergeTexture_ = …`:

```cpp
    occlusionTexture_ = MakeTarget(RT_GL_R8, RT_GL_RED, GL_UNSIGNED_BYTE, width, height);
```

and after the merge framebuffer:

```cpp
    glGenFramebuffers(1, &occlusionFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    complete = Attach({occlusionTexture_}, 0) && complete;
```

`destroyTargets`: add `&occlusionFbo_` to the framebuffer list and `&occlusionTexture_` to the texture list.

New method, after `geometryPass`:

```cpp
bool Renderer::occlusionPass(const float* projection, const float* inverseProjection) {
    occlusionReady_ = false;
    if (!lighting_.occlusion || !(lighting_.occlusionRadius > 0.f) || !(lighting_.occlusionStrength > 0.f)) {
        return true;
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(gtao_.id);
    BindTexture(kUnitDepth, depthTexture_);
    BindTexture(kUnitNormal, normalTexture_);
    glUniformMatrix4fv(gtao_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform2f(gtao_.texel, 1.f / static_cast<float>(targetWidth_), 1.f / static_cast<float>(targetHeight_));
    glUniform1f(gtao_.occlusionRadius, lighting_.occlusionRadius);
    // Pixels per world unit at view depth 1: half the height times the projection's [1][1].
    glUniform1f(gtao_.projectionScale, 0.5f * static_cast<float>(targetHeight_) * projection[5]);
    glBindVertexArray(emptyVao_);
    if (!CanDraw(gtao_.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);
    glDepthMask(GL_TRUE);
    occlusionReady_ = true;
    return true;
}
```

`draw` (L655): insert the pass between the G-buffer and the light pass:

```cpp
        drawn = cubesReady && geometryPass(meshes, meshCount, projection) &&
                occlusionPass(projection, inverseProjection.m) && lightPass(projection, inverseProjection.m) &&
                skyPass(inverseProjection.m) && transparencyPass(meshes, meshCount, projection, inverseProjection.m) &&
                mergePass();
```

`lightPass`, in the IBL block after `bindSky(ibl_);`:

```cpp
    BindTexture(kUnitOcclusion, occlusionReady_ ? occlusionTexture_ : whiteTexture_);
    glUniform1f(ibl_.occlusionStrength, std::max(lighting_.occlusionStrength, 0.f));
```

- [ ] **Step 6: Run the checks to verify they pass**

Run: `cmake --build build --config Debug --target scene-render-check`, then `build/Debug/scene-render-check.exe`
Expected: `scene render checks passed`.

If "open floor" or "far open floor" fails by a few units, that is depth-precision self-occlusion, Review Focus #1. Print the values. Do not widen the tolerance. Instead, add a bias to `horizonCosAt` that ignores samples closer than `0.02 × −P.z` to `P`'s tangent plane (`abs(dot(delta, N)) < 0.02 * -P.z` → return `low`), then re-run.

- [ ] **Step 7: Commit**

```bash
git add resources/shaders/pipeline/gtao.frag resources/shaders/pipeline/ibl.frag resources/shaders/pipeline/image_lighting.glsl resources/shaders/pipeline/forward.frag src/runner/gl.hpp src/runner/Renderer.hpp src/runner/Renderer.cpp tests/SceneRenderCheck.cpp
git commit -m "Shade the sky and ambient light with ground-truth ambient occlusion"
```

---

### Task 3: The depth- and normal-aware blur

**Files:**
- Create: `resources/shaders/pipeline/aoBlur.frag`
- Modify: `src/runner/Renderer.hpp` (`blurDirection` in `Program`, `occlusionBlur_`, the blur target)
- Modify: `src/runner/Renderer.cpp` (`buildProgram`, `initialize`, `shutdown`, `ensureTargets`, `destroyTargets`, `occlusionPass`)
- Test: `tests/SceneRenderCheck.cpp` (the Task 2 block)

**Interfaces:**
- Consumes: `occlusionPass`, `occlusionTexture_`, `occlusionFbo_`, `kUnitOcclusion` (Task 2); `at` and `drawWith` in the Task 2 block.
- Produces: `Program::blurDirection` (`uBlurDirection`), `occlusionBlur_`, `occlusionBlurTexture_` and `occlusionBlurFbo_`. `occlusionTexture_` still holds the final visibility when `occlusionPass` returns.

- [ ] **Step 1: Write the failing check**

In the Task 2 block, right after the "nor the cube's open top" check, add:

```cpp
            // Along the cube's front edge the shade is smooth, not speckled by the sampling noise.
            int lightest = 0;
            int darkest = 3 * 255;
            for (int i = 0; i < 8; ++i) {
                const int sum = Sum(at(-0.35f + 0.1f * static_cast<float>(i), -0.5f, 0.6f));
                lightest = std::max(lightest, sum);
                darkest = std::min(darkest, sum);
            }
            Expect(lightest - darkest <= 12, "the shade along the cube's front edge is smooth (" +
                                                 std::to_string(darkest) + " to " + std::to_string(lightest) + ")");
```

- [ ] **Step 2: Run the checks to verify the new one fails**

Run: `cmake --build build --config Debug --target scene-render-check`, then `build/Debug/scene-render-check.exe`
Expected: `FAIL the shade along the cube's front edge is smooth (…)`.

If it already passes without the blur, the noise happens to be quiet at 128 px. Lower the bound to the printed spread minus 1, so it fails now and pins the improvement. Note the value in the commit message.

- [ ] **Step 3: Write `aoBlur.frag`**

Create `resources/shaders/pipeline/aoBlur.frag`:

```glsl
#version 330 core
// One direction of the blur that smooths gtao.frag's noise, run across and
// then down. A neighbor counts less the farther it sits from the middle
// pixel's tangent plane, as a part of the middle's depth, and the more its
// normal turns away, so occlusion does not bleed across a silhouette or a
// crease. Depth comes from the depth buffer itself, not a copy in the
// occlusion buffer. Renderer puts lighting.glsl in after the #version line.
in vec2 vUv;
out vec4 outOcclusion;

uniform sampler2D uOcclusion;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
// 1 over the buffers' size in pixels.
uniform vec2 uTexel;
// (1, 0) across, (0, 1) down.
uniform vec2 uBlurDirection;

const int kBlurRadius = 4;
// A neighbor this far from the middle's tangent plane, as a part of the middle's depth, no longer counts.
const float kPlaneTolerance = 0.02;
const float kNormalPower = 8.0;
// A Gaussian over 9 taps.
const float kWeights[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(uDepth, pixel, 0).r;
    if (depth >= 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    vec3 P = viewPositionAt(vUv, depth);
    vec3 N = normalize(texelFetch(uNormal, pixel, 0).xyz);
    float tolerance = kPlaneTolerance * -P.z;
    ivec2 last = textureSize(uDepth, 0) - 1;
    ivec2 stride = ivec2(uBlurDirection);

    float sum = texelFetch(uOcclusion, pixel, 0).r * kWeights[0];
    float total = kWeights[0];
    for (int i = 1; i <= kBlurRadius; ++i) {
        for (int side = -1; side <= 1; side += 2) {
            ivec2 texel = clamp(pixel + stride * (i * side), ivec2(0), last);
            float sampleDepth = texelFetch(uDepth, texel, 0).r;
            if (sampleDepth >= 1.0) {
                continue;
            }
            vec3 Ps = viewPositionAt((vec2(texel) + 0.5) * uTexel, sampleDepth);
            float planeWeight = clamp(1.0 - abs(dot(Ps - P, N)) / tolerance, 0.0, 1.0);
            vec3 Ns = normalize(texelFetch(uNormal, texel, 0).xyz);
            float normalWeight = pow(clamp(dot(N, Ns), 0.0, 1.0), kNormalPower);
            float weight = kWeights[i] * planeWeight * normalWeight;
            sum += texelFetch(uOcclusion, texel, 0).r * weight;
            total += weight;
        }
    }
    outOcclusion = vec4(sum / total);
}
```

- [ ] **Step 4: Run it twice in occlusionPass**

`src/runner/Renderer.hpp`: in `Program`, after `occlusionStrength`, add `int blurDirection = -1;`. After `Program gtao_;`, add `Program occlusionBlur_;`. After `occlusionTexture_`:

```cpp
    // The blur's halfway buffer: across into it, then down back into occlusionTexture_.
    unsigned occlusionBlurFbo_ = 0;
    unsigned occlusionBlurTexture_ = 0;
```

`src/runner/Renderer.cpp`:

- `buildProgram`: add `program.blurDirection = at("uBlurDirection");`.
- `initialize`: after the `gtao_` build, add

  ```cpp
          buildProgram(occlusionBlur_, "Occlusion blur", "pipeline/fullscreen.vert", "pipeline/aoBlur.frag",
                       {"pipeline/lighting.glsl"}) &&
  ```

- `shutdown`: delete `occlusionBlur_.id` alongside `gtao_.id`.
- `ensureTargets`: add `occlusionBlurTexture_ = MakeTarget(RT_GL_R8, RT_GL_RED, GL_UNSIGNED_BYTE, width, height);` and

  ```cpp
      glGenFramebuffers(1, &occlusionBlurFbo_);
      glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionBlurFbo_);
      complete = Attach({occlusionBlurTexture_}, 0) && complete;
  ```

- `destroyTargets`: add `&occlusionBlurFbo_` and `&occlusionBlurTexture_`.

In `occlusionPass`, replace the tail from `DrawFullscreen(emptyVao_);` through `return true;` with:

```cpp
    DrawFullscreen(emptyVao_);

    // Across into the halfway buffer, then down back into occlusionTexture_.
    glUseProgram(occlusionBlur_.id);
    glUniformMatrix4fv(occlusionBlur_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform2f(occlusionBlur_.texel, 1.f / static_cast<float>(targetWidth_),
                1.f / static_cast<float>(targetHeight_));
    if (!CanDraw(occlusionBlur_.id)) {
        return false;
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionBlurFbo_);
    BindTexture(kUnitOcclusion, occlusionTexture_);
    glUniform2f(occlusionBlur_.blurDirection, 1.f, 0.f);
    DrawFullscreen(emptyVao_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    BindTexture(kUnitOcclusion, occlusionBlurTexture_);
    glUniform2f(occlusionBlur_.blurDirection, 0.f, 1.f);
    DrawFullscreen(emptyVao_);
    glDepthMask(GL_TRUE);
    occlusionReady_ = true;
    return true;
```

- [ ] **Step 5: Run the checks to verify they pass**

Run: `cmake --build build --config Debug --target scene-render-check`, then `build/Debug/scene-render-check.exe`
Expected: `scene render checks passed`, including every Task 2 check. The "past the top edge" check is now also guarding against the blur bleeding across the silhouette.

- [ ] **Step 6: Commit**

```bash
git add resources/shaders/pipeline/aoBlur.frag src/runner/Renderer.hpp src/runner/Renderer.cpp tests/SceneRenderCheck.cpp
git commit -m "Smooth ambient occlusion with a depth- and normal-aware blur"
```

---

### Task 4: Specular occlusion, multi-bounce, and the README

**Files:**
- Modify: `resources/shaders/pipeline/image_lighting.glsl` (`multiBounce`, `specularOcclusion`, `skyLight`)
- Modify: `README.md:16` (the Lighting bullet)
- Test: `tests/SceneRenderCheck.cpp` (the Skybox block that declares `runner::SceneLighting lit;` and `drawSky`)

**Interfaces:**
- Consumes: `skyLight(…, float occlusion)` (Task 2). In the Skybox block: `lit`, `drawSky(meshes, count)`, `cube`, and `ReadPixel`, `Sum`, `Text`.
- Produces: GLSL `vec3 multiBounce(float visibility, vec3 albedo)` and `float specularOcclusion(float NdotV, float visibility, float roughness)` in `image_lighting.glsl`.

- [ ] **Step 1: Write the failing check**

Find the Skybox block in `tests/SceneRenderCheck.cpp`, the one that declares `runner::SceneLighting lit;` and the `drawSky` lambda (`grep -n "const auto drawSky" tests/SceneRenderCheck.cpp`). At the end of that block, before its closing brace and before it resets the lighting, add:

```cpp
            // A smooth metal floor reflects the sky; where the cube stands on it the reflection is dimmed.
            {
                // up is the enclosing block's, from the Camera checks above.
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                                   runner::Renderer::kCameraFovYDegrees);
                engine_core::Matrix4 floorModel = engine_core::matrix4_translation(0.f, -0.55f, 0.f);
                floorModel.m[0] = 20.f;
                floorModel.m[5] = 0.1f;
                floorModel.m[10] = 20.f;
                runner::MeshDraw metalFloor{cube, floorModel};
                metalFloor.metalness = 1.f;
                metalFloor.roughness = 0.2f;
                const runner::MeshDraw scene[2] = {runner::MeshDraw{cube, engine_core::matrix4_identity()}, metalFloor};
                const engine_core::Matrix4 view =
                    engine_core::matrix4_inverse(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up));
                const float focal = 1.f / std::tan(0.5f * runner::Renderer::kCameraFovYDegrees * 0.01745329252f);
                const engine_core::Vec3 p = engine_core::matrix4_point(view, {0.f, -0.5f, 0.56f});
                const int cx = static_cast<int>((focal * p.x / -p.z * 0.5f + 0.5f) * static_cast<float>(fbWidth));
                const int cy = static_cast<int>((focal * p.y / -p.z * 0.5f + 0.5f) * static_cast<float>(fbHeight));

                runner::SceneLighting metalOff = lit;
                metalOff.occlusion = false;
                renderer.setLighting(metalOff);
                drawSky(scene, 2);
                const Pixel reflectedOff = ReadPixel(cx, cy);
                runner::SceneLighting metalOn = lit;
                metalOn.occlusion = true;
                renderer.setLighting(metalOn);
                drawSky(scene, 2);
                const Pixel reflectedOn = ReadPixel(cx, cy);
                Expect(Sum(reflectedOn) + 15 < Sum(reflectedOff), "a metal floor's reflection is dimmed where the cube "
                                                                  "stands on it (" +
                                                                      Text(reflectedOn) + " under " +
                                                                      Text(reflectedOff) + ")");
                renderer.setLighting(lit);
            }
```

If the block's `drawSky` takes different parameters than `(const runner::MeshDraw*, int)`, match its signature. Do not change it.

- [ ] **Step 2: Run the checks to verify it fails**

Run: `cmake --build build --config Debug --target scene-render-check`, then `build/Debug/scene-render-check.exe`
Expected: `FAIL a metal floor's reflection is dimmed where the cube stands on it (…)`. A metal has no diffuse term, and Task 2 occluded only diffuse.

- [ ] **Step 3: Add multi-bounce and specular occlusion**

`resources/shaders/pipeline/image_lighting.glsl`, above `skyLight`:

```glsl
// Jimenez et al. 2016's fit for light that bounces between occluders: a
// bright surface loses less to occlusion than its visibility alone says.
vec3 multiBounce(float visibility, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(visibility), ((visibility * a + b) * visibility + c) * visibility);
}

// Lagarde and de Rousiers 2014: how much of a reflection the occlusion
// keeps, by how rough the surface is and how squarely it is seen.
float specularOcclusion(float NdotV, float visibility, float roughness) {
    return clamp(pow(NdotV + visibility, exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility, 0.0, 1.0);
}
```

In `skyLight`, the legacy branch becomes

```glsl
        return ambientLight(viewDirection, N, albedo, metallic, roughness, reflectivity, ambient, skyRadiance) *
               multiBounce(occlusion, albedo);
```

and the image-based tail becomes

```glsl
    vec3 diffuseOcclusion = multiBounce(occlusion, albedo);
    float reflectionOcclusion = specularOcclusion(NdotV, occlusion, roughness);
    vec3 sky = (kD * albedo * irradiance * diffuseOcclusion +
                reflected * (F0 * brdf.x + brdf.y) * reflectionOcclusion) *
               uSkyColor;
    // Lighting.Ambient lights every surface alike, as light from no direction.
    return sky + albedo * (1.0 - metallic) * ambient / kPi * diffuseOcclusion;
```

With `occlusion` 1 (forward.frag, or AO off), `multiBounce` is 1 and `specularOcclusion` is `clamp(pow(NdotV + 1, e) - 1 + 1)`, which is 1 or more and clamps to 1. See-through surfaces are unchanged.

- [ ] **Step 4: Run the checks to verify they pass**

Run: `cmake --build build --config Debug --target scene-render-check`, then `build/Debug/scene-render-check.exe`
Expected: `scene render checks passed`. Every Task 2 and 3 check still holds. Multi-bounce lightens a white floor's contact a little, which stays well within "+ 15 <".

- [ ] **Step 5: Update the README**

`README.md:16`, the Lighting bullet. In the property list, change "`Exposure`, `Saturation`, and `Gamma`" to "`Exposure`, `Saturation`, `Gamma`, `AmbientOcclusion`, `OcclusionRadius`, and `OcclusionStrength`". Change "The Scene View reads `Ambient`, `Exposure`, `Saturation`, and `Gamma`" to "The Scene View reads `Ambient`, `Exposure`, `Saturation`, `Gamma`, and the occlusion properties". Then add after that sentence:

> With `AmbientOcclusion` on, as it is by default, surfaces are shaded where their neighbors hide the sky and ambient light from them: in creases, under objects, where a wall meets the floor. Lights are not shaded by it. `OcclusionRadius` (0 to 10, 1 by default) is how far, in world units, a surface looks for what hides it, and `OcclusionStrength` (0 to 4, 1 by default) darkens the shade above 1 and lightens it below; either at 0 turns it off. See-through surfaces get none.

- [ ] **Step 6: Build everything and run the full suite**

Run:
- `cmake --build build --config Debug`
- `ctest --test-dir build -C Debug --output-on-failure`
- `build/Debug/scene-render-check.exe`
- `cmake --build build --config Release`

Expected: all pass, and both builds succeed.

Then launch the studio over MCP (see the `drive-studio-over-mcp` memory). Place a cube on a floor with a Skybox, screenshot the Scene View, and open the screenshot in the editor to look at it. Toggle `Lighting.AmbientOcclusion` and screenshot again. Check by eye for:

- contact shading under the cube;
- no dark ring around silhouettes;
- no visible speckle when the camera is still.

- [ ] **Step 7: Commit**

```bash
git add resources/shaders/pipeline/image_lighting.glsl README.md tests/SceneRenderCheck.cpp
git commit -m "Occlude reflections and let bright surfaces keep bounced light"
```

---

## Deferred (revisit later)

These are deliberately out of scope. Each is a separate plan when the time comes.

- **Visibility bitmask** (Therrien et al. 2023): replace the max-horizon with 32 occluded sectors and an assumed thickness. This fixes thin poles darkening a wide disc of floor, and the same march can produce screen-space indirect lighting. It needs a thin-object pixel check that compares footprint widths.
- **Temporal accumulation:** once the renderer has TAA or motion vectors, drop to 1 slice, vary the noise per frame, and accumulate. That would cost about a third of today's taps for a cleaner result.
- **Half resolution with a depth-aware upsample:** a near-4× saving for large panes. Not worth it at Scene View sizes yet.
- **Depth mip chain:** for large radii and cache efficiency. Matters only once radii grow or resolution goes up.
- **Bent normals:** for better-directed irradiance lookups.
- **G-buffer view:** a debug view showing the AO buffer in the Scene View (research doc I17).
- **Depth-derived normals for AO:** XeGTAO reconstructs normals from depth so normal maps do not self-occlude. This plan uses the G-buffer's normal-mapped normal. Revisit if bumpy materials show speckle that the blur's normal weight cannot remove.
