# Custom Shaders, Phase 1: the Shader Asset and Its Editor

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A user can add a Shader in the Assets pane, write GLSL in a docked editor with highlighting and find, see header and uniform mistakes as a red mark and a Problems row as they type, and have the source saved as a `.glsl` file beside its `.meta.json`.

**Architecture:** `LuaSource`'s text handling moves into a new base, `SourceText`, which `LuaSource` and the new `Shader` both derive from; every place that saves, loads, copies, undoes, or replicates source keys on `SourceText` and its file extension instead of `LuaSource` and `.luau`. A pure parser, `parse_shader`, reads the header, `render_mode`, `group`, and `uniform` lines into a parameter list, an error, and GLSL with those additions blanked out. The Studio side follows the CSS editor's pattern for the editor and the script editor's for marks and find.

**Tech Stack:** C++17, MSVC, CMake, Catch2 (sandbox), the custom `expect` harness (tests/), JadeFX UI, Luau.

**Spec:** `docs/superpowers/specs/2026-10-20-custom-shaders-design.md`. This plan builds the sections "The Shader asset" and "The language" (parsing only), plus the Studio items for the asset (Assets pane, shader editor, Problems, MCP). It builds no rendering, no `Material.Shader`, and no `ShaderEffect`: those are phases 2 and 3.

## Global Constraints

- The source file is `<Name>.<guid>.glsl` beside `<Name>.<guid>.meta.json`; a Shader that has children uses the folder form `init.glsl` with `init.meta.json`, as a script uses `init.luau`.
- `.gitattributes` written for a new project gains `*.glsl text eol=lf`.
- `Kind` is `Enum.ShaderKind` (`Surface` 0, `Post` 1), read-only, not saved. `Error` is a read-only string, not saved: empty, or `"<line>: <message>"` with the 1-based line.
- Supported uniform types: `float`, `int`, `bool`, `vec2`, `vec3`, `sampler2D`. Hints: `range(min, max)`, `color`, `repeat`, `clamp`, `white`, `black`, `normal`, `nearest`, `hidden`. At most 64 uniforms; at most 3 `sampler2D` in a surface shader and 4 in a post shader.
- `render_mode`: surface takes `transparent`, `two_sided`, `no_shadow`; post takes `hdr`.
- Names are engine names, not Roblox's. Distances are "units" in any copy.
- Every file this plan touches keeps LF line endings (`git diff --stat` shows only the lines changed).
- The Shader class does not get Luau analysis, does not run, and is not a `LuaSource`.

## Review Focus

- A Shader renamed in the Explorer while its editor is open: the tab title and the files on the next save both follow, with no orphaned `.glsl` left on disk. Pinned in Task 4 (files) and Task 5 (title).
- A project folder hand-edited so a Shader's source is `Dissolve.<guid>.luau`, or a Script's is `.glsl`: the load fails with a message naming the file and the extension its class keeps, not a crash or a silent empty source. Pinned in Task 4.
- Stop after editing a Shader's source during a playtest: the source, `Kind`, and `Error` all come back to the authored text. Pinned in Task 3.
- A shader with a syntax error inside a comment or after `#if 0`: comments are skipped by the parser; preprocessor branches are not evaluated, so a uniform under `#if 0` still counts. The test pins the comment case and documents the `#if` behavior. Pinned in Task 1.
- An existing project with only scripts, saved by the new code: every `.luau` and `.meta.json` byte-identical to before. Pinned in Task 2.

---

### Task 1: The shader parser

**Files:**
- Create: `src/engine_core/ShaderSource.hpp`, `src/engine_core/ShaderSource.cpp`
- Modify: `CMakeLists.txt` (engine_core sources beside `src/engine_core/Attributes.cpp`; sandbox list beside `sandbox/attribute_tests.cpp`)
- Test: `sandbox/shader_source_tests.cpp`

**Interfaces:**
- Produces, namespace `engine_core`:

```cpp
enum class ShaderKind { Surface = 0, Post = 1 };
enum class ShaderParamType { Float, Int, Bool, Vec2, Vec3, Color, Sampler };

struct ShaderParam {
    std::string name;
    ShaderParamType type = ShaderParamType::Float;
    std::string group;          // from the latest `group Name;` above it, "" for none
    bool ranged = false;        // range(min, max) given
    double min = 0, max = 0;
    bool hidden = false;
    // A sampler2D's hints: color (sRGB), normal, the unset fallback, wrapping, filtering.
    bool srgb = false, normal = false, black = false, clamp = false, nearest = false;
    LuaSlot fallback;           // the default: Number, Bool, Vec2, Vec3, or Color; Nil for a sampler
    int line = 0;               // 1-based
};

struct ShaderError {
    int line = 1;               // 1-based
    std::string message;
};

struct ShaderParse {
    ShaderKind kind = ShaderKind::Surface;
    std::vector<std::string> modes;     // render_mode words, in order
    std::vector<ShaderParam> params;    // declaration order
    std::optional<ShaderError> error;   // the first problem, or none
    std::string glsl;                   // the source, same line count, additions blanked (see below)
};

ShaderParse parse_shader(std::string_view source);
std::string shader_error_text(const ShaderError& error);   // "<line>: <message>"
// The engine's own names in a shader, which a uniform cannot take.
bool shader_reserved_name(std::string_view name);
```

Parse rules, which the tests pin:
- Comments (`//` to end of line, `/* ... */`) are skipped for every rule below. Preprocessor lines (`#...`) pass through untouched and are not evaluated.
- The first statement must be `shader surface;` or `shader post;`; otherwise line 1 (or the statement's line) gets "a shader starts with `shader surface;` or `shader post;`".
- `render_mode a, b;` may follow. An unknown word is "unknown render_mode <w>; a surface shader has transparent, two_sided, no_shadow" (post: "a post shader has hdr").
- `group Name;` sets the group for the uniforms after it; a bare `group;` clears it.
- `uniform <type> <Name> [: hint, hint...] [= default];` at top level. Refusals, each at the statement's line: `vec4`, `mat2`-`mat4`, `sampler3D`, `samplerCube`, and any other type give "a uniform is float, int, bool, vec2, vec3, or sampler2D"; `[` after the name gives "a uniform cannot be an array"; `uniform Name {` gives "uniform blocks are not supported"; a reserved name gives "<Name> is the engine's name"; a repeat gives "<Name> is declared twice"; an unknown hint gives "unknown hint <h>"; `range` on a type other than float and int gives "range is for float and int"; `color` on other than vec3 and sampler2D gives "color is for vec3 and sampler2D"; `repeat`, `clamp`, `white`, `black`, `normal`, `nearest` on other than sampler2D give "<h> is for sampler2D"; `range(a, b)` with b not above a gives "range takes two numbers, the first below the second"; a default of the wrong shape gives "the default must be a <type>"; a default on a sampler2D gives "a sampler2D has no default; use white or black"; the 65th uniform gives "a shader has at most 64 uniforms"; the 4th sampler in a surface shader gives "a surface shader has at most 3 sampler2D" (post: 5th, 4).
- Defaults: `float`/`int` a number (an int default is truncated toward zero); `bool` `true`/`false`; `vec2(x, y)` or `vec2(s)`; `vec3(x, y, z)` or `vec3(s)`. A `color` vec3's fallback is a Color slot, otherwise a Vec3 slot. Without `=`: 0, false, zero vector, white for a color. `fallback` of a sampler is Nil.
- The function the kind needs must be present outside comments: "a surface shader needs void surface()" / "a post shader needs void post()", at the last line.
- Only the first error is kept; parsing stops there, and `params` holds what came before it.
- `glsl`: the source with the `shader`, `render_mode`, and `group` statements replaced by spaces (newlines kept) and each uniform's `: hints` and `= default` replaced by spaces, so `uniform float Progress : range(0, 1) = 0.0;` becomes `uniform float Progress` plus spaces plus `;` on the same line. The line count is always the source's, so a driver's line number is the user's line.
- Reserved names: `ALBEDO ALPHA NORMAL METALNESS ROUGHNESS REFLECTIVITY EMISSIVE UV COLOR VIEW_POSITION WORLD_POSITION VIEW_NORMAL WORLD_NORMAL SCREEN_UV SCREEN_SIZE TEXEL FRONT_FACING TIME CAMERA_POSITION VIEW PROJECTION INV_VIEW INV_PROJECTION SCREEN DEPTH NORMALS MATERIAL TEXTURE_DIFFUSE TEXTURE_NORMAL TEXTURE_ROUGHNESS TEXTURE_METALNESS TEXTURE_EMISSIVE`, any name starting with `gl_`, and any name of `u` followed by an uppercase letter (the engine's own uniforms).

- [ ] **Step 1: Write the failing tests** in `sandbox/shader_source_tests.cpp`, tag `[shader]`:
  - `SH1` the spec's Dissolve example parses: kind Surface, no error, 3 params in order `Progress` (Float, ranged 0..1, fallback 0), `EdgeColor` (Color, fallback (1, 0.5, 0.1)), `Noise` (Sampler, `clamp` false, fallback Nil); `glsl` has the same number of `\n` as the source and contains `uniform float Progress` and no `range(`.
  - `SH2` the Vignette example: kind Post, 2 params, no error.
  - `SH3` the header: missing header, `shader fancy;`, and a header after a uniform each give their exact message and line.
  - `SH4` render_mode: `transparent, two_sided` kept in `modes`; `hdr` in a surface shader and `bogus` refused with the exact messages.
  - `SH5` every refusal listed above, each with its exact message and line (one `SECTION` each).
  - `SH6` groups: two uniforms after `group Edge;` have group "Edge"; one after `group;` has "".
  - `SH7` defaults: int `= 2.9` gives 2; `vec3(0.5)` splats; `bool = true`; no default gives white for a color and 0 otherwise.
  - `SH8` comments: a `uniform vec4 X;` inside `/* */` or after `//` is ignored; `#if 0` / `uniform float Y;` / `#endif` still declares Y (documented behavior).
  - `SH9` limits: 64 uniforms are fine, 65 refused; 3 samplers in a surface shader fine, 4 refused; 4 in a post shader fine.
  - `SH10` `shader_error_text({9, "x"}) == "9: x"`.
- [ ] **Step 2:** Run `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox`. Expected: compile failure, `ShaderSource.hpp` not found.
- [ ] **Step 3:** Implement `ShaderSource.hpp/.cpp`. One pass: a small scanner that tracks comments, splits top-level statements on `;` (and stops at `{` for function bodies, counting brace depth so nothing inside a function is read as a statement), and records each statement's start line.
- [ ] **Step 4:** Run `build/Release/sandbox.exe "[shader]"`. Expected: all pass.
- [ ] **Step 5:** Commit: `Shaders: parse a shader's header, render_mode, groups, and uniforms with their hints`.

---

### Task 2: SourceText, the base LuaSource and Shader share

**Files:**
- Create: `src/engine_instances/SourceText.hpp`, `src/engine_instances/SourceText.cpp`
- Modify: `src/engine_instances/LuaSource.hpp/.cpp` (derive from SourceText; keep analysis), and every source-plumbing `dynamic_cast<LuaSource*>` that is about the text, not about Luau: `src/engine_core/Project.cpp` (:1257-1265, :1284, :3580, :3865), `src/engine_core/DataModelPlace.cpp` (:151-153, :465-468, :507-514), `src/engine_core/DataModelHistory.cpp` (:275-277, :317, :356, :454), `src/engine_core/InstanceFile.cpp` (:42-45, :171-173), `src/engine_core/Replicator.cpp` (:101, :132, :385), `src/engine_core/ReplicaApplier.cpp` (:239, :290). Leave ScriptAnalysis, ScriptRuntime, ScriptBindings, IdeProblems, IdeScriptEditor, and McpTools on `LuaSource`.
- Modify: `CMakeLists.txt` (engine_instances source list)
- Test: the existing suites; no new behavior.

**Interfaces:**
- Produces:

```cpp
// Text an instance keeps in a file of its own beside its .meta.json: a Script's
// Luau, a Shader's GLSL. Source is never a saved property.
class SourceText : public DataModel {
public:
    void set_source(std::string source);            // SimulationThread; records undo, emits Field::Source
    const std::string& source() const;
    std::uint64_t source_version() const;
    // The file extension without the dot: "luau", "glsl".
    virtual const char* source_extension() const = 0;
    void context_actions(std::vector<ContextAction>& out) const override;   // Edit first
    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;
protected:
    // After the text changed, by set_source or a Stop's read_place.
    virtual void on_source_changed() {}
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;
    std::string source_;
    std::uint64_t source_version_ = 1;
};
```

  `load_property`'s refusal reads "Source belongs in the .<ext> file, not in json". `LuaSource` overrides `source_extension()` to `"luau"` and `on_source_changed()` to invalidate script analysis (what `set_source` and `read_place` do today).
- Consumes: nothing from Task 1.

- [ ] **Step 1: Write the guard test** `P50 a script-only project saves byte-identical files` in `sandbox/project_tests.cpp`: create a project with a Script, a ModuleScript, and a Script with a child under a Folder; save; read every file under `src/` into a map; save again after touching nothing; compare maps equal; then load into a second Game and save it to a second folder, and compare file names and bytes with the first (GUIDs are kept, so names match).
- [ ] **Step 2:** Run it on the code before the refactor: `build/Release/sandbox.exe "P50*"`. Expected: PASS (it guards the refactor; it is not a new behavior).
- [ ] **Step 3:** Extract `SourceText`, move `LuaSource`'s text members and overrides into it, and switch the listed casts to `SourceText`.
- [ ] **Step 4:** Run `build/Release/sandbox.exe` (whole suite) and `build/Release/engine-tests.exe`. Expected: everything that passed before passes, P50 included.
- [ ] **Step 5:** Commit: `Scripts: SourceText holds the text a file keeps beside its .meta.json, so another class can share it`.

---

### Task 3: The Shader class and the Shaders category

**Files:**
- Create: `src/engine_instances/Shader.hpp`, `src/engine_instances/Shader.cpp`, `resources/icons/Shader.png` (a copy of `resources/icons/CSS.png` until there is real art)
- Modify: `src/engine_datatypes/Enum.cpp` (add `ShaderKind` beside `ShadingModel`, to `kTypes`, with a `shader_kind_enum()` accessor declared where `shading_model_enum()` is); `src/engine_services/GameService.hpp/.cpp` (a `Shaders` service class); `src/engine_core/Game.cpp:43-49` (`add_service<Shaders>` after Animations); `src/engine_core/Containment.hpp:22-28` (`{"Shaders", "Assets"}`); `src/engine_core/Containment.cpp:19-22` (`{"Shaders", "Shader", "Shaders"}`) and the message at :114-120 to list every category; `src/engine_core/Project.cpp:130-158` (the `existing_service` table, `static_assert` 14 → 15) and :198-221 (factory); `src/engine_core/ScriptBindings.cpp` (~:257 factory, ~:320 `register_lua_creatable("Shader", ...)`); `src/engine_core/LuaApi.cpp` (completion docs for Shader's Source, Kind, Error); `src/ide/ClassOrder.hpp` (Shader beside Material); `src/ide/IdeIcons.cpp` (`Shaders` → `AssetFolder.png`); `src/ide/AssetBrowser.cpp:350-371` (`new_kind` list gains "Shader"); `CMakeLists.txt`
- Test: `sandbox/shader_tests.cpp`, and update `sandbox/game_services_tests.cpp` GS2's category list

**Interfaces:**
- Consumes: Task 1's `parse_shader`, `shader_error_text`, `ShaderKind`; Task 2's `SourceText`.
- Produces:

```cpp
class Shader : public SourceText {
public:
    const char* class_name() const override;              // "Shader"
    const char* source_extension() const override;        // "glsl"
    ShaderKind kind() const;
    const std::string& error() const;                     // "" or "<line>: <message>"
    // The latest parse of source(), shared and immutable; never null.
    std::shared_ptr<const ShaderParse> parse() const;
protected:
    void on_source_changed() override;                    // reparse; emit_property("Kind") and ("Error") when they change
    void on_reuse() override;                             // the starter source
};
inline constexpr const char* kShaderStarterSource = /* the text below */;
```

  The starter source, exactly:

```glsl
shader surface;

// Each uniform is an attribute of the Material that uses this shader.
uniform vec3 Tint : color = vec3(1.0);

void surface() {
    ALBEDO *= Tint;
}
```

  Lua class `Shader`, base `Instance`: `Source` (string, writable, through `set_source`), `Kind` (`Enum.ShaderKind`, read-only), `Error` (string, read-only). A new Shader starts with the starter source (constructor and `on_reuse`), so it is never empty and never in error.

- [ ] **Step 1: Write the failing tests** in `sandbox/shader_tests.cpp`, tag `[shader]`:
  - `SHA1` a new Shader under `game.Assets.Shaders` has the starter source, Kind Surface, Error "", and `parse()->params` holds `Tint`.
  - `SHA2` setting `Source` to the Vignette example makes Kind Post; to a source with a bad hint on line 4 makes Error `"4: unknown hint glow"`; `Changed` fires for Source, Kind, and Error once each.
  - `SHA3` placement: a Shader is refused outside `Assets.Shaders` (and its Folders) with the category message; a Material is refused in Shaders.
  - `SHA4` undo of a Source edit puts Source, Kind, and Error back.
  - `SHA5` Stop after a playtest edit puts Source, Kind, and Error back (Review Focus).
  - `SHA6` a script: `Instance.new("Shader")`, set Source, read Kind and Error, parent under `game.Assets.Shaders`; writing Kind or Error raises.
  - Update `GS2` in `sandbox/game_services_tests.cpp` to expect `Shaders` last in the category list.
- [ ] **Step 2:** Run `build/Release/sandbox.exe "[shader],GS2*"`. Expected: compile failure on `Shader.hpp`, then (after a stub) failures for each case.
- [ ] **Step 3:** Implement the class, the enum, the service, the category, and the registrations listed under Files.
- [ ] **Step 4:** Run `build/Release/sandbox.exe "[shader],[GS1],[GS2],[GS4]"` and then the whole sandbox. Expected: all pass.
- [ ] **Step 5:** Commit: `Shaders: a Shader asset under Assets.Shaders parses its source into Kind, Error, and its uniforms`.

---

### Task 4: Shaders on disk as .glsl, and copy and paste

**Files:**
- Modify: `src/engine_core/Project.cpp`: `kGitattributes` (:84-90) adds `*.glsl text eol=lf`; `scan()` (:775-856) collects source files of every known extension into one GUID map (two source files for one GUID fail "GUID <g> has two source files in <dir>"); `read_folder()` (:858-877) accepts `init.<ext>`; `guid_claims` (:1046-1095) and `own_folder` (:1104) know every extension; the load check (:1257-1265) fails "<path>: a <Class> keeps Source in a .<ext> file" when the file's extension is not the class's `source_extension()`; `plan_files` (:2032-2042) and the incremental `paths` lambda (:3856-3878) write `stem + "." + ext` and `init.` + ext. The known extensions are one list, `kSourceExtensions[] = {"luau", "glsl"}`, beside `kGitattributes`. `PlanNode`/`AuthoredNode` carry the extension (`const char* source_extension`, null for none) where they carry `has_source`.
- Test: `sandbox/shader_tests.cpp`

**Interfaces:**
- Consumes: Task 2's `SourceText::source_extension()`, Task 3's `Shader`.
- Produces: nothing new for later tasks.

- [ ] **Step 1: Write the failing tests**, tag `[shader][project]`:
  - `SHA7` save writes `src/Assets/Shaders/Dissolve.<guid>.glsl` holding the source byte for byte and `Dissolve.<guid>.meta.json` without a Source key; load reads it back with Kind and Error recomputed.
  - `SHA8` rename the Shader and save: the new `.glsl` and `.meta.json` exist and the old ones are gone (Review Focus).
  - `SHA9` a Shader given a child Folder saves as `Dissolve.<guid>/init.glsl` with `init.meta.json`, and loads back.
  - `SHA10` a hand-renamed `Dissolve.<guid>.luau` fails the load with "a Shader keeps Source in a .glsl file"; a Script's source renamed to `.glsl` fails with "a Script keeps Source in a .luau file" (Review Focus).
  - `SHA11` `.gitattributes` of a new project has the `*.glsl` line.
  - `SHA12` copy and paste a Shader (`copy_tree` / `paste_copies`) and clone it: the copy has the same source and Kind; an `.aeinst` round trip keeps it.
- [ ] **Step 2:** Run `build/Release/sandbox.exe "[shader]"`. Expected: SHA7-SHA11 fail (the source is written as `.luau` or not at all); SHA12 may already pass through Task 2's casts.
- [ ] **Step 3:** Implement the Project changes.
- [ ] **Step 4:** Run `build/Release/sandbox.exe "[shader],[project]"`, then the whole sandbox. Expected: all pass, P50 included.
- [ ] **Step 5:** Commit: `Shaders: a Shader's source is a .glsl file beside its .meta.json`.

---

### Task 5: The shader editor

**Files:**
- Create: `src/ide/GlslHighlight.hpp/.cpp`, `src/ide/IdeShaderEditor.hpp/.cpp`, `tests/GlslHighlightTest.cpp`
- Modify: `src/ide/IdeLayout.hpp` (`open_shaders_` map beside `open_css_` at :648, `edit_shader`, `open_shader_editor(id)`), `src/ide/IdeLayoutEditing.cpp` (`edit` dispatch at :662-701 sends a `Shader` to `edit_shader`, which mirrors `edit_css` at :750-769; `flush_editors` :945, `close_script_editors` :1010/:1036, `editors_unflushed` :1049 walk `open_shaders_` too), `src/ide/IdeAssets.cpp` (double-click at :1576-1586 and the item menu at :897-923 run Edit for a Shader as for a Prefab), `tests/LuaEngineTest.cpp:571` (call `RunGlslHighlightTests`), `CMakeLists.txt` (studio and engine-tests sources)
- Test: `tests/GlslHighlightTest.cpp` (engine-tests), `tests/StudioLayoutTest.cpp` (studio-tests)

**Interfaces:**
- Produces:

```cpp
struct GlslSpan { const char* style = nullptr; int length = 0; };   // as CssSpan: code points
std::vector<GlslSpan> highlight_glsl(std::string_view source);
// Enter and { as the CSS editor has them (CssHighlight.hpp:25-40), for a brace language.
// Each may call enter_css / brace_css when those key only on braces; tests pin GLSL's cases.
CssEnter enter_glsl(std::string_view source, int caret, std::string_view indent);
CssBrace brace_glsl(std::string_view source, int caret);

class IdeShaderEditor : public IdePane {
public:
    IdeShaderEditor(engine_core::Engine& engine, std::uint32_t id);
    std::uint32_t instanceId() const;
    bool isLoaded() const;
    std::string text() const;
    bool hasUnflushedText() const;
    void focus();
    void flush();
    // Puts the caret at the start of a 1-based line and scrolls to it.
    void showLine(int line);
    // The line the error mark covers, 1-based, or 0 for none. For tests.
    int errorLine() const;
};
```

  Styles reuse the script editor's classes: `keyword` (`if else for while do return break continue discard const in out inout uniform struct` and the header words `shader render_mode group`, and preprocessor lines from `#` to the line's end), `datatype` (`void bool int float vec2 vec3 vec4 ivec2 ivec3 ivec4 mat2 mat3 mat4 sampler2D`), `builtin` (the reserved engine names from Task 1 and the GLSL functions `texture mix clamp smoothstep step min max abs floor fract mod pow sqrt length normalize dot cross sin cos tan reflect dFdx dFdy`), `number`, `comment`. The editor is the CSS editor's shape: tab title `<Name>.glsl` that follows renames, icon `Shader.png`, Source written half a second after the last keystroke as one undo step named "Edit Shader", the buffer wins at Stop. The Shader's `Error` line gets an error `TextMark` across the line and a right-lane `ScrollMark` in `--error-color`, refreshed when Error changes.

- [ ] **Step 1: Write the failing tests:**
  - `tests/GlslHighlightTest.cpp` (`expect` harness): the Dissolve example's spans name `shader`, `uniform`, `float`, `vec3`, `sampler2D`, `void`, `discard` and `EMISSIVE`, `texture`, `smoothstep` with the right styles, numbers as `number`, and a `//` comment as `comment`; odd text (unterminated `/*`, lone `#`, empty) gives spans whose lengths sum to the code point count.
  - `tests/StudioLayoutTest.cpp`: inserting a Shader and running Edit opens one `IdeShaderEditor` tab titled `Shader.glsl`; Edit again reveals the same tab; renaming the Shader retitles it (Review Focus); typing a bad hint and flushing gives `errorLine()` that line, and fixing it gives 0; closing the tab and reopening shows the flushed text.
- [ ] **Step 2:** Run `cmake --build build --config Release --target engine-tests studio-tests`. Expected: compile failures for the missing headers.
- [ ] **Step 3:** Implement the highlighter, the editor, the Edit routing, and the Assets pane changes.
- [ ] **Step 4:** Run `build/Release/engine-tests.exe` and `build/Release/studio-tests.exe`. Expected: all pass.
- [ ] **Step 5:** Commit: `Shaders: a shader editor tab with GLSL highlighting and the Shader's error marked on its line`.

---

### Task 6: Shader errors in Problems

**Files:**
- Modify: `src/ide/IdeProblems.cpp` (`gather` :194-208 also collects Shaders whose `error()` is not empty; `refresh` :345-391 builds their `ProblemSource` from it; a refresh also runs when the place's `authored_revision` moves, so an Error change shows within a frame), `src/ide/ProblemsModel.cpp:203` (the summary says "file"/"files" when any source is not a script, else "script"/"scripts"), `src/ide/IdeLayoutEditing.cpp:178-187` (`host.open` calls `open_shader_editor(id)->showLine(line)` for a Shader)
- Test: `tests/ProblemsTest.cpp`, `tests/ProblemsModelTest.cpp`

**Interfaces:**
- Consumes: Task 3's `Shader::error()`; Task 5's `IdeLayout::open_shader_editor` and `IdeShaderEditor::showLine`.
- Produces: a Shader's problem is `Problem{line, 0, <code points on that line>, Severity::Error, "Shader", message}` in a `ProblemSource{id, name, "Shader", path, ...}`.

- [ ] **Step 1: Write the failing tests:**
  - `ProblemsTest`: a Shader with a bad hint on line 4 is a row "Dissolve" with one error at line 4; fixing its source removes the row within a refresh; opening the problem calls the host's open with that Shader's id and line 4.
  - `ProblemsModelTest`: the summary of one script and one Shader problem reads "files".
  - `StudioLayoutTest`: opening a Shader problem opens the shader editor with the caret on line 4.
- [ ] **Step 2:** Run the studio and engine tests. Expected: those cases fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `build/Release/engine-tests.exe` and `build/Release/studio-tests.exe`. Expected: all pass.
- [ ] **Step 5:** Commit: `Problems: a Shader's error is a row that opens the shader editor at its line`.

---

### Task 7: Find and replace in the shader editor

**Files:**
- Modify: `src/ide/IdeShaderEditor.hpp/.cpp`, following `IdeScriptEditor`'s find bar (`IdeScriptEditor.cpp:205-241` for construction, :323-360 for open/close/seed, `refresh_scroll_marks` :863-890 for the left-lane match marks)
- Test: `tests/StudioLayoutTest.cpp` (or `tests/FindReplaceTest.cpp` if it can host an `IdeShaderEditor`)

**Interfaces:**
- Produces on `IdeShaderEditor`: `void openFind(bool replace); void closeFind(); bool findOpen() const; int replaceAll(const std::string& query, const std::string& replacement);` with the script editor's keys (Ctrl+F, Ctrl+H; Cmd on macOS), match highlighting, left-lane scroll marks, and Escape to close. A replace reaches Source the way typing does.

- [ ] **Step 1: Write the failing test:** open the editor on the Dissolve example, Ctrl+F with the caret on `Progress` seeds the find text and counts 3 matches; replace-all `Progress` → `Amount` writes 3 replacements, and after flush the Shader's params name `Amount`; Escape closes the bar.
- [ ] **Step 2:** Run the studio tests. Expected: the test fails (no find bar).
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `build/Release/studio-tests.exe`. Expected: all pass.
- [ ] **Step 5:** Commit: `Shaders: find and replace in the shader editor`.

---

### Task 8: MCP reads and writes Shader source

**Files:**
- Modify: `src/ide/McpTools.cpp` (`ScriptAt` :554-560 accepts any `SourceText`, its message naming "a Script, ModuleScript, or Shader"; `WriteSource` :726-757 records "Edit Shader" for a Shader and, instead of `AddProblems`' Luau check, adds the Shader's `Error` as the result's problems; `get_properties` :368-373 shows "(use read_script)" for any `SourceText`; `CollectScripts` stays scripts-only), `src/ide/McpToolSpecs.cpp:69-95` (descriptions say "Script, ModuleScript, or Shader")
- Test: `tests/McpTest.cpp`

**Interfaces:**
- Consumes: Task 2's `SourceText`, Task 3's `Shader`.

- [ ] **Step 1: Write the failing tests:** `read_script` on `Assets.Shaders.Tint` returns the starter source; `write_script` with a bad hint returns a problem naming the line and message and leaves the Source written; `get_properties` on the Shader shows Source as "(use read_script)" and Kind as "Surface".
- [ ] **Step 2:** Run `build/Release/mcp-tests.exe`. Expected: those checks fail ("is a Shader, not a Script or ModuleScript").
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `build/Release/mcp-tests.exe`. Expected: "mcp tests passed".
- [ ] **Step 5:** Commit: `MCP: read_script and write_script take a Shader, and report its error`.

---

### Task 9: Docs

**Files:**
- Modify: `README.md` (a short Shaders paragraph under The studio: the Shaders category, the editor, errors in Problems, that a Shader does nothing on screen until phase 2's `Material.Shader`), `src/engine_instances/README.md` (the Shader class beside the asset classes), `src/engine_core/README.md` (SourceText and the extension list)

- [ ] **Step 1:** Write the three paragraphs.
- [ ] **Step 2:** Commit: `Docs: Shader assets and SourceText`.

---

## Self-review notes

- Spec coverage for phase 1: the asset with Source in a `.glsl` file, Kind, Error (parse errors; compile errors arrive with the renderer in phase 2), the starter source, Insert from the Assets pane, the shader editor (highlighting, auto-indent and braces, debounced one-step writes, find and replace, error marks in the text and scroll bar), Problems, MCP, `.gitattributes`, copy and paste, exports (GameExport packs `src/` already, so nothing changes there). Uniform parsing with every hint, default, limit, and refusal the spec names.
- Left for phase 2: the GLSL template splice beyond `glsl`'s blanking, compile errors, `Material.Shader`, parameters as Properties rows, the renderer. Phase 3: `ShaderEffect` and the post chain.
- Names across tasks: `SourceText`, `source_extension()`, `on_source_changed()`, `Shader::kind/error/parse`, `parse_shader`, `ShaderParse`, `ShaderParam`, `ShaderError`, `shader_error_text`, `shader_reserved_name`, `IdeShaderEditor::showLine/errorLine`, `IdeLayout::open_shader_editor`, `highlight_glsl`.
- Calls the plan makes that the spec does not: the parser keeps line counts by blanking (the spec allowed subtracting a prefix instead); the Shader icon starts as a copy of CSS.png; Problems' summary says "files" when a Shader is among the sources; `#if` branches are not evaluated by the parser.
