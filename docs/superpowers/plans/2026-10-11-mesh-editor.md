# MeshEditor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `mesh:Edit()` returns a `MeshEditor` through which scripts build and keep editing arbitrary meshes vertex by vertex, add bones, and weight vertices to them; the old `Mesh:Add*`/`Clear` move onto it.

**Architecture:** A pure model, `MeshEditor` (engine_instances), holds vertex/triangle/bone slots with stable ids and `build()`s AMESH data. A registry, `MeshEditors` (engine_core, owned by the DataModel beside `CollisionGroups`), keeps one editor per Mesh, commits each changed editor once per step into the Mesh's live copy (today's session copy, now also used stopped), writes stopped files when idle, freed, before Play, and before a save, and ends editors that no longer apply. `MeshEditorBindings.cpp` is the Lua face.

**Tech Stack:** C++17 (MSVC VS2019, /W4), Luau userdata with destructors, AMESH, Catch2 sandbox.

**Spec:** `docs/superpowers/specs/2026-10-11-mesh-editor-design.md`

## Global Constraints

- C++17 only (the sandbox builds with `cxx_std_17`; no `<span>`, no designated initializers).
- Build: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target <t> -- /m /v:minimal`. Test exes are not in ALL_BUILD; build them by name. Run: `build/Release/sandbox.exe "[tag]"`.
- Error texts, verbatim: `no vertex <id>`, `no triangle <id>`, `no bone <id>`, `this MeshEditor has ended; call mesh:Edit() again`, `a triangle needs three different vertices`, `a bone named <name> already exists`, `a bone's name must not be empty`, `bones and weights must be the same length`, `a weight must be a finite number of 0 or more`, `the weights must not all be 0`, `a bone's rest transform must have an inverse`.
- Ids are never reused within one editor; `Clear()` does not restart them.
- At most 4 influences per vertex (`kMaxInfluences`); the 4 largest are kept and renormalized.
- Bone rest transforms are mesh space; AMESH `Bone::m` is row-major, `m[row][column] = Matrix4.m[column * 4 + row]`, `t = m[12..14]` (as `make_skeleton` reads it).
- Idle write after 1 s without edits; a written live copy is dropped 2 s after the write (MeshCache rechecks a file at most once a second).
- One refinement over the spec: a stopped `Edit()` on a Mesh with no Path gives it its Path and an empty file at once, inside the caller's undo step (the spec said "on its first write"); the spec is updated to say so in Task 5.
- Engine distances are "units"; never write "studs".
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. Undo of the Path that `Edit()` gave a Mesh while edits are pending: the editor must end and drop its live copy, never write to the old path. Test: R4.
2. An imported skinned mesh opened and committed unchanged keeps its skeleton and weights (quantized weights must round-trip). Test: MB7.
3. A script that holds an editor across Play or Stop gets the documented error, not a crash or a write into the wrong world. Tests: R5, L4.
4. A bone removed while vertices still weight it, or a vertex removed while triangles use it, leaves no dangling index in `build()`. Tests: ME1, MB2.
5. Same-frame reads (`GetBone`, `GetBoneNames`, `GetBoundingBox`, `OriginOffset`) see this frame's edits. Test: L3.

---

### Task 1: The `MeshEditor` model

**Files:**
- Create: `src/engine_instances/MeshEditor.hpp`, `src/engine_instances/MeshEditor.cpp`
- Create: `sandbox/mesh_editor_tests.cpp`
- Modify: `CMakeLists.txt` (engine_instances after `src/engine_instances/MeshShapes.cpp`; sandbox after `sandbox/mesh_shapes_tests.cpp`)

**Interfaces:**
- Produces: `engine_core::MeshEditError : std::runtime_error`; `engine_core::MeshEditor` with `Id = std::uint32_t`, `Influence = std::pair<Id, float>`, `Owner {InstanceId mesh; std::uint32_t generation; bool playing;}`, `explicit MeshEditor(const anarchy::amesh::Data& = {})`, `add_vertex(Vec3) -> Id`, `remove_vertex(Id)`, `vertices() -> std::vector<Id>`, `position/set_position`, `normal(Id) -> Vec3`, `set_normal(Id, std::optional<Vec3>)`, `uv/set_uv (Vec2)`, `color(Id) -> std::array<float,4>`, `set_color(Id, const std::array<float,4>&)`, `add_triangle(Id,Id,Id) -> Id`, `remove_triangle(Id)`, `triangles()`, `triangle_vertices(Id) -> std::array<Id,3>`, `add_shape(const std::function<void(anarchy::amesh::Data&)>&) -> std::vector<Id>`, `clear()`, `add_bone(const std::string&, std::optional<Id>, const Matrix4&) -> Id`, `remove_bone(Id)`, `bones()`, `find_bone(const std::string&) -> std::optional<Id>`, `bone_name/set_bone_name`, `bone_parent/set_bone_parent (std::optional<Id>)`, `bone_transform/set_bone_transform (Matrix4)`, `set_vertex_bones(Id, const std::vector<Id>&, const std::vector<float>*)`, `vertex_bones(Id) -> const std::vector<Influence>&`, `build() const -> anarchy::amesh::Data`, `edits() -> std::uint64_t`, public `Owner owner`, `ended()`, `end()`.

- [ ] **Step 1: Write the failing tests** — `sandbox/mesh_editor_tests.cpp`, tag `[mesh-editor]`:
  - ME1 ids survive removals; `remove_vertex` removes its triangles; `build()` compacts in id order. Vertices a(0,0,0) b(1,0,0) c(0,0,-1) d(1,0,-1); t1 = (a,b,c), t2 = (b,d,c); remove a → `vertices() == {b,c,d}`, `triangles() == {t2}`, build indices `{0,2,1}`; `position(a)` throws "no vertex 0".
  - ME2 `remove_triangle` keeps its vertices; a repeated vertex in `add_triangle` throws; missing ids throw.
  - ME3 normals: an unset normal is computed from its triangles ((0,1,0) for the two triangles above, and for a lone vertex); a set normal is normalized and kept by `build()`; `set_normal(nullopt)` goes back to computed; zero length throws.
  - ME4 `add_shape(add_box)` returns 24 ids in order, and `build()` keeps the box's normals.
  - ME5 `MeshEditor(data).build()` gives the same positions, normals, UVs, colors, and indices back.
  - ME6 `clear()`: old ids are dead, new ids carry on.
  - ME7 UV and color round-trip (color through bytes, within 1/255), a non-finite position throws.
  - MB1 bones come out parents-first after `set_bone_parent` reorders them: A, B (parent A), C; `set_bone_parent(A, C)` → build order C, A, B with parents remapped.
  - MB2 `remove_bone` moves children to its parent (keeping their rest) and renormalizes; a vertex left with no weights is bound to the root by `build()`.
  - MB3 weights: default equal shares; more than four keeps the four largest, renormalized; duplicates merge; length mismatch, negative, and all-zero throw; an empty list clears.
  - MB4 an unweighted vertex in a skinned build is the first root's at weight 1, while `vertex_bones` stays empty.
  - MB5 `cull_radius` reaches the farthest weighted vertex.
  - MB6 duplicate and empty names, a parent cycle, and a singular rest throw; a rename frees the old name.
  - MB7 a skinned build opened again and built gives the same bones and weights (within 1/255 after an AMESH write and read).
  - MB8 the skinned build writes and reads as AMESH, and `make_skeleton` sees each rest transform as given.
- [ ] **Step 2: Run, expect a compile failure (no MeshEditor.hpp).**
- [ ] **Step 3: Implement** `MeshEditor.hpp/.cpp`:
  - Slots: `VertexSlot {live, p, normal_set, n, uv, rgba{255,255,255,255}, skin, triangles}`, `TriangleSlot {live, corners}`, `BoneSlot {live, name, parent, rest}`, `bone_names_` (name → id), live counts, `edits_`, `ended_`.
  - Lookups throw `MeshEditError("no vertex N")` etc. for a dead or out-of-range id.
  - Constructor: vertices with `normal_set = true`, skins from `bone/weight` pairs with weight > 0, triangles from all indices, bones from `rest_of(bone)`.
  - `normal()` sums `cross(b - a, c - a)` over its triangles, as `amesh::compute_normals` does.
  - `build()`: bones parents-first (walk each live bone's unplaced ancestors, place them root-down), first root = lowest live id with no parent, vertices in id order with skins remapped (an unweighted vertex → root at 1), cull radius from each bone's rest origin, triangles in id order, `compute_normals` then the set normals put back, `compute_tangents`, `compute_aabb`, `FLAG_SKINNED` with bones and `FLAG_VERTEX_COLOR` with any non-white.
  - Limits: `kMaxVertices`, `kMaxTriangles`, `kMaxBones`, counted on live slots.
- [ ] **Step 4: Build sandbox and run `[mesh-editor]`; expect PASS.**
- [ ] **Step 5: Commit** `MeshEditor: a mesh's vertices, triangles, and bones under stable ids, built into AMESH`.

### Task 2: The Mesh's live copy and the `MeshEditors` registry

**Files:**
- Modify: `src/engine_instances/AssetInstances.hpp/.cpp` (Mesh)
- Create: `src/engine_core/MeshEditors.hpp`, `src/engine_core/MeshEditors.cpp`
- Modify: `src/engine_core/DataModel.hpp/.cpp`, `src/engine_core/DataModelState.hpp` (own a `MeshEditors`, `mesh_editors()`)
- Modify: `sandbox/support.hpp` (`ScriptRig` keeps a test clock; `frames` steps the registry)
- Create: `sandbox/mesh_editors_tests.cpp`; Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `MeshEditor`.
- Produces: `Mesh::set_live_geometry(std::shared_ptr<const anarchy::amesh::Data>)`, `Mesh::drop_live_geometry()`, `Mesh::read_geometry(anarchy::amesh::Data&) const -> std::optional<std::string>`, `Mesh::prepare_file() -> std::optional<std::string>`, `Mesh::write_geometry(const anarchy::amesh::Data&) -> std::optional<std::string>`; `session_geometry()` now returns a stopped copy while stopped and a playing copy while playing. `MeshEditors` with `Clock = std::chrono::steady_clock`, `kIdleWrite = 1000ms`, `kDropAfterWrite = 2000ms`, `open(DataModel&, Mesh&, std::string& error) -> std::shared_ptr<MeshEditor>`, `usable(const DataModel&, const MeshEditor&) const -> bool`, `commit(DataModel&)`, `step(DataModel&, Clock::time_point)`, `write_all(DataModel&, bool end)`, `size()`. `DataModel::mesh_editors() const -> MeshEditors&`. `ScriptRig::clock`.

- [ ] **Step 1: Write the failing tests** — `sandbox/mesh_editors_tests.cpp`, tag `[mesh-editors]`, with an `EditorRig : ScriptRig` holding a temp resources folder:
  - R1 one editor per Mesh; opening stopped gives a Mesh with no Path `meshes/<Name>.<guid>.amesh` and an empty file; a file with LODs and a game with no project are refused with their reasons.
  - R2 an edit draws as the live copy at the next step and the file waits: not written after half a second idle, written after one second, the live copy dropped two seconds after the write.
  - R3 an editor nobody else holds is written at the next step and forgotten.
  - R4 a Path change (an undo of the one `open` set) or another writer changing the file ends the editor and drops its live copy, and nothing is written.
  - R5 playing: a commit makes the session copy, the file is never written, Stop ends the editor, and the next Play starts from the file.
  - R6 a destroyed Mesh ends its editor.
- [ ] **Step 2: Run, expect a compile failure.**
- [ ] **Step 3: Implement.** Mesh: `session_playing_` beside `session_generation_`; `session_geometry()` also requires `session_playing_ == simulation_running()`; the revision counter moves to a shared `next_geometry_revision()`; `edit_geometry`'s playing branch calls `set_live_geometry`. `read_geometry` is the live copy, else `read_file` (refusing LODs). `prepare_file` refuses without a resources folder, picks today's `meshes/<Name>.<guid>.amesh` when there is no Path, writes an empty AMESH when there is no file, and sets the Path. `write_geometry` writes to Path. `MeshEditors` as the spec's section 2: entries hold the editor, `built`/`seen` edit counts, when edits last moved, `unwritten`, the Path and file stamp at open or last write, and an optional drop time. `step` ends editors that are not `usable`, or (stopped) whose Path or file stamp changed (dropping their live copy), commits, writes a stopped editor that is unwritten and idle or held only by the registry, drops a live copy past its drop time, and forgets ended editors. A write failure warns through `game.warn`.
- [ ] **Step 4: Run `[mesh-editors]` and `[shapes]`; expect PASS.**
- [ ] **Step 5: Commit** `MeshEditors: one editor per Mesh, committed each step to a live copy and written when idle`.

### Task 3: Play, save, and the engine loop

**Files:**
- Modify: `src/engine_core/DataModelPlace.cpp` (`start_simulation`: `write_all(*this, true)` before the flag flips)
- Modify: `src/engine_core/Project.cpp` (`save_tree`: `write_all(world, false)` when stopped)
- Modify: `src/engine_core/Engine.cpp` (play step: `step` after the scripts' and tools' event drain; stopped tick: `step` first in the stopped-physics block)
- Test: `sandbox/mesh_editors_tests.cpp`

- [ ] **Step 1: Write the failing tests:** H1 Play writes pending stopped edits, drops the stopped copy, and ends the editor; H2 a project save writes pending edits and the editor carries on.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement the three hooks.**
- [ ] **Step 4: Run `[mesh-editors]`, `[shapes]`, `[bone]`; expect PASS.**
- [ ] **Step 5: Commit** `MeshEditors: written before Play and a save, and stepped by the engine`.

### Task 4: Lua: `Mesh:Edit` and `MeshEditor`

**Files:**
- Create: `src/engine_core/MeshEditorBindings.cpp`; Modify: `CMakeLists.txt` (engine_core after `SkeletonBindings.cpp`)
- Modify: `src/engine_core/ScriptBindings.hpp/.cpp` (drop `mesh_add_*`, `mesh_clear`, `edit_mesh`, the shape argument helpers; add `mesh_edit`, `game_of`, `open_mesh_editor`; commit before `GetBoundingBox`)
- Modify: `src/engine_core/SkeletonBindings.cpp` (commit before `GetBone`, `GetBoneNames`)
- Modify: `src/engine_instances/AssetInstances.cpp` (`read_origin_offset` commits on the gameplay thread); remove `Mesh::edit_geometry`
- Modify: `src/engine_core/ScriptRuntime.cpp` (`open_mesh_editor(state)`)
- Modify: `src/engine_core/LuaApi.cpp` (docs: drop Mesh `Add*`/`Clear`; add `Mesh:Edit`, the `MeshEditor` type, every method)
- Modify: `sandbox/mesh_shapes_tests.cpp` (port to `:Edit()`; `ShapeRig::run` settles as the old immediate methods did)
- Create: `sandbox/mesh_editor_lua_tests.cpp`, tag `[mesh-editor-lua]`

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: Lua `Mesh:Edit() -> MeshEditor` and every method in the spec's section 1.

- [ ] **Step 1: Write the failing tests:** L1 `Edit()` twice is the same editor (`==`, and one sees the other's vertices); L2 primitives return their vertex ids; L3 a script builds a two-bone tube with blended weights, then in the same chunk `GetBoneNames`, `GetBone("Tip").Transform`, an `Offset` that moves it, and `OriginOffset` all see the edits; L4 an editor held across Play errors with the documented text; L5 every error text in Global Constraints reaches Lua; L6 the old `Mesh:AddBox` is gone. Port `[shapes]`.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** the bindings (userdata with a destructor holding `std::shared_ptr<MeshEditor>`, methods table as `__index`, `__eq`, `__tostring`, `__type`; every method checks `usable` first; a `MeshEditError` becomes a Lua error raised outside the catch), the flushes, the docs, and the removals.
- [ ] **Step 4: Build sandbox, run `[mesh-editor-lua]`, `[shapes]`, `[bone]`, `[mesh-editor]`, `[mesh-editors]`, then the whole sandbox; expect PASS.**
- [ ] **Step 5: Commit** `Lua: Mesh:Edit and MeshEditor; the Mesh shape methods move onto it`.

### Task 5: Docs and the live check

**Files:**
- Modify: `README.md` (the Mesh paragraph), `src/engine_instances/README.md`, `src/amesh/amesh.hpp` (header comment), the spec (Path at `Edit()`)

- [ ] **Step 1: Rewrite the docs** to `mesh:Edit()`.
- [ ] **Step 2: Build Release `AnarchyStudio`, `AnarchyPlayer`, and `sandbox`; run the whole sandbox.**
- [ ] **Step 3: Live:** launch Release Studio over MCP; in the command bar build a two-bone tube with weights blended along its length on a GameObject; set the tip `Bone.Offset`; screenshot that the tube and its shadow bend; Play, Stop; check the file is on disk.
- [ ] **Step 4: Commit** `Docs: MeshEditor in the READMEs`.
