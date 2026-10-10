# Anarchy Engine Code Audit

| | |
|---|---|
| **Date** | 2026-10-10 |
| **Commit** | `2676e0b` on `main` |
| **Scope** | All of `src/` (105,028 lines), the pipeline shaders under `resources/`, `tests/`, `sandbox/`, and the build files |
| **Looking for** | Illogical code, dead code, duplicated code, obviously buggy code |

> **Bottom line.** The code is in good shape. Nothing found here crashes the studio, corrupts a project, or loses work; the eleven critical and high findings of the September review are fixed. What remains is one Medium rendering bug that can silently turn off point and spot shadows for a session, one Medium copy-paste omission that hides a revived Dragger, a handful of Low bugs, several places where the README, the tests, and the code disagree about defaults and class hierarchy, and a large amount of copy-pasted helper code (roughly 1,500 lines) that has already started to drift.

Line numbers refer to commit `2676e0b`. Paths drop the `src/` prefix where the file name is unique.

**Method.** Every source file was read in full across seven subsystem reviews (core data layer; scripting; physics, terrain, and audio; instances and services; renderer and shaders; IDE shell, panels, and MCP; IDE editors, terminal, and importers). Each finding below was re-checked by hand against the source and its callers. Dead-code claims were checked against `src/`, `tests/`, `sandbox/`, and `resources/` (Lua plugins), since the unit tests live in `sandbox/`. Two mechanical passes ran over the whole tree: a header-declared-but-never-referenced scan and a 12-line duplicate-block scan ignoring whitespace and comments. Nothing was run under a sanitizer and no code was changed.

## Contents

1. [Status of the September review](#1-status-of-the-september-review)
2. [Bugs](#2-bugs)
3. [Illogical code and documentation drift](#3-illogical-code-and-documentation-drift)
4. [Dead code](#4-dead-code)
5. [Duplicated code](#5-duplicated-code)
6. [Fix plan](#6-fix-plan)
7. [Checked and ruled out](#7-checked-and-ruled-out)

---

## 1. Status of the September review

`docs/reviews/2026-09-28-code-quality-review.md` listed eleven verified defects. 842 commits later, spot checks show each one addressed:

| Finding | Evidence it is fixed |
|---|---|
| C1/C2 coroutine and callback refs leaked | `ScriptRuntime.cpp:81, 1374` call `lua_unref` |
| C3 Lua exceptions terminate the studio | `Engine.cpp:207-211` catches `ContractViolation`, `std::exception`, and `...` |
| C4 full event queue aborts | `Events.cpp:379` `EventQueue::grow()` |
| C6 non-transactional save | `Project.cpp:256, 2861` temp-then-rename with an undo journal |
| C7 `Color3.fromHSV` NaN hue | `Color3.cpp:203` `std::isfinite` check |
| C8 completion parser recursion | `LuauComplete.cpp:333` depth tracking |

No `#if 0` block and no `TODO`, `FIXME`, `XXX`, or `HACK` marker exists anywhere in `src/`. Every `.cpp` under `src/`, `tests/`, and `sandbox/` is listed in a CMake target, and every header is included somewhere.

---

## 2. Bugs

Ranked by severity, then confidence. **Latent** means the bad path exists but nothing in the current code reaches it.

### B1. A stale GL error turns off point and spot shadows for the whole session — Medium

`runner/ShadowRenderer.cpp:95-96`

```cpp
unsigned texture = MakeDepth(size, pages, nullptr);
bool made = glGetError() == GL_NO_ERROR;
```

`glGetError` returns the oldest *unread* error, and nothing in the per-frame path drains errors before this call (`Renderer::initialize` drains once at startup, `Renderer.cpp:409`). An error left pending by the UI layer, a texture upload, or an earlier pass in the same frame makes the first atlas allocation look refused. The fallback at `:216-219` then sets `refused_ = true`, which only `shutdown()` clears (`:85`), and every later call in `:180, 292, 318, 374` returns early. Result: every PointLight and SpotLight loses its shadows until the view is recreated, with no console message tying the loss to the real cause.

**Fix.** Drain `glGetError()` in a loop before `MakeDepth`, or rely on `glCheckFramebufferStatus` alone (which the code already does on the next line). Consider logging once when `refuse()` fires.

### B2. A Dragger brought back by undo or Stop is invisible to the dragger query — Medium

`engine_core/DataModelPlace.cpp:187-204` vs `engine_core/DataModel.cpp:579-599`

`spawn()` tags a new entity with `steps`, `physics_body`, `terrain`, `sound_source`, `dragger`, `billboard`, and `wireframe`. `adopt_slot()`, the path `revive_record` (undo of a Delete, redo of a Create, `DataModelHistory.cpp:341`) and `restore_record` (`DataModelPlace.cpp:225`) use to re-create an instance in a pooled slot, repeats the same list minus the `dragger()` branch. A Dragger is `Instance.new`-able (`ScriptBindings.cpp:248`). Delete one, press Ctrl+Z: it is back in the tree and in Properties, but `DataModel::draggers()` (`DataModel.cpp:959`) omits it, so `DraggerWorld::dispatch` never hovers or drags it and `SnapshotPump::resolve_draggers` (`SnapshotPump.cpp:641`) never draws its handles.

**Fix.** One `tag_entity(Slot&, DataModel&)` helper called from both `spawn` and `adopt_slot`. The duplication is exactly where the omission crept in.

### B3. `Color3:ToHSV()` is declared to the type checker as returning one number — Medium

`engine_datatypes/Color3.cpp:55-64, 177`; `engine_core/LuaApi.cpp:810, 817`

The binding returns three values (`return 3;`), but the class registration says `lua_method("ToHSV", "number", nullptr)` and both doc entries give `"number"`. `AnalysisDefinitions.cpp:89-92` emits `function ToHSV(self): number`, so `local h, s, v = color:ToHSV(); print(s * 2)` gets a Type diagnostic (an error under `--!strict`) on correct code. Every other multi-return method follows the convention Matrix4 documents at `Matrix4.cpp:840-841`: null field type and a comma-separated doc type, as `ToAxisAngle` does at `LuaApi.cpp:913`.

**Fix.** `lua_method("ToHSV", nullptr, nullptr)` and doc type `"number,number,number"` for both `ToHSV` and `toHSV`. Add an analysis test.

### B4. Terrain cache records are trusted without bounds checks — Medium, latent

`engine_core/terrain/AlodStore.cpp:287-306`

A record is accepted when its key matches and the reader consumed exactly the record's bytes. Neither `indices16 % 3 == 0` nor `index < vertices` is checked, and `u16s` leaves its output unsized when the count exceeds the remaining bytes. A bit-flipped or half-written `.alod` record therefore comes back as a `CompactMesh` whose indices address vertices that do not exist; `LodNode.cpp:196-203` copies them verbatim and `LodBuilder.cpp:19-22` indexes `mesh.vertices[index]` unchecked, as does the GPU upload. The header comment at `AlodStore.hpp:21-23` promises that a record that "does not check out" fails `open()`, which records never do.

**Fix.** After the reads: require `positions.size() == vertices * 3`, `indices16 % 3 == 0`, `indices32 % 3 == 0`, and every index below `vertices`; return `nullptr` otherwise, and treat it as a cache miss.

### B5. Project folders with non-ANSI characters break the terminal and a dialog on Windows — Medium

`ide/IdeLayoutProject.cpp:65, 1127`

```cpp
host.folder = [this] { return project_ ? project_->root().string() : std::string(); };
options.directory = project_->root().string();
```

On MSVC `path::string()` yields the ANSI code page, but `PtyWindows.cpp:49` decodes the cwd as UTF-8 and `FolderDialogOptions::directory` is `u8string()` everywhere else (`:766`). A project under a folder such as `Straße` opens its terminal in the wrong directory, or fails `CreateProcessW`, and opens the profile save dialog in the wrong folder. Line `:1115` has the same mistake in an error message.

**Fix.** Use the `utf8_path` helper the rest of the file uses.

### B6. Interpolated-string tokens span from the start of the file — Low

`ide/LuauComplete.cpp:203`

```cpp
emit(Token::String, 0, i, {});
```

Every other `emit` passes the token's start. Hover resolution picks the first token containing the caret, so hovering whitespace anywhere before a backtick string resolves to that string. Today the hover then reports "not found", so nothing visible breaks, but any future reader of `.begin` on String tokens is wrong for backtick strings. One-line fix: pass `start`.

### B7. `check_enum_arg` accepts non-integral numbers by truncation — Low

`engine_datatypes/Enum.cpp:407-411`

`lua_tointeger` truncates, so `part.Shape = 1.9` is taken as item 1 instead of the argument error the header promises (`Enum.hpp:353-355`). Check that `lua_tonumber` is integral first.

### B8. A Brush body driving a GameObject with a Prefab rebuilds its hull on every move — Low

`engine_core/PhysicsWorld.cpp:1842-1848, 1359-1375`

`make_brush_shape` never sets `record.prefab` and pins `record.center` to the origin, but `recenter` compares both against Prefab-derived values. When the driven GameObject has a Prefab with a non-zero origin offset, `center_for()` is never near zero, so every externally applied move (a Move-tool drag, a script write) remakes the hull: a full `brush::hull_pieces` clip for the same result, and each remake drops the body's contacts, which the file's own comment at `:211-214` says makes resting stacks creep. Record `center_for`/`prefab_guid` as `make_object_shape` does, or skip `recenter` for Brush bodies as it already does for controllers.

### B9. `TaskScheduler::unbind` on a render-phase job keeps the closure and the entry — Low, latent

`engine_core/TaskScheduler.cpp:292-295, 339-351`; header promise at `TaskScheduler.hpp:35-36`

Render-phase entries are only marked `retired`, never erased, and their `std::function` is kept. `ScriptRuntime::attach`/`detach` bind and unbind a `RenderStepped` job, so each cycle leaks one entry capturing `this` of a destroyed runtime, and after 64 cycles `bind_job` aborts with "phase job capacity exhausted" (`:308-309`). Production has one runtime per engine, so only test rigs that re-attach can hit it.

### B10. `GetProfile` leaves the profiler acquired on exception — Low

`ide/McpTools.cpp:1579-1601`: `profiler::acquire()` and `release()` bracket a call that can throw; every other tool reports errors by throwing. Wrap in a scope guard.

### B11. Process-attribute list leaked on a partial failure — Low

`ide/PtyWindows.cpp:373-391` and `ide/RunProcess.cpp:451-456`: when `InitializeProcThreadAttributeList` succeeds and `UpdateProcThreadAttribute` fails, `DeleteProcThreadAttributeList` is skipped. Error path only.

### B12. Terrain array mip sizes use two rounding conventions — Low, latent

`runner/Renderer.cpp:1561` allocates levels with `(n + 1) / 2`; `:1743` and `TerrainLevelFits` validate with `n >> level`. For a non-power-of-two size the chains diverge at the small levels, `validFirst` never descends, and the Terrain stays on the flat-colour path. `TerrainTextures.cpp:25-36` only yields 256 to 2048 today. GL itself requires floor-halved mips, so the allocation side is the one to change, through one shared `mip_size(base, level)` helper (`LayerBuilder.cpp:172` is a third copy).

### B13. A voxel-size change keeps colliders built at the old scale — Low, latent

`engine_core/TerrainWorld.cpp:818-831` clears meshes, jobs, and batches but not `collider_map`/`colliders_vec`, so PhysicsWorld keeps the old-scale shapes until each chunk is re-walked. Dormant because `Terrain.cpp:573` fixes VoxelSize at 1.

### B14. Optimistic rename retitles the editor tab before validation — Low, latent

`ide/IdeLayoutEditing.cpp:519-521` and `ide/IdeExplorer.cpp:683-686` set the new name in the UI, then ask the simulation; a refused rename leaves the tab showing the refused name because the editor only re-reads on a tree revision change. `rename_error` today refuses only services and dead ids, which cannot have an open editor.

### B15. `EventQueue::count(WriteOrigin::SnapshotOverride)` is always zero — Low

`engine_core/Events.cpp:389-393` returns before the per-origin counter increments for that origin; the real count is `suppressed_overrides_`. A trap for the next reader, not a behaviour bug.

---

## 3. Illogical code and documentation drift

### D1. DynamicSky defaults: the code, the README, and the tests say three different things — Medium

| Property | Code (`DynamicSky.hpp:43-54`) | README.md:35 | `sandbox/dynamic_sky_tests.cpp:62-71` |
|---|---|---|---|
| Brightness | 2 | 3 | 3 |
| SunSize / MoonSize | 4 / 4 | 2 / 2 | 2 / 2 |
| CloudCover | 0.3 | 0.3 | 0.5 |
| CloudDensity | 0 | 0 | 0.5 |

Commit `2d00273` changed Brightness and the disc sizes deliberately ("new sky and roughness defaults") without touching the README or the tests, and the tests were already wrong about the clouds after `a3d2c5b`. This is the "DS1/DS3" red test your notes call a pre-existing flake. Pick the code as the source of truth, then fix the README and the tests.

### D2. The README still says Camera and the lights are GameObject subclasses

`README.md:47` and `src/engine_instances/README.md:7` describe `Camera`, `PointLight`, and `SpotLight` as GameObject subclasses. Since `ab794e8` they register under `PVInstance` (`Camera.cpp:111`, `Light.cpp:366`), and `camera:IsA("GameObject")` is false. The instances README class list also omits `Brush`, `Dragger`, `WireframeAdornment`, and the Light classes.

### D3. The README's PhysicsObject paragraph describes the pre-decomposition behaviour

`README.md:43` says `Size` is "the box a Hull's or Custom's mesh is fitted to" and that an unanchored Custom becomes a Hull "with a warning". The code (`PhysicsWorld.cpp:125-127`) and test P37 (`sandbox/physics_tests.cpp:1228`) say Size plays no part for Hull and Custom; an unanchored Custom is V-HACD convex pieces (`:2069-2107`) and the warning fires only when no piece can be built (`:2086-2092`).

### D4. "studs" survives in user-facing text despite the units rule

README.md lines 23, 35, 39, 41 ("studs per second", "0 to 1000 studs"); `LuaApi.cpp:1037, 1048, 1080, 1121, 1204, 1242, 1253` (doc strings shown in the editor); and comments in `SnapshotPump.hpp:118, 153, 164`, `AmbientOcclusionEffect.hpp:21`, `ScreenSpaceReflections.hpp:19`, `DynamicSky.hpp:28`, `Dragger.hpp:19`, `Terrain.hpp:26`, `SceneService.hpp:49`.

### D5. Slider ranges that disagree with the property's write

- `ScreenSpaceReflections.cpp:136-138` slider max is a literal `200.0` while the write clamps to `kMaxMaxDistance = 1000` and the README says 1000. Every other slider in the file uses the constant.
- `AssetInstances.cpp:917-919` registers `TextureScale` with a slider from 0, but `set_texture_scale` (`:620-622`) refuses `v <= 0`, so the slider's left stop is a refused write with an error.

### D6. Light `Color3` is treated as linear while every other `Color3` is sRGB-decoded

`runner/Renderer.hpp:171-172` comments "Linear, as the Color3 holds it", but `MeshDraw::color` and `tint` are documented sRGB and decoded (`surface.glsl:56`, `DrawBatches.cpp:46`, `Renderer.cpp:1851`); `SnapshotPump.cpp:67-70` copies the light's colour raw and `lighting.glsl:100` uses it undecoded. A Light and a Material both set to 0.5 grey differ by about 2.3x in linear light. Separately, `Renderer.cpp:1847-1851` and `DrawBatches.cpp:46` claim to decode "as surface.glsl's toLinear" but use a 2.2 power where the shader uses the piecewise sRGB curve. Decide the intended convention and make the three sites agree.

### D7. Alpha-tested surfaces cast solid shadows

`deferred.frag:19-21` and `forward.frag:33` discard cut-out pixels, but `shadow.vert` takes only position and `shadow.frag` is empty, so a leaf or fence texture shadows as a full quad. The README's "see-through objects cast no shadow" is honoured for Transparency (`ShadowRenderer.cpp:201`); this is the alpha-cutout case.

### D8. Stale or misleading comments and specs

- `ide/McpToolSpecs.cpp:48`: the `parent` description lists four scene services and omits `Gui`. `:54-67`: `import_assets` says "image, sound, and model files" but its `files` property says "image and model files". The LLM client is told the wrong thing.
- `runner/Renderer.hpp:104-107`: "nothing draws textured yet"; `terrain.frag:104-118` draws textured.
- `runner/Visibility.hpp:41-43`: `screenRadius` is "0 until LOD selection fills it"; no LOD selection exists and the value is computed per visible draw every frame for no reader outside tests.
- `engine_core/TerrainWorld.hpp:108-110` and `PhysicsWorld.hpp:89-90, 145-146` describe `set_collider_interest` as PhysicsWorld's call path; PhysicsWorld never calls it (only `sandbox/` does).
- `ide/PropertiesPanel.cpp:48-49`: a comment about lock waits now annotates `kPad = 6`.
- `ide/IdeLayout.cpp:166`: a View menu item `"Maybe :)"` with no action.
- `ide/LuauComplete.cpp:2300, 2387-2389`: `plan_hover` takes `world`, `script_id`, `script_global` and `(void)`s all three; `hover_luau` threads a real `script_global` into the ignored slot.
- `engine_core/ScriptAnalysis.cpp:1402`: `(with_self || (fn.hasSelf && with_self))` is just `with_self`. `:1986-1987` computes a receiver name and discards it.
- `engine_core/ScriptRuntime.hpp:240`: `Park` has five states but only `== None` is ever read.
- `engine_core/PhysicsWorld.cpp:2519-2527`: `collision_outline` builds a full Box3D BVH and destroys it just to test buildability, on every outline refresh.
- `engine_core/DataModel.cpp:2198-2208` says a class property wins over a stale extra of the same name; `InstanceFile.cpp:192-195` and `Project.cpp:1076-1082` let the extra win. Needs a colliding extra from an older build's `.aeinst` to matter; the three sites should share one helper with one rule.

---

## 4. Dead code

Verified against `src/`, `tests/`, `sandbox/`, and `resources/`. Everything here is declared and defined and has no caller anywhere.

| Location | What | Note |
|---|---|---|
| `engine_core/DataModel.hpp:382`, `.cpp:1143` | `ancestry_changed()` | The `AncestryChanged` signal kind and emit path exist, but no Lua binding or C++ caller can connect to it. Either expose it or remove the accessor. |
| `engine_core/PhysicsWorld.cpp:209` | `same_vec3()` | |
| `engine_core/terrain/VoxelVolume.hpp:91` | `has_dirty()` | |
| `engine_core/terrain/AlodStore.hpp:81`, `.cpp:383-390` | `live_bytes()` | `file_bytes()` is the one tests use |
| `engine_core/BrushVisuals.cpp:187-194` | `texture_scales` lambda takes `const Brush&` and `(void)`s it | Both call sites pass `*brush` for nothing |
| `engine_core/PluginUi.hpp:108-110, 156`, `.cpp:399` | `activations()` / `activations_` | Comment says the Scene View reads it; nothing does |
| `engine_core/LuaEngine.hpp:20, 29-32` | `HostArgs::isNil`, `pushNil`, `pushBoolean`, `pushString` | |
| `engine_core/SnapshotPump.hpp:309`, `.cpp:157` | `set_camera()` and the `camera_pending_`/`pending_camera_` plumbing it feeds | |
| `engine_services/SceneService.hpp:35, 38` | `is_scene_service_class()`, `scene_service_guid()` | |
| `ide/IdeLayout.hpp:474`, `IdeLayoutProject.cpp:33` | `conflicts_pane()` | |
| `ide/EditorFont.hpp:16`, `.cpp:222` | `editor_font_choice()` | |
| `ide/IdePrefabEditor.hpp:89`, `.cpp:1162` | `newModelTile()` | |
| `ide/CompletionPopup.hpp:48`, `.cpp:731` | `replaceEnd()` | |
| `ide/IdeExplorer.hpp:186` | `std::shared_ptr<jadefx::Menu> menu_` | Zero references in the `.cpp`; replaced by `InsertPopup` |
| `ide/IdeExplorer.cpp:33` | `kActionWait(250)` | Third name for one number, with `layout_detail::kActionWait` and `kDropLockWait` |
| `runner/Renderer.hpp:480-484, 503, 520-527` | 14 `Program` uniform-location members (`diffuse`, `normalMap`, `depth`, `albedo`, `nodeLevel`, ...) | Assigned in `buildProgram` (`Renderer.cpp:256-284`), never read; sampler units are bound by the separate `sampler` lambda. `uNodeLevel` is also uploaded every terrain draw (`:2165`) to a uniform `terrain.frag:43` never reads. |
| `runner/gl.hpp:324, 345, 387, 392` and `gl.cpp` | `glGetTexParameteriv`, `glUniform1fv`, `glQueryCounter`, `glGetInteger64v` loaders | |

Also: `.worktrees/luau-ac/` (257 MB) is a worktree checkout that `git worktree list` no longer knows about and only `.git/info/exclude` hides. It can be deleted.

Test probes (called only from `sandbox/`) were checked and are **not** listed: `body_count`, `voice_count`, `watch_global`, `queued_with_args`, `can_suspend`, `write_depth`, `pause_for_test`, `set_budget`, `decode_level`, `instance_for`, `place_fingerprint`, and the `runner/*Math.hpp` CPU mirrors of shader math, among others.

---

## 5. Duplicated code

Ordered by how much a shared helper would remove and how much the copies have already drifted.

### P1. Instance property boilerplate, copied into 18 files (~400 lines)

Byte-identical anonymous-namespace helpers across `engine_instances/` and `engine_services/`: `number_slot` in 18 files, `bool_slot` in 10, `color_slot` in 7, `refuse(LuaSlot&, optional<string>)` in 14, `require_thread` in 10 (differing only in the message), `number_json` in 11, `quality_slot` in 3, a `read_number<Get>`/`write_number<Set>` template pair in 9, and `set_number(property, slot, value, max)` with a 0..max clamp in 4. `PhysicsBase.hpp:112-150` already exports the generic form as `physics_detail::`; hoisting it to `engine_core/LuaSlotHelpers.hpp` is the whole fix.

On top of that: twelve copies of the same 12-line enum setter (`set_quality`, `set_reflection_quality`, `set_antialiasing`, `set_terrain_quality`, `set_tone_mapping`, `set_shading_model`, `set_roll_off_mode`, `set_shape`, `set_texture_size`, `set_offset_space`, `set_space`, `set_streaming`), eight copies of the Color3 setter (`GameObject`, `Light`, `DirectionalLight`, `Lighting`, `Material`, `Skybox`, `Brush`, `WireframeAdornment`, the last comparing r/g/b by hand instead of `same_color`), and four copies of a bool setter in `Light.cpp`.

### P2. Luau binding helpers, six and five copies

- "self instance of type T or raise": `ScriptBindings.cpp:1210-1228` (two), `CameraBindings.cpp:103-111`, `WireframeBindings.cpp:277-286`, `BrushBindings.cpp:657-665`, `TerrainBindings.cpp:208-216`. One `template <class T> T& instance_self(lua_State*)`.
- "Vector3 argument or raise": `TerrainBindings.cpp:81-87`, `WireframeBindings.cpp:142-148`, `RaycastBindings.cpp:119-125`, `Matrix4.cpp:405-411`, `ScriptBindings.cpp:1183-1192`, `BrushBindings.cpp:47-56`. Home: `LuaUserdata.hpp`.
- `points_arg` verbatim in `WireframeBindings.cpp:163-177` and `BrushBindings.cpp:416-430`.
- Datatype metatable installation repeated in `Vector2.cpp:251-280`, `Color3.cpp:130-154`, `Matrix4.cpp:802-826`, `BrushBindings.cpp:696-715`, with the `Operator` struct duplicated between Vector2 and Matrix4.
- Weak-valued registry cache: creation byte-identical at `ScriptRuntime.cpp:727-733` and `PluginBindings.cpp:462-468`; lookup-or-insert in `push_instance`, `set_plugin_global`, `plugin_get_mouse`.

### P3. Four Luau lexers with diverging keyword sets

`LuauHighlight.cpp` (`Scanner`), `LuauComplete.cpp:101-285` (`Tokenize`), `ScriptPairs.cpp:88-531` (`Scan::tokenize`), and `ScriptPairs.cpp:1174-1290` (`fold_ranges_luau`) each implement long brackets, strings, comments, numbers, and name classification. `LongSeparator` at `LuauComplete.cpp:69-87` and `ScriptPairs.cpp:28-46` are character-identical. They disagree on whether `type` is a keyword (`ScriptPairs.cpp:23-25` yes, `LuauComplete.cpp:50` no). B6 lives in one of these copies. One tokenizer with a keyword-set parameter.

### P4. `IdeCssEditor` is a trimmed copy of `IdeScriptEditor`'s persistence core (~150 lines)

`IdeCssEditor.cpp:99-104, 156-203, 236-314` mirror `IdeScriptEditor.cpp:152-156, 746-804, 1107-1214`: the `acked/epoch` handshake, the `try_begin_recording` guard, the "stopped wins" generation logic. They have drifted: the CSS version checks `acked != epoch` before the `missing_` reload, the Script version after. A shared source-editor base keeps them in step.

### P5. `IdePrefabEditor` and `IdeTerrainEditor` share ~250 lines

`text_label`, `spacer`, `icon_box`, `icon_button`, class `NameField`, `sync_cards`, `beginRename`, `finish_rename`, `show_selection`, and the `pending_add_`/`pending_picker_` blocks in `layoutChildren` differ only in the CSS prefix (`pe-`/`te-`) and view type. `TerrainMaterials.hpp:17` even says it was "Patterned on PrefabModels". The 12-line duplicate scan flagged `IdePrefabEditor.cpp:273` vs `IdeTerrainEditor.cpp:264` as the longest exact match in the tree (26 lines).

### P6. IDE pane boilerplate

- `text_label(text, style_class)`: identical in `IdeSearch.cpp:113`, `IdeProblems.cpp:101`, `IdeAssets.cpp:245`, `IdeConflicts.cpp:57`, `IdePrefabEditor.cpp:249`, `IdeTerrainEditor.cpp:256`, `AssetPicker.cpp:100`. `spacer()` in four files.
- "set/has style class": `NodeClasses.hpp:9-28` is the shared home, yet `IdeLayoutInternal.hpp:531-540`, `AssetPicker.cpp:339-349`, `IdePrefabEditor.cpp:256-270`, and `IdeAssets.cpp:392-400` each redefine it.
- "N thing / N things": `Strings.hpp:42` `counted`, `IdeLayoutInternal.hpp:86` `Counted`, `IdeLayoutProject.cpp:205` `Count`, same body.
- ASCII lowering: `Strings.hpp:26` `AsciiLower` exists, yet `AssetChoices.cpp:96`, `AssetPicker.cpp:325`, `McpSetup.cpp:63`, `TextureImport.cpp:17`, `ModelImport.cpp:45`, `AssetImport.cpp:97`, `AssetBrowser.cpp:117`, `EditorFont.cpp:606` each roll their own, some with locale-aware `std::tolower`, so asset search and class filter can disagree on non-ASCII names.
- The scrolling-list core of `CompletionPopup.cpp:426-560` and `InsertPopup.cpp:596-700` (`rowExtent`, `windowStart`, `clampScroll`, `reveal`, `scrollBy`, `relayout`, `pressScroll`) is the same ~130 lines; the completion-popup key handling is repeated between `IdeScriptEditor.cpp:1347-1371` and `IdeConsole.cpp:342-366`.
- "finished alerts purge" 5-liner in seven places (`IdeLayout.cpp:1086`, `IdeLayoutEditing.cpp:962`, `IdeLayoutProject.cpp:411, 717, 997`, `PreferencesPanel.cpp:765, 943`); `InsertResult` completion block in three; tree-pick `clicked()` walk in `IdeSearch`, `IdeProblems`, `IdeConflicts`; `ResultsTree`/`ProblemsTree` identical subclasses; `collect`/`gather` script-tree walks; `FindBar::setReplaceShown` vs `IdeSearch::setReplaceShown`.
- Windows argument quoting: `RunProcess.cpp:330-355` `QuoteArg` and `PtyWindows.cpp:135-162` `AppendArgument` are the same algorithm line for line.
- Importer suffix probing (`kMaxSuffix`, `name-2`, `name-3`) in `TextureImport.cpp:62-88` and `ModelImport.cpp:56-67`; `is_sound_file`/`is_texture_file`/`is_model_file` same body against three lists.

### P7. Core and simulation

- `DataModel.cpp:937-1003`: six identical "run cached query, push ids" functions (`physics_bodies`, `terrains`, `draggers`, `billboards`, `sound_sources`, `wireframes`) plus `step_instances` and `stepper_count`; ~70 lines for one `collect_ids(query, out)`.
- `DataModel.cpp:579-599` vs `DataModelPlace.cpp:187-204`: the tag block where B2 crept in.
- `DataModel.cpp:735-767`: `set_simulated`/`set_visual_only` identical but for tag id and field.
- `InstanceFile.cpp:266-281`: `save_instance_file` wraps `write_file` in a second temp-then-rename that `write_file` (`FileBytes.hpp:339-371`) already does, and builds its error with `path.u8string()` instead of `utf8_path`.
- `TerrainWorld.cpp:311-326, 490-498, 570-583, 656-662, 1039-1043`: five copies of "queue a chunk job" (draw revision, write `pending_jobs`, `chunk_queued`, build `MeshInput`, `mesher_.queue`). The stale-result logic depends on the order of these steps.
- `PhysicsWorld.cpp:279-293, 2364-2385, 2400-2412`: three `b3MeshDef` builders differing in one flag.
- World point to chunk coordinate written out at `PhysicsWorld.cpp:820-824, 1098-1100` and `TerrainWorld.cpp:463-465, 515-517`.
- Four private FNV-1a hashes (`ConvexDecomposition.cpp:38-53`, `AlodStore.cpp:158-165`, `TextureBake.cpp:15-27`, `BrushVisuals.cpp:24-36`) and three little-endian codecs (`AvoxFile.cpp:30-77`, `AlodStore.cpp:16-111`, `Atex.cpp:16-31`).
- Trilinear sampling (`VoxelSampler.cpp:51-72` vs `SurfaceNets.cpp:134-150`), `floor_div` (`LodNode.cpp:13` vs `VoxelChunk.cpp:18`, with a comment admitting it), and dot/cross/normalize helpers in `DraggerMath.cpp:14-23` and `LodBuilder.cpp:26-31` with no shared header in `engine_core`.
- `SnapshotPump.cpp:368-379, 482-494`: identical `texture_path` lambdas. `Events.cpp:238-247, 303-312`: identical `Restore` RAII structs. `Project.cpp:618-626` vs `:810-818`: identical parent-index loops.
- `ScriptRuntime.cpp:1765-1774` vs `:1815-1824` dead-plugin sweep; `:267-278` vs `:554-563` scheduler pass; `LuaEngine::print`/`interrupt` mirror `ScriptRuntime::lua_print`/`interrupt`.

### P8. Renderer

- `GameView.cpp:424-438` and `:545-559`: material-from-`VisualMesh` block (five texture lookups plus five scalar copies), already differing in how `transparency` is combined.
- File stat-and-reload boilerplate in `MeshCache.cpp:190-209`, `TextureCache.cpp:441-467, 633-651`, `GuiTree.cpp:524-545`.
- `glDeleteFramebuffers`-if-nonzero loops in six places while `DeleteTexture(GLuint&)` exists; 1x1 placeholder texture creation in `Renderer.cpp:466-473` and `TextureCache.cpp:429-436`; `outlinePass`/`handlePass` (`Renderer.cpp:1466-1513`) the same 20 lines; the per-array anisotropy block at `Renderer.cpp:2150-2158` repeated for two arrays and re-applied every draw; `terrain.frag:157-213` three copies of an 18-line triplanar sample.

---

## 6. Fix plan

Six small, independent batches, each a single commit with its own tests, in the order they pay off. No batch depends on another. Estimated effort assumes one person familiar with the code.

### Batch 1. Behaviour bugs (half a day)

1. **B1** `ShadowRenderer::makeAtlas`: drain `glGetError()` before `MakeDepth`; log once in `refuse()`. Test: a `shadow_atlas_tests.cpp` case that leaves a GL error pending (if the test harness has a GL context) or at minimum a unit test on the refusal path.
2. **B2** Extract `tag_entity()` from `spawn` and `adopt_slot`. Test in `sandbox/dragger_tests.cpp`: create a Dragger, delete, undo, assert it is in `draggers()`.
3. **B3** `ToHSV`/`toHSV` return type. Test in `sandbox/analysis_tests.cpp`: `local h, s, v = c:ToHSV()` produces no Type diagnostic under `--!strict`.
4. **B5** `utf8_path` at `IdeLayoutProject.cpp:65, 1115, 1127`.
5. **B6**, **B7**, **B10**, **B11**: one-line to five-line fixes each; add a Luau test for `part.Shape = 1.9` being refused.

### Batch 2. Latent data-integrity holes (half a day)

6. **B4** Validate `AlodStore::load` records (vertex count vs positions, index multiple of three, index range). Test: corrupt a record's index bytes in `terrain_lod_tests.cpp` and assert a cache miss, not a mesh.
7. **B8** Record `center_for`/`prefab_guid` in `make_brush_shape`. Test in `physics_tests.cpp`: a Brush driving a GameObject with an offset Prefab; move it; assert `shapes_made` does not grow.
8. **B12** One `mip_size()` helper for `Renderer.cpp:1561, 1743` and `LayerBuilder.cpp:172`.
9. **B9**, **B13**, **B15**: small, do them while in the files.

### Batch 3. Docs, defaults, and sliders (two hours)

10. **D1** Decide the DynamicSky defaults (recommend: keep the code's 2 / 4 / 4 / 0.3 / 0, since `2d00273` chose them on purpose), then fix README.md:35 and `dynamic_sky_tests.cpp:62-71`. This turns the DS1/DS3 flake green.
11. **D2**, **D3**: rewrite README.md:43 and :47 and `engine_instances/README.md:3-7` to match `ab794e8` and the decomposition spec.
12. **D4**: replace "studs" with "units" in README.md, the seven `LuaApi.cpp` doc strings, and the nine header comments.
13. **D5**: SSR slider max to `kMaxMaxDistance`; TextureScale slider min to a small positive value (or teach `lua_slider` an exclusive minimum).
14. **D8**: the stale comments and the `"Maybe :)"` menu item; fix the MCP `parent` and `import_assets` descriptions.

### Batch 4. Rendering conventions (half a day, needs a decision)

15. **D6** Decide whether Light `Color3` is sRGB like every other Color3. If yes, decode in `SnapshotPump.cpp:67-70` (or in `lightPass`) and update `Renderer.hpp:171`. Also replace the two `pow(2.2)` tint decodes with the piecewise sRGB curve so Tint and Material.Color agree. Check existing places visually, since this changes how every light looks; `profile-warmed-up` notes apply when comparing.
16. **D7** Alpha-cutout shadows: give `shadow.frag` an optional diffuse sampler and the same discard threshold as `deferred.frag`, enabled per batch when the material has a diffuse texture with alpha. Optional; cost is one texture fetch in the shadow pass.

### Batch 5. Dead code (one hour)

17. Remove everything in section 4 except `ancestry_changed`, which should instead get a Lua binding (`AncestryChanged` is the natural fifth instance signal and the emit path already exists) or be removed together with its `SignalKind` and emit code.
18. Delete `.worktrees/luau-ac/` (257 MB).

### Batch 6. Duplication (two to three days, in this order)

Each item is its own commit so a regression bisects cleanly. Run `sandbox/` and the `tests/` programs after each.

19. **P1** `engine_core/LuaSlotHelpers.hpp`: hoist `physics_detail` (`number_slot`, `bool_slot`, `color_slot`, `refuse`, `require_thread<Msg>`, `number_json`, `read_number`/`write_number`, `set_clamped_number`), then a `set_enum_property<E>` and `set_color_property` following `Lighting.cpp:212-237`. Convert the 18 files. ~400 lines out.
20. **P7** `tag_entity`, `collect_ids`, the `TerrainWorld::queue_chunk` helper, one `b3MeshDef` builder, `chunk_at_local`, one FNV-1a in `engine_core/Hash.hpp`, one little-endian codec, and a `VectorMath.hpp` use in `DraggerMath`/`LodBuilder`. Delete the outer rename in `save_instance_file`.
21. **P2** `instance_self<T>`, `check_vector3`, one `install_datatype_metatable` helper, one registry weak-cache helper.
22. **P3** One Luau tokenizer with a keyword-set parameter, used by `LuauHighlight`, `LuauComplete`, and both `ScriptPairs` scanners. Fix B6 on the way. This is the riskiest item; the highlight and completion test programs in `tests/` cover it.
23. **P4** A `SourceEditorCore` shared by `IdeScriptEditor` and `IdeCssEditor`; resolve the `acked != epoch` ordering drift deliberately.
24. **P5**/**P6** `ide/CardWidgets.hpp` (`text_label`, `spacer`, `icon_box`, `icon_button`, `NameField`), a `CardEditorBase` for the Prefab and Terrain editors, everyone on `NodeClasses.hpp`/`Strings.hpp`, a `prune_alerts` helper, `InsertResult::finish`, a shared `ScrollingRows` for the two popups, `CompletionPopup::handleKey`, one Windows quoting helper in `RunProcess.hpp`, and `extension_in`/`free_name` for the importers.
25. **P8** `MaterialDraw(const VisualMesh&, TextureCache&)`, `ResourceStamp`, `DeleteFramebuffer(GLuint&)`, a 1x1 texture helper, merge `outlinePass`/`handlePass`, move anisotropy to the quality-change path, and a `sampleAxis` function in `terrain.frag`. Drop the 14 unread `Program` members and the `uNodeLevel` upload.

---

## 7. Checked and ruled out

So the next reader does not re-chase them:

- **Locks.** Every MCP-thread DataModel access goes through `ReadLock`/`RunEdit`; `SelectionService`, `UserInputService`, and script output have their own locks. `IdeExplorer::run_on_selection` re-takes the read lock inside `offers()` (`IdeExplorer.cpp:369-381`), which can spuriously drop an item from a multi-select action if a writer is waiting; low value, noted only.
- **Lua stack discipline.** `push_instance`, `plugin_get_mouse`, `matrix4_tostring`, `terrain_write_voxels`, `print_source`, and the `__index` closures all balance. `lua_pushvalue`/`lua_xmove` in the vendored Luau call `ensure_stack`, so the missing `lua_checkstack` in `task_thread` is not a bug. Escaping `std::exception`s are converted by `luaD_rawrunprotected`.
- **Physics.** Sphere radius is `size.x * 0.5` (README "diameter in X"); Capsule `half = (size.y - size.x) / 2`; "first in tree moves it" is implemented by `first_in_tree_order`; Box3D's hull edge limit is 128, so `hull_pieces(shape, 64, 64)` fits; miniaudio's `none` attenuation really does disable panning.
- **Instances.** Every setter fires `Changed` exactly once; equal-value writes return early; every README range for Skybox, Bloom, AO, SSR Intensity/MaxRoughness, SoundEmitter, Camera, PlayerController, TerrainMaterial matches the clamps; IsA hierarchies match the README (except D2); containment for the five Lighting classes is one function in `Containment.cpp:139`.
- **Renderer.** No UBOs, so no std140 issues; every FBO is completeness-checked; `destroyTargets` chains the bloom, reflection, and occlusion teardown; only the first shadowed DirectionalLight casts; Transparency > 0 casts nothing; absent Bloom/SSR/AO allocate nothing.
- **IDE.** `TextUndoStack` is symmetric; `TerminalScreen` delegates CSI parsing to libvterm and bounds-checks its own indexing; `PtyWindows` closes every handle on every path except B11; `ThumbnailLoader` only calls thread-safe JadeFX entry points; `SavedLayout` round-trips symmetrically; `JsonValue` accessors are permissive so malformed MCP arguments cannot crash; MCP `kSpecs` and `kToolCode` match one-to-one and the registry enforces it at startup.
- **Core.** Slot generation and free-list handling, `grow_ring`, `DenseIdSet::erase`, UTF-8 fitting, JSON surrogate handling, `save_tree`'s journal, `outside_changes`, and the `contract_fail` temporary lifetime all hold up.
