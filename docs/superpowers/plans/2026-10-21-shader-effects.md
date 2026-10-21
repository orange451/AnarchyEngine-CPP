# ShaderEffect (Post Shaders) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A post Shader (`Kind` Post, `void post()`) draws over the finished frame through a new `ShaderEffect` instance under `Lighting`, in the Scene View and the player, with its uniforms as the effect's attributes, ordered by `Order`, before or after the tone map by the Shader's `Hdr` property; the Shader Editor previews it and lists the effects that use it.

**Architecture:** `ShaderEffect` is a `ReferenceAsset` under `Lighting` (one `Shader?` slot, `Enabled`, `Order`), registered like `BloomEffect` and placed like it. Its binding reuses `SurfaceBinding` (shader id, version, parse, samplers, values) built by a holder-generic `make_shader_binding` and cached by holder id in `SurfaceBindingCache`. The snapshot carries a `VisualShaderEffect` list in `Order`; `GameView` turns it into `SceneEffect`s on `SceneLighting`, resolving textures as `ApplySurface` does. `SurfacePrograms::Built` gains a `post` variant assembled from a new `post.frag` + `post_shader.glsl` template (the engine's uppercase globals and the G-buffer helpers), so errors travel the existing `takeSurfaceErrors` route and programs are reclaimed with the rest. `Renderer::draw` gets two insertion points: `hdr` effects ping-pong on `mergeTexture_` and a second RGBA16F target after merge and before bloom; LDR effects ping-pong on `ldrTexture_` and a second RGBA8 target after the tone map and before FXAA.

**Tech Stack:** C++17/MSVC, CMake (`MSYS_NO_PATHCONV=1 cmake --build build --config Release --target <t>`), Catch2 sandbox (`./build/Release/sandbox.exe "<filter>"`), the Expect harnesses in tests/ (`shader-editor-tests`, `studio-tests`, `properties-tests`, `engine-tests`), `scene-render-check` (GL, run by hand from the repo root), GLSL 330, JadeFX UI. `SceneFeedTest.cpp` is part of `engine-tests`.

**Spec:** `docs/superpowers/specs/2026-10-20-custom-shaders-design.md`, sections "A vignette", "Post shaders", "ShaderEffect", "The post chain", "Snapshot", "Studio". Read with `docs/superpowers/specs/2026-10-20-shader-editor-design.md`, which moved Kind and the render modes onto the Shader as properties and gave it the Shader Editor; this plan follows those changes, not the older text where they differ.

## Global Constraints

- Class `ShaderEffect`, under `Lighting` only (through Folders), refused elsewhere with `"A ShaderEffect must be in Lighting"`. Script-creatable. Properties: `Enabled` (boolean, true), `Shader` (`Shader?`, nil), `Order` (number, 0, finite). Every enabled ShaderEffect runs, lowest `Order` first, equal Orders in tree order.
- A surface Shader on a ShaderEffect is refused with `"<Name> is a surface shader; a ShaderEffect takes a post shader"`; a Shader whose Kind later becomes Surface is kept by reference and draws nothing (as `Material::shader` does for a post Shader).
- The post stage's modes: a new saved Shader property `Hdr` (boolean, false). A phase 1 `render_mode hdr;` line sets it (and `render_mode` with no `hdr` clears it) on a post Shader, as the surface modes are set today; on a surface Shader `hdr` stays a parse error. Shown in the Shader Editor's render modes card for a post Shader (in place of the three surface toggles).
- Post built-ins (uppercase, reserved): `COLOR` (vec4, writable), `SCREEN`, `SCREEN_UV`, `SCREEN_SIZE`, `TEXEL`, `DEPTH`, `NORMALS`, `ALBEDO`, `MATERIAL` (sampler2D), `TIME`, `CAMERA_POSITION`, `VIEW`, `PROJECTION`, `INV_VIEW`, `INV_PROJECTION`; functions `depthAt(uv)` (linear, units; 1e9 at the sky), `viewPositionAt(uv)`, `worldPositionAt(uv)`, `normalAt(uv)` (view space, decoded). `DEPTH` reads raw depth; `depthAt` is what a user wants.
- `COLOR` is premultiplied. In the LDR stage rgb is 0..1 display color after the tone map (before FXAA); with `Hdr` it is linear light after merge, before bloom.
- A post Shader has up to `kMaxPostSamplers` (4) user textures, bound to texture units 12..15 (`kPostTextureUnit = 12`), the sky units no post program reads.
- A disabled effect, a nil Shader, a Shader that is not Post, a parse error, or a failed program is skipped; it never fails the frame.
- Effects run in `GameView` (Scene View and player). `ViewportRenderer` and `MaterialBall` set their own `SceneLighting` and run none, except the Shader Editor's own preview, which passes exactly one effect (the Shader at its defaults).
- Errors: a post program that fails reports `"<line>: <message>"` for the fragment stage through the existing `SurfacePrograms::Error` route; `Shader.Error` reads `"fragment <line>: <message>"`.
- No `studs`; distances are units. New APIs are named for this engine.

## Review Focus

- An `Hdr` effect that writes `COLOR.a` or outputs NaN: the tone map and bloom read it next; a NaN must not spread to the whole pane. Pinned in Task 5 (the template clamps `COLOR.rgb` to `>= 0` and keeps alpha finite).
- Two effects, one `Hdr` and one not, with the same `Order`: both run, each in its own stage, each stage in tree order. Pinned in Task 6.
- A ShaderEffect whose Shader's text is changed to a surface Shader mid-session (`Kind` set to Surface): the effect stops drawing and no stale program is bound. Pinned in Task 3 and 6.
- The pane with FXAA off and one LDR effect: the result still reaches the pane blended over the clear color with coverage kept in alpha for a transparent pane. Pinned in Task 6 (the LDR chain ends on the pane through a blit program, not the tone map).
- Resizing the pane: the second ping-pong targets follow `ensureTargets` (made, parked, unparked, destroyed with the rest); no GL error. Pinned in Task 6.

---

### Task 1: The parser's post stage: reserved names and `Hdr`

**Files:**
- Modify: `src/engine_core/ShaderSource.hpp`, `src/engine_core/ShaderSource.cpp` (`kReserved`), `src/engine_instances/Shader.hpp`, `src/engine_instances/Shader.cpp`
- Test: `sandbox/shader_source_tests.cpp`, `sandbox/shader_tests.cpp`

**Interfaces:**
- Consumes: `parse_shader(source, stage, kind)`, `ShaderParse::header_modes`, `Shader::set_mode(property, field, value, rebuilds)`, `Shader::prepare_source`, `write_place`/`read_place`'s property bytes.
- Produces:

```cpp
// ShaderSource.cpp: these names join kReserved (a uniform may not use them):
// "SCREEN", "DEPTH", "NORMALS", "MATERIAL", "INV_PROJECTION", "CAMERA_POSITION",
// "depthAt", "viewPositionAt", "worldPositionAt", "normalAt".
// Shader.hpp:
bool hdr() const { return hdr_; }
std::optional<std::string> set_hdr(bool on);   // saved property "Hdr"; a new source_version
```

- [ ] **Step 1: Write the failing tests.** In `shader_source_tests.cpp`: `uniform float SCREEN = 1.0;` in a post text errors `"SCREEN is a reserved name"` at its line, and so does `uniform vec3 DEPTH;`; `render_mode hdr;` under kind Post gives `header_modes == {"hdr"}`; under kind Surface it errors `"unknown render_mode hdr; a surface shader has transparent, two_sided, no_shadow"`. In `shader_tests.cpp` (`SHA36`): a Shader with Kind Post and `set_source("render_mode hdr;\nvoid post() {}\n")` has `hdr() == true` and its stored source's first line blank; `set_source("render_mode;\nvoid post() {}\n")` then clears it; `set_hdr(true)` moves `source_version()`, fires Changed `"Hdr"`, undoes as one step, and survives `write_place`/`read_place`; the Lua property `Hdr` reads and writes it; a surface Shader's `Hdr` is writable but ignored by `make_surface_binding`.
- [ ] **Step 2:** Run: `./build/Release/sandbox.exe "[shader]"` (both files are tagged `[shader]`; name the new cases `SH40`+ and `SHA36`). Expected: FAIL (`hdr` undeclared; `SCREEN` accepted).
- [ ] **Step 3: Implement.** Add the names to `kReserved`. Add `hdr_`/`set_hdr` beside `transparent_` (same `set_mode` template, `rebuilds = true`), register `lua_saved_property("Hdr", "boolean", ...)`, carry it in `write_place`/`read_place` (a new byte after the margin; a shorter record reads false), reset it in `on_reuse`. In `prepare_source`, when `kind_ == ShaderKind::Post` and `peek.header_modes` is set, `set_hdr(contains "hdr")` instead of the three surface setters. Doc the property in `LuaApi.cpp` beside `Transparent`.
- [ ] **Step 4:** Run the same filter. Expected: PASS.
- [ ] **Step 5: Commit:** `Shaders: a post Shader's Hdr property, and the post names are reserved`.

---

### Task 2: The ShaderEffect instance

**Files:**
- Create: `src/engine_instances/ShaderEffect.hpp`, `src/engine_instances/ShaderEffect.cpp`
- Modify: `src/engine_core/Containment.cpp` (the Lighting-only list at line ~149), `src/engine_core/Project.cpp` (factory table, line ~282), `src/engine_core/ScriptBindings.cpp` (`register_lua_creatable`), `src/engine_core/LuaApi.cpp` (class docs), `CMakeLists.txt` (engine_instances sources, `sandbox/shader_effect_tests.cpp`), `src/ide/ClassOrder.hpp` (cluster 7 with the effects), `tests/LuauCompleteTest.cpp` (the insert-list expectation: `"ShaderEffect"` after `"ScreenSpaceReflections"`)
- Test: `sandbox/shader_effect_tests.cpp` (new; modeled on `sandbox/bloom_tests.cpp` BLM1, BLM2, BLM4)

**Interfaces:**
- Consumes: `ReferenceAsset` (`reference_specs`, `refuse_target`, `read_reference<0>`/`write_reference<0>`), `BloomEffect`'s registration shape (`lua_saved_property`, `register_lua_class("…", "Instance", …)`, `register_suited_parents`), `Shader::kind()`.
- Produces:

```cpp
namespace engine_core {
// A post Shader drawn over the finished frame, under Lighting. Every enabled
// one runs, Order low to high, equal Orders in tree order.
class ShaderEffect : public ReferenceAsset {
public:
    static constexpr std::size_t kShaderReference = 0;
    static constexpr bool kDefaultEnabled = true;
    static constexpr double kDefaultOrder = 0.0;
    const char* class_name() const override { return "ShaderEffect"; }
    bool enabled() const; double order() const;
    std::optional<std::string> set_enabled(bool value);
    std::optional<std::string> set_order(double value);   // "Order must be a finite number"
    // The live post Shader it draws with, or null (none, gone, or not Post).
    const Shader* shader(const DataModel& world) const;
protected:
    const ReferenceSpec* reference_specs(std::size_t& count) const override;  // {"Shader", "Shader"}
    std::optional<std::string> refuse_target(std::size_t index, const DataModel& target) const override;
};
}
```

- [ ] **Step 1: Write the failing tests** (`SFX1`..`SFX4`): `Enabled`/`Order` default, refuse a non-finite Order, record undo (one step each), save and load, come back at Stop; a ShaderEffect under Workspace is refused `"A ShaderEffect must be in Lighting"` and under a Folder under Lighting is allowed; `set_reference(kShaderReference, surfaceShader)` is refused `"Look is a surface shader; a ShaderEffect takes a post shader"` and a post Shader is accepted, `shader(world)` returning it; setting that Shader's Kind to Surface makes `shader(world)` null while the reference is kept; a script `Instance.new("ShaderEffect")` with `.Shader`, `.Order = 2`, `:SetAttribute("Strength", 0.8)`, `.Parent = game.Lighting`.
- [ ] **Step 2:** Run: `./build/Release/sandbox.exe "[shader-effect]"`. Expected: FAIL to compile (no class).
- [ ] **Step 3: Implement** the class and every registration listed under Files. `register_suited_parents("ShaderEffect", {"Lighting"})`. The Lua class doc: "A post Shader drawn over the finished frame, in the Scene View and the player. Its uniforms are its attributes."
- [ ] **Step 4:** Run `"[shader-effect]"`, then `./build/Release/engine-tests.exe` (the insert list; its other failure is pre-existing). Expected: PASS; the insert list check passes.
- [ ] **Step 5: Commit:** `ShaderEffect: a post Shader's home under Lighting`.

---

### Task 3: A holder-generic binding, cached by holder

**Files:**
- Modify: `src/engine_core/SurfaceBinding.hpp`, `src/engine_core/SurfaceBinding.cpp`
- Test: `sandbox/shader_tests.cpp`

**Interfaces:**
- Consumes: `make_surface_binding(world, material)` (its body), `SurfaceBindingCache::get(world, const Material*)`, `ShaderEffect::shader(world)`, `DataModel::attribute(name)`.
- Produces:

```cpp
// holder's binding of shader: its attributes as the uniforms' values, held to
// type and range, and its sampler Textures. The caller chose shader (a
// Material's surface Shader, a ShaderEffect's post Shader).
std::shared_ptr<const SurfaceBinding> make_shader_binding(const DataModel& world, const DataModel& holder, const Shader& shader);
// A ShaderEffect's binding, or null when it has no live post Shader.
std::shared_ptr<const SurfaceBinding> make_effect_binding(const DataModel& world, const ShaderEffect& effect);
// make_default_surface_binding now also serves a post Shader (its Kind is in parse->kind).
std::shared_ptr<const SurfaceBinding> make_default_surface_binding(const Shader& shader);
class SurfaceBindingCache {
    // One per holder id this frame; kept from last frame while the same Shader,
    // version, and textures, its values swapped.
    std::shared_ptr<const SurfaceBinding> get(const DataModel& world, const Material* material);
    std::shared_ptr<const SurfaceBinding> get(const DataModel& world, const ShaderEffect* effect);
};
```

- [ ] **Step 1: Write the failing tests** (`SHA37`): a ShaderEffect with a post Shader declaring `uniform float Strength : range(0, 1) = 0.5;` and `uniform sampler2D Lut : color;`: `make_effect_binding` gives `parse->kind == Post`, `values()->slots[0].number == 0.5` unset, `0.8` after `SetAttribute("Strength", 0.8)`, `1.0` after `1.7` (held to range), and the Texture's path in `samplers[0]` after `SetAttribute("Lut", texture)`; `make_default_surface_binding(postShader)` is non-null with the defaults; a `SurfaceBindingCache` gives the same pointer two frames running for an unchanged effect and a new one when its Shader's text changes.
- [ ] **Step 2:** Run: `./build/Release/sandbox.exe "SHA37*"`. Expected: FAIL to compile.
- [ ] **Step 3: Implement.** Move `make_surface_binding`'s body into `make_shader_binding` (holder = the Material, shader = `material.shader(world)`); `make_surface_binding` and `make_effect_binding` call it. The surface-only fields (`modes`, `displacement_margin`, `has_vertex`) are filled only when `shader.kind() == Surface`. `make_default_surface_binding` drops its Post early return. The cache's two `get`s share one private `get(world, holder_id, make)`.
- [ ] **Step 4:** Run `"[shader]"`. Expected: PASS, with SHA16..SHA35 unchanged.
- [ ] **Step 5: Commit:** `Shaders: a binding for any holder of a Shader, a ShaderEffect's too`.

---

### Task 4: The snapshot carries the effects

**Files:**
- Modify: `src/engine_core/SnapshotPump.hpp` (`VisualShaderEffect`, `VisualSnapshot::effects`), `src/engine_core/SnapshotPump.cpp` (`resolve_lighting`, `blit`), `src/runner/SceneFeed.cpp` (copy `effects`)
- Test: `sandbox/shader_effect_tests.cpp` (`SFX5`), `tests/SceneFeedTest.cpp`

**Interfaces:**
- Consumes: `SurfaceBindingCache::get(world, const ShaderEffect*)`, `find_first_of<T>` (replace with a walk collecting every ShaderEffect under Lighting through Folders, in tree order), `VisualSnapshot`.
- Produces:

```cpp
// Every ShaderEffect under Lighting with a live post Shader, by Order then
// tree order, as the renderer runs them. Enabled false ones ride along so a
// toggle is one snapshot field, not a list change.
struct VisualShaderEffect {
    InstanceId effect = 0;
    bool enabled = true;
    double order = 0.0;
    // Kept across frames while nothing but its values changed (SurfaceBindingCache).
    std::shared_ptr<const SurfaceBinding> binding;
};
std::vector<VisualShaderEffect> effects;   // on VisualSnapshot, beside bloom
```

- [ ] **Step 1: Write the failing tests.** SFX5: a `SnapshotPump` over a game with three ShaderEffects under Lighting (Orders 2, 0, 0; the last two in that tree order; one with Enabled false; one with a surface Shader) snapshots `effects` of two rows in Order (0 first, then its tree sibling, then 2), the surface one left out, the disabled one present with `enabled == false`; a second snapshot with nothing changed keeps the same `binding` pointers. SceneFeedTest: `Frame()` fills one effect row; `Whole()` checks it rides through the feed.
- [ ] **Step 2:** Run: `./build/Release/sandbox.exe "SFX5*"` and `./build/Release/engine-tests.exe` (it holds SceneFeedTest). Expected: FAIL to compile.
- [ ] **Step 3: Implement.** In `resolve_lighting`, after bloom: walk Lighting's subtree (Folders included), collect `ShaderEffect`s, build each row with `surfaces_.get(game, effect)`, drop rows with a null binding, `std::stable_sort` by `order`. `blit` and `SceneFeed` copy the vector.
- [ ] **Step 4:** Run both. Expected: PASS.
- [ ] **Step 5: Commit:** `Snapshot: the ShaderEffects under Lighting, in Order`.

---

### Task 5: The post program: template, build, and errors

**Files:**
- Create: `resources/shaders/pipeline/post.frag`, `resources/shaders/pipeline/post_shader.glsl`
- Modify: `src/runner/ShaderAssembly.hpp`, `src/runner/ShaderAssembly.cpp` (`AssemblePost`), `src/runner/SurfacePrograms.hpp`, `src/runner/SurfacePrograms.cpp` (`Built::post`, `build` branching on kind, `FindParams(parse, variant, first_unit)`), `src/runner/RenderProgram.hpp` (if a new location is needed), `src/runner/Renderer.cpp` (`finishProgram` finds `uScreen`, `uTexel`, `uInverseProjection` for post programs; it already finds `uTime`, `uScreenSize`, `uView`, `uProjection`, `uInverseView`)
- Test: `tests/SceneRenderCheck.cpp` (a `PostFor` helper beside `SurfaceFor`; the checks below run through Task 6's chain, so this task's own check is the build: a program links, a bad one reports its line)

**Interfaces:**
- Consumes: `AssembleSurface`'s shape, `LinkProgramLog`, `MapShaderError`, `SurfacePrograms::link`.
- Produces:

```cpp
// main_file post.frag's #version line, "#define POST_SHADER", gbuffer.glsl and
// lighting.glsl's viewPositionAt, post_shader.glsl, user_glsl, then the rest of post.frag.
AssembledSource AssemblePost(const std::string& user_glsl);
struct SurfacePrograms::Built { /* ... */ Variant post; };   // built for a Post binding; gbuffer/forward/shadow stay empty
constexpr int kPostTextureUnit = 12;                          // user samplers 12..15
```

`post_shader.glsl` (no `#version`), the contract in one place:

```glsl
// A Shader asset's post(), run once per pixel of the stage's image.
vec4 COLOR;                       // in: the pixel; out: the result. Premultiplied.
uniform sampler2D uScreen;        // the stage's whole image
uniform sampler2D uDepth; uniform sampler2D uNormal; uniform sampler2D uAlbedo; uniform sampler2D uMaterial;
uniform vec2 uScreenSize; uniform float uTime;
uniform mat4 uView; uniform mat4 uProjection; uniform mat4 uInverseView; uniform mat4 uInverseProjection;
#define SCREEN uScreen
#define DEPTH uDepth
#define NORMALS uNormal
#define ALBEDO uAlbedo
#define MATERIAL uMaterial
#define SCREEN_SIZE uScreenSize
#define TEXEL (1.0 / uScreenSize)
#define TIME uTime
#define VIEW uView
#define PROJECTION uProjection
#define INV_VIEW uInverseView
#define INV_PROJECTION uInverseProjection
vec2 SCREEN_UV;
vec3 CAMERA_POSITION;
float depthAt(vec2 uv);          // -viewPositionAt(uv, raw).z; 1e9 where raw == 1.0
vec3 viewPositionAt(vec2 uv);    // lighting.glsl's viewPositionAt(uv, texture(uDepth, uv).r)
vec3 worldPositionAt(vec2 uv);   // (uInverseView * vec4(viewPositionAt(uv), 1.0)).xyz
vec3 normalAt(vec2 uv);          // decodeNormal(texture(uNormal, uv))
void post();
// post.frag's main: COLOR = texture(uScreen, vUv); SCREEN_UV = vUv; CAMERA_POSITION = uInverseView[3].xyz;
// post(); fragColor = vec4(max(COLOR.rgb, 0.0), clamp(COLOR.a, 0.0, 1.0));
```

- [ ] **Step 1: Write the failing check.** In `SceneRenderCheck.cpp`, beside `SurfaceFor`, add `PostFor(id, version, source, values = {})` (parses under `ShaderKind::Post`, `describe_surface`, `set_values`). In the surface-shader section add: `programs.get(*PostFor(9301, 1, "shader post;\nvoid post() {\n    COLOR.rgb = 1.0 - COLOR.rgb;\n}\n"), now).post.program.id != 0` ("post shader: a post program builds"); `PostFor(9302, 1, "shader post;\nvoid post() {\n    COLOR.rgb = ;\n}\n")` builds nothing and `take_errors()` holds `(9302, 1, Fragment, "3: …")` (line 3 or 4; the driver may report one late, as the surface check allows).
- [ ] **Step 2:** Build and run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target scene-render-check && ./build/Release/scene-render-check.exe`. Expected: FAIL ("a post program builds").
- [ ] **Step 3: Implement.** `AssemblePost`; `SurfacePrograms::get` no longer returns `kNone` for Post; `build` branches: Post links `fullscreen.vert` + `AssemblePost(glsl)` into `built.post` with `FindParams(parse, variant, kPostTextureUnit)`; Surface keeps today's path with `kSurfaceTextureUnit`. `destroy` covers `post`. `finishProgram` looks up `uScreen` (set to `kUnitScene`), `uDepth`/`uNormal`/`uAlbedo`/`uMaterial` (set to their units, as `bindGBuffer` binds them), and `uInverseProjection` on any program that has them.
- [ ] **Step 4:** Run the check. Expected: PASS, no GL error.
- [ ] **Step 5: Commit:** `Shaders: a post Shader builds into a post program`.

---

### Task 6: The post chain in the renderer

**Files:**
- Modify: `src/runner/Renderer.hpp` (`SceneEffect`, `SceneLighting::effects`, `postPass`, `blitPass`, the second targets in `ensureTargets`/`TargetSet`/park/unpark/destroy), `src/runner/Renderer.cpp`, `src/runner/SceneDraws.hpp/.cpp` (`ResolveShaderTextures(binding, textures, unsigned* out, count)` shared with `ApplySurface`), `src/runner/GameView.cpp` (`collectMeshes`, where `SceneLighting` is filled from the snapshot and `renderer_.setLighting(lighting)` is called at line ~557: fill `lighting.effects` from `snapshot.effects`)
- Create: `resources/shaders/pipeline/blit.frag` (`fragColor = texture(uScene, vUv)`, with `uKeepAlpha` as fxaa.frag has it)
- Test: `tests/SceneRenderCheck.cpp`

**Interfaces:**
- Consumes: `SurfacePrograms::Built::post`, `bindSurface`'s uniform upload (factor the per-param upload out of `bindSurface` into `uploadParams(variant, binding, const unsigned* textures, int first_unit)` so both passes share it), `toneMapPass`, `fxaaPass`, `mergeTexture_`, `ldrTexture_`, `ensureTargets`.
- Produces:

```cpp
// One ShaderEffect this frame, as the renderer runs it.
struct SceneEffect {
    engine_core::InstanceId effect = 0;
    bool enabled = true;
    bool hdr = false;                                     // from binding->hdr (Task 3 fills it from Shader::hdr())
    std::shared_ptr<const engine_core::SurfaceBinding> binding;
    unsigned textures[4] = {0, 0, 0, 0};                  // the user samplers, loaded by GameView
};
std::vector<SceneEffect> effects;   // on SceneLighting, in Order
// Renderer: runs every enabled effect of stage hdr over `image` (mergeTexture_ or
// ldrTexture_), ping-ponging with the stage's second target; returns the texture
// holding the result (image itself when none ran). Never fails the frame.
unsigned postPass(bool hdr, unsigned image, int width, int height);
// image onto whatever is bound (the pane), as fxaaPass draws ldrTexture_.
bool blitPass(unsigned image);
```

Note: `SurfaceBinding` gains `bool hdr = false;` in this task (Task 3's `make_shader_binding` sets it from `shader.hdr()` for a post Shader; `same_binding` compares it).

- [ ] **Step 1: Write the failing checks** in the bloom block's style (draw twice, read the second): with `hardEdges` lighting (FXAA off) and one LDR effect `COLOR.rgb = 1.0 - COLOR.rgb;`, the middle pixel is the plain frame's inverse (each channel within 3 of `255 - plain`); with FXAA on, the same; a disabled effect draws the plain frame exactly; two effects, `Order` 0 `COLOR.rgb *= 0.5` and `Order` 1 `COLOR.rgb += 0.25`, give `plain * 0.5 + 0.25`, and swapped Orders give `(plain + 0.25) * 0.5` (so Order is honored); an `Hdr` effect `COLOR.rgb = vec3(0.0);` on the bright emissive cube from the bloom block leaves no halo past its edge ("an hdr effect runs before bloom"); an LDR effect that reads `depthAt(SCREEN_UV)` and writes `COLOR.rgb = vec3(d < 1e8 ? 1.0 : 0.0)` is white on the cube and black on the sky; an effect whose program failed draws the plain frame; after `renderer.draw` at a new size, an effect still draws and `glGetError() == GL_NO_ERROR`; a transparent pane (`setTransparentOutput(true)` as the existing transparent check does) with an LDR effect keeps alpha 0 where nothing was drawn.
- [ ] **Step 2:** Build and run the check. Expected: FAIL ("inverts the pane").
- [ ] **Step 3: Implement.** `ensureTargets` makes `mergePingTexture_` (RGBA16F, linear) and `ldrPingTexture_` (RGBA8, linear) with FBOs, parked/unparked/destroyed with the rest. In `draw`: after `mergePass`, `const unsigned merged = postPass(true, mergeTexture_, w, h)`; bloom and the tone map read `merged` (pass it into `bloomPass`/`toneMapPass` in place of `mergeTexture_`). Then: when any enabled LDR effect has a program, the tone map always goes to `ldrFbo_` (as under FXAA today), `const unsigned shown = postPass(false, ldrTexture_, w, h)`, and the pane gets `fxaaPass(shown)` under FXAA or `blitPass(shown)` without; with no LDR effect today's branches stand. `postPass` for each effect: `surfacePrograms_.get(*binding, shaderTime())`, skip if `post.program.id == 0`; bind the ping FBO, `glUseProgram`, `BindTexture(kUnitScene, current)`, `bindGBuffer(program)`, the matrices and time as `useGeometryProgram` sets them plus `uInverseProjection`, `uploadParams(post, *binding, effect.textures, kPostTextureUnit)`, `DrawFullscreen`, swap. `GameView::collectMeshes` fills `lighting.effects` from `snapshot.effects` with `ResolveShaderTextures` before `setLighting`. `postSurfaceErrors` already carries post errors.
- [ ] **Step 4:** Run the check twice. Expected: PASS both (the GPU-timer check may flake once).
- [ ] **Step 5: Commit:** `Renderer: ShaderEffects run before bloom or after the tone map`.

---

### Task 7: Properties rows, the Shader Editor, and docs

**Files:**
- Modify: `src/ide/PropertySheet.cpp` (lines ~638-660 and ~877: the uniform rows for any selection of holders sharing one Shader, a `Material` or a `ShaderEffect`), `src/ide/IdeShaderEditor.cpp` (`read()`'s users walk Lighting for ShaderEffects naming the Shader, tagged "ShaderEffect"; the render modes card shows one `Hdr` toggle for a post Shader; the preview passes the post binding as `MaterialLook::effect`), `src/ide/IdeShaderEditor.hpp` (`modeToggle("Hdr")`), `src/ide/MaterialPreviews.hpp` (`MaterialLook::effect`, compared like `surface`), `src/ide/MaterialBall.cpp` (`render` puts `look.effect` into its `SceneLighting::effects`), `src/ide/IdeIcons.cpp` (`ShaderEffect` → `Shader.png`), `README.md` (a `ShaderEffect` paragraph after `BloomEffect`'s at line ~45; the Shaders paragraph's "Post shaders draw nothing yet" sentence replaced), `src/engine_instances/README.md` (ShaderEffect beside BloomEffect), `src/engine_core/README.md` (line ~34: the binding paragraph gains ShaderEffects and the post chain)
- Test: `tests/PropertiesTest.cpp` (the `properties-tests` harness; it pins the Material uniform rows), `tests/ShaderEditorPaneTest.cpp`

**Interfaces:**
- Consumes: `ShaderEffect::shader(world)`, `make_default_surface_binding` (Task 3), `SceneLighting::effects` (Task 6), `ShaderEditorHost::set_mode` (property `"Hdr"`).

- [ ] **Step 1: Write the failing tests.** Properties: a selected ShaderEffect with a post Shader shows the Shader's uniform rows (group, default dimmed, writing sets the attribute) as a Material does; a Material and a ShaderEffect selected together show none. Pane test (`Rig` gains a post Shader "Fade" and a ShaderEffect "Vignette" under Lighting using it): `usedByNames()` on Fade's editor is `{"Vignette"}` with its row tagged ShaderEffect, clicking it calls `host.select(effect)`; `modeToggle("Hdr")` exists for Fade and none of the three surface toggles; `modeToggle("Hdr")` is null for the surface Shader Wave; clicking Hdr calls `set_mode(fade, "Hdr", true)`; `previewDue` is true for a post Shader after it opens.
- [ ] **Step 2:** Run: `./build/Release/properties-tests.exe` and `./build/Release/shader-editor-tests.exe`. Expected: FAIL.
- [ ] **Step 3: Implement** the four UI changes and write the docs. The README paragraph: what a ShaderEffect is, where it lives and its refusal text, `Enabled`, `Shader` (post only, with the refusal text), `Order`, that uniforms are attributes, `Hdr`, the built-ins and the four functions, that it runs in the Scene View and the player and not in ViewportPanes or material balls.
- [ ] **Step 4:** Run both harnesses and `./build/Release/studio-tests.exe`. Expected: PASS.
- [ ] **Step 5: Commit:** `Studio: a ShaderEffect's uniforms in Properties, and the Shader Editor knows post Shaders`.

---

### Task 8: Samples and the Ocean's effects

**Files:**
- Create: `samples/post/Vignette.glsl` (the spec's vignette, `Strength`, `Radius`), `samples/post/DepthFog.glsl` (`Hdr`; `uniform vec3 FogColor : color`, `uniform float Density : range(0, 0.1) = 0.01`, `uniform float Start : range(0, 500) = 20`; `COLOR.rgb = mix(COLOR.rgb, FogColor * COLOR.a, 1.0 - exp(-max(depthAt(SCREEN_UV) - Start, 0.0) * Density))`, the sky left alone)
- Modify: `sandbox/ocean_demo.cpp` (two Shaders from the files, DepthFog with `Hdr` true; two ShaderEffects under Lighting, Fog `Order` 0, Vignette `Order` 1, the fog tinted to the sunset)
- Test: the generator itself (`[.ocean-demo]`), and `shader_effect_tests.cpp` `SFX6`: both sample files parse as post Shaders with no error.

- [ ] **Step 1:** Write SFX6; run `"SFX6*"`. Expected: FAIL (files missing).
- [ ] **Step 2:** Write the two files and the generator changes.
- [ ] **Step 3:** Run `"SFX6*"`, then regenerate: `OCEAN_DEMO="C:\Users\Andrew\Documents\Anarchy Engine Projects\Ocean" ./build/Release/sandbox.exe "[.ocean-demo]"` (delete the folder first). Open it in the Release studio: both effects build with no Problems row, the fog reads at the horizon, the vignette at the corners; tune `Density`/`Start` by eye; screenshots to the user, opened with Start-Process.
- [ ] **Step 4: Commit:** `Samples: Vignette and DepthFog post Shaders, on the Ocean`.

---

## Self-review notes

- **Spec coverage:** ShaderEffect placement, properties, Order (Task 2, 4); uniforms as attributes (Task 3, 7); the post built-ins and G-buffer functions (Task 5); `hdr` vs LDR insertion points, ping-pong targets, skip rules, Scene View and player only (Task 6); Properties, Explorer icon, Shader Editor (Task 7); errors with a line to the Shader (Task 5, through the existing route). The spec's "a post shader has one variant" holds: `Built::post`.
- **Deviations from the spec, by the Shader Editor spec's changes:** `hdr` is the property `Hdr`, not a `render_mode` word (the line still sets it on write); "Used by" lists ShaderEffects too; the Shader Editor preview runs the effect over the ball.
- **Type consistency:** `SurfaceBinding` is the one binding type (surface and post), `SceneEffect::binding` holds it, `SurfacePrograms::get` takes it, `uploadParams` takes `(variant, binding, textures, first_unit)` in both passes.
- **Review Focus:** each line names its task.
- **Proportion:** the plan is shorter than the spec sections it implements; code blocks are signatures and one template contract.
