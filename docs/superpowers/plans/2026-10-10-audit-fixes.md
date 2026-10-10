# Audit Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix every bug, documentation drift, and dead-code finding in the 2026-10-10 code audit, plus the two duplications that produced bugs, as small independently testable commits.

**Architecture:** Each task is one finding or one tight cluster of findings, with a failing test first where the sandbox can reach the code, a minimal fix, and a commit. Tasks touch disjoint files except where noted, so they can be done in any order. The big duplication refactors (audit section 5, items P1 through P8) and alpha-cutout shadows (D7) are deliberately **not** in this plan; they get their own plan after these land, so refactors never mix with behaviour fixes in one commit.

**Tech Stack:** C++17 (MSVC on Windows, /W4 clean), CMake, Catch2 (`sandbox` target), Luau, Box3D, OpenGL 3.3. Tests run from the repo root.

**Spec:** `docs/reviews/2026-10-10-code-audit.md`. Finding IDs below (B1, D1, ...) refer to it.

## Global Constraints

- Build with `cmake --build build --config Release --target <target>` from the repo root. "Rebuild" means Release, not Debug.
- Run the Catch2 suite as `build/Release/sandbox.exe "[tag]"` from the repo root (it needs `resources/`). Run `tests/` programs the same way, e.g. `build/Release/engine-tests.exe`.
- Pre-existing flakes that are **not** this plan's fault, unless a task says it fixes one: studio-tests "six frames", jadefx tree-view, Debug Q-decomposition timeouts under load, DS1/DS3 CloudCover default (fixed by Task 14).
- Engine distances are "units". Never write "studs" in code, comments, docs, or messages.
- One commit per task. Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not touch `main` directly: work on a branch named `audit-fixes` cut from `main` at `2676e0b` or later. Never push.
- No new third-party dependencies. No changes to file formats on disk (`.alod`, `.avox`, `.atex`, project JSON).
- Keep `/W4` clean on MSVC: no unused parameters or variables left behind by a removal.

## Review Focus

Inputs the audit implies but no existing test exercises, each pinned to the task that owns the code:

1. **A GL error pending from an earlier pass when the shadow atlas is first made** must not disable shadows. Task 1 (no GL in the sandbox; verified by the structural assertion in Task 1 Step 3 and a manual check).
2. **Undo of a deleted Dragger** must leave it hoverable and drawn. Task 2.
3. **A corrupt terrain cache record** (index past the vertex count, or an index count that is not a multiple of three) must be a cache miss, not a mesh. Task 5.
4. **A Brush body under a GameObject whose Prefab is not centred on its origin**, moved by a script, must not rebuild its hull every step. Task 6.
5. **`part.Shape = 1.9` and `uis:IsKeyDown(97.5)`** must be argument errors, not item 1 / `A`. Task 4.

---

### Task 1: B1 Shadow atlas refusal reads only its own GL error

**Files:**
- Modify: `src/runner/ShadowRenderer.cpp:89-121`

**Interfaces:**
- Consumes: `MakeDepth(int size, int pages, const void*)` (same file), `DeleteTexture(unsigned&)` (`gl.hpp`).
- Produces: nothing new.

The sandbox has no GL context, so this task has no automated test. The assertion in Step 3 is the "test": the drain loop must sit between `DeleteTexture(atlas_)` and `MakeDepth`.

- [ ] **Step 1: Read the current function**

`src/runner/ShadowRenderer.cpp:89-114` is `ShadowRenderer::makeAtlas`. Line 96 is `bool made = glGetError() == GL_NO_ERROR;`, which reads the *oldest unread* error, not the one `MakeDepth` raised.

- [ ] **Step 2: Drain stale errors before allocating, and say why a refusal happened**

Replace lines 93-96:

```cpp
    // GL cannot add a layer in place, so a new page makes the whole array
    // again. Past the GPU's limits, or its memory, glTexImage3D says so.
    unsigned texture = MakeDepth(size, pages, nullptr);
    bool made = glGetError() == GL_NO_ERROR;
```

with:

```cpp
    // GL cannot add a layer in place, so a new page makes the whole array
    // again. Past the GPU's limits, or its memory, glTexImage3D says so.
    // glGetError reports the oldest error still unread, which may be another
    // pass's: drain those first so only MakeDepth's own failure counts here.
    while (glGetError() != GL_NO_ERROR) {
    }
    unsigned texture = MakeDepth(size, pages, nullptr);
    bool made = glGetError() == GL_NO_ERROR;
```

Then make `refuse()` (lines 116-121) name the size it was trying, so a report can be traced. Change its signature in `src/runner/ShadowRenderer.hpp:86` from `void refuse();` to `void refuse(int size, int pages);` and the body to:

```cpp
void ShadowRenderer::refuse(int size, int pages) {
    if (!refused_) {
        std::fprintf(stderr,
                     "This driver will not draw shadow maps (a %d x %d depth array with %d pages was refused); "
                     "lights are drawn without shadows.\n",
                     size, size, pages);
        refused_ = true;
    }
}
```

Update every caller of `refuse()` in `ShadowRenderer.cpp` (grep `refuse(`) to pass the size and page count it just failed with; where cascades fail rather than the atlas, pass `cascadeSize_` and `1`.

- [ ] **Step 3: Build and assert the ordering**

Run:

```bash
cmake --build build --config Release --target AnarchyStudio
```

Expected: builds with no warnings. Then:

```bash
grep -n "while (glGetError() != GL_NO_ERROR)" src/runner/ShadowRenderer.cpp
```

Expected: one hit, on a line between `DeleteTexture(atlas_);` and `unsigned texture = MakeDepth(`.

- [ ] **Step 4: Manual check**

Launch the studio with the MCP port and token (see memory `drive-studio-over-mcp`), open a place with a shadowed PointLight, and take a screenshot. Shadows must be present. Nothing in stderr should mention "will not draw shadow maps".

- [ ] **Step 5: Commit**

```bash
git add src/runner/ShadowRenderer.cpp src/runner/ShadowRenderer.hpp
git commit -m "Shadows: a stale GL error no longer turns the atlas off for the session

makeAtlas judged MakeDepth by glGetError, which returns the oldest unread
error, so an error left by any earlier pass refused the atlas and latched
refused_ until shutdown. Drain first, and say what was refused.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: B2 One `tag_entity` for `spawn` and `adopt_slot`, so a revived Dragger is found

**Files:**
- Modify: `src/engine_core/DataModel.cpp:579-599` (`spawn`)
- Modify: `src/engine_core/DataModelPlace.cpp:187-204` (`adopt_slot`)
- Modify: `src/engine_core/DataModelState.hpp` (declare the helper next to `EcsIds`)
- Test: `sandbox/dragger_tests.cpp`

**Interfaces:**
- Produces: `void tag_entity(ecs_world_t* world, ecs_entity_t entity, const EcsIds& ids, const DataModel& object)` in namespace `engine_core`, declared in `DataModelState.hpp`, defined in `DataModel.cpp`.
- Consumes: `DataModel::steps()`, `physics_body()`, `terrain()`, `sound_source()`, `dragger()`, `billboard_gui()`, `wireframe()` virtuals; `ecs_add_id`.

- [ ] **Step 1: Write the failing test**

Append to `sandbox/dragger_tests.cpp` (inside the file's existing anonymous-namespace helpers are `DragRig`, `add_dragger`; `begin_step`/`end_step` come from `support.hpp`):

```cpp
TEST_CASE("DR20 a Dragger deleted and undone is in the dragger query again", "[DR20]") {
    DragRig drag;
    engine_core::DataModel& game = drag.rig.game;
    std::vector<InstanceId> found;
    game.draggers(found);
    REQUIRE(std::find(found.begin(), found.end(), drag.dragger) != found.end());

    begin_step(game, "Delete");
    game.destroy(drag.dragger);
    end_step(game);
    game.draggers(found);
    REQUIRE(std::find(found.begin(), found.end(), drag.dragger) == found.end());

    game.history().undo();
    REQUIRE(game.alive(drag.dragger));
    game.draggers(found);
    // The revived Dragger must be tagged like a new one, or it is in the tree
    // but never hovered, dragged, or drawn.
    REQUIRE(std::find(found.begin(), found.end(), drag.dragger) != found.end());
}
```

Add `#include <algorithm>` and `#include <vector>` to the file's includes if missing.

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[DR20]"
```

Expected: FAIL on the last `REQUIRE` (the Dragger is alive but absent from `found`).

- [ ] **Step 3: Declare the helper**

In `src/engine_core/DataModelState.hpp`, directly after the `EcsIds` struct definition (grep `struct EcsIds`), add:

```cpp
// Adds every ECS tag the object's class asks for: the one list spawn() and
// adopt_slot() both use, so a revived instance is tagged like a new one.
void tag_entity(ecs_world_t* world, ecs_entity_t entity, const EcsIds& ids, const DataModel& object);
```

(`DataModel` is already forward-declared or included in that header; if not, add `class DataModel;` above.)

- [ ] **Step 4: Define it and use it in `spawn`**

In `src/engine_core/DataModel.cpp`, add near the top of the `engine_core` namespace (before `DataModel::spawn`):

```cpp
void tag_entity(ecs_world_t* world, ecs_entity_t entity, const EcsIds& ids, const DataModel& object) {
    if (object.steps()) {
        ecs_add_id(world, entity, ids.steps);
    }
    if (object.physics_body()) {
        ecs_add_id(world, entity, ids.physics_body);
    }
    if (object.terrain()) {
        ecs_add_id(world, entity, ids.terrain);
    }
    if (object.sound_source()) {
        ecs_add_id(world, entity, ids.sound_source);
    }
    if (object.dragger()) {
        ecs_add_id(world, entity, ids.dragger);
    }
    if (object.billboard_gui()) {
        ecs_add_id(world, entity, ids.billboard);
    }
    if (object.wireframe()) {
        ecs_add_id(world, entity, ids.wireframe);
    }
}
```

Then in `spawn` replace lines 579-599 (the seven `if (object->...()) { ecs_add_id(...); }` blocks) with:

```cpp
    tag_entity(ecs_world(), world.slots[index].entity, world.ecs_ids, *object);
```

- [ ] **Step 5: Use it in `adopt_slot`**

In `src/engine_core/DataModelPlace.cpp` replace lines 187-204 (the six `if` blocks) with:

```cpp
    tag_entity(ecs_world(), part.entity, state_->ecs_ids, *object);
```

- [ ] **Step 6: Run the test and the whole sandbox**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[DR20]"
build/Release/sandbox.exe
```

Expected: DR20 passes; the full run has no new failures (DS1/DS3 may still be red until Task 14).

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/DataModel.cpp src/engine_core/DataModelPlace.cpp src/engine_core/DataModelState.hpp sandbox/dragger_tests.cpp
git commit -m "DataModel: one tag_entity for spawn and adopt_slot; a revived Dragger is found again

adopt_slot copied spawn's tag list minus the Dragger branch, so a Dragger
brought back by undo or Stop was in the tree but never hovered or drawn.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: B3 `Color3:ToHSV` declares three results

**Files:**
- Modify: `src/engine_datatypes/Color3.cpp:177`
- Modify: `src/engine_core/LuaApi.cpp:810, 817`
- Test: `sandbox/analysis_tests.cpp`

**Interfaces:**
- Consumes: `lua_method(name, type_name, doc)` (`LuaApi.hpp`), `add(class, member, doc, type, is_method, params)` (local to `LuaApi.cpp`), `has_code(diagnostics, code)` and `ScriptRig`/`add_script`/`settle` from `analysis_tests.cpp`.

- [ ] **Step 1: Write the failing test**

Append to `sandbox/analysis_tests.cpp`:

```cpp
TEST_CASE("A40 Color3:ToHSV returns three numbers to the type checker", "[A40]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Hsv", R"(--!strict
local color = Color3.new(0.2, 0.4, 0.6)
local h, s, v = color:ToHSV()
local h2, s2, v2 = Color3.toHSV(color)
print(h * 2, s * 2, v * 2, h2 * 2, s2 * 2, v2 * 2)
)");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE_FALSE(has_code(analysis.diagnostics(script.id()), "Type"));
}
```

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[A40]"
```

Expected: FAIL; the dump shows a `Type` diagnostic about `s` or `v` being `nil`.

- [ ] **Step 3: Fix the registration and the two docs**

`src/engine_datatypes/Color3.cpp:177`, change

```cpp
        lua_method("ToHSV", "number", nullptr),               lua_method("ToHex", "string", nullptr),
```

to

```cpp
        lua_method("ToHSV", nullptr, nullptr),                lua_method("ToHex", "string", nullptr),
```

(A null type with a comma-separated doc type is the multi-return convention; see `Matrix4.cpp:840-841` and `ToAxisAngle` at `Matrix4.cpp:869`.)

`src/engine_core/LuaApi.cpp:810-811`, change the return type `"number"` to `"number,number,number"`:

```cpp
    add("Color3", "toHSV", "The hue, saturation, and value of a color, each 0 to 1.", "number,number,number", false,
        {P("color", "Color3")});
```

`src/engine_core/LuaApi.cpp:817`:

```cpp
    add("Color3", "ToHSV", "The hue, saturation, and value, each 0 to 1.", "number,number,number", false, {});
```

- [ ] **Step 4: Run the test, the analysis suite, and the runtime test**

```bash
cmake --build build --config Release --target sandbox engine-tests
build/Release/sandbox.exe "[A40]"
build/Release/sandbox.exe "[A1],[A2],[A3]"
build/Release/engine-tests.exe
```

Expected: all pass (`engine-tests` has the runtime `ToHSV` check at `tests/LuaEngineTest.cpp:274`).

- [ ] **Step 5: Commit**

```bash
git add src/engine_datatypes/Color3.cpp src/engine_core/LuaApi.cpp sandbox/analysis_tests.cpp
git commit -m "Color3: ToHSV and toHSV are typed as three numbers, as they return

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: B7 Enum arguments must be whole numbers

**Files:**
- Modify: `src/engine_datatypes/Enum.cpp:407-411`
- Test: `sandbox/physics_tests.cpp` (extend P14)

**Interfaces:**
- Consumes: `check_enum_arg(lua_State*, int index, const EnumType&)`; `enum_item_name(type, value)`.

- [ ] **Step 1: Extend the failing test**

In `sandbox/physics_tests.cpp`, test P14 (line 440), the `_G.refused` expression lists what must be refused. Add a non-integral number and a non-finite one. Change:

```lua
        _G.refused = not pcall(function() body.Shape = "Torus" end)
            and not pcall(function() body.Shape = Enum.KeyCode.A end)
            and not pcall(function() body.Shape = Vector3.new() end)
            and body.Shape == Enum.PhysicsShape.Hull
```

to:

```lua
        _G.refused = not pcall(function() body.Shape = "Torus" end)
            and not pcall(function() body.Shape = Enum.KeyCode.A end)
            and not pcall(function() body.Shape = Vector3.new() end)
            and not pcall(function() body.Shape = 1.9 end)
            and not pcall(function() body.Shape = 0 / 0 end)
            and body.Shape == Enum.PhysicsShape.Hull
```

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[physics]" -c "P14*"
```

(If `-c` does not select, run `build/Release/sandbox.exe "P14 scripts set Shape by item, name, or value, and nothing else"`.)
Expected: FAIL; `1.9` truncates to item 1 and is accepted.

- [ ] **Step 3: Require an integral number**

`src/engine_datatypes/Enum.cpp:407-411`, change:

```cpp
    } else if (lua_type(state, index) == LUA_TNUMBER) {
        const int value = static_cast<int>(lua_tointeger(state, index));
        if (enum_item_name(type, value) != nullptr) {
            return value;
        }
    }
```

to:

```cpp
    } else if (lua_type(state, index) == LUA_TNUMBER) {
        // Only a whole number names an item: 1.9 is not item 1.
        const double number = lua_tonumber(state, index);
        if (std::isfinite(number) && number == std::floor(number) && number >= INT_MIN && number <= INT_MAX) {
            const int value = static_cast<int>(number);
            if (enum_item_name(type, value) != nullptr) {
                return value;
            }
        }
    }
```

Add `#include <climits>` and `#include <cmath>` at the top of `Enum.cpp` if not present.

- [ ] **Step 4: Run the test and the enum-using suites**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[physics]"
build/Release/sandbox.exe "[gui]"
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_datatypes/Enum.cpp sandbox/physics_tests.cpp
git commit -m "Enum: a fractional number is an argument error, not the truncated item

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: B4 Terrain cache records are bounds-checked

**Files:**
- Modify: `src/engine_core/terrain/AlodStore.cpp:287-306` (`AlodStore::load`)
- Test: `sandbox/terrain_streaming_tests.cpp`

**Interfaces:**
- Consumes: `AlodStore::create/put/commit/load/entries()`, `CompactMesh`, `TempFile`, `test_mesh(seed, wide)` (all already in that test file).

- [ ] **Step 1: Write the failing test**

Append to `sandbox/terrain_streaming_tests.cpp` after AL2:

```cpp
// Flips bytes of key's record in the file so its first index points past
// the last vertex. The record's size and key stay right, so only load()'s
// own checks can catch it.
void corrupt_first_index(const std::filesystem::path& path, const AlodEntry& entry, bool wide_indices) {
    // Record layout, after the key: f32 error, vec3 min, vec3 max, u32
    // surface_index_count, vec3 origin, vec3 scale, u32 vertices, u32
    // indices16, u32 indices32, then positions (u16 x 3 per vertex), normals
    // (u8 x 2), ids (u8 x 4), weights (u8 x 4), indices16, indices32.
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());
    const std::uint64_t key_bytes = 4 * sizeof(std::int32_t);
    const std::uint64_t header = key_bytes + 4 + 12 + 12 + 4 + 12 + 12;
    file.seekg(static_cast<std::streamoff>(entry.offset + header));
    std::uint32_t vertices = 0;
    file.read(reinterpret_cast<char*>(&vertices), 4);
    const std::uint64_t per_vertex = 6 + 2 + 4 + 4;
    const std::uint64_t first_index = entry.offset + header + 12 + vertices * per_vertex;
    file.seekp(static_cast<std::streamoff>(first_index));
    if (wide_indices) {
        const std::uint32_t bad = 0xFFFFFFF0u;
        file.write(reinterpret_cast<const char*>(&bad), 4);
    } else {
        const std::uint16_t bad = 0xFFF0u;
        file.write(reinterpret_cast<const char*>(&bad), 2);
    }
    REQUIRE(file.good());
}

TEST_CASE("AL5 a record whose index is past its vertices is a miss, not a mesh", "[terrain]") {
    TempFile file("al5.alod");
    std::optional<AlodStore> store = AlodStore::create(file.path, 0x55u, 1.f);
    REQUIRE(store);
    REQUIRE(store->put(NodeKey{2, 0, 0, 0}, test_mesh(1, false), 1.f, Vec3{}, Vec3{1, 1, 1}));
    REQUIRE(store->put(NodeKey{2, 1, 0, 0}, test_mesh(2, true), 1.f, Vec3{}, Vec3{1, 1, 1}));
    REQUIRE(store->commit());
    REQUIRE(store->load(NodeKey{2, 0, 0, 0}) != nullptr);
    REQUIRE(store->load(NodeKey{2, 1, 0, 0}) != nullptr);

    corrupt_first_index(file.path, store->entries().at(NodeKey{2, 0, 0, 0}), false);
    corrupt_first_index(file.path, store->entries().at(NodeKey{2, 1, 0, 0}), true);
    REQUIRE(store->load(NodeKey{2, 0, 0, 0}) == nullptr);
    REQUIRE(store->load(NodeKey{2, 1, 0, 0}) == nullptr);
}
```

Confirm the record layout against `AlodStore::put` and `read_key` before relying on the offsets; if `read_key` writes the key as something other than four `int32`, adjust `key_bytes` to match. Add `#include <fstream>` if missing.

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "AL5*"
```

Expected: FAIL on the first `== nullptr` (load returns a mesh with an out-of-range index).

- [ ] **Step 3: Validate the record**

In `src/engine_core/terrain/AlodStore.cpp`, replace lines 303-306:

```cpp
    if (!r.ok || r.at != bytes.size()) {
        return nullptr;
    }
    return mesh;
```

with:

```cpp
    if (!r.ok || r.at != bytes.size()) {
        return nullptr;
    }
    // The index says this record is key's and this long; nothing else was
    // checked. A short read, a torn write, or a flipped bit can leave
    // indices that point past the vertices, which the LOD builder and the
    // GPU upload would follow. Such a record is a miss, as if never put.
    if (mesh->positions.size() != vertices * 3 || mesh->normals.size() != vertices * 2 ||
        mesh->ids.size() != vertices * 4 || mesh->weights.size() != vertices * 4 ||
        mesh->indices.size() % 3 != 0 || mesh->indices32.size() % 3 != 0 ||
        (!mesh->indices.empty() && !mesh->indices32.empty())) {
        return nullptr;
    }
    const std::size_t index_count = mesh->indices.empty() ? mesh->indices32.size() : mesh->indices.size();
    if (mesh->surface_index_count > index_count) {
        return nullptr;
    }
    for (const std::uint16_t index : mesh->indices) {
        if (index >= vertices) {
            return nullptr;
        }
    }
    for (const std::uint32_t index : mesh->indices32) {
        if (index >= vertices) {
            return nullptr;
        }
    }
    return mesh;
```

Also update the header comment at `src/engine_core/terrain/AlodStore.hpp:21-23` so the sentence about records matches: "A header or footer that does not check out makes open() fail; a record that does not check out makes load() a miss."

- [ ] **Step 4: Run the terrain suites**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[terrain]"
```

Expected: AL1 through AL5 and every other `[terrain]` case pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/terrain/AlodStore.cpp src/engine_core/terrain/AlodStore.hpp sandbox/terrain_streaming_tests.cpp
git commit -m "Terrain LOD cache: a record with indices past its vertices is a miss

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: B8 A Brush body records its centre and Prefab like any other body

**Files:**
- Modify: `src/engine_core/PhysicsWorld.cpp:1842-1848` (`make_brush_shape`)
- Test: `sandbox/brush_tests.cpp`

**Interfaces:**
- Consumes: `center_for(game, driven)`, `scale_for(game, driven)` (same file, lines 179-207), `GameObject::prefab_guid()`, `PhysicsWorld::shapes_made(InstanceId)`, `physics_rig::PhysicsRig`.

- [ ] **Step 1: Write the failing test**

Append to `sandbox/brush_tests.cpp`. It already has `using namespace engine_core; using namespace physics_rig;` and includes `physics_rig.hpp`, `Brush.hpp`, and `PhysicsObject.hpp`; add `#include "AssetInstances.hpp"`, `#include "GameObject.hpp"`, `#include "MeshShapes.hpp"`, and `#include "amesh.hpp"` if they are missing. The construction below is `prefab_body` from `sandbox/physics_tests.cpp:665-693` with a Brush in place of the PhysicsObject.

```cpp
namespace {

LuaSlot slot_of(InstanceId id) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A 2 by 2 by 2 box whose bottom is at the Mesh's origin: its middle is (0, 1, 0).
void box_above_origin(anarchy::amesh::Data& data) { add_box(data, Vec3{2.f, 2.f, 2.f}, Vec3{0.f, 1.f, 0.f}); }

}  // namespace

TEST_CASE("BP7 a Brush under a GameObject with an off-centre Prefab keeps its hull across moves", "[brush]") {
    PhysicsRig rig;
    rig.floor();
    Game& game = rig.game;
    // A Prefab whose Model's Mesh is not centred on the origin, so its
    // origin_offset is non-zero and center_for() is never Vec3{}.
    Mesh& mesh = game.create<Mesh>();
    game.set_parent(mesh.id(), game.service("Meshes"));
    Prefab& prefab = game.create<Prefab>();
    game.set_parent(prefab.id(), game.service("Prefabs"));
    Model& model = game.create<Model>();
    game.set_parent(model.id(), prefab.id());
    REQUIRE_FALSE(model.set_reference(Model::kMeshReference, slot_of(mesh.id())));
    REQUIRE_FALSE(mesh.edit_geometry(box_above_origin));
    REQUIRE(prefab.origin_offset().y != 0.f);

    GameObject& part = game.create<GameObject>();
    part.set_transform(at(0.f, 2.f, 0.f));
    REQUIRE_FALSE(part.set_prefab(slot_of(prefab.id())));
    game.set_parent(part.id(), workspace_of(game));
    // A Brush whose parent is a GameObject drives it (PhysicsBase.cpp:74-80).
    Brush& brush = game.create<Brush>();
    game.set_parent(brush.id(), part.id());
    REQUIRE(brush.anchored());

    rig.play();
    rig.steps(1);
    const int made = rig.physics.shapes_made(brush.id());
    REQUIRE(made >= 1);
    // Someone else moves the GameObject each step, as a script or a drag would.
    for (int step = 0; step < 30; ++step) {
        part.set_transform(at(0.f, 2.f + 0.01f * static_cast<float>(step), 0.f));
        rig.steps(1);
    }
    // A move is not a reason to clip the hull again.
    REQUIRE(rig.physics.shapes_made(brush.id()) == made);
}
```

`at(x, y, z)` and `workspace_of` come from `physics_rig.hpp`; `add_box(anarchy::amesh::Data&, Vec3, Vec3)` is in `MeshShapes.hpp` (the physics test calls it as `engine_core::add_box`). If `mesh.edit_geometry` must run during play as the physics test does, move the `edit_geometry` line to just after `rig.play()` and keep the `origin_offset` check after it.

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "BP7*"
```

Expected: FAIL; `shapes_made` grows with each move.

- [ ] **Step 3: Record what `recenter` compares against**

`src/engine_core/PhysicsWorld.cpp:1842-1845`, change:

```cpp
    void make_brush_shape(DataModel& game, Brush& brush, Body& record) {
        drop_shape(record);
        record.scale = scale_for(game, record.driven);
        record.center = Vec3{};
```

to:

```cpp
    void make_brush_shape(DataModel& game, Brush& brush, Body& record) {
        drop_shape(record);
        record.scale = scale_for(game, record.driven);
        // The Brush's solid is placed by its own faces, not by a Prefab, but
        // recenter() compares these against the driven GameObject's Prefab:
        // record them as make_object_shape does, or every move remakes the hull.
        record.center = center_for(game, record.driven);
        const GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr;
        record.prefab = driven != nullptr ? driven->prefab_guid() : std::string();
```

Check that the pieces built below still place themselves by the Brush's own geometry (they do not read `record.center`; confirm with a grep for `record.center` inside `make_brush_shape`). If any piece offset uses `record.center`, leave that offset at `Vec3{}` explicitly.

- [ ] **Step 4: Run the brush and physics suites**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[brush]"
build/Release/sandbox.exe "[physics]"
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.cpp sandbox/brush_tests.cpp
git commit -m "Physics: a Brush body records its centre and Prefab, so a move does not remake its hull

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: B12 One mip-size rule for terrain arrays

**Files:**
- Modify: `src/engine_core/texture/BlockCompress.hpp` (add `mip_size`)
- Modify: `src/engine_core/terrain/LayerBuilder.cpp:172`
- Modify: `src/runner/Renderer.cpp:1561, 1743`
- Test: `sandbox/texture_streaming_tests.cpp`

**Interfaces:**
- Produces: `inline int mip_size(int base, int level)` in namespace `engine_core::texture`, declared in `BlockCompress.hpp`: `std::max(base >> level, 1)`, the floor-halving GL requires for a mipmap-complete texture.

- [ ] **Step 1: Write the failing test**

Append to `sandbox/texture_streaming_tests.cpp`:

```cpp
TEST_CASE("TS9 mip_size floors like GL and reaches 1 for any base", "[texture]") {
    using engine_core::texture::mip_size;
    REQUIRE(mip_size(256, 0) == 256);
    REQUIRE(mip_size(256, 8) == 1);
    REQUIRE(mip_size(256, 9) == 1);
    // Odd sizes floor, as glTexImage*D expects: 96 -> 48, 24, 12, 6, 3, 1.
    REQUIRE(mip_size(96, 5) == 3);
    REQUIRE(mip_size(96, 6) == 1);
    REQUIRE(mip_size(1, 3) == 1);
}
```

- [ ] **Step 2: Run it to see it fail to compile**

```bash
cmake --build build --config Release --target sandbox
```

Expected: compile error, `mip_size` is not a member of `engine_core::texture`.

- [ ] **Step 3: Add the helper**

In `src/engine_core/texture/BlockCompress.hpp`, after the `level_bytes` declaration (line 22):

```cpp
// The side of mip `level` of a texture whose level 0 is `base` on that side:
// halved and floored each level, never below 1, as GL sizes mipmaps. Every
// place that allocates, builds, or uploads terrain levels must agree on this.
inline int mip_size(int base, int level) {
    return std::max(base >> level, 1);
}
```

Add `#include <algorithm>` to the header if missing.

- [ ] **Step 4: Use it in the three places**

`src/engine_core/terrain/LayerBuilder.cpp:172`: delete `next_level_size` and replace each use (grep `next_level_size(`) with the level-based form. If a loop walks `cw = next_level_size(cw)`, rewrite it to compute `cw = engine_core::texture::mip_size(w, level)` from the base and the level counter instead. Keep the level *count* unchanged.

`src/runner/Renderer.cpp:1561`: change `levelSize = std::max((levelSize + 1) / 2, 1);` to `levelSize = engine_core::texture::mip_size(size, level + 1);`.

`src/runner/Renderer.cpp:1743`: change `const int levelSize = std::max(filling.size >> level, 1);` to `const int levelSize = engine_core::texture::mip_size(filling.size, level);`.

- [ ] **Step 5: Run the texture and terrain suites**

```bash
cmake --build build --config Release --target sandbox AnarchyStudio
build/Release/sandbox.exe "[texture]"
build/Release/sandbox.exe "[terrain]"
```

Expected: pass, including the existing terrain texture round-trips (sizes 256 to 2048 are unchanged by this, since powers of two floor and round-up the same).

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/texture/BlockCompress.hpp src/engine_core/terrain/LayerBuilder.cpp src/runner/Renderer.cpp sandbox/texture_streaming_tests.cpp
git commit -m "Terrain textures: one mip_size rule for building, allocating, and uploading levels

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: B5, B10, B11 Small IDE correctness fixes

**Files:**
- Modify: `src/ide/IdeLayoutProject.cpp:65, 1115, 1127`
- Modify: `src/ide/McpTools.cpp:1579-1601`
- Modify: `src/ide/PtyWindows.cpp:325-343`
- Modify: `src/ide/RunProcess.cpp:155-160, 185`

None of these has a sandbox-reachable test (Windows path encoding, a throwing profiler, and a Win32 error path). The build and the existing `terminal-tests`, `mcp-tests` runs are the gate.

- [ ] **Step 1: UTF-8 paths (B5)**

`src/ide/IdeLayoutProject.cpp:65`:

```cpp
    host.folder = [this] { return project_ ? engine_core::utf8_path(project_->root()) : std::string(); };
```

Line 1115: `error = "Could not write " + engine_core::utf8_path(file) + ".";`

Line 1127: `options.directory = engine_core::utf8_path(project_->root());`

(`utf8_path` is `inline` in `src/engine_core/FileBytes.hpp:13` and the file already uses it at line 372.)

- [ ] **Step 2: Profiler release on every path (B10)**

In `src/ide/McpTools.cpp`, replace the `if (own) { profiler::acquire(); ... }` / `if (own) { profiler::release(); }` pair (lines 1579-1601) so the release is a scope guard. Add near the top of the file's anonymous namespace:

```cpp
// Releases the profiler when it leaves scope, so a tool that throws
// mid-report does not leave it recording for the rest of the session.
struct ProfilerHold {
    bool held = false;
    ~ProfilerHold() {
        if (held) {
            profiler::release();
        }
    }
};
```

Then in `GetProfile`:

```cpp
    ProfilerHold hold;
    if (own) {
        profiler::acquire();
        hold.held = true;
        const auto began = std::chrono::steady_clock::now();
        while (recorded < seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            profiler::collect();
            recorded = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        }
    } else if (!profiler::paused()) {
        profiler::collect();
    }
```

and delete the trailing `if (own) { profiler::release(); }`.

- [ ] **Step 3: Attribute list freed on the partial-failure path (B11)**

`src/ide/PtyWindows.cpp:325-343`: split the condition so `Initialize` success always reaches `Delete`:

```cpp
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        failure = ErrorText("Can't attach the pseudo-console", GetLastError());
    } else {
        if (!UpdateProcThreadAttribute(attributes, 0, kPseudoConsoleAttribute, console, sizeof(console), nullptr,
                                       nullptr)) {
            failure = ErrorText("Can't attach the pseudo-console", GetLastError());
        } else {
            // ... the existing CreateProcessW block, unchanged ...
        }
        DeleteProcThreadAttributeList(attributes);
    }
```

`src/ide/RunProcess.cpp:155-160`: same shape. After a successful `Initialize`, every later `return result;` before line 185 must be preceded by `DeleteProcThreadAttributeList(attributes);`. Simplest: wrap it.

```cpp
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        result.error = "Could not set up the child's handles: " + Describe(GetLastError());
        return result;
    }
    struct AttributeList {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeList() { DeleteProcThreadAttributeList(list); }
    } attribute_list{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   inherited_count * sizeof(HANDLE), nullptr, nullptr)) {
        result.error = "Could not set up the child's handles: " + Describe(GetLastError());
        return result;
    }
```

and delete the explicit `DeleteProcThreadAttributeList(attributes);` at line 185.

- [ ] **Step 4: Build and run the affected test programs**

```bash
cmake --build build --config Release --target AnarchyStudio terminal-tests mcp-tests
build/Release/terminal-tests.exe
build/Release/mcp-tests.exe
```

Expected: build clean; both pass.

- [ ] **Step 5: Commit**

```bash
git add src/ide/IdeLayoutProject.cpp src/ide/McpTools.cpp src/ide/PtyWindows.cpp src/ide/RunProcess.cpp
git commit -m "IDE: UTF-8 project paths for the terminal and dialogs; profiler and attribute-list releases on every path

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: B6 A backtick string token starts where it starts

**Files:**
- Modify: `src/ide/LuauComplete.cpp:194-204`
- Test: `tests/LuauCompleteTest.cpp`

**Interfaces:**
- Consumes: `ide::hover_luau(source, index)` returning `HoverInfo` with `begin`/`end` (see `tests/LuauCompleteTest.cpp:1374-1397` for `expect_hover` and `find_nth`).

- [ ] **Step 1: Write the failing test**

Append to `tests/LuauCompleteTest.cpp`, next to the hover tests (after line ~1397), using the file's existing check style:

```cpp
    {
        // A backtick string's token must begin at its backtick, not at the
        // start of the file; otherwise a hover over earlier whitespace lands
        // on it.
        const std::string source = "local count = 1\nlocal text = `n {count}`\n";
        const int gap = static_cast<int>(source.find("= 1")) + 3;   // the space after "1"
        const ide::HoverInfo info = ide::hover_luau(source, gap);
        CHECK(!info.found);
        CHECK(info.begin == 0 || info.begin >= gap || info.end <= gap);
    }
```

Follow the surrounding tests' macro (`CHECK`/`REQUIRE`/custom `expect`) exactly as the file uses it.

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build --config Release --target engine-tests
build/Release/engine-tests.exe
```

Expected: the new check fails (`info.begin` is 0 and `info.end` is past `gap`, so the token covers the gap).

- [ ] **Step 3: Pass the real start**

`src/ide/LuauComplete.cpp:194-204`, change:

```cpp
        if (code == U'`') {
            ++i;
            while (i < caret && text[static_cast<std::size_t>(i)] != U'`') {
                ++i;
            }
            if (i >= caret) {
                scan.blocked = true;
                return scan;
            }
            ++i;
            emit(Token::String, 0, i, {});
            continue;
        }
```

to:

```cpp
        if (code == U'`') {
            const int start = i;
            ++i;
            while (i < caret && text[static_cast<std::size_t>(i)] != U'`') {
                ++i;
            }
            if (i >= caret) {
                scan.blocked = true;
                return scan;
            }
            ++i;
            emit(Token::String, start, i, {});
            continue;
        }
```

(If the enclosing loop already has a `start` variable for this iteration, use it and skip the declaration.)

- [ ] **Step 4: Run engine-tests**

```bash
cmake --build build --config Release --target engine-tests
build/Release/engine-tests.exe
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/ide/LuauComplete.cpp tests/LuauCompleteTest.cpp
git commit -m "Completion: a backtick string token begins at its backtick

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: B9 `unbind`'s header tells the truth about render jobs

**Scope correction.** The audit proposed freeing a retired render-phase entry. Reading `run_phase` (`TaskScheduler.cpp:440-444`) shows the render thread calls `entry.job(dt)` through the entry while the simulation thread could be erasing it, and `jobs_` is a `reserve`d vector precisely so the render thread can iterate it without a lock. Freeing the entry safely needs a hand-off point that does not exist today. Production binds one render job per `ScriptRuntime` for the engine's life, so the leak only bites test rigs. This task fixes the false promise in the header and pins the real behaviour; the real fix moves to the deferred list.

**Files:**
- Modify: `src/engine_core/TaskScheduler.hpp:35-36`
- Test: `sandbox/tests.cpp`

- [ ] **Step 1: Write the test that documents the limit**

Append to `sandbox/tests.cpp` near the other scheduler tests (they use `engine_core::Engine engine;` and `engine.scheduler().bind(...)`, see line 146):

```cpp
TEST_CASE("TS2 a render-phase job's entry stays until shutdown, so a rig binds one and reuses it", "[scheduler]") {
    engine_core::Engine engine;
    engine_core::TaskScheduler& scheduler = engine.scheduler();
    // Engine reserves 64 entries per phase and a render entry is kept after
    // unbind (the render thread may be inside its closure). A rig that binds
    // and unbinds a render job per attach would exhaust the phase at 64; this
    // pins that a single bind-unbind leaves 63 more, not an abort.
    std::vector<engine_core::TaskScheduler::JobId> ids;
    for (int cycle = 0; cycle < 63; ++cycle) {
        ids.push_back(scheduler.bind(engine_core::Phase::RenderStepped, [](double) {}));
        scheduler.unbind(ids.back());
    }
    REQUIRE(ids.size() == 63u);
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}
```

- [ ] **Step 2: Run it**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[scheduler]"
```

Expected: PASS (this test pins current behaviour; a 64th cycle would abort).

- [ ] **Step 3: Correct the header**

`src/engine_core/TaskScheduler.hpp:35-36`, change:

```cpp
    // The job does not run again. Its closure is released now, or at the next
    // cancel_session_jobs when it is the job running on this thread.
```

to:

```cpp
    // The job does not run again. A simulation-phase closure is released now,
    // or at the next cancel_session_jobs when it is the job running on this
    // thread. A render-phase entry keeps its closure and its slot until
    // shutdown: the render thread may be inside it, and jobs_ is iterated
    // there without a lock. Bind a render job once and keep it; each phase
    // holds the entries Engine reserved (64), and bind past that aborts.
```

- [ ] **Step 4: Commit**

```bash
git add src/engine_core/TaskScheduler.hpp sandbox/tests.cpp
git commit -m "TaskScheduler: say that a render job's entry lives until shutdown

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: B13, B15 Two latent core fixes

**Files:**
- Modify: `src/engine_core/TerrainWorld.cpp:818-831`
- Modify: `src/engine_core/Events.cpp:389-402`, `src/engine_core/Events.hpp:176-177`

- [ ] **Step 1: Voxel-size change drops old colliders (B13)**

In `src/engine_core/TerrainWorld.cpp`, inside the `if (record.tree == nullptr || record.tree->voxel_size() != voxel_size) {` block, after `record.sync_built.clear();` add:

```cpp
            // Colliders were triangulated at the old size too; PhysicsWorld
            // must not keep handing them out until each chunk is re-walked.
            record.collider_map.clear();
            record.colliders_vec.clear();
```

(Use the real member names: grep `collider_map` and `colliders_vec` in `TerrainWorld.hpp`; if the vector is rebuilt from the map by a `publish_colliders`-style step, clearing the map alone and marking the vector dirty the way `chunks_dirty` is marked is enough.)

- [ ] **Step 2: `count()` reports suppressed overrides (B15)**

`src/engine_core/Events.cpp:50-56`:

```cpp
std::uint64_t EventQueue::count(WriteOrigin origin) const {
    // SnapshotOverride writes never queue an event; they are counted apart.
    if (origin == WriteOrigin::SnapshotOverride) {
        return suppressed_overrides_;
    }
    const int index = origin_index(origin);
    if (index < 0 || index > 2) {
        return 0;
    }
    return counts_[index];
}
```

Add to `Events.hpp:176` a comment above `count`: `// Events queued from origin; for SnapshotOverride, the writes suppressed instead.`

- [ ] **Step 3: Build and run**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[terrain]"
build/Release/sandbox.exe "[events],[E1]"
```

Expected: pass. If a test asserted `count(SnapshotOverride) == 0`, update it to assert against `suppressed_overrides()` instead.

- [ ] **Step 4: Commit**

```bash
git add src/engine_core/TerrainWorld.cpp src/engine_core/Events.cpp src/engine_core/Events.hpp
git commit -m "Terrain: a voxel-size change drops old colliders; EventQueue::count reports overrides

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: D6 Light Color3 is sRGB like every other Color3

**Assumption:** a Light's `Color` is picked in the same colour picker as a Material's `Color` and a GameObject's `Color`, so it is sRGB and must be decoded the same way. The alternative (document lights as linear) leaves a 0.5 grey light 2.3x brighter than a 0.5 grey surface. Flagged for the owner; proceed under this assumption.

**Files:**
- Create: `src/engine_datatypes/ColorSpace.hpp`
- Modify: `src/engine_core/SnapshotPump.cpp:67-70`, `src/engine_core/SnapshotPump.hpp:34`
- Modify: `src/runner/Renderer.hpp:171`, `src/runner/Renderer.cpp:1847-1851`
- Modify: `src/runner/DrawBatches.cpp:44-47`, `src/runner/DrawBatches.hpp:19`
- Modify: `src/engine_core/terrain/HeightDerive.cpp:13-16` (use the shared helper)
- Test: `sandbox/light_tests.cpp`, `sandbox/draw_batches_tests.cpp`

**Interfaces:**
- Produces: `inline float srgb_to_linear(float c)` in namespace `engine_core`, header `ColorSpace.hpp`: the piecewise sRGB transfer, identical to `surface.glsl:48-51`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/light_tests.cpp`, using the LIT7 rig at lines 203-212 (`SnapshotPump pump; pump.reserve(...)`, a `frame` lambda, `add_light<T>`):

```cpp
TEST_CASE("LIT9 a light's colour reaches the snapshot linear, as a Material's does", "[light][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    PointLight& light = add_light<PointLight>(game);
    // 0.5 is above the sRGB knee, 0.04045 is exactly at it, 1 is the top.
    REQUIRE_FALSE(light.set_color(engine_core::ColorRgb{0.5f, 0.04045f, 1.f, 1.f}));
    frame();
    REQUIRE(pump.find(light.id()) != nullptr);
    const VisualLight& shone = pump.find(light.id())->light;
    REQUIRE(shone.color[0] == Approx(0.2140f).margin(1e-3f));
    REQUIRE(shone.color[1] == Approx(0.04045f / 12.92f).margin(1e-6f));
    REQUIRE(shone.color[2] == Approx(1.f));
}
```

(`Approx` is `Catch::Approx`; the file already uses it or includes `catch2/catch_approx.hpp`.)

In `sandbox/draw_batches_tests.cpp`, test B7 (around line 192) currently pins the wrong curve:

```cpp
    REQUIRE(out.instances[1].tint[1] == Approx(std::pow(0.5f, 2.2f)));
```

Change it to the sRGB value and add a knee check in the same test:

```cpp
    REQUIRE(out.instances[1].tint[1] == Approx(0.2140f).margin(1e-3f));   // sRGB 0.5, not pow(0.5, 2.2)
```

Then append, using the file's `Frame` fixture (lines 21-50) and `BuildBatches`:

```cpp
TEST_CASE("B9 a tint at the sRGB knee decodes linearly, not by a power", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5, 0.f, {0.04045f, 0.f, 1.f});
    const DrawItem* items = frame.ready();
    DrawBatches out;
    BuildBatches(items, frame.visible, kView, out);
    REQUIRE(out.instances.size() == 1);
    REQUIRE(out.instances[0].tint[0] == Approx(0.04045f / 12.92f).margin(1e-6f));
    REQUIRE(out.instances[0].tint[1] == 0.f);
    REQUIRE(out.instances[0].tint[2] == 1.f);
}
```

- [ ] **Step 2: Run them to see them fail**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[light]"
build/Release/sandbox.exe "[batches]"
```

Expected: LIT9 fails (colour arrives raw, 0.5); B7's changed line and B9 fail (`pow(0.04045, 2.2)` is 0.00086, not 0.00313; `pow(0.5, 2.2)` is 0.2176, not 0.2140).

- [ ] **Step 3: Add the shared transfer function**

Create `src/engine_datatypes/ColorSpace.hpp`:

```cpp
#pragma once

#include <cmath>

namespace engine_core {

// The sRGB transfer curve, exactly as surface.glsl's toLinear: a color as a
// picker or an image holds it, made linear for lighting. Not a 2.2 power,
// so darks are not crushed. Any Color3 shown to a person is sRGB; anything
// multiplied into light must go through this first.
inline float srgb_to_linear(float c) {
    if (c <= 0.f) {
        return 0.f;
    }
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

}  // namespace engine_core
```

- [ ] **Step 4: Use it at the four sites**

`src/engine_core/SnapshotPump.cpp:67-70`:

```cpp
    const ColorRgb color = light->color();
    out.color[0] = srgb_to_linear(color.r);
    out.color[1] = srgb_to_linear(color.g);
    out.color[2] = srgb_to_linear(color.b);
```

and `#include "ColorSpace.hpp"`. Apply the same three lines wherever a `DirectionalLight`'s colour is copied into a `VisualLight` (grep `color.r` in `SnapshotPump.cpp`). Change the comment at `SnapshotPump.hpp:34` to `// Linear: the Color3, which is sRGB, decoded.` and at `Renderer.hpp:171` likewise.

`src/runner/Renderer.cpp:1847-1851`:

```cpp
    // Tint is a color as picked, sRGB, made linear as surface.glsl makes a Material's.
    const float exposure = std::max(lighting_.sky.exposure, 0.f);
    for (int channel = 0; channel < 3; ++channel) {
        skyColor_[channel] = dynamic ? 1.f : exposure * engine_core::srgb_to_linear(lighting_.sky.tint[channel]);
    }
```

`src/runner/DrawBatches.cpp:44-47`:

```cpp
    for (int channel = 0; channel < 3; ++channel) {
        const float tint = item.tint != nullptr ? item.tint[channel] : 1.f;
        data.tint[channel] = engine_core::srgb_to_linear(tint);
    }
```

`src/engine_core/terrain/HeightDerive.cpp:13-16`: delete the local `srgb_to_linear` and include `ColorSpace.hpp`.

Check `light_tests.cpp:228` (`REQUIRE(pump.find(sun.id())->light.color[1] == 0.f)`): 0 decodes to 0, so it still holds.

- [ ] **Step 5: Run the suites and look at the result**

```bash
cmake --build build --config Release --target sandbox AnarchyStudio
build/Release/sandbox.exe "[light]"
build/Release/sandbox.exe "[batches]"
build/Release/sandbox.exe "[terrain]"
```

Expected: pass. Then launch the studio over MCP and screenshot a place with a coloured PointLight and a coloured Material at the same Color3: they should now look the same brightness. White lights (1,1,1) are unchanged.

- [ ] **Step 6: Commit**

```bash
git add src/engine_datatypes/ColorSpace.hpp src/engine_core/SnapshotPump.cpp src/engine_core/SnapshotPump.hpp src/runner/Renderer.hpp src/runner/Renderer.cpp src/runner/DrawBatches.cpp src/runner/DrawBatches.hpp src/engine_core/terrain/HeightDerive.cpp sandbox/light_tests.cpp sandbox/draw_batches_tests.cpp
git commit -m "Lighting: every Color3 is sRGB; lights and tints decode with the one sRGB curve

A Light's Color was multiplied into light raw while a Material's was
decoded, so equal picks lit unequally. Tints used a 2.2 power where the
shader uses the piecewise curve.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 13: D5 Sliders agree with their writes

**Files:**
- Modify: `src/engine_instances/ScreenSpaceReflections.cpp:136-138`
- Modify: `src/engine_instances/AssetInstances.cpp:917-919`, `AssetInstances.hpp:271`
- Test: `sandbox/reflections_tests.cpp`

- [ ] **Step 1: Write the failing test**

`engine_core::lua_class_find(const char* class_name, std::string_view name)` (`LuaApi.hpp:300`) returns a registered class's `LuaField*` or null, and `LuaField` carries `slider_min`/`slider_max` (`LuaApi.hpp:91-92`). Append to `sandbox/reflections_tests.cpp`:

```cpp
TEST_CASE("SSR9 the MaxDistance slider spans the whole range the write takes", "[reflections]") {
    const engine_core::LuaField* field = engine_core::lua_class_find("ScreenSpaceReflections", "MaxDistance");
    REQUIRE(field != nullptr);
    REQUIRE(field->slider_max == engine_core::ScreenSpaceReflections::kMaxMaxDistance);
}
```

and to `sandbox/terrain_texture_tests.cpp` (Material's terrain fields are tested there):

```cpp
TEST_CASE("TT9 the TextureScale slider does not start at a value the write refuses", "[terrain]") {
    const engine_core::LuaField* field = engine_core::lua_class_find("Material", "TextureScale");
    REQUIRE(field != nullptr);
    REQUIRE(field->slider_min > 0.0);
    REQUIRE(field->slider_min == engine_core::Material::kMinTextureScaleSlider);
}
```

Both files need `#include "LuaApi.hpp"` if they do not have it.

- [ ] **Step 2: Run to see them fail**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "SSR9*"
build/Release/sandbox.exe "TT9*"
```

Expected: SSR9 fails (200 != 1000); TT9 fails to compile (`kMinTextureScaleSlider` undefined) or fails on `slider_min > 0`.

- [ ] **Step 3: Fix both**

`src/engine_instances/ScreenSpaceReflections.cpp:136-138`: replace the literal `200.0` with `SSR::kMaxMaxDistance`.

`src/engine_instances/AssetInstances.hpp:271`, after `kDefaultTextureScale`, add:

```cpp
    // Where the Properties slider starts. The write refuses 0 ("must be
    // greater than 0"), so the slider's left stop is the smallest sensible
    // repeat rather than a refused value.
    static constexpr double kMinTextureScaleSlider = 0.25;
```

`src/engine_instances/AssetInstances.cpp:917-919`: change `0.0, 16.0` to `Material::kMinTextureScaleSlider, 16.0`.

- [ ] **Step 4: Run the suites**

```bash
cmake --build build --config Release --target sandbox properties-tests
build/Release/sandbox.exe "[reflections]"
build/Release/sandbox.exe "[terrain]"
build/Release/properties-tests.exe
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_instances/ScreenSpaceReflections.cpp src/engine_instances/AssetInstances.cpp src/engine_instances/AssetInstances.hpp sandbox/reflections_tests.cpp sandbox/terrain_texture_tests.cpp
git commit -m "Properties: the SSR MaxDistance and Material TextureScale sliders match their writes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 14: D1 DynamicSky defaults: code is the truth; README and tests follow

**Assumption:** commit `2d00273` chose Brightness 2 and disc size 4 on purpose ("new sky and roughness defaults"), so the code stands and the README and tests change. If the owner prefers 3 / 2 / 2, swap the direction of this task: change `DynamicSky.hpp:43, 50-51` instead and leave the test lines for brightness and size as they are.

**Files:**
- Modify: `sandbox/dynamic_sky_tests.cpp:62, 64-65, 70-71`
- Modify: `README.md:35`

- [ ] **Step 1: Run DS1 to see the current failure**

```bash
build/Release/sandbox.exe "[dynamic_sky]"
```

Expected: DS1 fails at `brightness() == 3.0` (and would fail at the cloud and size lines).

- [ ] **Step 2: Fix the test to the code's constants**

`sandbox/dynamic_sky_tests.cpp:62`: `REQUIRE(sky.brightness() == DynamicSky::kDefaultBrightness);`
Line 64: `REQUIRE(sky.cloud_cover() == DynamicSky::kDefaultCloudCover);`
Line 65: `REQUIRE(sky.cloud_density() == DynamicSky::kDefaultCloudDensity);`
Line 70: `REQUIRE(sky.sun_size() == DynamicSky::kDefaultSunSize);`
Line 71: `REQUIRE(sky.moon_size() == DynamicSky::kDefaultMoonSize);`

Then, so the test still pins the numbers a reader expects, add directly after line 71:

```cpp
    // The documented defaults, as README.md states them.
    REQUIRE(DynamicSky::kDefaultBrightness == 2.0);
    REQUIRE(DynamicSky::kDefaultCloudCover == 0.3);
    REQUIRE(DynamicSky::kDefaultCloudDensity == 0.0);
    REQUIRE(DynamicSky::kDefaultSunSize == 4.0);
    REQUIRE(DynamicSky::kDefaultMoonSize == 4.0);
```

- [ ] **Step 3: Fix the README**

In `README.md` line 35, change `` `Brightness` (0 to 20, 3 by default) `` to `` `Brightness` (0 to 20, 2 by default) `` and `` `SunSize` and `MoonSize` (degrees, 2 by default) `` to `` `SunSize` and `MoonSize` (degrees, 4 by default) ``. Check `docs/superpowers/plans/2026-10-05-dynamic-sky.md:574` and leave it (a historical plan), but add one line under it: "Defaults changed in 2d00273 to Brightness 2, SunSize/MoonSize 4."

- [ ] **Step 4: Run**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[dynamic_sky]"
```

Expected: DS1 and DS3 pass. Remove the "DS1/DS3 CloudCover default" entry from any flake list you maintain.

- [ ] **Step 5: Commit**

```bash
git add sandbox/dynamic_sky_tests.cpp README.md docs/superpowers/plans/2026-10-05-dynamic-sky.md
git commit -m "DynamicSky: README and tests follow the defaults 2d00273 chose

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 15: D2, D3, D4 README and doc-string drift

**Files:**
- Modify: `README.md:43, 47` and lines 21, 35, 39, 41 (studs)
- Modify: `src/engine_instances/README.md:3, 7`
- Modify: `src/engine_core/LuaApi.cpp:1037, 1048, 1080, 1121, 1204, 1242, 1253, 1323, 1325, 1490, 1512`
- Modify: comments in `SnapshotPump.hpp:118,153,164`, `terrain/ShapeDistance.hpp:54`, `terrain/VoxelChunk.hpp:44-45`, `terrain/VoxelVolume.hpp:79`, `AmbientOcclusionEffect.hpp:21`, `Dragger.hpp:19`, `DynamicSky.hpp:28`, `ScreenSpaceReflections.hpp:19`, `Terrain.hpp:26`, `SceneService.hpp:49`, `runner/Renderer.hpp:183,238,250,274`, `runner/ShadowMath.hpp:8`, `runner/SkyMath.hpp:17`

- [ ] **Step 1: Camera and lights are PVInstances (D2)**

`README.md:47`, change:

> Every class with a `Transform` is a `PVInstance`: `GameObject`, its subclasses `Camera`, `PointLight`, and `SpotLight`, `PhysicsObject`, `PlayerController`, and `Attachment`.

to:

> Every class with a `Transform` is a `PVInstance`: `GameObject`, `Camera`, `PointLight`, `SpotLight`, `DirectionalLight`, `PhysicsObject`, `PlayerController`, `Brush`, `Attachment`, `Terrain`, and `Dragger`. `Camera` and the lights are not GameObjects: they have no Prefab, Scale, or Color, and `IsA("GameObject")` is false of them.

`src/engine_instances/README.md:7`, change `` `Camera` is a `GameObject` with a `FieldOfView` `` to `` `Camera` is a `PVInstance` (a `SpatialObject`, not a GameObject) with a `FieldOfView` ``. In line 3's class list, add `` `PointLight`, `SpotLight`, `DirectionalLight`, `Brush`, `Dragger`, `WireframeAdornment` `` after `` `Camera` ``.

- [ ] **Step 2: PhysicsObject Hull and Custom (D3)**

`README.md:43`, change:

> `Size` sizes it: a Box's extents, a Sphere's diameter in X, a Capsule's diameter in X and height in Y, and the box a Hull's or Custom's mesh is fitted to.

to:

> `Size` sizes a Box (its extents), a Sphere (its diameter in X), and a Capsule (its diameter in X and height in Y); a Hull or a Custom is its `Mesh` at the Mesh's own size, and `Size` plays no part.

and:

> Box3D lets a whole mesh collide only on a static body, so a Custom is its whole Mesh while `Anchored`, and a Hull of it, with a warning, while not.

to:

> Box3D lets a whole mesh collide only on a static body, so a Custom is its whole Mesh while `Anchored`; while not, it is a set of convex pieces that together fill the Mesh, built once and cached, so its hollows and steps still collide. When no piece can be built it falls back to a Hull and says so once.

- [ ] **Step 3: Units, not studs (D4)**

Run:

```bash
grep -rln "studs\|Studs" README.md src --include='*.cpp' --include='*.hpp' --include='*.md'
```

In every hit replace "studs" with "units" and "Studs" with "Units", keeping the sentence grammatical ("How many units this light reaches", "units per second"). `ShadowMath.hpp:8` becomes `Lengths are world units, and ...`. In `VoxelChunk.hpp:44-45` rename the parameter `studs` to `distance`. Re-run the grep; expected: no hits.

- [ ] **Step 4: Build and run the doc-touching suites**

```bash
cmake --build build --config Release --target sandbox engine-tests
build/Release/sandbox.exe "[A1],[A2],[A3]"
build/Release/engine-tests.exe
```

Expected: pass (hover and completion tests read the doc strings; none asserts the word "studs". If one does, update its expected string).

- [ ] **Step 5: Commit**

```bash
git add README.md src/engine_instances/README.md src/engine_core/LuaApi.cpp src/engine_core/SnapshotPump.hpp src/engine_core/terrain src/engine_instances src/engine_services/SceneService.hpp src/runner
git commit -m "Docs: Camera and lights are PVInstances, Hull and Custom ignore Size, units not studs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 16: D8 Stale comments, the joke menu item, and the MCP descriptions

**Files:**
- Modify: `src/ide/McpToolSpecs.cpp:48, 66`
- Modify: `src/runner/Renderer.hpp:104-107`, `src/runner/Visibility.hpp:41-43`
- Modify: `src/engine_core/TerrainWorld.hpp:108-110`, `src/engine_core/PhysicsWorld.hpp:89-90, 145-146`
- Modify: `src/ide/PropertiesPanel.cpp:48-49`, `src/ide/IdeLayout.cpp:166`
- Modify: `src/engine_core/ScriptAnalysis.cpp:1402, 1986-1987`
- Modify: `src/ide/LuauComplete.cpp:2300-2389, 2571`, `src/ide/LuauComplete.hpp:144`
- Modify: `src/engine_core/DataModel.cpp:2198-2208`, `src/engine_core/InstanceFile.cpp:192-195`, `src/engine_core/Project.cpp:1076-1082`
- Test: `tests/McpTest.cpp`

- [ ] **Step 1: MCP descriptions name all five services and all three import kinds**

`src/ide/McpToolSpecs.cpp:48`: change `which holds only the scene services Workspace, Lighting, Storage, and Scripts` to `which holds only the scene services Workspace, Lighting, Storage, Scripts, and Gui`.
Line 66: change `"Absolute paths of image and model files."` to `"Absolute paths of image, sound, and model files."`

Add to `tests/McpTest.cpp`, in the test that lists tools (grep `tools/list`):

```cpp
    // The create_instance parent description names every scene service.
    CHECK(listing.find("Workspace, Lighting, Storage, Scripts, and Gui") != std::string::npos);
```

- [ ] **Step 2: Comments that describe code that is gone**

- `Renderer.hpp:104-107`: replace `(Task 6 reads it; nothing draws textured yet)` with `(terrain.frag's fetchLook reads it)`.
- `Visibility.hpp:41-43`: `screenRadius` comment becomes `// Its sphere's radius as projected, in pixels; infinite with the camera inside it. Read by tests and kept for a future LOD pick.` and the `lod` comment becomes `// Which LOD it draws: copied from MeshDraw::lod by findVisible.`
- `TerrainWorld.hpp:108-110` and `PhysicsWorld.hpp:89-90, 145-146`: replace "PhysicsWorld's ask, at the start of its own sync" / "asking that TerrainWorld (set_collider_interest)" with the real path: `PhysicsWorld::sync drives this through change_collider_interest; set_collider_interest is the direct form tests use.`
- `PropertiesPanel.cpp:48-49`: delete the orphan comment line `// Same wait as the explorer: a busy simulation step must not freeze the shell.` and the blank line after it.
- `IdeLayout.cpp:166`: delete `AddItem(*view, "Maybe :)", "Smile.png", 0, 0);` and, if the separator above it is now trailing, delete that too. Remove `Smile.png` from `resources/` only if nothing else references it (`grep -rn Smile.png src resources`).
- `ScriptAnalysis.cpp:1402`: `if (index == 0 && (with_self || (fn.hasSelf && with_self)))` becomes `if (index == 0 && with_self)`.
- `ScriptAnalysis.cpp:1986-1987`: delete `std::string named;` and the `describe_object(...)` call whose only output is `named`, unless `describe_object` also fills `out.receiver_instance_known`/`out.receiver_instance` (it does, per the call's arguments): then keep the call and rename `named` to `unused_name` with a comment `// Only the instance fields are wanted here.`
- `LuauComplete.cpp:2300-2389`: `plan_hover` ignores `world`, `script_id`, `script_global`. Remove the three parameters from the signature, the header at `LuauComplete.hpp:144`, and the two callers (`ask_hover` at `:2571`, `hover_luau` at `:~2609`). Rebuild `engine-tests` to confirm nothing else called it.

- [ ] **Step 3: One rule for extras (D8 last item)**

`DataModel::merged_properties` (`DataModel.cpp:2198-2208`) says a class property wins over a stale extra. Make `InstanceFile.cpp:192-195` and `Project.cpp:1076-1082` do the same: in each, change the loop that applies extras so it skips a key `bag` already holds. The shape in both is:

```cpp
    object->save_properties(bag);
    for (const auto& [key, value] : object->extras()) {
        if (bag_find(bag, key) == nullptr) {   // a key the class writes wins over a stale extra
            bag_set(bag, key, value);
        }
    }
```

(Use the real iteration and `bag_find`/`bag_set` names from each file.) Add a test to `sandbox/instance_file_tests.cpp`: create a `GameObject`, call `set_extra_property("Name", JsonValue::string("stale"))` on it, run `copy_tree`/`save_instance_file` to a bag or file, and assert the saved `Name` is the live name, not `"stale"`.

- [ ] **Step 4: Build and run**

```bash
cmake --build build --config Release --target sandbox engine-tests mcp-tests AnarchyStudio
build/Release/sandbox.exe "[instance_file],[project]"
build/Release/engine-tests.exe
build/Release/mcp-tests.exe
```

Expected: pass, build has no new warnings.

- [ ] **Step 5: Commit**

```bash
git add src/ide/McpToolSpecs.cpp src/runner/Renderer.hpp src/runner/Visibility.hpp src/engine_core/TerrainWorld.hpp src/engine_core/PhysicsWorld.hpp src/ide/PropertiesPanel.cpp src/ide/IdeLayout.cpp src/engine_core/ScriptAnalysis.cpp src/ide/LuauComplete.cpp src/ide/LuauComplete.hpp src/engine_core/InstanceFile.cpp src/engine_core/Project.cpp sandbox/instance_file_tests.cpp tests/McpTest.cpp
git commit -m "Stale comments, the placeholder menu item, MCP descriptions, and one rule for extras

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 17: Dead code, part 1: engine

**Files:**
- Modify: `src/engine_core/PhysicsWorld.cpp:209`
- Modify: `src/engine_core/terrain/VoxelVolume.hpp:91`
- Modify: `src/engine_core/terrain/AlodStore.hpp:81`, `AlodStore.cpp:383-390`
- Modify: `src/engine_core/BrushVisuals.cpp:187-194, 229, 285`
- Modify: `src/engine_core/PluginUi.hpp:108-110, 156`, `PluginUi.cpp:399`
- Modify: `src/engine_core/LuaEngine.hpp:20, 29-32`, `LuaEngine.cpp:72-74, 115-123, 130-134`
- Modify: `src/engine_core/SnapshotPump.hpp:309, 382-383`, `SnapshotPump.cpp:157-163, 749-752`
- Modify: `src/engine_services/SceneService.hpp:35, 38`, `SceneService.cpp:17-26`

Each removal was verified unreferenced across `src/`, `tests/`, `sandbox/`, `resources/` at `2676e0b`. Re-verify each with grep before deleting, since Task 1 through 16 may have been applied first.

- [ ] **Step 1: Delete**

For each item, `grep -rn -w <name> src tests sandbox resources` must show only the declaration and definition; then delete both.

- `same_vec3` (`PhysicsWorld.cpp:209`).
- `VoxelVolume::has_dirty()` (`VoxelVolume.hpp:91`).
- `AlodStore::live_bytes()` (declaration and definition).
- `BrushVisuals.cpp` `texture_scales` lambda: drop the `const Brush& brush` parameter and the `(void)brush;`, and change both call sites to `texture_scales(*mesh);`.
- `PluginUi::activations()` and `activations_` and the `++activations_`/`fetch_add` at `PluginUi.cpp:399`.
- `HostArgs::isNil`, `pushNil`, `pushBoolean`, `pushString` (declarations and definitions). Keep `isBoolean`, `isNumber`, `isString`, `pushNumber`, which `Runner.cpp`/`engine-tests` use (confirm by grep).
- `SnapshotPump::set_camera`, `camera_pending_`, `pending_camera_`, and the `if (camera_pending_) { ... }` block at `SnapshotPump.cpp:749-752`.
- `is_scene_service_class` and `scene_service_guid` (declarations in `SceneService.hpp:35, 38`, definitions in `SceneService.cpp:17-26`). Keep `kSceneServiceClasses` and the private `service_guid`.

- [ ] **Step 2: Build everything and run the sandbox**

```bash
cmake --build build --config Release
build/Release/sandbox.exe
build/Release/engine-tests.exe
```

Expected: full build clean (a stale caller would fail to link); sandbox has no new failures.

- [ ] **Step 3: Commit**

```bash
git add -A src/engine_core src/engine_services
git commit -m "Engine: remove unreferenced helpers and the unused snapshot camera plumbing

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 18: Dead code, part 2: IDE and renderer

**Files:**
- Modify: `src/ide/IdeLayout.hpp:474`, `IdeLayoutProject.cpp:33-36`
- Modify: `src/ide/EditorFont.hpp:16`, `EditorFont.cpp:222`
- Modify: `src/ide/IdePrefabEditor.hpp:89`, `IdePrefabEditor.cpp:1162`
- Modify: `src/ide/CompletionPopup.hpp:48`, `CompletionPopup.cpp:731`
- Modify: `src/ide/IdeExplorer.hpp:186`, `IdeExplorer.cpp:33, 369`
- Modify: `src/runner/Renderer.hpp:480-484, 503, 520-527`, `Renderer.cpp:250-253, 266, 273-280, 2165`
- Modify: `resources/shaders/pipeline/terrain.frag:43`
- Modify: `src/runner/gl.hpp:324, 345, 387, 392, 486, 515, 520`, `gl.cpp:45, 66, 99, 104, 196, 217, 251, 256`

- [ ] **Step 1: IDE removals**

Re-verify each with `grep -rn -w <name> src tests sandbox resources`, then delete declaration and definition:

- `IdeLayout::conflicts_pane()`.
- `editor_font_choice()` (keep `set_editor_font_choice` and `editor_font_wanted`, which are used).
- `IdePrefabEditor::newModelTile()`.
- `CompletionPopup::replaceEnd()`.
- `IdeExplorer::menu_` member.
- `IdeExplorer.cpp:33` `kActionWait`: delete it and change line 369 to use `kDropLockWait` from `LockWaits.hpp` (already included), which is the same 250 ms.

- [ ] **Step 2: Renderer removals**

In `Renderer.hpp`, delete the `Program` members `diffuse`, `normalMap`, `roughnessMap`, `metalnessMap`, `emissiveMap`, `nodeLevel`, `depth`, `albedo`, `normal`, `material`, `emissiveBuffer`, `accumulation`, `transparencyBuffer`, `scene`, and in `Renderer.cpp:250-280` their `program.X = at("uX");` assignments. Delete the `glUniform1i(terrain_.nodeLevel, ...)` line at `Renderer.cpp:2165`. In `terrain.frag:43` delete `uniform int uNodeLevel;` (the four other mentions are comments; leave them, they explain history). Before deleting any member, grep `program\.<member>\b\|_\.<member>\b` across `src/runner` to confirm it is only assigned: the sampler units are bound by the separate `sampler(...)` lambda at `Renderer.cpp:337-376`.

In `gl.hpp`/`gl.cpp`, delete the four loaders `rt_glGetTexParameteriv`, `rt_glUniform1fv`, `rt_glQueryCounter`, `rt_glGetInteger64v`: the `extern` declarations, the `#define`s, the definitions, and the `LOAD(...)`/`LOAD_OPTIONAL(...)` lines.

- [ ] **Step 3: Build everything, run the IDE test programs, and look at a terrain**

```bash
cmake --build build --config Release
build/Release/studio-tests.exe
build/Release/explorer-tests.exe
build/Release/prefab-editor-tests.exe
build/Release/sandbox.exe "[terrain]"
```

Expected: clean build; tests pass (the studio "six frames" flake is pre-existing). Launch the studio over MCP and screenshot a textured Terrain: it must still draw textured (the `uNodeLevel` removal must not have broken shader compilation; check stderr for GLSL errors).

- [ ] **Step 4: Commit**

```bash
git add -A src/ide src/runner resources/shaders/pipeline/terrain.frag
git commit -m "IDE and renderer: remove unreferenced members, uniform slots, and GL loaders

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 19: `ancestry_changed` is pinned by a test, like `child_added`

**Scope correction.** The audit suggested exposing `AncestryChanged` to scripts "the way ChildAdded is". Reading the registration (`DataModel.cpp:2329-2343`) shows only `Changed` reaches Lua: `ChildAdded` and `ChildRemoved` are C++-only signals whose sole callers are `sandbox/tests.cpp:1594-1598, 4085`. `ancestry_changed` is the same kind of accessor with no test, which is why the dead-code scan caught it and not its siblings. The emit path (`DataModel::emit_ancestry`, `DataModel.cpp:1357-1370`, walking the moved subtree) is real behaviour nothing pins. This task pins it. Exposing all three signals to Luau is a feature for the owner to decide; it is listed under Deferred.

**Files:**
- Test: `sandbox/tests.cpp`

**Interfaces:**
- Consumes: `DataModel::ancestry_changed(InstanceId) -> Signal&` (`DataModel.hpp:382`), `Signal::connect(std::function<void(InstanceId, Field)>)`, `game.events().drain()` under a `SimRole`.

- [ ] **Step 1: Write the test**

Append to `sandbox/tests.cpp` after the ChildAdded test that ends near line 1640 (grep `root_added` to find it):

```cpp
TEST_CASE("SG9 ancestry_changed fires on the moved instance and every descendant, once each", "[signals]") {
    engine_core::Game game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));
    engine_core::Folder& child = game.create<engine_core::Folder>();
    game.set_parent(child.id(), folder.id());
    engine_core::GameObject& grandchild = game.create<engine_core::GameObject>();
    game.set_parent(grandchild.id(), child.id());
    engine_core::Folder& bystander = game.create<engine_core::Folder>();
    game.set_parent(bystander.id(), workspace_of(game));

    int folder_fired = 0, child_fired = 0, grandchild_fired = 0, bystander_fired = 0;
    game.ancestry_changed(folder.id()).connect([&](engine_core::InstanceId id, engine_core::Field field) {
        REQUIRE(id == folder.id());
        REQUIRE(field == engine_core::Field::Parent);
        ++folder_fired;
    });
    game.ancestry_changed(child.id()).connect([&](engine_core::InstanceId id, engine_core::Field) {
        REQUIRE(id == child.id());
        ++child_fired;
    });
    game.ancestry_changed(grandchild.id()).connect([&](engine_core::InstanceId, engine_core::Field) { ++grandchild_fired; });
    game.ancestry_changed(bystander.id()).connect([&](engine_core::InstanceId, engine_core::Field) { ++bystander_fired; });

    game.set_parent(folder.id(), game.scene_service("Storage"));
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(folder_fired == 1);
    REQUIRE(child_fired == 1);
    REQUIRE(grandchild_fired == 1);
    REQUIRE(bystander_fired == 0);

    // A move to the same parent is not a change.
    game.set_parent(folder.id(), game.scene_service("Storage"));
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(folder_fired == 1);
}
```

`workspace_of` and `SimRole` are in `sandbox/support.hpp` (the file's earlier tests use both). Add `#include "Folder.hpp"` if the file lacks it.

- [ ] **Step 2: Run it**

```bash
cmake --build build --config Release --target sandbox
build/Release/sandbox.exe "[signals]"
```

Expected: PASS. If the second `set_parent` to the same parent does fire (the contract at `DataModel.cpp:1506-1533` may emit before checking for no change), change the final `REQUIRE` to match what the code does and note it in the test's comment: the point is to pin the behaviour, not to change it in this task.

- [ ] **Step 3: Note it in the header**

`src/engine_core/DataModel.hpp:382`, above `ancestry_changed`, add: `// C++ only, as child_added and child_removed are; scripts see Changed alone. See sandbox SG9.`

- [ ] **Step 4: Commit**

```bash
git add sandbox/tests.cpp src/engine_core/DataModel.hpp
git commit -m "Tests: pin ancestry_changed on a moved subtree

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 20: Housekeeping and the audit's status

**Files:**
- Delete: `.worktrees/luau-ac/` (257 MB, not a registered worktree)
- Modify: `docs/reviews/2026-10-10-code-audit.md` (status table at the top)

- [ ] **Step 1: Confirm the orphan**

```bash
git worktree list
ls .worktrees
```

Expected: `git worktree list` shows only the main checkout and `../AnarchyEngine-buildspeed`; `.worktrees/luau-ac` is not listed. Confirm it has no uncommitted work worth keeping:

```bash
git -C .worktrees/luau-ac status --short | head
git -C .worktrees/luau-ac log --oneline -1
```

If `status` is empty and the commit is on `main` (`git branch --contains <sha>` lists main), delete it:

```bash
rm -rf .worktrees/luau-ac
```

If it has uncommitted changes, stop and ask the owner.

- [ ] **Step 2: Mark the audit**

Add a short "Status" section under the header table of `docs/reviews/2026-10-10-code-audit.md`:

```markdown
## Status

Fixed on branch `audit-fixes` (plan: `docs/superpowers/plans/2026-10-10-audit-fixes.md`): B1-B8, B10-B13, B15, D1-D6, D8, and section 4 dead code; `ancestry_changed` kept and pinned by a test rather than removed. Deferred to a follow-up plan: B9 (needs a render-thread hand-off point), B14 (latent by design), D7 (alpha-cutout shadows), exposing the three instance signals to Luau, and section 5 duplication P1-P8.
```

- [ ] **Step 3: Full build and full test run**

```bash
cmake --build build --config Release
cd build && ctest -C Release --output-on-failure; cd ..
```

Expected: every suite passes except the known pre-existing flakes (studio-tests "six frames", jadefx tree-view). DS1/DS3 must now pass.

- [ ] **Step 4: Commit**

```bash
git add docs/reviews/2026-10-10-code-audit.md
git commit -m "Audit: record what the audit-fixes branch addressed

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Deferred to the next plan

Written here so nothing is lost; none of it belongs in a behaviour-fix branch.

- **D7** alpha-cutout shadows: give `shadow.frag` an optional diffuse sampler and the `deferred.frag` discard threshold, enabled per batch for materials with an alpha texture.
- **B9** freeing a retired render-phase `TaskScheduler` entry needs a hand-off point where the render thread is known to be outside `run_phase` (it calls `entry.job(dt)` through the entry and iterates `jobs_` without a lock). Add one (a flag set by the render loop between phases, or a generation the render thread publishes) and erase under it.
- **B14** optimistic rename: acceptable while `rename_error` refuses only services and dead ids; revisit if it grows rules.
- **Instance signals in Luau**: `ChildAdded`, `ChildRemoved`, and `AncestryChanged` exist as C++ `Signal`s with emit paths but only `Changed` is registered for scripts (`DataModel.cpp:2329-2343`). Exposing them is one `lua_property(..., "Signal", ...)` each plus a resolver in `ScriptBindings`, docs in `LuaApi.cpp`, and a README sentence. A feature, so the owner decides.
- **P1** `engine_core/LuaSlotHelpers.hpp` hoisting `physics_detail` and adding `set_enum_property`/`set_color_property` (18 files, ~400 lines).
- **P2** `instance_self<T>`, `check_vector3`, one metatable installer, one registry weak-cache helper.
- **P3** one Luau tokenizer with a keyword-set parameter for `LuauHighlight`, `LuauComplete`, and both `ScriptPairs` scanners.
- **P4** a shared source-editor persistence core for `IdeScriptEditor` and `IdeCssEditor`; resolve the `acked != epoch` ordering drift deliberately.
- **P5/P6** `ide/CardWidgets.hpp`, a card-editor base for Prefab and Terrain editors, everyone on `NodeClasses.hpp`/`Strings.hpp`, `prune_alerts`, `InsertResult::finish`, a shared scrolling-rows popup core, `CompletionPopup::handleKey`, one Windows quoting helper, `extension_in`/`free_name` for importers.
- **P7** `collect_ids`, `TerrainWorld::queue_chunk`, one `b3MeshDef` builder, `chunk_at_local`, one FNV-1a, one little-endian codec, `VectorMath.hpp` in `DraggerMath`/`LodBuilder`, drop the outer rename in `save_instance_file`.
- **P8** `MaterialDraw`, `ResourceStamp`, `DeleteFramebuffer`, a 1x1 texture helper, merge `outlinePass`/`handlePass`, anisotropy on the quality-change path, `sampleAxis` in `terrain.frag`.
