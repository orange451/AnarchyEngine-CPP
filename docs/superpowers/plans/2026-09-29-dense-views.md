# Dense Views Implementation Plan (flecs)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** GameObject's spatial data lives in flecs components; Heartbeat stepping and physics are flecs queries; the render snapshot holds a row only for live GameObjects under Workspace, kept current by scope tags and an `Ancestry` invalidation.

**Architecture:** Every live instance owns a flecs entity (issued with its slot, deleted with it). Components (`Transform`, `ColorRgb`, `ecs::Size`, `ecs::Velocity`, `ecs::Instance`) and tags (`ecs::InGame`, `ecs::InWorkspace`, `ecs::Steps`, `ecs::Simulated`, `ecs::VisualOnly`) replace GameObject's members and `Slot`'s flags. Hot paths call flecs' C API with ids cached per world. The pump stays incremental on `DenseIdSet`.

**Tech Stack:** C++17, MSVC 19.23 (Debug config: `./build/Debug/sandbox.exe`), CMake 3.16, flecs v4.1.6, Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-09-29-dense-views-design.md` (revised 2026-09-30 for flecs)

## Global Constraints

- Worktree `.worktrees/dense-views`, branch `dense-views`. Do not `cd` out of it. Never bare `git stash`.
- flecs v4.1.6, `distr/flecs.h` SHA256 `526036a5a41678e2a43a3cb835e9eaa70fd1993868b1978950c0d275752f69b1`, `distr/flecs.c` SHA256 `6005392eb13c0f3c7abdecb2f85271e50934f63919afebf0bbe85f6dfc7320d6`.
- Public flecs definitions: `FLECS_CPP_NO_ENUM_REFLECTION FLECS_CUSTOM_BUILD FLECS_CPP FLECS_LOG FLECS_OS_API_IMPL`, plus `FLECS_NDEBUG` (or `FLECS_DEBUG` when `ENGINE_FLECS_CHECKS=ON`). Never define `flecs_STATIC`.
- `flecs.h` is included only by `src/engine_core/Ecs.hpp`, inside `#pragma warning(push, 0)`/`pop`. Only `engine_core` and `engine_instances` sources include `Ecs.hpp`; `DataModel.hpp` does not.
- The C++ flecs API is for setup (world, registration, query building). Per-access and per-frame code uses the C API with `EcsIds`.
- No flecs structural change (add/remove component or tag, create/delete entity) inside a running query iteration.
- The world is touched only under the DataModel lock or on the gameplay thread before threads start.
- Engine code builds with no new warnings at `/W4`.
- Every task ends with the FULL suite green: `./build/Debug/sandbox.exe` → 0 failures (baseline after Task 2: 326 cases, 1 skipped).
- Build with `cmake --build build --target sandbox --parallel`. After restoring a file from a copy, `touch` it (MSBuild uses timestamps).
- Step and physics order is unspecified; no test asserts an order.

## Review Focus

1. Undo of a destroy, and Stop, recreate the entity with the right components and tags (Simulated, VisualOnly, scope) — Task 4 "undo and Stop keep spatial values and flags", Task 5 "destroy clears scope; undo restores it" and "Stop restores scope".
2. A stale `GameObject&` held across its destroy reads zero, never another entity's data — Task 4 "a destroyed GameObject reads zero".
3. A GameObject recolored while in Storage arrives complete when moved into Workspace — Task 7 "a row arrives complete".
4. `destroy_tree` of a Folder subtree under Workspace removes every row and stepper — Task 7 "destroy_tree clears rows".
5. `Instance.new("GameObject")` parented into Workspace renders — Task 7 "Instance.new renders".

---

### Task 1: DenseIdSet — DONE (e7fb497)

`src/engine_core/DenseIdSet.hpp`, `sandbox/dense_views_tests.cpp` `[dense]` tests. See git.

### Task 2: SnapshotPump on DenseIdSet — DONE (87f92a2; T4 race fix c5fe2be)

`base_ids_` replaced `base_index_`/`remember`. See git and the ledger ruling.

---

### Task 3: Flecs dependency, world, and an entity per instance

**Files:**
- Create: `cmake/flecs/CMakeLists.txt`
- Create: `src/engine_core/Ecs.hpp`, `src/engine_core/Ecs.cpp`
- Modify: `CMakeLists.txt` (FetchContent after stb_image_write; `add_subdirectory`; `Ecs.cpp` in `engine_core`; link `flecs` PRIVATE into `engine_core` and `engine_instances`; option)
- Modify: `src/engine_core/DataModel.hpp` (Slot::entity; `entity_count()`; private ecs accessors; forward decls)
- Modify: `src/engine_core/DataModelState.hpp` (world + ids)
- Modify: `src/engine_core/DataModel.cpp` (root ctor; `spawn`; `release_to_pool`; accessors)
- Modify: `src/engine_core/DataModelPlace.cpp` (`adopt_slot`)
- Test: `sandbox/dense_views_tests.cpp`

**Interfaces:**
- Produces: `namespace engine_core::ecs { struct Size{float x=1,y=1,z=1;}; struct Velocity{float x=0,y=0,z=0;}; struct Instance{InstanceId id=0;}; struct InGame{}; struct InWorkspace{}; struct Steps{}; struct Simulated{}; struct VisualOnly{}; }`, `struct engine_core::EcsIds { ecs_id_t transform, color, size, velocity, instance, in_game, in_workspace, steps, simulated, visual_only; }`, `EcsIds register_ecs(flecs::world&)`. `Slot::entity` (`std::uint64_t`, 0 = none). Private `DataModel`: `ecs_world_t* ecs_world() const`, `const EcsIds& ecs_ids() const`, `std::uint64_t entity_of(InstanceId) const` (0 when dead), `void issue_entity(Slot&, InstanceId)`. Public: `std::size_t entity_count() const`.

- [ ] **Step 1: Failing test**

Append to `sandbox/dense_views_tests.cpp`:

```cpp
TEST_CASE("every live instance has one entity", "[dense][entity]") {
    SimRole role;
    engine_core::Game game;
    const std::size_t services = game.entity_count();
    REQUIRE(services > 0);  // the service tree already has entities
    engine_core::DataModel& folder = game.create();
    engine_core::GameObject& part = game.create_game_object();
    REQUIRE(game.entity_count() == services + 2);
    game.set_parent(part.id(), folder.id());
    game.destroy(folder.id());  // orphans part, deletes folder's entity
    REQUIRE(game.entity_count() == services + 1);
    game.destroy(part.id());
    REQUIRE(game.entity_count() == services);
    game.history().undo();  // revives part
    REQUIRE(game.entity_count() == services + 1);
}
```

(Check `sandbox/history_tests.cpp` for whether a freshly built `Game` records history for these edits; if the first undo revives something else, undo until `game.alive(part_id)` and assert the count then.)

- [ ] **Step 2: Run → compile FAIL** (`entity_count` missing).

- [ ] **Step 3: CMake**

`cmake/flecs/CMakeLists.txt`:

```cmake
# flecs builds in its own directory so its Debug C flags can differ from the
# engine's: /RTC1 cannot combine with /O2, and flecs is optimized in every
# configuration (a Debug entity read is ~14 ns instead of ~340 ns).
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "/Od" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
add_library(flecs STATIC "${FLECS_SOURCE_DIR}/flecs.c")
target_include_directories(flecs PUBLIC "${FLECS_SOURCE_DIR}")
target_compile_definitions(flecs PUBLIC
    FLECS_CPP_NO_ENUM_REFLECTION FLECS_CUSTOM_BUILD FLECS_CPP FLECS_LOG FLECS_OS_API_IMPL)
if(ENGINE_FLECS_CHECKS)
    target_compile_definitions(flecs PUBLIC FLECS_DEBUG)
else()
    target_compile_definitions(flecs PUBLIC FLECS_NDEBUG)
endif()
if(MSVC)
    target_compile_options(flecs PRIVATE $<$<CONFIG:Debug>:/O2>)
endif()
```

Root `CMakeLists.txt`, after the stb_image_write block, two `FetchContent_Declare` blocks in the httplib style (`URL`, `URL_HASH SHA256=...`, `DOWNLOAD_NO_EXTRACT TRUE`, `DOWNLOAD_DIR "${FETCHCONTENT_BASE_DIR}/flecs-src"` for both so the two files share a directory), names `flecs_header` and `flecs_source`, URLs `https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/distr/flecs.h` and `.../flecs.c`, populated the same way; then:

```cmake
option(ENGINE_FLECS_CHECKS "Build flecs with its debug checks (slower entity access)" OFF)
set(FLECS_SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/flecs-src")
add_subdirectory(cmake/flecs "${CMAKE_BINARY_DIR}/flecs")
```

Add `src/engine_core/Ecs.cpp` to `engine_core`'s sources, and after the package links: `target_link_libraries(engine_core PRIVATE flecs)` and `target_link_libraries(engine_instances PRIVATE flecs)`.

- [ ] **Step 4: Ecs.hpp / Ecs.cpp**

`src/engine_core/Ecs.hpp`:

```cpp
#pragma once

// The engine's one include of flecs. engine_core and engine_instances only:
// flecs ids never leave them. Hot paths use the C API with EcsIds; the C++
// API is for setup, since its inline wrappers are slow in Debug builds.

#pragma warning(push, 0)
#include "flecs.h"
#pragma warning(pop)

#include "Color.hpp"
#include "Transform.hpp"
#include "types.hpp"

namespace engine_core {
namespace ecs {

// GameObject's spatial data. Transform and ColorRgb are components as they are.
struct Size {
    float x = 1.f;
    float y = 1.f;
    float z = 1.f;
};
struct Velocity {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};
// The instance an entity belongs to.
struct Instance {
    InstanceId id = 0;
};
// Tags.
struct InGame {};
struct InWorkspace {};
struct Steps {};
struct Simulated {};
struct VisualOnly {};

}  // namespace ecs

struct EcsIds {
    ecs_id_t transform = 0;
    ecs_id_t color = 0;
    ecs_id_t size = 0;
    ecs_id_t velocity = 0;
    ecs_id_t instance = 0;
    ecs_id_t in_game = 0;
    ecs_id_t in_workspace = 0;
    ecs_id_t steps = 0;
    ecs_id_t simulated = 0;
    ecs_id_t visual_only = 0;
};

// Registers every component and tag with world and returns their ids.
EcsIds register_ecs(flecs::world& world);

// A component of e, or null when e is 0 or has none.
template <typename T>
const T* ecs_read(ecs_world_t* world, ecs_entity_t e, ecs_id_t id) {
    return e == 0 ? nullptr : static_cast<const T*>(ecs_get_id(world, e, id));
}

template <typename T>
void ecs_write(ecs_world_t* world, ecs_entity_t e, ecs_id_t id, const T& value) {
    ecs_set_id(world, e, id, sizeof(T), &value);
}

inline bool ecs_tag(ecs_world_t* world, ecs_entity_t e, ecs_id_t tag) {
    return e != 0 && ecs_has_id(world, e, tag);
}

inline void ecs_set_tag(ecs_world_t* world, ecs_entity_t e, ecs_id_t tag, bool on) {
    if (on) {
        ecs_add_id(world, e, tag);
    } else {
        ecs_remove_id(world, e, tag);
    }
}

}  // namespace engine_core
```

`src/engine_core/Ecs.cpp`: `register_ecs` calls `world.component<T>().id()` for each of the ten types into an `EcsIds`. Verify `ecs_set_id`'s v4 signature (`world, entity, id, size, ptr`) against `flecs.h` before relying on it.

- [ ] **Step 5: DataModel wiring**

`DataModel.hpp`: before `namespace engine_core {` add `struct ecs_world_t;`; inside, forward-declare `struct EcsIds;`. `Slot` gains, after `body`: `// This instance's flecs entity (engine_core only). 0 when the slot is free.` / `std::uint64_t entity = 0;`. Public, next to `room_left`: `// Live flecs entities that belong to instances. Tests and diagnostics.` / `std::size_t entity_count() const;`. Private: the four members listed in Interfaces.

`DataModelState.hpp`: `#include "Ecs.hpp"`; as the FIRST members of `State` (destroyed last):

```cpp
    // Every instance's components and tags. Declared first so it outlives
    // everything that might reach it during teardown.
    flecs::world ecs;
    EcsIds ecs_ids;
```

`DataModel.cpp`:
- Root ctor: `world.ecs_ids = register_ecs(world.ecs);` before anything else.
- `issue_entity(Slot& part, InstanceId id)`: `ecs_world_t* w = state_->ecs.c_ptr(); part.entity = ecs_new(w); const ecs::Instance tag{id}; ecs_write(w, part.entity, state_->ecs_ids.instance, tag);` — verify `ecs_new(world)` is v4's plain-entity constructor.
- `spawn`: call `issue_entity(world.slots[index], id)` between `allocate()` and `pooled_object(...)`.
- `release_to_pool`: after `on_release()`, `if (part.entity != 0) { ecs_delete(state_->ecs.c_ptr(), part.entity); part.entity = 0; }`.
- `entity_of(id)`: `const Slot* part = slot(id); return part == nullptr ? 0 : part->entity;`
- `ecs_world()`: `return state_->ecs.c_ptr();` (const_cast as needed); `ecs_ids()`: `return state_->ecs_ids;`
- `entity_count()`: `return static_cast<std::size_t>(ecs_count_id(ecs_world(), state_->ecs_ids.instance));`

`DataModelPlace.cpp` `adopt_slot`: `issue_entity(part, id);` right before `pooled_object(...)`.

- [ ] **Step 6: Reconfigure, build, run `[entity]` → PASS; full suite → 0 failures; check no new `/W4` warnings in engine sources** (compare `warning C` lines outside `_deps` with before).

- [ ] **Step 7: Commit** "Give every instance a flecs entity".

---

### Task 4: GameObject's fields into components; Simulated and VisualOnly tags

No behavior change: the existing suite is the main check. Two new tests pin the edges.

**Files:**
- Modify: `src/engine_instances/GameObject.hpp/.cpp`
- Modify: `src/engine_core/DataModel.hpp` (delete `Slot::simulated`, `Slot::visual_only`)
- Modify: `src/engine_core/DataModel.cpp` (`authorize`, `allocate`, `apply_transform`, `apply_color`, `set_simulated`, `set_visual_only`, `simulated()`, `visual_only()`, `integrate_simulated`, the property-bag `Simulated`/`VisualOnly` block ~:1587)
- Modify: `src/engine_core/DataModelPlace.cpp` (capture :94-95, `retire_slot` :135-136, `adopt_slot` :156-157, `restore_record` :190-191)
- Modify: `src/engine_core/DataModelHistory.cpp` (capture :277-278)
- Test: `sandbox/dense_views_tests.cpp`

**Interfaces:**
- Consumes: Task 3's `entity_of`, `ecs_world`, `ecs_ids`, `ecs_read`, `ecs_write`, `ecs_tag`, `ecs_set_tag`.
- Produces: GameObject private `void store_transform(const Transform&)`, `void store_color(ColorRgb)` for `DataModel::apply_*`. `GameObject` constructor defined out of line, calling `reset_spatial()`.

- [ ] **Step 1: Guard tests**

```cpp
TEST_CASE("a destroyed GameObject reads zero", "[dense][entity]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = game.create_game_object();
    part.set_color(rgb(0.5f, 0.5f, 0.5f));
    part.set_size(2.f, 2.f, 2.f);
    game.destroy(part.id());
    float size[3] = {9.f, 9.f, 9.f};
    REQUIRE_FALSE(part.copy_size(size));
    REQUIRE(part.transform().m[0] == 0.f);  // zero matrix, not identity
    REQUIRE(part.color().r == engine_core::ColorRgb{}.r);
}

TEST_CASE("undo and Stop keep spatial values and flags", "[dense][entity]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId id = part.id();
    game.set_parent(id, workspace_of(game));
    part.set_color(rgb(0.25f, 0.5f, 0.75f));
    part.set_size(1.f, 2.f, 3.f);
    game.set_simulated(id, true);
    game.set_visual_only(id, true);
    game.destroy(id);
    game.history().undo();
    REQUIRE(game.alive(id));
    REQUIRE(game.simulated(id));
    REQUIRE(game.visual_only(id));
    REQUIRE(game.game_object(id)->color().g == 0.5f);

    game.capture_place();
    game.start_simulation();
    game.set_simulated(id, false);
    game.game_object(id)->set_size(5.f, 5.f, 5.f);
    game.stop_simulation();
    REQUIRE(game.simulated(id));
    float size[3] = {};
    REQUIRE(game.game_object(id)->copy_size(size));
    REQUIRE(size[1] == 2.f);
}
```

Both are guards for behavior that exists today: they should PASS before the change and must stay green through it. The task is a storage refactor; its RED is the compile break when the members go, and the whole suite is its RED→GREEN harness. Ledger that.

- [ ] **Step 2: GameObject**

`GameObject.hpp`: constructor becomes a declaration `GameObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);`. Delete `transform_`, `color_`, `size_`, `velocity_`, and `clear_spatial`. Add private `void store_transform(const Transform& transform);` and `void store_color(ColorRgb color);`.

`GameObject.cpp` (`#include "Ecs.hpp"`):

```cpp
GameObject::GameObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {
    reset_spatial();
}
```

- `transform()`: `const Transform* value = ecs_read<Transform>(ecs_world(), entity_of(id_), ecs_ids().transform); return value != nullptr ? *value : Transform{};`
- `color()`: same with `ColorRgb`, fallback `ColorRgb{}`.
- `copy_size(out)`: read `ecs::Size`; null → `return false`; else copy x/y/z, `return true`.
- `set_size`: after the existing guards, read the current size, compare for the early out, write `ecs::Size{x, y, z}`; keep `record_size`, `note`, `emit_change` with the old values as previous.
- `set_linear_velocity`: same shape with `ecs::Velocity`.
- `store_transform`/`store_color`: `ecs_write(ecs_world(), entity_of(id_), ecs_ids().transform, transform)` / `.color`.
- `save_properties`: read `transform()`, `color()`, `copy_size(size)` instead of members; output identical.
- `on_release()`: nothing left to clear — delete the override (the entity is deleted right after it).
- `reset_spatial()`: write `transform_identity()`, `ColorRgb{}`, `ecs::Size{}`, `ecs::Velocity{}` to `entity_of(id_)`; nothing when it is 0.
- `write_place`: fill `pod` from `transform()`, `color()`, `copy_size(pod.size)`.
- `read_place`: velocity reset → `ecs_write(..., ecs::Velocity{})`; the fallback `reset_spatial()` stays; the success path writes `pod.transform`, `pod.color`, `ecs::Size{pod.size[0], pod.size[1], pod.size[2]}`.

- [ ] **Step 3: DataModel**

- `apply_transform`: `if (target == nullptr) return; const Transform previous = target->transform(); if (same_transform(previous, transform)) return; target->store_transform(transform); record_transform(id, previous, transform);` then unchanged. `apply_color` the same shape.
- `Slot`: delete `simulated`, `visual_only`. `allocate`: delete their two reset lines.
- `authorize`: `if (ecs_tag(ecs_world(), part.entity, state_->ecs_ids.visual_only) || force_sim_write)`.
- `set_simulated`: `const bool previous = ecs_tag(w, part->entity, ids.simulated); if (previous == simulated) return; ecs_set_tag(w, part->entity, ids.simulated, simulated);` then `record_bool`/`emit_change` unchanged. `set_visual_only` likewise.
- `simulated(id)`/`visual_only(id)`: `ecs_tag(ecs_world(), entity_of(id), ids.x)`.
- Property bag (~:1587): `if (simulated(id_))` / `if (visual_only(id_))`.
- `integrate_simulated` keeps its slot scan for now (Task 6 replaces it) but reads through the body: skip unless `simulated(id) && !visual_only(id)`; read velocity via `ecs_read<ecs::Velocity>`, zero → skip; `Transform t = body.transform(); t.m[12..14] += ...; body.store_transform(t);` then `note`/`notify_watchers` unchanged.

`DataModelPlace.cpp`: capture → `record.simulated = simulated(record.id); record.visual_only = visual_only(record.id);`. `retire_slot` and `adopt_slot`: delete the two reset lines (a fresh entity has no tags). `restore_record`: `ecs_set_tag(w, live.entity, ids.simulated, record.simulated)` and the same for visual_only — direct, not through the setters, which record history.

`DataModelHistory.cpp` capture: `record.simulated = simulated(id); record.visual_only = visual_only(id);`.

`grep -n "simulated\b\|visual_only\b\|transform_\|color_\|size_\[\|velocity_" src/engine_core/*.cpp src/engine_instances/*.cpp` must show no remaining member or Slot-field use (Lighting's own `color_` is unrelated).

- [ ] **Step 4: Build; `[entity]` PASS; full suite 0 failures (the path B/D, place, project, and history suites are this task's real check). Commit** "Store GameObject's spatial data and flags in flecs".

---

### Task 5: Scope tags

**Files:** `DataModel.hpp` (public `in_game`, `in_workspace`; private `refresh_scope`, `apply_scope`), `DataModelState.hpp` (`std::vector<InstanceId> scope_walk;` reserved to `kMaxInstances` in the root ctor), `DataModel.cpp` (`set_parent`, `detach_links`), `DataModelPlace.cpp` (`link_children`, comment on `clear_hierarchy`), tests.

**Interfaces:** Produces `bool in_game(InstanceId) const`, `bool in_workspace(InstanceId) const` (false for dead). Task 7 extends `apply_scope`'s flip block with the Ancestry note.

- [ ] **Step 1: Failing tests** — append:

```cpp
TEST_CASE("scope tags follow the tree", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    const engine_core::InstanceId storage = game.scene_service("Storage");
    REQUIRE(game.in_game(ws));
    REQUIRE_FALSE(game.in_workspace(ws));
    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId id = part.id();
    REQUIRE_FALSE(game.in_game(id));
    game.set_parent(id, ws);
    REQUIRE(game.in_game(id));
    REQUIRE(game.in_workspace(id));
    game.set_parent(id, storage);
    REQUIRE(game.in_game(id));
    REQUIRE_FALSE(game.in_workspace(id));
    game.set_parent(id, engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(game.in_game(id));
    REQUIRE_FALSE(game.in_game(engine_core::make_instance_id(9, 999)));
}

TEST_CASE("scope tags flip a whole subtree", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    engine_core::GameObject& part = game.create_game_object();
    engine_core::GameObject& nested = game.create_game_object();
    game.set_parent(part.id(), folder.id());
    game.set_parent(nested.id(), part.id());
    game.set_parent(folder.id(), workspace_of(game));
    REQUIRE(game.in_workspace(folder.id()));
    REQUIRE(game.in_workspace(nested.id()));
    game.set_parent(folder.id(), engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(game.in_game(part.id()));
    REQUIRE_FALSE(game.in_workspace(nested.id()));
}

TEST_CASE("destroy clears scope; undo restores it", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& parent = game.create_game_object();
    engine_core::GameObject& child = game.create_game_object();
    game.set_parent(parent.id(), workspace_of(game));
    game.set_parent(child.id(), parent.id());
    const engine_core::InstanceId parent_id = parent.id();
    const engine_core::InstanceId child_id = child.id();
    game.destroy(parent_id);
    REQUIRE(game.alive(child_id));
    REQUIRE_FALSE(game.in_game(child_id));
    game.history().undo();
    REQUIRE(game.in_workspace(parent_id));
    REQUIRE(game.in_workspace(child_id));
}

TEST_CASE("Stop restores scope", "[dense][scope]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::GameObject& authored = game.create_game_object();
    game.set_parent(authored.id(), ws);
    game.capture_place();
    game.start_simulation();
    game.set_parent(authored.id(), game.scene_service("Storage"));
    REQUIRE_FALSE(game.in_workspace(authored.id()));
    game.stop_simulation();
    REQUIRE(game.in_workspace(authored.id()));
}
```

(If undo in the third test needs more than one step to relink the child, undo until the child's parent is `parent_id`, then assert.)

- [ ] **Step 2: Run → compile FAIL.**

- [ ] **Step 3: Implement**

```cpp
bool DataModel::in_game(InstanceId id) const { return ecs_tag(ecs_world(), entity_of(id), state_->ecs_ids.in_game); }
bool DataModel::in_workspace(InstanceId id) const {
    return ecs_tag(ecs_world(), entity_of(id), state_->ecs_ids.in_workspace);
}

void DataModel::refresh_scope(InstanceId id) {
    const Slot* part = slot(id);
    if (part == nullptr) {
        return;
    }
    bool game = false;
    bool workspace = false;
    if (part->parent == 0) {
        game = true;
    } else if (part->parent != kNoParent) {
        game = in_game(part->parent);
        workspace = in_workspace(part->parent) || part->parent == scene_service("Workspace");
    }
    if (in_game(id) == game && in_workspace(id) == workspace) {
        return;
    }
    apply_scope(id, game, workspace);
}

void DataModel::apply_scope(InstanceId id, bool in_game_now, bool in_workspace_now) {
    // Top-down over the subtree. A node whose tags come out unchanged prunes
    // its children: their tags were derived from its tags.
    ecs_world_t* w = ecs_world();
    const EcsIds& ids = state_->ecs_ids;
    const InstanceId workspace_id = scene_service("Workspace");
    std::vector<InstanceId>& queue = state_->scope_walk;
    queue.clear();
    queue.push_back(id);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const InstanceId cur = queue[i];
        const Slot* part = slot(cur);
        if (part == nullptr) {
            continue;
        }
        bool game = in_game_now;
        bool workspace = in_workspace_now;
        if (i > 0) {
            game = in_game(part->parent);
            workspace = in_workspace(part->parent) || part->parent == workspace_id;
        }
        const bool had_game = ecs_tag(w, part->entity, ids.in_game);
        const bool had_workspace = ecs_tag(w, part->entity, ids.in_workspace);
        if (had_game == game && had_workspace == workspace) {
            continue;
        }
        if (had_game != game) {
            ecs_set_tag(w, part->entity, ids.in_game, game);
        }
        if (had_workspace != workspace) {
            ecs_set_tag(w, part->entity, ids.in_workspace, workspace);
            // Task 7: Ancestry note for GameObjects goes here.
        }
        for (InstanceId child = part->first_child; child != 0;) {
            if (queue.size() == queue.capacity()) {
                contract_fail("scope walk capacity exhausted");
            }
            queue.push_back(child);
            const Slot* child_slot = slot(child);
            if (child_slot == nullptr) {
                break;
            }
            child = child_slot->next_sibling;
        }
    }
}
```

Hooks: `set_parent` — `refresh_scope(id);` right after the unlink/link block, before `record_parent`. `detach_links` — after clearing each child's parent/sibling fields, `refresh_scope(child);`. `link_children` — after each `link_child(parent, child)`, `refresh_scope(child);`. `clear_hierarchy` — comment: tags are left stale on purpose; restore relinks every live instance through `link_children`, which refreshes it. Then `grep -n "link_child(\|unlink_parent(" src/engine_core/*.cpp` and confirm every caller is inside `set_parent`, `detach_links`, `link_children`, or history's sibling reorder (which relinks through `link_children`).

- [ ] **Step 4: `[scope]` PASS; full suite 0 failures. Commit** "Keep InGame and InWorkspace tags current across the tree".

---

### Task 6: Stepping and physics as queries

**Files:** `DataModel.hpp` (`virtual bool steps() const`; `step_descendants` → `step_instances`; public `stepper_count()`), `DataModelState.hpp` (`flecs::query<> step_query; flecs::query<> physics_query;` declared after `ecs`), `DataModel.cpp` (root ctor builds queries; `spawn` adds Steps; `step_instances`; `integrate_simulated`), `DataModelPlace.cpp` (`adopt_slot` adds Steps), `TestTriangle.hpp` (`steps()` true), `Engine.cpp:299`, tests.

**Interfaces:** Produces `virtual bool steps() const`, `void step_instances(double dt)`, `std::size_t stepper_count() const` (live entities with Steps and InGame).

- [ ] **Step 1: Failing tests** — add `#include "TestTriangle.hpp"` at the top of the test file, then:

```cpp
TEST_CASE("a triangle steps only while it is under game", "[dense][step]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId ws = workspace_of(game);
    engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
    const engine_core::InstanceId id = triangle.id();
    game.step_instances(0.25);
    REQUIRE(triangle.angle_degrees() == 0.0);
    game.set_parent(id, ws);
    game.step_instances(0.25);  // 90 deg/s
    REQUIRE(triangle.angle_degrees() == 22.5);
    game.set_parent(id, engine_core::DataModel::kNoParent);
    game.step_instances(0.25);
    REQUIRE(triangle.angle_degrees() == 22.5);
    game.set_parent(id, ws);
    game.destroy(id);
    game.step_instances(0.25);
    REQUIRE_FALSE(game.alive(id));
}

TEST_CASE("plain instances never step", "[dense][step]") {
    SimRole role;
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    game.set_parent(folder.id(), workspace_of(game));
    REQUIRE(game.stepper_count() == 0);
    engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
    game.set_parent(triangle.id(), workspace_of(game));
    REQUIRE(game.stepper_count() == 1);
}

TEST_CASE("physics moves simulated bodies only", "[dense][physics]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& moving = game.create_game_object();
    engine_core::GameObject& idle = game.create_game_object();
    engine_core::GameObject& shown = game.create_game_object();
    for (engine_core::GameObject* body : {&moving, &idle, &shown}) {
        game.set_parent(body->id(), workspace_of(game));
        body->set_linear_velocity(4.f, 0.f, 0.f);
    }
    game.set_simulated(moving.id(), true);
    game.set_simulated(shown.id(), true);
    game.set_visual_only(shown.id(), true);
    game.invalidations().clear();
    game.integrate_simulated(0.5);
    REQUIRE(moving.transform().m[12] == 2.f);
    REQUIRE(idle.transform().m[12] == 0.f);
    REQUIRE(shown.transform().m[12] == 0.f);
    REQUIRE(game.invalidations().size() == 1);
}
```

Check `integrate_simulated` and `invalidations()` are callable from a test; if not public, drive physics as the existing physics tests do.

- [ ] **Step 2: Run → compile FAIL.**

- [ ] **Step 3: Implement**

- `DataModel.hpp`: `// True for a class Heartbeat steps. Read once, when its entity is issued.` / `virtual bool steps() const { return false; }` next to `step()`; update `step()`'s comment. Replace `step_descendants` with `step_instances` (comment: unspecified order, stable copy, a step may create/destroy/reparent). Add `std::size_t stepper_count() const;`.
- `TestTriangle.hpp`: `bool steps() const override { return true; }`.
- `spawn` and `adopt_slot`: after `pooled_object(...)`, `if (object->steps()) { ecs_add_id(ecs_world(), part.entity, state_->ecs_ids.steps); }`.
- Root ctor: build both queries after `register_ecs` with the C++ builder and caching: step query terms `ecs::Instance`, with `ecs::Steps`, with `ecs::InGame`; physics query terms `ecs::Instance` (in), `Transform` (inout), `ecs::Velocity` (in), with `ecs::Simulated`, without `ecs::VisualOnly`. Confirm v4's spelling of caching (`.cached()` or `.cache_kind(flecs::QueryCacheAuto)`) and term access (`.in()`/`.inout()`). Field indexes follow term order.
- `step_instances(dt)`:

```cpp
void DataModel::step_instances(double dt) {
    // Collect first: step() may create, destroy, or reparent, which flecs
    // would defer inside a running query.
    std::vector<InstanceId>& ids = state_->step_ids;
    ids.clear();
    ecs_iter_t it = ecs_query_iter(ecs_world(), state_->step_query.c_ptr());
    while (ecs_query_next(&it)) {
        const auto* owners = static_cast<const ecs::Instance*>(ecs_field_w_size(&it, sizeof(ecs::Instance), 0));
        for (std::int32_t i = 0; i < it.count; ++i) {
            ids.push_back(owners[i].id);
        }
    }
    for (const InstanceId id : ids) {
        if (DataModel* object = instance(id)) {
            object->step(dt);
        }
    }
}
```

- `stepper_count()`: iterate `step_query` summing `it.count`.
- `integrate_simulated(dt)`:

```cpp
void DataModel::integrate_simulated(double dt) {
    const float step = static_cast<float>(dt);
    ecs_iter_t it = ecs_query_iter(ecs_world(), state_->physics_query.c_ptr());
    while (ecs_query_next(&it)) {
        const auto* owners = static_cast<const ecs::Instance*>(ecs_field_w_size(&it, sizeof(ecs::Instance), 0));
        auto* transforms = static_cast<Transform*>(ecs_field_w_size(&it, sizeof(Transform), 1));
        const auto* velocities = static_cast<const ecs::Velocity*>(ecs_field_w_size(&it, sizeof(ecs::Velocity), 2));
        for (std::int32_t i = 0; i < it.count; ++i) {
            const ecs::Velocity& v = velocities[i];
            if (v.x == 0.f && v.y == 0.f && v.z == 0.f) {
                continue;
            }
            transforms[i].m[12] += v.x * step;
            transforms[i].m[13] += v.y * step;
            transforms[i].m[14] += v.z * step;
            note(owners[i].id, VisualField::Transform, WriteOrigin::Simulation);
            notify_watchers(owners[i].id);
        }
    }
}
```

  First read `notify_watchers`: if any callback it runs can reach the DataModel or flecs, collect ids during the query and notify after it instead; ledger which.
- Delete `step_descendants`. `Engine.cpp:299` → `game_.step_instances(render_dt_);` with the comment reworded.

- [ ] **Step 4: `[step]` and `[physics]` PASS; full suite 0 failures. Commit** "Step Heartbeat and integrate physics through flecs queries".

---

### Task 7: Snapshot membership, test migration, and the Lua regression

**Files:** `types.hpp` (Ancestry), `DataModel.cpp` (`apply_scope` note; `for_each_rendered`), `DataModel.hpp`, `DataModelState.hpp` (`flecs::query<> render_query;`, built in the root ctor), `SnapshotPump.cpp` (`apply_live`, `resync`), `sandbox/support.hpp` (`create_part`), `sandbox/tests.cpp` (migration), tests.

**Interfaces:** Produces `VisualField::Ancestry = 1u << 4`; `void DataModel::for_each_rendered(const std::function<void(const GameObject&)>&) const`.

- [ ] **Step 1: Failing tests** — append:

```cpp
namespace {

void pump_frame(engine_core::SnapshotPump& pump, engine_core::DataModel& game) {
    pump.prepare_copy(game);
    pump.publish();
}

}  // namespace

TEST_CASE("only Workspace GameObjects have snapshot rows", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& part = game.create_game_object();
    const engine_core::InstanceId id = part.id();
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);
    game.set_parent(id, workspace_of(game));
    pump_frame(pump, game);
    REQUIRE(pump.find(id) != nullptr);
    game.set_parent(id, game.scene_service("Storage"));
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);
    game.set_parent(id, workspace_of(game));
    game.set_parent(id, engine_core::DataModel::kNoParent);
    pump_frame(pump, game);
    REQUIRE(pump.find(id) == nullptr);
}

TEST_CASE("a row arrives complete after edits made outside Workspace", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), game.scene_service("Storage"));
    pump_frame(pump, game);
    part.set_color(rgb(0.25f, 0.5f, 0.75f));
    part.set_size(2.f, 3.f, 4.f);
    pump_frame(pump, game);
    REQUIRE(pump.find(part.id()) == nullptr);
    game.set_parent(part.id(), workspace_of(game));
    pump_frame(pump, game);
    const engine_core::VisualInstance* row = pump.find(part.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->color.r == 0.25f);
    REQUIRE(row->size[1] == 3.f);
}

TEST_CASE("a move within Workspace keeps the row", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::DataModel& folder = game.create();
    game.set_parent(folder.id(), workspace_of(game));
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), workspace_of(game));
    part.set_color(rgb(0.1f, 0.2f, 0.3f));
    pump_frame(pump, game);
    game.set_parent(part.id(), folder.id());
    pump_frame(pump, game);
    const engine_core::VisualInstance* row = pump.find(part.id());
    REQUIRE(row != nullptr);
    REQUIRE(row->color.g == 0.2f);
}

TEST_CASE("destroy_tree clears rows and steppers", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::DataModel& folder = game.create();
    engine_core::GameObject& a = game.create_game_object();
    engine_core::TestTriangle& t = game.create<engine_core::TestTriangle>();
    game.set_parent(folder.id(), workspace_of(game));
    game.set_parent(a.id(), folder.id());
    game.set_parent(t.id(), folder.id());
    const engine_core::InstanceId a_id = a.id();
    pump_frame(pump, game);
    REQUIRE(pump.find(a_id) != nullptr);
    REQUIRE(game.stepper_count() == 1);
    game.destroy_tree(folder.id());
    pump_frame(pump, game);
    REQUIRE(pump.find(a_id) == nullptr);
    REQUIRE(game.stepper_count() == 0);
}

TEST_CASE("overflow resync keeps Workspace membership", "[dense][member]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& shown = game.create_game_object();
    engine_core::GameObject& stored = game.create_game_object();
    game.set_parent(shown.id(), workspace_of(game));
    game.set_parent(stored.id(), game.scene_service("Storage"));
    engine_core::Transform moved = engine_core::transform_identity();
    for (std::size_t i = 0; i <= engine_core::DataModel::kMaxInvalidations; ++i) {
        moved.m[12] = static_cast<float>(i + 1);  // equal writes skip the note
        shown.set_transform(moved);
    }
    REQUIRE(game.invalidations().overflow());
    pump_frame(pump, game);
    REQUIRE(pump.find(shown.id()) != nullptr);
    REQUIRE(pump.find(stored.id()) == nullptr);
}
```

(If the overflow loop is slow because each write records undo, disable history for the loop the way other bulk tests in `sandbox/tests.cpp` do.)

- [ ] **Step 2: Run → the first `[member]` test FAILS** (unparented object gets a row today).

- [ ] **Step 3: Implement**

- `types.hpp`: `Ancestry = 1u << 4` after `Removed`, commented: scope changed; re-evaluate membership and read a joining row whole.
- `apply_scope`: at the Task 5 marker: `if (part->body != nullptr) { note(cur, VisualField::Ancestry, current_origin()); }`.
- `SnapshotPump::apply_live`: after the Removed/dead erase, `if (!game.in_workspace(change.id)) { erase_base(change.id); return; }`; then `const bool joined = any(change.fields, VisualField::Ancestry);` and each field branch becomes `if (joined || any(change.fields, VisualField::X))`.
- Root ctor: `render_query` with terms `ecs::Instance`, with `ecs::InWorkspace`, with `Transform` (only GameObjects carry it), cached.
- `for_each_rendered(fn)`: iterate `render_query` via the C API, map each `ecs::Instance` to `game_object(id)`, call `fn` for non-null. `resync` calls it instead of `for_each_game_object`. If `for_each_game_object` then has no callers, delete it.

- [ ] **Step 4: `[member]` PASS; full suite — EXPECT failures only in older `sandbox/tests.cpp` tests reading rows of unparented objects. List them. Any other failure is a bug in this step: fix first.**

- [ ] **Step 5: Migration** — `sandbox/support.hpp` after `workspace_of`:

```cpp
// A GameObject parented under Workspace, so it has a snapshot row.
inline engine_core::GameObject& create_part(engine_core::DataModel& game) {
    engine_core::GameObject& object = game.create_game_object();
    game.set_parent(object.id(), workspace_of(game));
    return object;
}
```

For each failing test from Step 4, switch its `create_game_object()` to `create_part(...)`. Leave tests that don't read rows alone.

- [ ] **Step 6: Lua regression test** — append:

```cpp
TEST_CASE("Instance.new GameObject renders once parented into Workspace", "[dense][member][lua]") {
    ScriptRig rig;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    add_script(rig.game, "Spawner", R"(
        local part = Instance.new("GameObject")
        part.Name = "Spawned"
        part.Parent = workspace
    )");
    rig.frames(2);
    const engine_core::InstanceId id = rig.game.find_first_child(workspace_of(rig.game), "Spawned");
    REQUIRE(id != 0);
    pump_frame(pump, rig.game);
    REQUIRE(pump.find(id) != nullptr);
}
```

(Match `add_script`'s real signature.)

- [ ] **Step 7: Full suite 0 failures; run `[T4]` ×20 and the whole suite ×3 for flakes. Commit** "Limit the snapshot to Workspace GameObjects and migrate tests".
