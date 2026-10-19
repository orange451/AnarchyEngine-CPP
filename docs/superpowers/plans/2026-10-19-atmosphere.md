# Atmosphere Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An `Atmosphere` under Lighting fogs the scene, using Godot-style analytic height fog. Distant surfaces fade into air that takes the sky's color, the air glows toward the sun, and the sky's horizon goes hazy. This adds no new pass and no new texture.

**Architecture:**
- **Instance.** A new `Atmosphere` instance has saved properties and reaches the renderer through a `VisualAtmosphere` snapshot row, which becomes `SceneAtmosphere`.
- **Shader include.** `atmosphere.glsl` holds the closed-form fog. It has its own uniforms and its own cube sampler, and is included in `merge.frag` (opaque surfaces, terrain and sky), `forward.frag` and `particle.frag`.
- **Uniforms.** The Renderer works out the per-frame uniforms once, in `atmosphereUniforms()`.
- **CPU mirror.** `AtmosphereMath` mirrors the formula on the CPU for tests and tuning.

**Tech Stack:** C++17, GLSL 330, Catch2 v3 sandbox, CMake, MSVC.

**Spec:** `docs/superpowers/specs/2026-10-14-atmosphere-design.md` (revised 2026-10-19). Read its "The model" and "Where it applies" sections before Task 2.

## Global Constraints

- **Lua names, exactly:**
  - the class `Atmosphere`;
  - the properties `Enabled`, `Density`, `HeightFalloff`, `BaseHeight`, `Color`, `AerialPerspective`, `SunScatter`, `SkyHaze`.
- **Defaults and ranges, exactly:**

  | Property | Default | Range |
  |---|---|---|
  | Enabled | true | |
  | Density | 0.25 | 0 to 1 |
  | HeightFalloff | 0.01 | 0 to 0.2 |
  | BaseHeight | 0 | any finite value |
  | Color | (0.75, 0.8, 0.88) | |
  | AerialPerspective | 0.5 | 0 to 1 |
  | SunScatter | 0.25 | 0 to 1 |
  | SkyHaze | 1 | 0 to 1 |

  Values that are not finite are refused with "<Property> must be a finite number"; the rest are clamped.
- Containment refuses with "An Atmosphere must be in Lighting". This comes from the existing vowel branch in `Containment.cpp`.
- **No Atmosphere, Enabled false, or Density 0** means the output is pixel-identical to main, and the shader takes an early-out branch.
- **Budget:** at most 0.1 ms at 1080p on the UHD 770, measured warmed-up and Release against Release (see memory "profile-warmed-up").
- **Style:**
  - Comments are in the engine's voice.
  - Distances are "units", never "studs".
  - Files use LF.
  - Each class header documents every property, its default and its range, as `BloomEffect.hpp` does.
- **Where to work:** a worktree, `.claude/worktrees/atmosphere`, on branch `atmosphere` from `main`. The main checkout has uncommitted shadow work; do not touch it. Set the worktree up as the memory "worktree-build-setup" describes.
- **Build:** run `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target sandbox AnarchyStudio > build/atmo.log 2>&1; grep -E " error |error C|LNK" build/atmo.log` from the worktree. Never run `sandbox.exe` while a build is running.
- **Test names:** `ATM<n> sentence` with tag `[atmosphere]`, and `ATMM<n>` with tag `[atmosphere-math]`.
- **Commits:** the message is `Area: what changed, as a sentence`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Add files by explicit path only. Never push, and never merge into main.
- **Review:** one review round per task with a mid-tier reviewer, and park cosmetic findings (memory "sdd-speed-over-ceremony").

## Review Focus

1. **Camera far below BaseHeight** (a cave, or BaseHeight 5000). Without the clamp, the `exp(-f*h0)` factor overflows to inf, which turns into NaN pixels and makes the bloom blow up. With the clamp the output is finite and the scene is fully fogged. → ATMM3.
2. **Rays near the horizon** (`dy ≈ 0`) and a HeightFalloff of 0. Dividing by `f*dy*d` gives 0/0. The series branch must give the uniform-fog answer, and its value must not jump at the switch-over. → ATMM2.
3. **A DynamicSky whose cube is not ready yet,** or no sky at all. With no cube, aerial perspective must fall back to `Color`, never to black from sampling an unbound cube. → `atmosphereUniforms` sets `skyReady=false`, and Task 4's render check covers no-sky.
4. **Additive particles,** such as fire and explosions in fog. They must dim with distance and add no air light. Otherwise distant fireballs turn into grey squares. → Task 5's particle branch, checked by hand.
5. **Toggling Density to 0 or Enabled to false.** The frame must match main exactly, with no tint left over. → ATM4 (snapshot) and the render check in Task 4.

---

### Task 1: The Atmosphere instance

**Files:**
- Create: `src/engine_instances/Atmosphere.hpp`, `src/engine_instances/Atmosphere.cpp`. Copy `BloomEffect`'s shape, and Color3 handling from `Skybox.cpp`'s Tint.
- Modify:
  - `CMakeLists.txt`: the sources, and `sandbox/atmosphere_tests.cpp` next to `bloom_tests.cpp` at line 1191.
  - `src/engine_core/Containment.cpp:148`: add `"Atmosphere"` to the Lighting-only list.
  - `src/engine_core/LuaApi.cpp`: a doc line per property, next to the BloomEffect lines (`:1356`).
  - `src/engine_core/Project.cpp:197`: the creatable list.
  - `src/engine_core/ScriptBindings.cpp:229` and `:294`: `register_lua_creatable`.
  - `tests/LuauCompleteTest.cpp`: the class and its properties.
  - `src/ide/ClassOrder.hpp`: after BloomEffect.
  - `src/ide/IdeIcons.cpp:47`: the icon. Reuse the Skybox icon unless a fitting one is in `resources/icons/`.
  - `src/engine_instances/README.md` and the root `README.md`: a short prose section.
- Test: `sandbox/atmosphere_tests.cpp`.

**Interfaces:**
- **Produces:** `engine_core::Atmosphere` with:
  - `kDefault*` and `kMax*` constants (`kMinHeightFalloff` is 0);
  - the getters `enabled()`, `density()`, `height_falloff()`, `base_height()`, `color()` (returns `Color3`), `aerial_perspective()`, `sun_scatter()`, `sky_haze()`;
  - the setters `set_<name>(…) -> std::optional<std::string>`.

- [ ] **Step 1: Write the failing tests, modelled on `bloom_tests.cpp`.**
  - **ATM1 "an Atmosphere's properties are checked, undo, save, and come back at Stop":**
    - The defaults match the Global Constraints table.
    - A default instance saves an empty `PropertyBag`.
    - Set, undo and redo work for Density and Color.
    - Every number clamps at both ends.
    - `set_base_height(-1e6)` keeps -1e6.
    - Each NaN is refused with "<Property> must be a finite number".
    - A Color with an inf channel is refused.
  - **ATM2 "an Atmosphere lives only under Lighting":**
    - Parenting it to Workspace gives "An Atmosphere must be in Lighting".
    - Parenting it to a Folder inside Lighting is allowed.
  - **ATM3 "Lua reads and writes an Atmosphere":**
    - `Instance.new("Atmosphere")` works.
    - Setting `.Density = 2` reads back 1.
    - `.Color = Color3.new(1,0,0)` round-trips.
    - A save then load round-trip keeps the values that are not defaults.
- [ ] **Step 2: Build and run** `sandbox.exe "[atmosphere]"`. Expect a compile failure or FAIL, because `Atmosphere` is not defined.
- [ ] **Step 3: Implement the class and its registration.**
  - Sliders go on every bounded number, using `lua_slider` with the ranges above.
  - BaseHeight has no slider.
  - The property group order follows the Global Constraints table.
- [ ] **Step 4: Run `sandbox.exe "[atmosphere]"` and `sandbox.exe "[containment]"`.** Expect PASS.
- [ ] **Step 5: Commit:** `Atmosphere: a new instance under Lighting holds the air's density, height falloff, color, and haze`.

### Task 2: AtmosphereMath, the CPU mirror

**Files:**
- Create: `src/runner/AtmosphereMath.hpp`, `src/runner/AtmosphereMath.cpp`.
- Test: `sandbox/atmosphere_math_tests.cpp`. Add it to the math test list next to `bloom_math_tests.cpp` (`CMakeLists.txt:1220`).

**Interfaces:**
- **Produces** (namespace `runner`):
  - `constexpr float kAtmosphereDensityScale = 0.004f;` Units⁻¹ at Density 1; Task 6 retunes it.
  - `constexpr float kAtmosphereSkyDistance = 20000.f;` How far the sky is treated as being, in units.
  - `constexpr float kAtmosphereMaxExponent = 80.f;`
  - `float AtmosphereTransmittance(float sigma0, float falloff, float heightAboveBase, float dy, float distance);` Follows the spec's model exactly, including the `|x| < 1e-4` series and the clamp on `-falloff*heightAboveBase`.
  - `float AtmosphereSunLobe(float cosAngle);` Returns `pow(max(cosAngle, 0), 8)`.
  - `float AtmosphereSkyLod(float transmittance, float maxLod);` Returns `maxLod * transmittance`.

- [ ] **Step 1: Write the failing tests.**
  - **ATMM1 "uniform air fades as exp(-σd)":** with falloff 0, check `exp(-0.01f*500)` within 1e-5, for dy values of -1, 0 and 1.
  - **ATMM2 "the horizon series matches the closed form":**
    - With σ0 0.01, f 0.05 and h0 10, take dy = 1e-7, 2e-6, 1e-3 and 0.5 at d = 1000. Each matches a double-precision numeric integral (1000 midpoint steps) within 1e-4.
    - The values just either side of the switch-over differ by under 1e-5.
  - **ATMM3 "a camera far below the base height stays finite":**
    - `AtmosphereTransmittance(0.01f, 0.2f, -1e5f, 0.f, 100.f)` is finite and in [0, 1].
    - The same holds for `d = 1e9`.
    - `distance = 0` gives exactly 1.
  - **ATMM4 "air thins with height":** at the same d with dy = 0, transmittance with h0 = 200 is greater than with h0 = 0. A ray looking up (dy = 1) from h0 = 0 is clearer than a horizontal one.
  - **ATMM5 "the sun lobe":** 1 at cos 1, 0 at cos ≤ 0, and `pow(0.9, 8)` at 0.9.
- [ ] **Step 2: Run `sandbox.exe "[atmosphere-math]"`.** Expect FAIL (the code is missing).
- [ ] **Step 3: Implement.** The functions are pure, with no GL.
- [ ] **Step 4: Run it again.** Expect PASS.
- [ ] **Step 5: Commit:** `Renderer: AtmosphereMath works out how much air lies along a ray, its sun glow, and its sky mip`.

### Task 3: The snapshot row and the scene struct

**Files:**
- Modify:
  - `src/engine_core/SnapshotPump.hpp`: add `VisualAtmosphere` next to `VisualBloom` (`:147`), and a member next to `bloom` (`:349`).
  - `src/engine_core/SnapshotPump.cpp`:
    - static_assert the defaults against `Atmosphere::kDefault*`, as at `:38`;
    - fill the row with `find_first<Atmosphere>` after bloom (`:684`);
    - copy it next to `:737`.
  - `src/runner/SceneFeed.cpp:55`: copy it.
  - `src/runner/Renderer.hpp`: add `SceneAtmosphere` next to `SceneBloom`, and an `atmosphere` member in `SceneLighting`.
  - `src/runner/GameView.cpp:521`: map the row onto `lighting.atmosphere`.
- Test: add to `sandbox/atmosphere_tests.cpp`.

**Interfaces:**
- **Produces** `struct VisualAtmosphere`:
  - `bool present=false; bool enabled=true;`
  - `float density, height_falloff, base_height;`
  - `float color[3];` as sRGB, the way the Color3 holds it;
  - `float aerial_perspective, sun_scatter, sky_haze;`
- **Produces** `struct SceneAtmosphere`:
  - `bool enabled=false;` true only when present, enabled and density > 0;
  - the same fields, named in camelCase.

- [ ] **Step 1: Write the failing test** **ATM4 "the snapshot carries the first Atmosphere under Lighting":**
  - With none, `present` is false.
  - Put two in a Folder under Lighting and set the first one's Density to 0.6. The row has 0.6 and `present`.
  - Disabling it keeps `present` but sets `enabled` false.
  - Copy the GameView mapping logic into `runner::ToSceneAtmosphere(const VisualAtmosphere&) -> SceneAtmosphere`, declared in `Renderer.hpp`, so it can be tested. With density 0, `ToSceneAtmosphere` gives `enabled == false`.
- [ ] **Step 2: Run `sandbox.exe "[atmosphere]"`.** Expect FAIL.
- [ ] **Step 3: Implement.** `GameView` calls `ToSceneAtmosphere`.
- [ ] **Step 4: Run `sandbox.exe "[atmosphere]"` and `sandbox.exe "[snapshot]"`.** Expect PASS.
- [ ] **Step 5: Commit:** `Snapshot: the render snapshot carries the first Atmosphere under Lighting to the renderer`.

### Task 4: atmosphere.glsl, applied in the merge (opaque surfaces, terrain, sky)

**Files:**
- Create: `resources/shaders/pipeline/atmosphere.glsl`.
- Modify:
  - `src/runner/Renderer.cpp`:
    - add `atmosphere.glsl` to `merge_`'s include list (`:460-513`);
    - look up the uniforms in the program-locations code near `:297`;
    - add the private `atmosphereFrame() -> AtmosphereFrame` (once per draw, stored in `atmosphere_`), and call `UploadAtmosphere(merge_.id, atmosphere_)` in `mergePass` (`:3050`).
  - Create `src/runner/AtmosphereUniforms.{hpp,cpp}`:
    - `struct AtmosphereFrame`: plain data, one field per `atmosphere.glsl` uniform, plus `unsigned skyCube` and `int skyUnit`;
    - `void UploadAtmosphere(unsigned program, const AtmosphereFrame&)`: looks up the uniforms by name (with a per-program cache), binds the cube to `skyUnit`, and uploads everything.

    Both the Renderer and the ParticleRenderer use it, so neither reaches into the other.
  - `resources/shaders/pipeline/merge.frag`: apply the fog to `color` before the see-through composite, and to `sky` in the `depth >= 1` branch, mixed in by `uAtmosphereSkyHaze`.
- Test: `tests/SceneRenderCheck.cpp`, a new block next to the bloom block (`:2842`).

**Interfaces:**
- **Consumes:** `SceneAtmosphere` and `kAtmosphere*` (Tasks 2 and 3).
- **Produces:**
  - **`atmosphere.glsl`.** It includes no other file. It declares:
    - `uniform float uAtmosphereEnabled;`
    - `uniform float uAtmosphereSigma0, uAtmosphereFalloff, uAtmosphereHeight;` (`uAtmosphereHeight` is the camera's height minus BaseHeight)
    - `uniform vec3 uAtmosphereUp;` (world up in view space)
    - `uniform vec3 uAtmosphereColor;` (linear)
    - `uniform float uAtmosphereAerial, uAtmosphereSkyHaze;`
    - `uniform vec3 uAtmosphereLightDirection, uAtmosphereLightColor;` (view space; the color is already times SunScatter)
    - `uniform samplerCube uAtmosphereSky; uniform mat3 uAtmosphereViewToSky; uniform float uAtmosphereSkyMaxLod, uAtmosphereSkyReady;`

    and the function `vec4 atmosphereAt(vec3 viewDirection, float distance)`. Its rgb is `C * (1 - T)` and its a is `T`, and it returns `vec4(0,0,0,1)` when `uAtmosphereEnabled < 0.5`. The math is AtmosphereMath's, line for line.
  - **Applying it:** `color = color * a.a + a.rgb`.
  - **`atmosphereFrame()`,** worked out once per draw in `Renderer::draw`:
    - the camera's height and `uAtmosphereUp` come from `view_`;
    - `uAtmosphereViewToSky` is the same matrix `bindSky` uploads as `uViewToSky`;
    - which light the glow uses: the DynamicSky's `lightDirection` and `lightColor` when `dynamicSky.enabled`; otherwise the first `SceneLight` of `Kind::Directional` (color times intensity, direction negated so it points toward the light); otherwise a zero color;
    - the cube is `uPrefiltered`'s texture, with `skyReady = skyReady_`. The cube goes on a texture unit no merge, forward or particle sampler uses; pick the next free one after the merge's highest.

- [ ] **Step 1: Write the failing render check.** It uses the existing check scaffolding: a grey cube 200 units from the camera, with no sky.
  - With `atmosphere.enabled = true`, density 1, falloff 0 and color pure red, the cube's center pixel is redder than without (red-minus-green grows by more than 0.2).
  - With `enabled = false`, the frame is byte-identical to the one with no Atmosphere at all.
- [ ] **Step 2: Build and run `SceneRenderCheck`.** Expect the new block to FAIL (the pixel does not change).
- [ ] **Step 3: Implement.**
  - **The distance in merge** is `length(viewPositionAt(vUv, depth))`, and the direction is the normalized view position.
  - **For sky pixels,** the direction is the inverse projection of the pixel at depth 1, and the distance is `kAtmosphereSkyDistance`, passed in as `uAtmosphereSkyDistance`. Then `sky = mix(sky, sky * a.a + a.rgb, uAtmosphereSkyHaze)`.
  - **When the sky was not drawn** (`uSkyDrawn < 0.5`), the sky pixels stay as they are, so the pane's color still shows.
- [ ] **Step 4: Run `SceneRenderCheck`, `sandbox.exe "[atmosphere]"` and `sandbox.exe "[bloom]"`.** Expect PASS.
- [ ] **Step 5: Commit:** `Renderer: the Atmosphere fogs opaque surfaces, terrain, and the sky in the merge, from a shared atmosphere.glsl`.

### Task 5: See-through surfaces and particles

**Files:**
- Modify:
  - `resources/shaders/pipeline/forward.frag`: fog `color` before `outColor`. The distance is `length(vViewPosition)`.
  - `resources/shaders/pipeline/particle.frag`:
    - **Normal:** `rgb = rgb * a.a + a.rgb` before premultiplying;
    - **Additive:** `rgb *= a.a`;
    - **Multiply:** `outColor.a *= a.a`.
  - `src/runner/ParticleRenderer.cpp:62`: build with `atmosphere.glsl` inserted after `#version`, the way `EnvironmentMap.cpp:50` passes includes. The particle draw call takes a `const AtmosphereFrame&` and calls `UploadAtmosphere(program_, frame)` after `glUseProgram`.
  - `src/runner/Renderer.cpp`: add `atmosphere.glsl` to `forward_`'s include list. Call `UploadAtmosphere(forward_.id, atmosphere_)` in `transparencyPass` (`:2935`), and pass `atmosphere_` to the particle draw (`:3011`).
- Test: extend Task 4's render check. A half-transparent glass pane 200 units away gets redder with the red Atmosphere.

- [ ] **Step 1: Write the failing check** (the glass pane above).
- [ ] **Step 2: Run it.** Expect FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run `SceneRenderCheck`, `sandbox.exe "[particles]"` and `sandbox.exe "[atmosphere]"`.** Expect PASS. Then check by hand: in Studio, an additive fire emitter 500 units into fog fades out and leaves no grey square.
- [ ] **Step 5: Commit:** `Renderer: see-through surfaces and particles take the Atmosphere too; additive particles dim without adding air`.

### Task 6: Tune, profile, document

**Files:**
- Modify:
  - `src/runner/AtmosphereMath.hpp`: tune `kAtmosphereDensityScale` and `kAtmosphereSkyDistance`. If the defaults change, also update `Atmosphere::kDefault*` and the spec's table.
  - `docs/superpowers/specs/2026-10-14-atmosphere-design.md`: the final constants.

- [ ] **Step 1: Look at it in Studio over MCP** (memory "drive-studio-over-mcp"). Use Bald Eagle Valley (H1Z1 Z1) with a default Atmosphere under Lighting, and check:
  - noon, sunset and night with the DynamicSky;
  - the same place with a Skybox;
  - the same place with no sky.

  Tune `kAtmosphereDensityScale` until the default reads as a clear day, with distant ridges clearly hazed and nearby trees crisp. Take one screenshot of each case for the user, at the end.
- [ ] **Step 2: Profile.** Use Release against Release, warmed up, with `JADEFX_MAX_FPS=0` and `per_call_ms` (memory "profile-warmed-up"). Compare the merge pass with and without the Atmosphere. Expect at most 0.1 ms on the UHD 770.
- [ ] **Step 3: Run the full suite once:** `sandbox.exe` and `SceneRenderCheck`. The known failures are those listed in memory "release-test-flakes".
- [ ] **Step 4: Commit:** `Atmosphere: tuned on Bald Eagle Valley; the spec records the final constants`.
