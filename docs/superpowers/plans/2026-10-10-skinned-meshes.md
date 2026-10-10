# Skinned Meshes (Skinning Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Imported rigged models keep their bones; each GameObject has its own pose, posed through `Bone` instances (additive `Offset`) and drawn with GPU skinning that keeps instanced batching.

**Architecture:** An immutable `Skeleton` is built from an AMESH bone table and cached on the `Mesh`. A GameObject computes its `Pose` lazily from its Prefab's skeleton and its `Bone` children's Offsets, cached until those inputs change, so an unchanged pose costs a child walk and nothing else. The snapshot pump hands each skinned row its shared, immutable `Pose`. The renderer packs the visible poses' skinning matrices into one RGBA32F texture, and the vertex shaders blend up to four of them per vertex, keyed by a per-instance `boneBase`.

**Tech Stack:** C++17, OpenGL 3.3 core, Assimp 5.3.1 (studio only), Catch2 (sandbox), the custom studio-tests harness.

**Spec:** `docs/superpowers/specs/2026-10-10-skinned-meshes-design.md`

## Global Constraints

- Engine distances are "units", never "studs" (comments, docs, messages).
- No implicit Bone creation: `GetBone` only finds; `AddBone` only makes.
- `Bone.Offset` is additive, right-multiplied after the bone's (animated) local transform.
- AMESH format/version unchanged.
- Static meshes keep their look and cost: same batching, no extra draw calls.
- Windows/MSVC: `/W4` clean for touched code; use `MSYS_NO_PATHCONV=1` for cmake from Git Bash.
- Build and test Release: `cmake --build build --config Release --target <t>`; sandbox runs from the repo root: `build/Release/sandbox.exe "[tag]"`.

## Deviations from the spec (decided while planning)

1. **The pose is computed lazily, not in a per-frame step after PreAnimation.** `GameObject::pose()` rebuilds only when its inputs (skeleton, Bone children's Names and Offsets, child order) differ from the cached ones. This gives the same results and frame-order guarantee (a read always sees current inputs), and a posed statue still costs nothing to rebuild. Sub-project 3's Animator joins those inputs.
2. **The pose lives on `GameObject`, behind a mutex, not in a flecs component.** Nothing iterates poses in bulk, and both the simulation thread (Bone.Transform) and the pump, under the DataModel lock, read them.
3. **There is one vertex program, not a `SKINNED` variant.** `geometry.vert` and `shadow.vert` branch on the per-instance `aBoneBase >= 0`. The branch is uniform across each instance, so the cost is nil, and runs keep a single program.
4. **Bone rest poses are in model space**, the space the importer already bakes static meshes into, rather than "the skeleton root's space". At rest every skinning matrix is still identity.
5. **A vertex whose weights sum to 0 is drawn unskinned** (the shader leaves it in place). The importer still binds every skinned vertex to some bone.

## Review Focus

- **A skinned FBX/glTF whose mesh node sits under a rotated or scaled parent.** The mesh must import in the same place it does today, not offset or rotated. Pinned by the Task 7 test, where the mesh node has a translation.
- **A Prefab whose Models have different bone tables, or whose mesh file is replaced on disk.** It must draw sensibly (bind pose) without crashing, and pick up the new skeleton within a second. Pinned in Tasks 2 and 5.
- **A Bone under a GameObject whose Transform is scaled or mirrored.** `Transform` must stay consistent with what is drawn, including the GameObject's `Scale` property. Pinned in Task 3.
- **Writing Bone.Transform, then undo.** It must undo as an Offset change. Pinned in Task 3.
- **Many skinned instances.** 100 instances must stay one run per Model, and must not overflow the bone texture's rows. Pinned in Task 6.

---

### Task 1: Skeleton and pose math

**Files:**
- Create: `src/engine_core/Skeleton.hpp`, `src/engine_core/Skeleton.cpp`
- Create: `sandbox/skeleton_tests.cpp`
- Modify: `CMakeLists.txt` (engine_core sources; sandbox sources)
- Modify: `src/amesh/amesh.hpp` (bone-field comment)

**Interfaces:**
- Produces:
```cpp
namespace engine_core {
struct Skeleton {
    struct Bone {
        std::string name;
        std::uint16_t parent = 0xFFFF;   // kNoBone
        Matrix4 rest;          // model space
        Matrix4 rest_local;    // parent's rest⁻¹ × rest (rest for a root)
        Matrix4 inverse_bind;  // rest⁻¹
        float cull_radius = 0.f;
    };
    std::vector<Bone> bones;
    std::vector<std::uint16_t> order;  // parents before children
    std::uint64_t signature = 0;       // names + parents + rest; equal tables, equal signature
    int find(std::string_view name) const;  // first bone so named, -1 for none
};
std::shared_ptr<const Skeleton> make_skeleton(const std::vector<anarchy::amesh::Bone>& bones);  // null when empty

struct PoseInput { std::uint16_t bone; Matrix4 offset; InstanceId owner; };
struct Pose {
    std::shared_ptr<const Skeleton> skeleton;
    std::vector<Matrix4> locals;    // before Offset (rest_local now; Animator later)
    std::vector<Matrix4> globals;   // model space, Offsets applied
    std::vector<float> palette;     // 12 per bone: rows 0..2 of globals × inverse_bind
    std::vector<InstanceId> owners; // per bone, the Bone instance that posed it, or 0
    Vec3 low{}, high{};             // model-space box: bone origins grown by cull_radius
};
Pose compute_pose(std::shared_ptr<const Skeleton> skeleton, const std::vector<PoseInput>& inputs);
}
```

- [ ] **Step 1: Write the failing tests** in `sandbox/skeleton_tests.cpp`, tag `[skeleton]`:
  - A three-bone chain: root at the origin, `mid` 1 unit up, `tip` 1 unit further, rotated 90° about Z. `make_skeleton` gives `rest_local[mid]` as a translation of (0,1,0), `inverse_bind × rest = I`, `find("tip") == 2`, and `find("nope") == -1`.
  - `compute_pose` with no inputs: every palette entry is the identity rows, `globals == rest`, and the box contains each origin grown by `cull_radius`.
  - An input rotating `mid` 90° about Z: `globals[tip]` origin moves to `mid`'s origin + R × (0,1,0), and `owners[mid]` is the input's owner.
  - Additivity: an Offset translation (1,0,0) on `mid` moves `mid`'s global origin by `rest_mid` rotation × (1,0,0).
  - Bones listed child-before-parent in the source table still compute (the order is topological).
  - Equal tables give equal signatures; a renamed bone changes the signature.
- [ ] **Step 2: Run** `cmake --build build --config Release --target sandbox`. Expected: a link/compile failure (no Skeleton.hpp).
- [ ] **Step 3: Implement.**
  - **Rest matrices:** `rest` is built from `amesh::Bone::m` (row-major 3×3) as `M.m[c*4+r] = m[r][c]`, with `t` in `m[12..14]`.
  - **Order:** a DFS from the roots, since the reader rejects cycles.
  - **Signature:** FNV-1a over the names, parents and the 12 floats of each bone.
  - **Pose:** walk `order`. `local = rest_local × offset`, where the offset is the last input for that bone (the caller dedupes). `global = global[parent] × local`. `palette = rows of global × inverse_bind`: three rows of four floats, row r = (M[0][r], M[4+r], M[8+r], M[12+r]).
  - **Comment:** update `amesh.hpp`'s comment to say `m`/`t` are the rest transform in model space, and that `m` may carry scale.
- [ ] **Step 4: Run** `build/Release/sandbox.exe "[skeleton]"`. Expected: all pass.
- [ ] **Step 5: Commit** "Skeleton and pose math for skinned meshes".

### Task 2: Mesh::skeleton and the Prefab's skeleton

**Files:**
- Modify: `src/engine_instances/AssetInstances.hpp`/`.cpp` (Mesh)
- Create: `src/engine_core/Skinning.hpp`, `src/engine_core/Skinning.cpp` (`prefab_skeleton`)
- Test: `sandbox/skeleton_tests.cpp`

**Interfaces:**
- Consumes: `make_skeleton`.
- Produces:
  - `std::shared_ptr<const Skeleton> Mesh::skeleton() const`: from this session's geometry, otherwise the file. Cached by session revision, Path and root. The file's time on disk is looked at again at most once a second. Null for no bones. Needs the DataModel lock; a read lock is enough.
  - `std::shared_ptr<const Skeleton> prefab_skeleton(const DataModel& game, const std::string& prefab_guid)`: the skeleton of the first Model, in child order, whose Mesh has bones. Null for none.
  - `bool mesh_poses_with(const Mesh& mesh, const Skeleton& skeleton)`: the Mesh's skeleton has the same signature.

- [ ] **Step 1: Failing tests** (`[skeleton]`):
  - Write a two-bone AMESH (a triangle; vertices weighted to bones 0 and 1) into a `TempDir`'s `meshes/arm.amesh` with `amesh::write`, then `game.set_resources_root(dir.path)`.
  - A Mesh with that Path has a 2-bone skeleton named as written; one with a static AMESH has none.
  - Rewrite the file with three bones and wait 1.1 s: `skeleton()` has three.
  - `prefab_skeleton`: a Prefab with a static Model first and the arm Model second gives the arm skeleton. An empty Prefab gives null.
- [ ] **Step 2: Run** and confirm the tests fail to compile.
- [ ] **Step 3: Implement.**
  - Members: `skeleton_mutex_`, `skeleton_revision_`, `skeleton_file_`, `skeleton_stamp_`, `skeleton_checked_` (a steady_clock time point), `skeleton_tried_`, `skeleton_`.
  - Reading the file uses the existing `read_file(..., allow_lods=true)`.
  - Reset all of these in `on_reuse`.
- [ ] **Step 4: Run** `[skeleton]`. Expected: pass.
- [ ] **Step 5: Commit** "Mesh skeletons, cached like its bounds".

### Task 3: Bone instance and GameObject::pose

**Files:**
- Create: `src/engine_instances/Bone.hpp`, `src/engine_instances/Bone.cpp`
- Modify: `src/engine_instances/Attachment.hpp` (`set_transform` virtual)
- Modify: `src/engine_instances/GameObject.hpp`/`.cpp` (`pose()`)
- Modify: `src/engine_core/Project.cpp:153`, `src/engine_core/ScriptBindings.cpp:201,247`, `src/engine_core/LuaApi.cpp:996`, `src/ide/ClassOrder.hpp:39`, `CMakeLists.txt`
- Create: `sandbox/bone_tests.cpp`

**Interfaces:**
- Consumes: `prefab_skeleton`, `compute_pose`.
- Produces:
  - `std::shared_ptr<const Pose> GameObject::pose() const`: null when the Prefab has no skeleton.
    - Inputs: the skeleton pointer, plus the ordered list of (bone index, Offset, Bone id) for each Bone child whose Name matches. The first Bone by child order claims a name.
    - It rebuilds only when those inputs differ from the cached ones.
  - `class Bone : public Attachment`, class name "Bone":
    - `int bone_index() const`: the bone this Bone poses, or -1 when unmatched (no skeleton, no such name, a duplicate, or its parent is not a GameObject).
    - `transform()`: matched, it is `GameObject.Transform × S(Scale) × pose.globals[i]`; otherwise `Attachment::transform()`.
    - `set_transform(T)`: matched, it solves `Offset = locals[i]⁻¹ × G_parent⁻¹ × (W·S)⁻¹ × T`, where `G_parent` is identity for a root.
  - Lua class "Bone", base "Attachment". Creatable, with suited parents {"GameObject"}.

- [ ] **Step 1: Failing tests** (`[bone]`), built on Task 2's two-bone file (root at the origin, `Hand` child 2 units up):
  - BONE1: an unposed `Bone` named `Hand` has `Transform == GO.Transform × rest[Hand]`.
  - BONE2: an Offset rotating `Root` 90° about Z moves `Hand`'s Bone.Transform origin to (-2, 0, 0) relative to the GameObject. It is additive: Hand's own Offset still applies on top.
  - BONE3: with the GameObject rotated 45° about Y, translated (5,0,0) and with Scale 2, `Hand`'s origin is at GO × S(2) × (0,2,0).
  - BONE4: setting `Hand.Transform` to a target and reading it back round-trips (1e-4). Undo after a recorded step restores the previous Offset.
  - BONE5: Name `"Nope"` acts as a plain Attachment (parent × Offset). A second `Hand` Bone acts as a plain Attachment, and `bone_index() == -1`.
  - BONE6: `pose()` returns the same pointer on two reads with no change, and a new one after an Offset write.
- [ ] **Step 2: Run** and confirm the tests fail to compile.
- [ ] **Step 3: Implement.**
  - **Bone.cpp:** registers the Lua class (no new fields; it inherits Offset, OffsetSpace and Transform) and its suited parents.
  - **Lazy inputs:** `on_parent_changed` and an Offset write need no hooks, because `pose()` compares its inputs.
  - **Registration sites:** add Bone everywhere Attachment is listed: `Project.cpp`, `create_bone` + `register_lua_creatable`, the docs `add("Bone", ...)`, ClassOrder `{"Bone", 7}`, and the CMake engine_instances list.
- [ ] **Step 4: Run** `[bone]` and `[attachment]`. Expected: pass.
- [ ] **Step 5: Commit** "Bone: an Attachment that poses a bone of its GameObject".

### Task 4: GetBoneNames, GetBone, AddBone

**Files:**
- Create: `src/engine_core/SkeletonBindings.cpp`
- Modify: `src/engine_core/ScriptBindings.hpp` (`game_object_self`, `link_skeleton_methods`, three method decls), `ScriptBindings.cpp:1588` (link call), `LuaApi.cpp` (docs), `CMakeLists.txt`
- Test: `sandbox/bone_tests.cpp`

**Interfaces:**
- Produces Lua methods on GameObject:
  - `GetBoneNames() -> {string}`: empty when not skinned.
  - `GetBone(name) -> Bone?`: finds the claiming Bone, never creates one.
  - `AddBone(name) -> Bone`:
    - errors "GameObject has no bone named X" when the skeleton lacks the name;
    - errors "GameObject already has a Bone for X; use GetBone" when one already exists;
    - otherwise creates the Bone, names it, parents it to self and returns it.

- [ ] **Step 1: Failing tests** (`[bone]`), with a ScriptRig on the two-bone Prefab. The script checks:
  - `#obj:GetBoneNames() == 2`;
  - `obj:GetBone("Hand") == nil`;
  - `local b = obj:AddBone("Hand")`, then `b.ClassName == "Bone"` and `obj:GetBone("Hand") == b`;
  - `pcall(obj.AddBone, obj, "Hand")` fails with the "already has" message;
  - `pcall(obj.AddBone, obj, "Nope")` fails with "no bone named".
- [ ] **Step 2: Run** and confirm failure ("GetBoneNames" is not a member).
- [ ] **Step 3: Implement** after `BrushBindings.cpp`'s pattern: `register_lua_class("GameObject", nullptr, methods, n)`, `lua_guard`, and `lua_create_instance(*runtime->game_, "Bone")`.
- [ ] **Step 4: Run** `[bone]`. Expected: pass.
- [ ] **Step 5: Commit** "GameObject:GetBoneNames, GetBone, AddBone".

### Task 5: Snapshot carries the pose

**Files:**
- Modify: `src/engine_core/SnapshotPump.hpp`/`.cpp`
- Test: `sandbox/bone_tests.cpp`

**Interfaces:**
- Produces:
  - `VisualInstance::pose`: `std::shared_ptr<const Pose>`, null for an unskinned row.
  - `VisualMesh::skinned`: `bool`, true when the Model's Mesh poses with the Prefab's skeleton.
  - `VisualPrefab::skeleton`: `std::shared_ptr<const Skeleton>`.
  - `resolve_prefabs` fills these. A new `resolve_poses(game)`, run after it in `take_changes`, sets `row.pose = game_object->pose()` for each row whose prefab entry has a skeleton, and resets it otherwise.

- [ ] **Step 1: Failing tests** (`[bone]`), with a pump `Scene` like `prefab_render_tests`:
  - A skinned GameObject's row has a pose whose palette is 24 floats.
  - After `Hand.Offset` changes and a frame passes, the row's pose differs.
  - In a Prefab with a static Model plus the arm, the static Model's `VisualMesh::skinned == false` and the arm's is true.
- [ ] **Step 2: Run** and confirm the tests fail to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `[bone]` and `[render]`. Expected: pass.
- [ ] **Step 5: Commit** "The render snapshot carries each skinned GameObject's pose".

### Task 6: GPU skinning

**Files:**
- Create: `src/runner/BonePalette.hpp`/`.cpp` (the CPU packer, no GL) and `src/runner/BoneTexture.hpp`/`.cpp` (GL)
- Modify:
  - `src/runner/DrawBatches.hpp`/`.cpp`: `InstanceData::boneBase` (float, slot 15), `DrawItem::boneBase`
  - `src/runner/InstanceBuffer.cpp`: slot 15
  - `src/amesh/amesh.hpp`: `kAttribInstanceBoneBase = 15`
  - `src/runner/Renderer.hpp`: `MeshDraw::bones`, `boneCount`, `posed`, `poseMin`/`poseMax`
  - `src/runner/Renderer.cpp`: pack in `findVisible`, bind unit 16, `kUnitCount` 17, posed bounds for culling, `uBones` sampler
  - `src/runner/ShadowRenderer.hpp`/`.cpp`: caster rows get `boneBase`; bind the bone texture; `uBones` sampler
  - `src/runner/GameView.cpp`: fill the MeshDraw fields from `row.pose` when `source.skinned`
  - `resources/shaders/pipeline/geometry.vert`, `resources/shaders/pipeline/shadow.vert`
- Test: `sandbox/draw_batches_tests.cpp`, `sandbox/visibility_tests.cpp`, plus a new `[bones]` section in draw_batches_tests

**Interfaces:**
- Produces:
  - `class BonePalette`:
    - `void clear()`;
    - `int add(const float* palette, int bones)` returns the first texel (3 per bone) and dedupes the last pointer added;
    - `const std::vector<float>& texels() const`;
    - `int rows(int width) const`.
  - `class BoneTexture`:
    - `void upload(const BonePalette&)`;
    - `void bind(int unit) const`;
    - `void destroy()`;
    - texture width `kBoneTextureWidth = 1024` texels.
  - Shader contract:
    - `uniform sampler2D uBones;` and `layout(location=15) in float aBoneBase;`.
    - Skin matrix rows are fetched at `t = int(aBoneBase) + 3*bone + r` as `ivec2(t % 1024, t / 1024)`.

- [ ] **Step 1: Failing tests:**
  - `BonePalette`: two adds of one pointer share a base, a second pointer gets base 6 (for 2 bones), and `rows(1024)` covers the texels.
  - `BuildBatches`: 100 items with one slot and distinct boneBases make one run, and the instances keep their boneBases in order.
  - `FindVisible`: a posed item uses the posed box. A box well outside the mesh bounds, placed in view, makes it visible.
  - `static_assert(sizeof(InstanceData) == 116)`.
- [ ] **Step 2: Run** and confirm failure.
- [ ] **Step 3: Implement.** In geometry.vert:
```glsl
layout (location = 5) in uvec4 aBones;
layout (location = 6) in vec4 aWeights;
layout (location = 15) in float aBoneBase;
uniform sampler2D uBones;
mat4 boneMatrix(uint bone) {
    int t = int(aBoneBase) + 3 * int(bone);
    vec4 r0 = texelFetch(uBones, ivec2(t % 1024, t / 1024), 0); t++;
    vec4 r1 = texelFetch(uBones, ivec2(t % 1024, t / 1024), 0); t++;
    vec4 r2 = texelFetch(uBones, ivec2(t % 1024, t / 1024), 0);
    return transpose(mat4(r0, r1, r2, vec4(0, 0, 0, 1)));
}
mat4 skin() {
    if (aBoneBase < 0.0) return mat4(1.0);
    mat4 m = mat4(0.0); float total = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (aWeights[k] > 0.0) { m += aWeights[k] * boneMatrix(aBones[k]); total += aWeights[k]; }
    }
    return total > 0.0 ? m / total : mat4(1.0);
}
```
  Position: `aModel * (skin() * vec4(aPosition, 1))`. Normal: `aNormalMatrix * (mat3(skin) * aNormal)`.
  - **shadow.vert:** the same functions, position only.
  - **GameView:** for each Model draw of a row with a pose and `source.skinned`, set `bones = pose->palette.data()`, `boneCount`, `posed = true`, and the pose box.
  - **Renderer::findVisible:** `bonePalette_.clear()`; for each drawable item with bones, `item.boneBase = bonePalette_.add(...)` and the posed bounds replace the mesh bounds; upload the texture once.
  - **Binding:** `glActiveTexture(16)` and bind it before the geometry, transparency and shadow passes.
  - **ShadowRenderer:** `setBones(const DrawItem* items, unsigned texture)` before `draw`/`drawSun`, so `addCasterRuns` copies `items[index].boneBase`.
- [ ] **Step 4: Run** `[batches]`, `[visibility]` and `[bones]`, then build AnarchyStudio Release. Expected: pass, and the studio builds.
- [ ] **Step 5: Commit** "GPU skinning: a bone texture and per-instance bone base".

### Task 7: Importer keeps bones

**Files:**
- Modify: `src/ide/ModelImport.cpp`, `src/ide/ModelImport.hpp` (comment)
- Test: `tests/ModelImportTest.cpp`

**Interfaces:**
- Consumes: the AMESH bone fields, as documented in Task 1.

- [ ] **Step 1: Failing test.** It's a glTF written in the test.
  - **Scene:** one mesh node with translation (0,0,5); a skin of two joints, `Root` (at the origin) and `Hand` (child, translation (0,2,0)); an identity inverse bind for Root and translate(0,-2,0) for Hand.
  - **Mesh:** two primitives (materials A and B), each one triangle. JOINTS_0 is u8, and WEIGHTS_0 gives the first vertex 0.75/0.25.
  - **Expectations:**
    - two AMESH files, each with the identical 2-bone table named Root/Hand;
    - `Hand` rest translation is (0,2,5);
    - every vertex's weights sum to 1 (±1/255);
    - vertices are at their model-space positions (z = 5);
    - no "came in static" note;
    - the Mesh loads a skeleton in the built Prefab.
- [ ] **Step 2: Run** `build/Release/studio-tests.exe` and see the new checks fail.
- [ ] **Step 3: Implement.**
  - **Pre-pass and table:** a `SkeletonBuilder` pre-pass collects the nodes named by any `aiBone`, adds their ancestors up to the root, and orders parents before children.
  - **Rest transforms:** a bone with an `aiBone` uses `meshNodeGlobal × offset⁻¹`; an ancestor-only node uses its node global.
  - **Vertex weights:** `AppendMesh` gets an optional bone map. Per vertex it keeps the top 4 weights and renormalizes; a skinned vertex with none is bound to the mesh node's nearest bone ancestor, or bone 0.
  - **Rigid children:** an unskinned mesh under a bone node is bound to that bone at weight 1.
  - **Writing:** every bucket's `Data.bones` gets the same table, with `cull_radius` measured per bone across all buckets.
  - **Assimp flag:** add `aiProcess_LimitBoneWeights`.
  - **Notes:** remove the "came in static" note.
- [ ] **Step 4: Run** studio-tests. Expected: no FAIL lines.
- [ ] **Step 5: Commit** "Model import keeps bones and skin weights".

### Task 8: Live check

- [ ] Build Release AnarchyStudio, and hide `../JadeFX_CPP/res/shaders` only if testing an export (not needed here).
- [ ] Write the Task 7 glTF (a two-bone bar, more triangles) to the scratchpad, launch Studio with MCP, import it, and place the Prefab as a GameObject.
- [ ] `AddBone("Hand")`, set its Offset to a 60° rotation, and screenshot: the bar bends, and so does its shadow. Then 100 copies: the frame stays smooth, and the profiler shows one instanced call per Model.
- [ ] Commit any fixes, then run the full sandbox and studio-tests Release suites.
