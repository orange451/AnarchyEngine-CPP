# Shader Editor and Vertex Stages Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Shader holds a vertex text and a fragment text, its Kind and render modes are properties, a surface Shader's vertex stage moves every vertex drawn with it each frame (shadows included), and double-clicking a Shader opens a docked Shader Editor (layout A) from which each stage's text editor opens; delivered with an Ocean shader and demo project.

**Architecture:** `SourceText` grows from one text to N stages, each its own file beside the `.meta.json`; `Shader` has two (fragment, as today's `.glsl`; vertex, a new `.vert.glsl`). `parse_shader` takes the stage and Kind, treats the old header lines as optional property-setting lines, and the Shader merges both stages' uniforms. A new `vertex_shader.glsl` is spliced into `geometry.vert` and `shadow.vert` the way `surface_shader.glsl` is spliced into the fragment files, under `#ifdef VERTEX_SHADER`; `SurfacePrograms` builds vertex-stage programs, shadows always have a program for a vertex-stage Shader, and culling grows each such draw's sphere by the Shader's margin. In the studio the current text editor becomes `IdeShaderText` (one stage per tab) and a new docked `IdeShaderEditor` pane is the Shader's home, with a `MaterialBall` preview that takes a mesh and a rotation.

**Tech Stack:** C++17/MSVC, CMake (`MSYS_NO_PATHCONV=1 cmake --build build --config Release --target <t>`), Catch2 sandbox, the custom Expect harnesses (tests/), scene-render-check (GL, run by hand from the repo root), JadeFX UI.

**Spec:** `docs/superpowers/specs/2026-10-20-shader-editor-design.md` (phase 1 and 2 context: `docs/superpowers/specs/2026-10-20-custom-shaders-design.md`).

## Global Constraints

- File names: the fragment stage keeps phase 1's `<Name>.<guid>.glsl` (and `init.glsl`), so every existing project loads unchanged; the vertex stage is `<Name>.<guid>.vert.glsl` (`init.vert.glsl`). The spec's `.vertex.glsl`/`.fragment.glsl` would be read as a GUID by `split_stem`; this is the plan's ruling.
- Text editor tabs are named after the files without the GUID: `Ocean.glsl` (Fragment) and `Ocean.vert.glsl` (Vertex).
- `Shader.Error` is `"<stage> <line>: <message>"` with stage `vertex` or `fragment`; `VertexError` and `FragmentError` are `"<line>: <message>"`; a compile error is kept only for the current `source_version`.
- New Shader properties, all saved: `Kind` (`Enum.ShaderKind`, Surface), `Transparent` (false), `TwoSided` (false), `CastsShadow` (true), `DisplacementMargin` (1, refused below 0). `Source` stays an alias of `FragmentSource`.
- A `shader surface;`/`shader post;` line in a written text sets Kind; a `render_mode` line sets all three booleans (a mode not named is off); no `render_mode` line leaves them alone; both lines are blanked to spaces in the stored text. One undo step with the write.
- Vertex stage built-ins: `VERTEX`, `NORMAL` (world space, writable), `UV`, `COLOR`, `TIME`, the uniforms; `void vertex()`. `VERTEX` joins the reserved names.
- A uniform in both stages must match in type, hints, and default, else both stages error `"<Name> is declared differently in the vertex stage"` (fragment) / `"... in the fragment stage"` (vertex), at the uniform's line.
- Culling: a vertex-stage draw's sphere radius grows by `DisplacementMargin` (world units). LOD and card bands are unchanged.
- A vertex-stage surface Shader always has a shadow program; its shadows redraw every frame when the vertex text names `TIME`.
- The preview draws at most every 50 ms and only while its pane is showing.
- No `studs`; distances are units. New APIs are named for this engine.

## Review Focus

- A phase 1 project with `.glsl` files only, including the folder form, loads with no migration and saves back without rewriting files whose text did not change. Pinned in Task 2.
- A Shader whose vertex text is deleted (set to "") draws plain again, is culled by its plain box, and its shadow goes back to the plain depth program. Pinned in Task 4 and 5.
- A script writing `Source` with phase 1 header lines, then reading it back, sees the text without the lines and the properties set. Pinned in Task 3.
- Closing the Shader Editor while a stage's text editor is open: both keep working independently, and reopening the Shader brings the pane forward without a duplicate. Pinned in Task 7.
- Uniform rows in Properties still cover a uniform declared only in the vertex stage, with its default. Pinned in Task 1 and 3.

---

### Task 1: The parser knows stages, optional headers, and merged uniforms

**Files:**
- Modify: `src/engine_core/ShaderSource.hpp`, `src/engine_core/ShaderSource.cpp`
- Test: `sandbox/shader_source_tests.cpp`

**Interfaces:**
- Produces:

```cpp
enum class ShaderStage : std::uint8_t { Fragment = 0, Vertex = 1 };
struct ShaderParse {
    ShaderKind kind;                                 // the kind parsed under
    ShaderStage stage = ShaderStage::Fragment;
    std::optional<ShaderKind> header_kind;           // a `shader X;` line was present
    std::optional<std::vector<std::string>> header_modes;  // a `render_mode` line was present
    std::vector<ShaderParam> params; std::optional<ShaderError> error; std::string glsl;
    bool reads_time = false; bool cuts = false;      // cuts: fragment only
};
// kind given: header optional. kind nullopt: header required (phase 1 behavior, kept for old callers).
ShaderParse parse_shader(std::string_view source, ShaderStage stage = ShaderStage::Fragment,
                         std::optional<ShaderKind> kind = std::nullopt);
// The two stages' uniforms as one list (fragment first, then the vertex stage's the
// fragment lacks); a mismatch is returned as (fragment error, vertex error).
struct MergedParams { std::vector<ShaderParam> params; std::optional<ShaderError> fragment_error, vertex_error; };
MergedParams merge_shader_params(const ShaderParse& fragment, const ShaderParse* vertex);
```

  The vertex stage requires `void vertex()` ("a vertex stage needs void vertex()" at the last line) and takes no `render_mode`. A `render_mode` word unknown to the Kind still errors as today. `header_modes` present with an empty list means `render_mode;` was written: all off.

- [ ] **Step 1: Write the failing tests** `SH12`–`SH16` in `sandbox/shader_source_tests.cpp` (tag `[shader]`): a text with no header parses under a given kind with `header_kind` empty; a header present fills `header_kind` and is blanked; `render_mode two_sided;` fills `header_modes == {"two_sided"}`; a vertex stage without `vertex()` errors at the last line; `VERTEX` as a uniform name errors "reserved"; `merge_shader_params` of `uniform float A = 1.0;` in both gives one param, and `float A = 2.0;` in the vertex gives both errors with the right lines.
- [ ] **Step 2:** `cmake --build build --config Release --target sandbox` then `build/Release/sandbox.exe "[shader]"`. Expected: compile failure, then failures.
- [ ] **Step 3:** Implement in `ShaderSource.*`: `Parser` takes stage and optional kind; `run()` accepts a missing header when kind is given; `header()` records `header_kind`; render_mode records `header_modes`; the required function by stage; `VERTEX` in `kReserved`; `merge_shader_params` compares `type, group, ranged, min, max, hidden, srgb, normal, black, clamp, nearest, fallback`.
- [ ] **Step 4:** Run `[shader]` and `[shader-assembly]`. Expected: pass (SH3 still passes through the nullopt-kind overload).
- [ ] **Step 5:** Commit: `Shaders: the parser knows stages, optional headers, and merged uniforms`.

---

### Task 2: SourceText stages, and a Shader's two files

**Files:**
- Modify: `src/engine_instances/SourceText.hpp/.cpp`, `src/engine_instances/Shader.hpp/.cpp`, `src/engine_core/DataModel.hpp` (`AuthoredNode`/`PlaceRecord` source fields become per stage; `Field::VertexSource`), `src/engine_core/DataModelHistory.cpp` (`HistoryProp::VertexSource`, create/destroy records), `src/engine_core/DataModelPlace.cpp`, `src/engine_core/Project.hpp/.cpp` (`kSourceExtensions` gains `vert.glsl`, `source_suffix_of` matches the longer suffix first, `Files` holds `std::vector<SourceFile>`, `plan_files`/incremental `paths`/`scan`/`read_folder`/`build`/`create_from`/`write_pack`/disk-merge handle each stage), `src/engine_core/InstanceFile.hpp/.cpp` (`CopiedNode::sources`, JSON `"sources"` beside the old `"source"` which still reads as stage 0), `src/engine_network/Replicator.cpp` and `ReplicaApplier` (the Source op carries a stage byte)
- Test: `sandbox/shader_tests.cpp`, existing SHA7–SHA12 and script tests stay green

**Interfaces:**
- Produces, `SourceText`:

```cpp
virtual int stage_count() const { return 1; }
virtual const char* source_extension(int stage = 0) const = 0;   // "luau"; Shader: "glsl", "vert.glsl"
const std::string& source(int stage = 0) const;
void set_source(std::string text);                 // stage 0, as today
void set_source(int stage, std::string text);
std::uint64_t source_version() const;              // bumped by any stage
protected: virtual void on_source_changed(int stage);
```

  `Shader`: `stage_count() == 2`, `kFragmentStage = 0`, `kVertexStage = 1`, `vertex_source()`, `set_vertex_source(text)`, `has_vertex_stage()` (non-empty text); `kShaderVertexStarterSource` (a `vertex()` leaving `VERTEX`, with a commented wave). `kShaderStarterSource` loses its `shader surface;` line. Lua: `Source` (alias), `FragmentSource`, `VertexSource`, all strings, writable (`lua_property`, since the text lives in files). A stage with empty text writes no file and an absent file reads as empty. Undo: `Field::VertexSource` → `HistoryProp::VertexSource`, applied through `set_source(1, ...)`; create/destroy records keep every stage; Stop's `write_place`/`read_place` carry every stage.

- [ ] **Step 1: Write the failing tests** `SHA20`–`SHA23`: `set_vertex_source` bumps `source_version` and fires Changed as `"VertexSource"`; save writes `Look.<guid>.glsl` and `Look.<guid>.vert.glsl` and a load reads both back, and an empty vertex text writes no `.vert.glsl`; a phase 1 project (only `.glsl`, leaf and folder form) loads with an empty vertex stage and a second save leaves the `.glsl` bytes untouched; undo of a vertex edit restores it and `copy_tree`/`paste_copies`/`clone_tree`/instance files keep both stages; a Script still has one stage.
- [ ] **Step 2:** Run `[shader]`, `[project]`, `[copy]`. Expected: compile failure, then failures.
- [ ] **Step 3:** Implement the stage generalization as listed in Files, keeping every existing single-stage path as `stage == 0`.
- [ ] **Step 4:** Run the whole sandbox, and `engine-tests`, `mcp-tests`. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: a Shader keeps a vertex text in its own file beside the fragment`.

---

### Task 3: Kind, render modes, margin, and errors as Shader properties

**Files:**
- Modify: `src/engine_instances/Shader.hpp/.cpp`, `src/engine_core/LuaApi.cpp` (docs), `src/engine_core/SurfaceBinding.hpp/.cpp` (`modes` and `displacement_margin` from the properties; `parse` is the merged parse), `src/ide/McpTools.cpp` + `McpToolSpecs.cpp` (`read_script`/`write_script`/`edit_script` take `stage: "vertex"|"fragment"`, default fragment; `problems` per stage), `src/ide/PropertySheet.cpp` (unchanged if `parse()->params` is merged; verify)
- Test: `sandbox/shader_tests.cpp`, `tests/McpTest.cpp`, `tests/PropertiesTest.cpp`

**Interfaces:**
- Produces, `Shader`:

```cpp
ShaderKind kind() const; std::optional<std::string> set_kind(ShaderKind);
bool transparent() const; bool two_sided() const; bool casts_shadow() const;   // + set_* (nullopt)
double displacement_margin() const; std::optional<std::string> set_displacement_margin(double);  // "DisplacementMargin must not be negative"
const std::string& error() const;             // "<stage> <line>: <message>" or ""
const std::string& vertex_error() const; const std::string& fragment_error() const;
void set_compile_error(std::uint64_t version, ShaderStage stage, std::string error);
std::shared_ptr<const ShaderParse> parse() const;          // merged params, fragment glsl, both reads_time/cuts
std::shared_ptr<const ShaderParse> stage_parse(ShaderStage) const;
```

  Lua: `Kind` saved enum (`lua_saved_enum`, as `PhysicsObject.Shape`), `Transparent`, `TwoSided`, `CastsShadow`, `DisplacementMargin` saved; `Error`, `VertexError`, `FragmentError` read-only. `on_source_changed(stage)`: reparse that stage under `kind()`; when `header_kind` is set, `set_kind` (recorded, so it joins the caller's step); when `header_modes` is set, set the three booleans; blank the lines in the stored text (no second Changed); recompute the merged parse and the errors; emit `Kind`/`Error`/`VertexError`/`FragmentError` as they change. `SurfaceBinding::modes` reads the booleans (`no_shadow = !casts_shadow`), gains `bool has_vertex`, `std::string vertex_glsl`, `bool vertex_reads_time`, `float displacement_margin`; its `version` is the Shader's. `surface_modes(parse)` is removed with its parse.modes use.

- [ ] **Step 1: Write the failing tests** `SHA24`–`SHA27` and MCP `TestShaderStages`: writing `Source` with `shader post;\nrender_mode hdr;` sets Kind Post, reads back without the lines, and undo puts Kind and text back in one step; `render_mode two_sided;` sets TwoSided and leaves Transparent; `set_displacement_margin(-1)` refuses; `Error` reads `"vertex 3: ..."` when only the vertex stage fails; `parse()->params` holds a uniform declared only in the vertex stage (and `TestShaderParamRows` sees its row); MCP `write_script` with `stage:"vertex"` writes `VertexSource` and `read_script` reads it.
- [ ] **Step 2:** Run `[shader]`, `mcp-tests`, `properties-tests`. Expected: failures.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the sandbox, `mcp-tests`, `properties-tests`. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: Kind, render modes, and the margin are a Shader's properties`.

---

### Task 4: The vertex stage draws

**Files:**
- Create: `resources/shaders/pipeline/vertex_shader.glsl`
- Modify: `resources/shaders/pipeline/geometry.vert` (world position and world normal computed into locals in both `main` and `billboard`, then `#ifdef VERTEX_SHADER runVertexShader(world, worldNormal, aUv, aColor * vec4(aTint.rgb, 1.0)); #endif`), `src/runner/ShaderAssembly.hpp/.cpp` (`AssembleVertex`), `src/runner/ShaderFile.hpp/.cpp` (`LinkProgramLog` reports which stage failed), `src/runner/SurfacePrograms.hpp/.cpp` (vertex-stage variants; errors carry the stage), `src/runner/Renderer.hpp/.cpp` (`takeSurfaceErrors` tuple gains the stage; `DrawItem::margin` filled from the binding), `src/runner/Visibility.hpp/.cpp` (`BoundItem` adds `item.margin` to the sphere radius), `src/runner/GameView.cpp` (posts the stage), `tests/SceneRenderCheck.cpp`
- Test: `sandbox/shader_assembly_tests.cpp` (`SA5`, `SA6`), `tests/SceneRenderCheck.cpp`

**Interfaces:**
- Produces:

```cpp
// main_file is geometry.vert or shadow.vert: its #version, `#define VERTEX_SHADER`,
// vertex_shader.glsl, the user's text, then the rest of the file.
AssembledSource AssembleVertex(const char* main_file, const std::string& user_glsl);
enum class LinkStage { Vertex, Fragment, Link };
GLuint LinkProgramLog(const std::string& vertex, const std::string& fragment, std::string& log, LinkStage& failed);
// SurfacePrograms::Built::error becomes "<stage> <line>: <message>"; take_errors() yields
// (shader id, version, ShaderStage, "<line>: <message>").
```

  `vertex_shader.glsl` declares `vec3 VERTEX; vec3 NORMAL; vec2 UV; vec4 COLOR; uniform float uTime; #define TIME uTime; void vertex(); void runVertexShader(inout vec3 world, inout vec3 worldNormal, vec2 uv, vec4 color)` which loads the globals, calls `vertex()`, and stores `VERTEX` and `normalize(NORMAL)` back. A binding with `has_vertex` builds its gbuffer and forward variants with `AssembleVertex("pipeline/geometry.vert", vertex_glsl)` and `useGeometryProgram` already uploads `uTime`. `item.margin = binding.has_vertex ? binding.displacement_margin : 0.f`.

- [ ] **Step 1: Write the failing checks:** `SA5` the assembled vertex text has `#define VERTEX_SHADER` after `#version` and the user's first line at `user_first_line`; `SA6` `MapShaderError` on a vertex log maps to the user's line. In `CheckSurfaceShaders`: a cube with `void vertex() { VERTEX.y += 1.0; }` draws with its middle 1 unit higher (the pixel a unit above the old middle is now the cube); a vertex error reports `(id, version, Vertex, "2: ...")`; a cube displaced 3 units sideways with `DisplacementMargin` 3 still draws, and with margin 0 it is culled (nothing drawn); a Shader with `has_vertex` false builds exactly as before (`builds()` unchanged).
- [ ] **Step 2:** Build `sandbox` and `scene-render-check`; run `[shader-assembly]` and `scene-render-check` from the repo root. Expected: those fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run both. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: a vertex stage moves every vertex drawn with the Material`.

---

### Task 5: Shadows follow the vertex stage

**Files:**
- Modify: `resources/shaders/pipeline/shadow.vert` (`#ifdef VERTEX_SHADER` hook on `world`), `src/runner/SurfacePrograms.cpp` (a `has_vertex` binding always builds `shadow`, with `shadow.frag` plain when it does not cut), `src/runner/ShadowRenderer.cpp` (`surfaceOf` returns the binding when `has_vertex`, whatever `cutouts`; `casterWatched_` 2 when `vertex_reads_time`; `SurfaceFingerprint` unchanged), `tests/SceneRenderCheck.cpp`

- [ ] **Step 1: Write the failing checks** in `CheckSurfaceShadows`: a cube lifted 2 units by its vertex stage casts its shadow where a cube at +2 would (the floor behind the rest position is lit); a vertex stage with `VERTEX.y += sin(TIME)` redraws a cached map on a later frame (the shadow moves between two frames); clearing the vertex text puts the plain depth program back (`shadowBuilds()` unchanged by a later draw).
- [ ] **Step 2:** Run `scene-render-check`. Expected: those fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `scene-render-check` and sandbox `[shadow]`. Expected: pass.
- [ ] **Step 5:** Commit: `Shaders: shadows follow a vertex stage`.

---

### Task 6: The text editor edits one stage

**Files:**
- Rename: `src/ide/IdeShaderEditor.hpp/.cpp` → `src/ide/IdeShaderText.hpp/.cpp` (class `IdeShaderText`), updating `CMakeLists.txt`, `IdeLayout.hpp` (`open_shader_texts_` keyed by `(id, stage)`, `edit_shader_text(id, stage, home)`, `open_shader_text(id, stage)`), `IdeLayoutEditing.cpp`, `IdeLayoutDocking.cpp`, `tests/ShaderEditorTest.cpp`
- Modify: `src/ide/IdeProblems.hpp/.cpp` (one problem per stage with `code` `"Vertex"`/`"Fragment"`; `ProblemsHost::open` gains `stage`), `src/ide/McpTools.cpp` `problems` codes

**Interfaces:**
- Produces: `IdeShaderText(engine_core::Engine&, std::uint32_t id, engine_core::ShaderStage stage)`; `stage()`; tab title `<Name>.glsl` or `<Name>.vert.glsl`; reads `source(stage)` and the stage's error; writes `set_source(stage, text)` under `"Edit Shader"`.

- [ ] **Step 1: Write the failing tests** in `tests/ShaderEditorTest.cpp` (run by `studio-tests`): `TestStageTabs`: opening the vertex stage makes a tab `Look.vert.glsl` whose typing reaches `VertexSource`, and the fragment tab `Look.glsl` is a separate tab; `TestProblemOpensShader` gains a vertex error that opens the vertex tab at its line. (`TestShaderTab` keeps double-click opening `Look.glsl` for now; Task 7 changes that.)
- [ ] **Step 2:** Build `studio-tests` and run it. Expected: failures.
- [ ] **Step 3:** Implement the rename and stage.
- [ ] **Step 4:** Run `studio-tests`, `mcp-tests`. Expected: pass.
- [ ] **Step 5:** Commit: `Studio: the shader text editor edits one stage`.

---

### Task 7: The Shader Editor pane

**Files:**
- Create: `src/ide/IdeShaderEditor.hpp/.cpp` (the pane), `tests/ShaderEditorPaneTest.cpp`, CMake target `shader-editor-tests` (as `prefab-editor-tests`: links `studio`, in ctest and the `tests` target)
- Modify: `src/ide/IdeLayout.hpp` (`open_shaders_` is now the panes; `edit_shader(id, home)` docks the pane; `shader_editor_host()`), `src/ide/IdeLayoutEditing.cpp` (`edit(id)` on a Shader docks the pane), `resources/css` theme files for the pane's classes, `tests/ShaderEditorTest.cpp` (`TestShaderTab`: double-click now opens the pane, whose Edit fragment opens `Look.glsl`), `README.md` paragraph in Task 10

**Interfaces:**
- Produces:

```cpp
struct ShaderEditorHost {
    std::function<void(InstanceId, ShaderKind)> set_kind;
    std::function<void(InstanceId, const char* property, bool)> set_mode;   // "Transparent", "TwoSided", "CastsShadow"
    std::function<void(InstanceId, double)> set_margin;
    std::function<void(InstanceId, ShaderStage)> edit_stage;                // opens the text editor
    std::function<void(InstanceId)> add_vertex_stage;                       // fills the starter, then edit_stage
    std::function<void(InstanceId)> select;                                 // a Material in Used by
};
class IdeShaderEditor : public IdePane {   // IdePane("Shader", true), icon Shader.png, class "shader-editor"
    IdeShaderEditor(engine_core::DataModel& world, InstanceId shader, ShaderEditorHost host);
    // For tests: kindBox(), stateLabel(), stageRow(ShaderStage) (its label and button), modeToggle(const char*),
    // marginField(), uniformRows(), usedByRows(), previewNode(), meshBar()
};
```

  Watches the world as `IdePrefabEditor` does (`watch_changes`, `tree_revision`, `authored_revision`); each host write is one undo step named `Set <Property>`. Materials in Used by: every Material whose `shader()` is this one, by name. A deleted Shader shows the Prefab editor's "gone" state.

- [ ] **Step 1: Write the failing tests** `ShaderEditorPaneTest.cpp`: the header shows the name, Kind, and "Built"/the error; the Vertex row reads "none" with Add vertex stage and, after the host adds it, "built · N lines"; a toggle calls `set_mode` and the row reflects the property on the next frame; uniform rows list the merged uniforms with defaults, grouped; Used by lists the Materials and clicking one calls `select`; the Fragment row's Edit calls `edit_stage(Fragment)`. In `ShaderEditorTest.cpp`: double-click docks the pane (class `shader-editor`), not a text tab; its Edit fragment opens `Look.glsl`; closing the pane leaves the text tab working and a second double-click reuses one pane.
- [ ] **Step 2:** Build `shader-editor-tests` and `studio-tests`, run both. Expected: failures.
- [ ] **Step 3:** Implement the pane and the layout wiring.
- [ ] **Step 4:** Run `shader-editor-tests`, `studio-tests`, `assets-tests`, `explorer-tests`. Expected: pass (the two known explorer-tests failures aside).
- [ ] **Step 5:** Commit: `Studio: a Shader opens in the Shader Editor, its stages from there`.

---

### Task 8: The live preview

**Files:**
- Modify: `src/ide/MaterialBall.hpp/.cpp` (`enum class PreviewMesh { Sphere, Cube, Plane }`; `setMesh(PreviewMesh)`, `setMesh(std::shared_ptr<const amesh::Data>)` for a place Mesh's live copy or file; `setRotation(float yaw, float pitch)`; `bool drawView(const MaterialLook&, runner::ViewPixels&)` = `draw` without `cut_ball`), `src/ide/IdeShaderEditor.cpp` (the preview `ImageView`, a `MaterialBall(512, 512)` owned by the pane, drawn in `renderContent` at most every 50 ms while showing, dragging sets the rotation, the mesh bar sets the mesh; `look` is `material_look`'s shape with the Shader's binding at defaults), `tests/SceneRenderCheck.cpp`, `tests/ShaderEditorPaneTest.cpp`

- [ ] **Step 1: Write the failing checks:** scene-render-check: `drawView` with `PreviewMesh::Cube` and the red Shader gives a red middle and a cleared corner; `setRotation(0, 90°)` on the Plane shows it edge-on (the middle row is clear) while `(0, 0)` shows it; pane test: a drag across the preview changes `rotation()`; the mesh bar's Cube selects `PreviewMesh::Cube`.
- [ ] **Step 2:** Run `scene-render-check` and `shader-editor-tests`. Expected: those fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run both. Expected: pass.
- [ ] **Step 5:** Commit: `Studio: the Shader Editor previews the Shader on a mesh you can turn`.

---

### Task 9: AddPlane segments, the Ocean shader, and the demo

**Files:**
- Modify: `src/engine_core/MeshShapes.hpp/.cpp` (`add_plane(data, width, depth, at, int segments = 1)`, clamped 1..256, `(segments+1)²` vertices, UVs 0..1 across), `src/engine_core/MeshEditorBindings.cpp` (`AddPlane(width, depth, position?, segments?)`), `src/engine_core/LuaApi.cpp` (doc)
- Create: `samples/ocean/Ocean.glsl`, `samples/ocean/Ocean.vert.glsl` (MIT credit to afl_ext in a comment), `sandbox/ocean_demo.cpp` (a Catch2 test tagged `[.ocean-demo]`, run by hand with `OCEAN_DEMO=<folder>`; makes the project: the Ocean Shader from the two sample files, an Ocean Material, a Mesh built with `add_plane(400, 400, {}, 256)`, a Prefab/Model/GameObject drawing it, a DynamicSky, a Camera at (0, 3, 0) looking at the horizon, three rock Brushes)
- Test: `sandbox/mesh_shapes_tests.cpp`, `sandbox/mesh_editor_lua_tests.cpp`

**Interfaces:**
- The Ocean vertex stage: uniforms `WaveHeight : range(0, 5) = 0.8`, `WaveScale : range(1, 200) = 40`, `Choppiness : range(0, 1) = 0.38`, `Speed : range(0, 4) = 1`; the summed `wavedx` octaves (12) over `VERTEX.xz / WaveScale`, lifting `VERTEX.y` by `WaveHeight * (h - 0.5)` and setting `NORMAL` from two offset samples. Fragment: `DetailOctaves` (int, 24) of the same field for `NORMAL` detail faded by distance; `DeepColor : color`, `ShallowColor : color`, `FoamColor : color`, `FoamAmount : range(0, 1)`; `ALBEDO` mixed by crest height, foam above a slope threshold, `ROUGHNESS = 0.08`, `REFLECTIVITY = 1.0`, `METALNESS = 0`.

- [ ] **Step 1: Write the failing tests:** `add_plane(2, 2, {}, 4)` makes 25 vertices and 32 triangles with corner UVs (0,0) and (1,1); Lua `AddPlane(2, 2, nil, 4)` returns 25 ids; `AddPlane(2, 2)` still returns 4.
- [ ] **Step 2:** Run `[mesh-shapes]`/the mesh editor tags. Expected: failures.
- [ ] **Step 3:** Implement `add_plane` segments; write the two Ocean files and the generator; run the generator into the user's projects folder as `Ocean`; open the project in the Release studio and tune the uniforms by eye (screenshots to the user).
- [ ] **Step 4:** Run the sandbox. Expected: pass. Check the Ocean Shader builds with no error in the studio.
- [ ] **Step 5:** Commit: `Shaders: an Ocean sample, and AddPlane takes segments`.

---

### Task 10: Docs

**Files:**
- Modify: `README.md` (the Shaders paragraph: the Shader Editor, two stages, Kind and modes as properties, the vertex built-ins, the margin, the file names), `src/engine_instances/README.md` (Shader's stages and properties), `src/engine_core/README.md` (SourceText stages, the vertex variant in the binding)

- [ ] **Step 1:** Write the paragraphs.
- [ ] **Step 2:** Commit: `Docs: the Shader Editor and vertex stages`.

---

## Self-review notes

- Spec coverage: editor layout A (Task 7, 8); stage text editors (6); Shader asset properties, files, phase 1 reading, `Source` writes (2, 3); the language and merged uniforms (1, 3); rendering, culling margin, cards (4); shadows (5); the ocean and demo (9); tests listed per task; docs (10).
- Rulings against the spec: file names keep the GUID and the fragment keeps `.glsl` (see Global Constraints); tab titles follow the file names; `AddPlane` segments default 1 for a plane (shapes' minimum of 3 applies to round shapes only).
- Names across tasks: `ShaderStage`, `parse_shader(source, stage, kind)`, `merge_shader_params`, `SourceText::set_source(stage, text)`, `Shader::has_vertex_stage`, `set_compile_error(version, stage, text)`, `SurfaceBinding::has_vertex/vertex_glsl/displacement_margin`, `AssembleVertex`, `LinkStage`, `DrawItem::margin`, `IdeShaderText`, `IdeShaderEditor`, `ShaderEditorHost`, `MaterialBall::drawView/setMesh/setRotation`.
