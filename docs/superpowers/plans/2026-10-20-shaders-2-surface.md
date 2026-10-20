# Custom Shaders, Phase 2: Surface Shaders on Materials

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Material takes a surface Shader; its uniforms appear in the Material's Properties as rows backed by attributes; every mesh, brush face, and material ball using the Material draws through the shader in the G-buffer, see-through, and shadow passes; and a GLSL compile error shows on the Shader's Error at the user's line.

**Architecture:** The engine's own `deferred.frag`, `forward.frag`, and `shadow.frag` gain one `#ifdef SURFACE_SHADER` hook each. A pure assembler builds a custom program's fragment text from the main file, the libraries, a new `surface_shader.glsl` (the uppercase globals and the hook), and the parse's `glsl`, and maps driver log lines back to the user's. The snapshot carries one shared `SurfaceBinding` per Material per frame (shader id, version, GLSL, and the resolved values and texture paths), copied by pointer into each `VisualMesh` and `MeshDraw`. The Renderer keeps a program cache keyed by Shader id and version, binds a run's program and values in place of the stock ones, and reports compile errors back through the simulation thread.

**Tech Stack:** C++17, OpenGL 3.3 core (GLSL 330), the hand-written loader in `src/runner/gl.*`, Catch2 (sandbox), the custom harnesses (tests/), `scene-render-check` (by hand, GL).

**Spec:** `docs/superpowers/specs/2026-10-20-custom-shaders-design.md`, sections "Surface shaders", "Attachment / Material.Shader", "Attributes / In the Properties panel" and "How a shader reads them", and "Rendering" except "The post chain" (phase 3).

## Global Constraints

- `Material.Shader` is a `Shader?` in the Surface group after the textures; `ReferenceAsset::kMaxReferences` goes from 6 to 8. A post shader is refused with "<Name> is a post shader; a Material takes a surface shader".
- Surface built-ins, writable: `ALBEDO` (vec3), `ALPHA` (float), `NORMAL` (view-space vec3), `METALNESS`, `ROUGHNESS`, `REFLECTIVITY` (float), `EMISSIVE` (vec3). Read-only: `UV`, `COLOR` (vec4), `VIEW_POSITION`, `WORLD_POSITION`, `VIEW_NORMAL`, `WORLD_NORMAL`, `SCREEN_UV`, `FRONT_FACING`, `TIME`, `CAMERA_POSITION`, `VIEW`, `PROJECTION`, `INV_VIEW`, `TEXTURE_DIFFUSE`, `TEXTURE_NORMAL`, `TEXTURE_ROUGHNESS`, `TEXTURE_METALNESS`, `TEXTURE_EMISSIVE`.
- `render_mode transparent` draws in the see-through pass whatever the Transparency, alpha `ALPHA × (1 − Transparency)`; without it `ALPHA` cuts out below 0.25 (G-buffer) and 0.1 (see-through). `two_sided` turns culling off. `no_shadow` casts nothing.
- User textures use units 6, 7, 8 (at most 3 per surface shader); sampling state from the hints (`clamp`, `nearest`) is a sampler object bound to the unit; a `color` sampler loads as `Usage::Color`, `normal` as `Usage::Normal`, otherwise `Usage::Mask`; an unset sampler reads white, or black with `black`.
- A `color` vec3 uploads linear (sRGB decode, as `toLinear` does).
- `TIME` is seconds since the Renderer was made, wrapped every 3600 s.
- A program that fails to build draws the plain Material; its error reaches `Shader.Error` as `"<line>: <message>"` at the user's line; nothing retries until the Shader's `source_version` moves.
- Programs of a Shader not drawn for 60 seconds are deleted.
- Everything stays within GL 3.3 / GLSL 330; nothing beyond GLSL ES 3.00 except sampler objects (core in 3.3).

## Review Focus

- A Material whose Shader is deleted, or set to a post shader by a hand-edited file: it draws plain and the Properties rows disappear, no crash. Pinned in Task 2 and Task 5.
- Two Materials sharing one Shader with different attribute values: each draws its own values; batching never merges their runs. Pinned in Task 5.
- A Shader edited while many objects use it: the program rebuilds once per source version, not once per draw or per frame. Pinned in Task 5.
- An attribute of the wrong kind (a string `Progress`): the row warns, the shader reads the default. Pinned in Task 3 and Task 4.
- A driver error line inside the engine's own code (a user name clashing with a library function): the error still reaches `Shader.Error`, at line 1 when it maps before the user's text. Pinned in Task 1.

---

### Task 1: Assembling a surface program's source and mapping its errors

**Files:**
- Create: `src/runner/ShaderAssembly.hpp`, `src/runner/ShaderAssembly.cpp`, `resources/shaders/pipeline/surface_shader.glsl`
- Modify: `resources/shaders/pipeline/deferred.frag`, `forward.frag`, `shadow.frag`, `shadow.vert` (the `#ifdef SURFACE_SHADER` hooks, below); `src/runner/ShaderFile.hpp/.cpp` (a `LoadShaderPieces` overload that takes extra source text after the libraries); `CMakeLists.txt`
- Test: `sandbox/shader_assembly_tests.cpp`

**Interfaces:**
- Produces, namespace `runner`:

```cpp
// A custom program's fragment (or vertex) text and where the user's lines start in it.
struct AssembledSource {
    std::string text;
    int user_first_line = 0;   // 1-based line in text where the user's glsl begins
    int user_lines = 0;
};
// main's #version line, "#define SURFACE_SHADER", each library, surface_shader.glsl
// (for a fragment), then user_glsl, then the rest of main. Empty text when a file is missing.
AssembledSource AssembleSurface(const char* main_file, std::initializer_list<const char*> libraries,
                                const std::string& user_glsl, bool fragment);
// The first error in a driver's compile log, as "<line>: <message>" in the user's
// lines: NVIDIA "0(12) : error C0000: msg", Mesa/Intel "0:12(3): error: msg",
// Apple and ANGLE "ERROR: 0:12: msg". A line before the user's text reads 1, after it the user's last line.
std::string MapShaderError(const std::string& log, const AssembledSource& source);
```

- The hooks, exactly where each goes:
  - `deferred.frag` `main()`: after `Surface s = readSurface(...)`, before the alpha test: `#ifdef SURFACE_SHADER` / `runSurfaceShader(s, vViewPosition, vViewNormal, vUv, vColor, gl_FrontFacing);` / `#endif`.
  - `forward.frag` `main()`: the same line in the same place.
  - `shadow.frag`: under `#ifdef SURFACE_SHADER`, `in vec2 vUv; in vec4 vColor; in vec3 vWorldPosition;` and in `main` a `Surface` built from `texture(uDiffuse, vUv)` alpha and `vColor`, then `runShadowSurfaceShader(s, vWorldPosition, vUv, vColor);` and `if (s.alpha < 0.25) discard;`.
  - `shadow.vert`: under `#ifdef SURFACE_SHADER` (which also defines `CUTOUT`'s UV), `layout(location = 4) in vec4 aColor; out vec4 vColor; out vec3 vWorldPosition;` set from `aColor` and `aModel * skin() * position`.
- `surface_shader.glsl` declares the globals (`vec3 ALBEDO; float ALPHA; ...`), `uniform float uTime; uniform mat4 uView; uniform mat4 uProjection; uniform mat4 uInverseView; uniform vec2 uScreenSize;`, `#define` aliases (`#define TIME uTime`, `#define VIEW uView`, `#define INV_VIEW uInverseView`, `#define PROJECTION uProjection`, `#define TEXTURE_DIFFUSE uDiffuse`, and the other four), the prototype `void surface();`, and `void runSurfaceShader(inout Surface s, vec3 viewPosition, vec3 viewNormal, vec2 uv, vec4 color, bool frontFacing)`: set the read-only globals (`WORLD_POSITION = (uInverseView * vec4(viewPosition, 1.0)).xyz`, `WORLD_NORMAL = normalize(mat3(uInverseView) * viewNormal)`, `CAMERA_POSITION = uInverseView[3].xyz`, `SCREEN_UV = gl_FragCoord.xy / uScreenSize`), copy `s` into the writable globals, call `surface()`, copy back (`NORMAL` normalized). `runShadowSurfaceShader` sets what the shadow pass has and leaves the rest neutral (zero, false, identity).

- [ ] **Step 1: Write the failing tests** (`[shader-assembly]`): `AssembleSurface("pipeline/deferred.frag", {"pipeline/gbuffer.glsl","pipeline/surface.glsl"}, glsl, true)` starts with `#version 330 core`, has `#define SURFACE_SHADER` on line 2, contains the user's text starting at `user_first_line`, and `runSurfaceShader` appears after `void surface();`; `MapShaderError` maps each of the three log formats for a line inside the user's text to `"<line - user_first_line + 1>: <message>"`, a line before it to `"1: ..."`, a line after it to the user's last line, and an empty log to `""`.
- [ ] **Step 2:** `cmake --build build --config Release --target sandbox`. Expected: compile failure, `ShaderAssembly.hpp` missing.
- [ ] **Step 3:** Implement the assembler, the mapping, `surface_shader.glsl`, and the four hooks.
- [ ] **Step 4:** `build/Release/sandbox.exe "[shader-assembly]"` passes; `scene-render-check` (unchanged checks) still passes, proving the hooks change nothing without the define.
- [ ] **Step 5:** Commit: `Shaders: assemble a surface shader into the engine's passes and map its errors to the user's lines`.

---

### Task 2: Material.Shader

**Files:**
- Modify: `src/engine_instances/AssetInstances.hpp:274` (`kMaxReferences` 8), `:307-312` (`kShaderReference = 6`), `AssetInstances.cpp` (Material's reference specs and `lua_saved_property("Shader", "Shader?", read_reference<6>, write_reference<6>, "null")` after `MetalnessTexture` in the Surface group; the write refuses a post shader), `src/engine_core/LuaApi.cpp` (completion doc for Material.Shader)
- Test: `sandbox/shader_tests.cpp`

**Interfaces:**
- Produces: `const Shader* Material::shader(const DataModel& world) const;` — the live surface Shader the reference names, or null (none, gone, or a post shader read from a file).

- [ ] **Step 1: Write the failing tests** (`SHA13`-`SHA15`): setting `Material.Shader` from Lua to a surface shader sticks and saves and loads by GUID; setting it to a post shader raises "Fx is a post shader; a Material takes a surface shader" and leaves it nil; a hand-written project file pointing a Material at a post shader loads, and `shader(world)` is null; destroying the Shader makes `shader(world)` null and the property read nil.
- [ ] **Step 2:** Run `build/Release/sandbox.exe "[shader]"`. Expected: SHA13-15 fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `[shader]` and `[project]`. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: a Material takes a surface Shader`.

---

### Task 3: The shader's uniforms as Properties rows

**Files:**
- Modify: `src/ide/PropertySheet.hpp` (`PropertyRow::defaulted`, `PropertyRow::mismatched`, `PropertyEdit::shader_param`), `src/ide/PropertySheet.cpp` (`read_sheet` and `write_edit`), `src/ide/PropertiesPanel.cpp` (dimmed defaulted rows with a Reset button that removes the attribute; a warning style on mismatched rows)
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Consumes: Task 2's `Material::shader`; phase 1's `ShaderParse::params`; the attributes API.
- Produces: for a sheet whose every instance is a Material with the same live Shader, one row per non-hidden parameter, in declaration order, with `attribute = true`, `group` = the parameter's group or else the Shader's Name, placed before the Attributes group; `kind` from the type (Float/Int → Number with `slider_min/max` when ranged; Bool; Vec2; Vec3; Color → Color3; Sampler → Ref typed `Texture?`); `value` from the attribute when its kind fits, else from `fallback` with `defaulted = true` (and `mismatched = true` when an attribute of another kind exists). Attributes a parameter covers are left out of the plain Attributes group. `PropertyEdit::shader_param = true` lets `write_edit` create the attribute when absent, clamps a ranged number to its range (an Int truncates), and accepts only a Texture for a sampler.

- [ ] **Step 1: Write the failing tests** (`TestShaderParamRows`): a Material holding the Dissolve shader shows rows Progress (slider 0..1, defaulted, value 0), EdgeColor (Color3, defaulted (1, 0.5, 0.1)), Noise (Ref to Texture, nil) under a "Dissolve" group, before Attributes, and no hidden parameter; typing 2 into Progress writes the attribute 1 (clamped) as one "Set Progress" step and the row is no longer defaulted; a string attribute named Progress makes the row mismatched with the default value and leaves the Attributes group without Progress; clearing the Shader removes the rows; Reset on Progress removes the attribute.
- [ ] **Step 2:** Run `build/Release/properties-tests.exe`. Expected: those checks fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `properties-tests` and `mcp-tests`. Expected: pass.
- [ ] **Step 5:** Commit: `Properties: a Material's shader uniforms are rows backed by its attributes`.

---

### Task 4: The surface binding in the snapshot and the draws

**Files:**
- Create: `src/engine_core/SurfaceBinding.hpp`, `src/engine_core/SurfaceBinding.cpp`
- Modify: `src/engine_core/SnapshotPump.hpp:215-255` (`VisualMesh::surface`), `SnapshotPump.cpp` (`fill_visual_material` fills it from a per-Prepare cache keyed by Material id), `src/engine_core/BrushVisuals.cpp` (the same for brush faces), `src/runner/Renderer.hpp:53-149` (`MeshDraw::surface`, and `SameDraw` compares it by pointer), `src/runner/SceneDraws.cpp` (`ApplyMaterial` and the brush path copy it and resolve its textures)
- Test: `sandbox/shader_tests.cpp`

**Interfaces:**
- Produces, namespace `engine_core`:

```cpp
// What a Material's surface Shader needs to draw, resolved once per Prepare and
// shared by every mesh that draws the Material. Immutable once built.
struct SurfaceBinding {
    InstanceId shader = 0;
    std::uint64_t version = 0;              // the Shader's source_version
    std::shared_ptr<const ShaderParse> parse;
    std::vector<LuaSlot> values;            // one per parse->params, in order; a sampler's is Nil
    struct Sampler { std::string path; bool flip_y = false; bool always_loaded = false; };
    std::vector<Sampler> samplers;          // one per sampler param, in order; empty path for none
    bool reads_time = false;                // the glsl names TIME, so shadows redraw each frame
    std::uint64_t values_hash = 0;          // of values and sampler paths, for shadow fingerprints
};
// The binding for material, or null when it has no live surface Shader.
std::shared_ptr<const SurfaceBinding> make_surface_binding(const DataModel& world, const Material& material);
```

  Values follow the spec: the attribute when its kind fits the type, else the fallback; ranged numbers clamped; an Int truncated; a `color` vec3 stays a Color slot (linearised at upload). `MeshDraw` gains `std::shared_ptr<const engine_core::SurfaceBinding> surface;` and `unsigned surfaceTextures[3] = {0, 0, 0};` filled by `ApplyMaterial` through `TextureCache::get` with the Usage the hints say.

- [ ] **Step 1: Write the failing tests** (`SHA16`-`SHA18`): `make_surface_binding` for a Material with the Dissolve shader and `Progress = 0.3` gives values 0.3, the EdgeColor default, Nil, and one sampler with an empty path; with `Progress` a string gives 0; with `Progress = 5` gives 1; two Materials on one Shader give two bindings with different values; a snapshot `VisualMesh` of a Prefab using the Material carries the same binding pointer for every Model sharing the Material within one Prepare; a Material without a Shader carries null.
- [ ] **Step 2:** Run `[shader]`. Expected: compile failure, then failures.
- [ ] **Step 3:** Implement, including the per-Prepare cache (cleared at the start of each `take_changes`).
- [ ] **Step 4:** Run the whole sandbox. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: the snapshot carries each Material's surface binding to its draws`.

---

### Task 5: Drawing opaque surfaces through the shader

**Files:**
- Create: `src/runner/SurfacePrograms.hpp`, `src/runner/SurfacePrograms.cpp` (the cache)
- Modify: `src/runner/gl.hpp/.cpp` (`glGenSamplers`, `glDeleteSamplers`, `glBindSampler`, `glSamplerParameteri`), `src/runner/Renderer.hpp/.cpp` (own a `SurfacePrograms`; `geometryPass` binds a run's custom program at :2563-2564 and the cards' at :2667-2668; per-program view/projection/`uInverseView`/`uTime`/`uScreenSize`; `bindSurface(program, draw)` uploads the values by location and binds `surfaceTextures` to units 6-8 with their sampler objects; per-program `CanDraw` flags), `src/runner/DrawBatches.hpp/.cpp` and `src/runner/Visibility.hpp` (`DrawItem::program`, the Shader id, sorted after `terrain` and before `key`, and joined runs require equal programs), `scene-render-check` (`tests/SceneRenderCheck.cpp`)

**Interfaces:**
- Consumes: Task 1's `AssembleSurface`/`MapShaderError`; Task 4's `MeshDraw::surface`.
- Produces:

```cpp
// Surface programs by Shader id; each built from one source_version.
class SurfacePrograms {
public:
    struct Built {
        unsigned gbuffer = 0, forward = 0, shadow = 0;   // 0 when not built or failed
        std::vector<int> gbufferLocations, forwardLocations, shadowLocations;   // per param
        std::string error;                               // "" or "<line>: <message>"
    };
    // The programs for binding, built on first use or when its version moved.
    const Built& get(const engine_core::SurfaceBinding& binding);
    // Errors found since the last call, as (shader id, version, text).
    std::vector<std::tuple<engine_core::InstanceId, std::uint64_t, std::string>> take_errors();
    // Deletes programs of shaders not asked for in the last 60 seconds.
    void reclaim(double now);
    void release();   // deletes every GL object
};
```

- [ ] **Step 1: Write the failing checks** in `tests/SceneRenderCheck.cpp`: a white cube whose `MeshDraw::surface` is a binding with `ALBEDO = vec3(1, 0, 0);` draws red; with `if (UV.x < 0.5) discard;` half its pixels show the clear color; a binding whose GLSL has a syntax error draws white and `take_errors()` returns its line; two draws of one mesh with two bindings of one Shader (`ALBEDO = Tint;` with red and blue) draw red and blue; drawing 200 cubes on one binding builds the program once (a `SurfacePrograms::builds()` counter for tests); a binding whose Shader id has no live parse (null `parse`) draws plain.
- [ ] **Step 2:** `cmake --build build --config Release --target scene-render-check` then `build/Release/scene-render-check.exe` from the repository root. Expected: the new checks fail.
- [ ] **Step 3:** Implement the loader functions, the cache, the pass changes, and the batching key.
- [ ] **Step 4:** Run `scene-render-check` and the sandbox `[draw-batches]` tests. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: opaque surfaces draw through their Material's shader`.

---

### Task 6: See-through surfaces and render modes

**Files:**
- Modify: `src/runner/Renderer.cpp` (`transparencyPass` binds a draw's forward program at :3073-3074 with the pass's per-pass uniforms uploaded to it once; `findVisible` treats `render_mode transparent` as see-through and `two_sided` as no culling), `src/runner/Visibility.cpp` (`transparency > 0` or the binding's `transparent` mode), `tests/SceneRenderCheck.cpp`

- [ ] **Step 1: Write the failing checks:** a cube with `render_mode transparent;` and `ALPHA = 0.5;` blends over a red background (pixel between the cube's color and red); a quad with `render_mode two_sided;` seen from behind draws; a see-through Material (Transparency 0.5) whose shader sets `ALBEDO` red draws red-tinted.
- [ ] **Step 2:** Run `scene-render-check`. Expected: those fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `scene-render-check`. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: see-through surfaces and render_mode transparent and two_sided`.

---

### Task 7: Shadows that follow the shader

**Files:**
- Modify: `src/runner/ShadowRenderer.hpp/.cpp` (`CasterRun::surface`; `addCasterRuns` groups by shader program after cutout; `drawCasters` binds the shadow program and `bindSurface`'s values and textures; casters with `no_shadow` are skipped), `src/runner/ShadowPlanner.hpp/.cpp` (`ShadowCaster::surface`: the Shader id, version, and `values_hash`, hashed; a binding with `reads_time` adds the frame counter), `tests/SceneRenderCheck.cpp`

- [ ] **Step 1: Write the failing checks:** a floor under a cube whose shader discards `UV.x < 0.5` shows light through the discarded half of the shadow; `render_mode no_shadow;` casts none; changing the binding's values (a new pointer with another `values_hash`) redraws a cached cascade; a shader without `discard` casts with the plain depth program (the shadow program count stays 0).
- [ ] **Step 2:** Run `scene-render-check`. Expected: those fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `scene-render-check` and sandbox `[shadow]`. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: shadows follow a shader's holes`.

---

### Task 8: Compile errors on Shader.Error

**Files:**
- Modify: `src/engine_instances/Shader.hpp/.cpp` (`set_compile_error(std::uint64_t version, std::string error)`: kept only for the current version, no history, `emit_property("Error")`; `error()` returns the parse error, else the compile error; a new source clears it), `src/runner/GameView.cpp` (after a draw, `renderer_.takeSurfaceErrors()` posts each through `engine_->on_simulation` as `reportViewportSize` does), `src/runner/Renderer.hpp/.cpp` (`takeSurfaceErrors()` forwarding the cache's)
- Test: `sandbox/shader_tests.cpp` (`SHA19`), `tests/ShaderEditorTest.cpp`

- [ ] **Step 1: Write the failing tests:** `SHA19`: `set_compile_error(version, "3: x")` on the current version sets Error and fires Changed once; on an old version does nothing; a new Source clears it. Studio: a Material using a Shader whose GLSL calls an undefined function, drawn once by the Scene View, gives the Shader an Error at that line, and the shader editor marks it.
- [ ] **Step 2:** Run the sandbox and studio tests. Expected: those fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run both suites. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: a compile error shows on the Shader's Error at its line`.

---

### Task 9: The material ball shows the shader

**Files:**
- Modify: `src/ide/MaterialPreviews.hpp/.cpp` (`MaterialLook::surface`, from `make_surface_binding`; `operator==` compares the Shader id, version, and `values_hash`), `src/ide/MaterialBall.cpp` (sets `ball.surface` and resolves its textures as `ApplyMaterial` does), `tests/SceneRenderCheck.cpp`

- [ ] **Step 1: Write the failing check:** a MaterialBall for a white Material whose shader sets `ALBEDO = vec3(1, 0, 0)` draws a red middle; a look with another `values_hash` is not equal.
- [ ] **Step 2:** Run `scene-render-check`. Expected: fails.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run it. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: the material ball previews the shader`.

---

### Task 10: Docs

**Files:**
- Modify: `README.md` (the Shaders paragraph: a Material's Shader, the uniform rows, the built-ins, render modes, errors from the driver), `src/engine_instances/README.md` (Material.Shader), `src/runner` README if present, else `src/engine_core/README.md` (SurfaceBinding)

- [ ] **Step 1:** Write the paragraphs.
- [ ] **Step 2:** Commit: `Docs: surface shaders on Materials`.

---

## Self-review notes

- Spec coverage: Material.Shader with the post-shader refusal (Task 2); parameter rows from attributes with defaults, Reset, clamping, mismatch warnings (Task 3); values per Material, not per mesh (Task 4); templates not a second path, programs per shader, binding on units 6-8, batching by shader (Tasks 1, 5); see-through and render modes (Task 6); shadows with holes, `no_shadow`, fingerprints and `TIME` (Task 7); compile errors back to Error at the user's line (Tasks 1, 8); material ball (Task 9); reclaim after 60 s (Task 5).
- Deliberate departures from the spec, to confirm in review: WORLD_POSITION and WORLD_NORMAL come from `uInverseView` in the fragment shader, so `geometry.vert` is unchanged (the spec had the vertex shader pass them); LOD cards bake whatever the shader draws at bake time, through the baker's own Renderer, but a shader or value change does not trigger a rebake in this phase (the card file hash is content-based and the bake trigger is outside this plan); terrain ignores a Material's Shader (spec: Later).
- Names across tasks: `AssembleSurface`, `MapShaderError`, `runSurfaceShader`, `runShadowSurfaceShader`, `Material::shader`, `SurfaceBinding`, `make_surface_binding`, `MeshDraw::surface`, `surfaceTextures`, `SurfacePrograms`, `take_errors`/`takeSurfaceErrors`, `Shader::set_compile_error`.
