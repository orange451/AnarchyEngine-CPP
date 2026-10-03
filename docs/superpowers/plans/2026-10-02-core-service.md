# Core Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `Core`, a hidden child of `game` where the studio's own tools live. Its contents render like Workspace's, have no physics, are never saved or undone, and stay unchanged through New, Open, Play, and Stop. Its Scripts run in the plugin VM all the time, and game scripts cannot see it. Then move the built-in plugins into it.

**Architecture:** `Core` is a `Service` (not a `SceneService`) that `Game` makes last. It is kept out of `kServices`, so the project reader and `clear_world` never touch it. An `InCore` scope tag marks its descendants, and `DataModel::core_holds(id)` is the one test every rule uses: containment, `clear_world`, `authored_tree`, place capture and restore, history, Lua visibility, scripts, and the render snapshot.

**Tech Stack:** C++17, flecs (scope tags and queries), Luau, Catch2 (sandbox suite), CMake.

**Spec:** `docs/superpowers/specs/2026-10-02-core-service-design.md`

## Global Constraints

- C++17. Follow the surrounding code's comment style: short declarative comments that say what and why.
- `contract_fail` reports a C++ programming error. The sandbox installs a throwing handler, so `REQUIRE_THROWS_AS(..., engine_core::ContractViolation)` tests it.
- Build the suite: `cmake --build build --target sandbox --parallel`. Run one tag: `./build/sandbox "[CO1]"`. Run all: `./build/sandbox`. Every test program: `cmake --build build --parallel && (cd build && ctest -C Release)`.
- Leave no new compiler warnings in touched files. Existing ones are not this plan's: `-Wunknown-pragmas` in Ecs.hpp, `-Wswitch` at DataModel.cpp:918 and IdeAssets.cpp:391, and `-Wmissing-field-initializers` in ScriptAnalysis.cpp.
- The working tree may hold the user's uncommitted `src/ide/IdeLayout.cpp` and `resources/icons/Export.png`. Never stage them. Stage only the files each task names.
- Core is "persistent" for as long as the studio runs. It is never written to disk.

## Review Focus

- **A plugin making a Dragger in edit mode:** `Instance.new` then `Parent = Core` must leave no undo step and must not end up merged into the user's next action (Task 4 test CO4b). This is the exact path the Dragger's Move tool takes.
- **A Core instance made during play:** after an instance captured at Test is destroyed in that same play session, the Core instance must survive Stop, not be retired by the restore (Task 3 test CO2b).
- **A plugin parenting a Script into Core from inside its own Lua call:** the new Script starts on the next step, not re-entrantly (Task 5 test CO7c).
- **Undo of a step recorded before an instance went into Core:** undo must not pull it back out of Core or destroy it (Task 4 test CO4c).
- **A game script reaching Core by name:** `game.Core`, `game:FindFirstChild("Core")`, and `game:WaitForChild("Core", 0.1)` all fail to reach it (Task 5 test CO6).

---

## File Structure

| File | Responsibility | Change |
| --- | --- | --- |
| `src/engine_core/Containment.hpp/.cpp` | `kCoreClass`; game may hold Core | Modify (Task 1) |
| `src/engine_services/SceneService.hpp/.cpp` | `class Core`, its Lua class | Modify (Task 1) |
| `src/engine_services/Game.cpp` | Makes Core last | Modify (Task 1) |
| `src/engine_core/Ecs.hpp/.cpp` | `ecs::InCore`, `EcsIds::in_core` | Modify (Task 2) |
| `src/engine_core/DataModel.hpp/.cpp` | `core()`, `in_core`, `core_holds`, scope, crossing rule, `clear_world`-safe destroy, render queries | Modify (Tasks 1, 2, 3, 4, 6) |
| `src/engine_core/DataModelState.hpp` | `place_slots`, `core_render_query` | Modify (Tasks 3, 6) |
| `src/engine_core/DataModelPlace.cpp` | Capture and restore leave Core alone; `authored_tree` skips it | Modify (Task 3) |
| `src/engine_core/Project.cpp` | `clear_world` skips Core's subtree | Modify (Task 3) |
| `src/engine_services/ChangeHistoryService.hpp/.cpp` | Never records Core; purges what entered it | Modify (Task 4) |
| `src/engine_core/ScriptBindings.hpp/.cpp` | Play VM cannot see Core | Modify (Task 5) |
| `src/engine_core/ScriptRuntime.hpp/.cpp` | Scripts in Core run in the plugin VM | Modify (Task 5) |
| `src/engine_core/SnapshotPump.cpp` | Rows for Core | Modify (Task 6) |
| `src/ide/PluginLoader.hpp/.cpp` | Loads into Core | Modify (Task 7) |
| `src/ide/IdeLayoutProject.cpp` | No reload after New and Open | Modify (Task 7) |
| `sandbox/core_tests.cpp` | CO tests | Create (Task 1), extend after |
| `sandbox/scene_services_tests.cpp` | Game now has 7 children | Modify (Task 1) |
| `CMakeLists.txt` | Add `sandbox/core_tests.cpp` | Modify (Task 1) |

---

### Task 1: Core exists

**Files:**
- Modify: `src/engine_core/Containment.hpp` (after `kServices`), `src/engine_core/Containment.cpp` (`placement_error`, "Game" branch)
- Modify: `src/engine_services/SceneService.hpp` (new class after `GuiService`), `src/engine_services/SceneService.cpp` (`class_name`, Lua registration)
- Modify: `src/engine_services/Game.cpp` (constructor)
- Modify: `src/engine_core/DataModel.hpp:155` area (`core()`), `src/engine_core/DataModel.cpp` (`core()`, `parent_error` service branch)
- Create: `sandbox/core_tests.cpp`
- Modify: `sandbox/scene_services_tests.cpp:41,89,263,511`
- Modify: `CMakeLists.txt` (sandbox sources)

**Interfaces:**
- Produces: `inline constexpr const char* kCoreClass = "Core";` in Containment.hpp. `class engine_core::Core : public Service`. `InstanceId DataModel::core() const;` (0 when there is none).

- [ ] **Step 1: Write the failing test**

Create `sandbox/core_tests.cpp`:

```cpp
// Core: the studio's own service, outside the place.

#include "support.hpp"

#include "Contract.hpp"
#include "Folder.hpp"
#include "SceneService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using engine_core::ContractViolation;
using engine_core::InstanceId;

InstanceId add_folder(engine_core::DataModel& game, const char* name, InstanceId parent) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    if (parent != engine_core::DataModel::kNoParent) {
        game.set_parent(folder.id(), parent);
    }
    return folder.id();
}

}  // namespace

TEST_CASE("CO9 a Game holds Core last, hidden from the explorer, and it cannot be moved, renamed, or destroyed",
          "[CO9]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    REQUIRE(core != 0);
    REQUIRE(game.parent(core) == 0);
    REQUIRE(game.get_children(0).back() == core);
    REQUIRE(std::string(game.instance(core)->class_name()) == "Core");
    REQUIRE(game.name(core) == "Core");
    REQUIRE(game.guid(core) == "core");
    REQUIRE(game.instance(core)->hidden_in_explorer());
    REQUIRE(game.instance(core)->is_service());
    REQUIRE_FALSE(game.instance(core)->is_scene_service());
    REQUIRE(game.scene_service("Core") == 0);
    REQUIRE_THROWS_AS(game.set_parent(core, game.scene_service("Workspace")), ContractViolation);
    REQUIRE_THROWS_AS(game.set_name(core, "Tools"), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy(core), ContractViolation);
    // What Workspace may hold, Core may hold.
    REQUIRE_NOTHROW(add_folder(game, "Tools", core));
}
```

Add it to the `sandbox` target in `CMakeLists.txt`, after `sandbox/event_args_tests.cpp`:

```cmake
    sandbox/event_args_tests.cpp
    sandbox/core_tests.cpp
)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -S . -B build >/dev/null && cmake --build build --target sandbox --parallel`
Expected: compile error, because `DataModel` has no member `core`.

- [ ] **Step 3: Implement**

In `src/engine_core/Containment.hpp`, after the `kServices` table:

```cpp
// The studio's own service, outside the place: never saved or undone, and left
// as it is by New, Open, Play, and Stop. Not in kServices, which is the place's
// services, the ones a project reads and writes. Game makes it last.
inline constexpr const char* kCoreClass = "Core";
```

In `Containment.cpp` `placement_error`, change the "Game" branch's test:

```cpp
    if (holder_class == "Game") {
        if (find_service(child_class) != nullptr || child_class == kCoreClass) {
            return std::nullopt;
        }
```

In `src/engine_services/SceneService.hpp`, after `class GuiService`:

```cpp
// The studio's own tools: plugins and what they make, such as the Move tool's
// Dragger. A child of game the explorer does not show and game scripts cannot
// reach. Nothing under it is saved or undone, and New, Open, Play, and Stop
// leave it as it is. Not a scene service: no play script runs under it.
class Core : public Service {
public:
    using Service::Service;
    const char* class_name() const override;
    bool hidden_in_explorer() const override { return true; }
};
```

In `SceneService.cpp`, after `const char* GuiService::class_name() const { return "Gui"; }`:

```cpp
const char* Core::class_name() const { return kCoreClass; }
```

In the same file's Lua registration, after `register_lua_class("Gui", "SceneService", nullptr, 0);`:

```cpp
    // Not a registered service: completion does not offer it to GetService.
    register_lua_class("Core", "Service", nullptr, 0);
```

In `Game.cpp`, the constructor, after `add_service<Audio>(*this);`:

```cpp
    // Not one of the place's services, so not in kServices: it holds the studio's own tools.
    Core& core = create<Core>();
    set_guid(core.id(), service_guid(kCoreClass));
    set_parent(core.id(), 0);
```

In `DataModel.hpp`, after `InstanceId service(std::string_view class_name) const;` and its comment:

```cpp
    // The Core service, or 0 in a DataModel that is not a Game.
    InstanceId core() const;
```

In `DataModel.cpp`, after `DataModel::service`:

```cpp
InstanceId DataModel::core() const { return service(kCoreClass); }
```

In `DataModel::parent_error`'s service branch, let Core be placed under game once:

```cpp
        const ServiceSpec* spec = find_service(object->class_name());
        InstanceId home = kNoParent;
        if (spec != nullptr) {
            home = spec->parent_class == nullptr ? 0 : service(spec->parent_class);
            if (spec->parent_class != nullptr && home == 0) {
                home = kNoParent;
            }
        } else if (std::string_view(object->class_name()) == kCoreClass) {
            home = 0;
        }
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO9]"`
Expected: PASS.

- [ ] **Step 5: Update the tests that count game's children**

Run: `./build/sandbox "[SS1],[SS2]"` and `./build/sandbox` and note the failures.
Expected: SS1, SS2, and the cases at scene_services_tests.cpp:263 and :511 fail on `get_children(0).size() == 6`.

Change each `== 6` to `== 7`. In SS1, after the loop over the five scene services, add:

```cpp
    // Assets, then Core, last.
    REQUIRE(std::string(game.instance(children[6])->class_name()) == "Core");
```

Run: `./build/sandbox`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/Containment.hpp src/engine_core/Containment.cpp src/engine_services/SceneService.hpp src/engine_services/SceneService.cpp src/engine_services/Game.cpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp sandbox/core_tests.cpp sandbox/scene_services_tests.cpp CMakeLists.txt
git commit -m "Add Core, a hidden service under game for the studio's own tools"
```

---

### Task 2: The InCore scope, and nothing crosses Core's edge

**Files:**
- Modify: `src/engine_core/Ecs.hpp:31-33,52-53`, `src/engine_core/Ecs.cpp:17-19`
- Modify: `src/engine_core/DataModel.hpp:355-362` (accessors), `:623` (`apply_scope`)
- Modify: `src/engine_core/DataModel.cpp:682-766` (`in_core`, `core_holds`, `refresh_scope`, `apply_scope`), `parent_error`
- Modify: `src/engine_core/DataModelPlace.cpp:209-226` (`clear_hierarchy`)
- Modify: `sandbox/core_tests.cpp`

**Interfaces:**
- Consumes: `DataModel::core()` (Task 1).
- Produces: `bool DataModel::in_core(InstanceId id) const;` (under Core, not Core itself) and `bool DataModel::core_holds(InstanceId id) const;` (Core or under it, false for 0 and dead ids). Every later task uses `core_holds`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/core_tests.cpp`:

```cpp
TEST_CASE("CO2a Core's descendants are in Core, and Core holds itself and them", "[CO2a]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId inner = add_folder(game, "Inner", tools);
    const InstanceId place = add_folder(game, "Place", game.scene_service("Workspace"));
    REQUIRE(game.in_core(tools));
    REQUIRE(game.in_core(inner));
    REQUIRE_FALSE(game.in_core(core));
    REQUIRE(game.core_holds(core));
    REQUIRE(game.core_holds(inner));
    REQUIRE_FALSE(game.core_holds(place));
    REQUIRE_FALSE(game.core_holds(0));
    REQUIRE(game.in_game(inner));
    REQUIRE_FALSE(game.in_workspace(inner));
}

TEST_CASE("CO5 nothing moves across Core's edge, but an instance with no parent may go in", "[CO5]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId other = add_folder(game, "Other", core);
    const InstanceId part = add_folder(game, "Part", workspace);
    // Out of Core, to the place or to no parent.
    REQUIRE(game.parent_error(tools, workspace).has_value());
    REQUIRE(game.parent_error(tools, engine_core::DataModel::kNoParent).has_value());
    REQUIRE_THROWS_AS(game.set_parent(tools, workspace), ContractViolation);
    // From the place into Core.
    REQUIRE(game.parent_error(part, core).has_value());
    REQUIRE_THROWS_AS(game.set_parent(part, tools), ContractViolation);
    // Within Core.
    REQUIRE_NOTHROW(game.set_parent(tools, other));
    // An instance with no parent, including one that left the place.
    const InstanceId loose = add_folder(game, "Loose", engine_core::DataModel::kNoParent);
    REQUIRE_NOTHROW(game.set_parent(loose, core));
    game.set_parent(part, engine_core::DataModel::kNoParent);
    REQUIRE_NOTHROW(game.set_parent(part, core));
    REQUIRE(game.in_core(part));
}
```

`parent_error` is public (DataModel.hpp:161).

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: compile error, because there is no `in_core` or `core_holds`.

- [ ] **Step 3: Add the tag**

`Ecs.hpp`:

```cpp
// Under game, under the Workspace service, under the Lighting service, and
// under the Core service.
struct InGame {};
struct InWorkspace {};
struct InLighting {};
struct InCore {};
```

and in `EcsIds`, after `ecs_id_t in_lighting = 0;`, add `ecs_id_t in_core = 0;`. In `Ecs.cpp` `register_ecs`, after `ids.in_lighting = ...;`, add `ids.in_core = world.component<ecs::InCore>().id();`.

- [ ] **Step 4: Compute it with the other scopes**

`DataModel.hpp`, the scope comment and accessors:

```cpp
    // Scope. in_game: under game. in_workspace: under the Workspace service,
    // which is not inside itself. in_lighting and in_core: under the Lighting
    // and Core services, the same way. All are false for a dead id. Kept
    // current at every tree change, so reading them costs no walk.
    bool in_game(InstanceId id) const;
    bool in_workspace(InstanceId id) const;
    bool in_lighting(InstanceId id) const;
    bool in_core(InstanceId id) const;
    // Core or anything under it: what is never saved, never undone, and left
    // alone by New, Open, Play, and Stop. False for 0 and for a dead id.
    bool core_holds(InstanceId id) const;
```

and change `apply_scope`'s declaration to:

```cpp
    void apply_scope(InstanceId id, bool in_game_now, bool in_workspace_now, bool in_lighting_now, bool in_core_now);
```

`DataModel.cpp`, after `in_lighting`:

```cpp
bool DataModel::in_core(InstanceId id) const {
    return has_tag(ecs_world(), entity_of(id), state_->ecs_ids.in_core);
}

bool DataModel::core_holds(InstanceId id) const {
    if (id == 0 || !alive(id)) {
        return false;
    }
    return in_core(id) || id == core();
}
```

Replace `refresh_scope`:

```cpp
void DataModel::refresh_scope(InstanceId id) {
    const Slot* part = slot(id);
    if (part == nullptr) {
        return;
    }
    bool game = false;
    bool workspace = false;
    bool lighting = false;
    bool in_core_now = false;
    if (part->parent == 0) {
        game = true;
    } else if (part->parent != kNoParent) {
        game = in_game(part->parent);
        workspace = in_workspace(part->parent) || part->parent == scene_service("Workspace");
        lighting = in_lighting(part->parent) || part->parent == scene_service("Lighting");
        in_core_now = in_core(part->parent) || part->parent == core();
    }
    if (in_game(id) == game && in_workspace(id) == workspace && in_lighting(id) == lighting &&
        in_core(id) == in_core_now) {
        return;
    }
    apply_scope(id, game, workspace, lighting, in_core_now);
}
```

In `apply_scope`, take the new parameter and track `InCore` like `InLighting`:

```cpp
void DataModel::apply_scope(InstanceId id, bool in_game_now, bool in_workspace_now, bool in_lighting_now,
                            bool in_core_now) {
    ...
    const InstanceId lighting_id = scene_service("Lighting");
    const InstanceId core_id = core();
    ...
        bool lighting = in_lighting_now;
        bool under_core = in_core_now;
        if (i > 0) {
            game = in_game(part->parent);
            workspace = in_workspace(part->parent) || part->parent == workspace_id;
            lighting = in_lighting(part->parent) || part->parent == lighting_id;
            under_core = in_core(part->parent) || part->parent == core_id;
        }
        const bool had_game = has_tag(world, part->entity, ids.in_game);
        const bool had_workspace = has_tag(world, part->entity, ids.in_workspace);
        const bool had_lighting = has_tag(world, part->entity, ids.in_lighting);
        const bool had_core = has_tag(world, part->entity, ids.in_core);
        if (had_game == game && had_workspace == workspace && had_lighting == lighting && had_core == under_core) {
            continue;
        }
        ...
        if (had_core != under_core) {
            set_tag(world, part->entity, ids.in_core, under_core);
        }
        // The render snapshot keeps rows for Workspace and Core, and for lights under Lighting.
        if ((had_workspace != workspace || had_lighting != lighting || had_core != under_core) &&
            (part->body != nullptr || (part->instance != nullptr && part->instance->has_visual_row()))) {
            note(cur, VisualField::Ancestry, current_origin());
        }
```

The `...` lines are the existing code, unchanged. Search for other callers of `apply_scope` (`grep -n "apply_scope(" src/engine_core/*.cpp`) and pass `in_core` the same way they pass `in_lighting`.

In `DataModelPlace.cpp` `clear_hierarchy`, after the `in_lighting` line:

```cpp
        set_tag(world, part.entity, state_->ecs_ids.in_core, false);
```

- [ ] **Step 5: Refuse crossings**

In `DataModel::parent_error`, after the service branch's closing `}` and before `if (new_parent == kNoParent) {`:

```cpp
    // Core's contents stay in Core, so history and the place never meet them.
    // An instance with no parent may go in. Destroy is the only way out.
    const bool from_core = in_core(id);
    const bool to_core = new_parent != kNoParent && (new_parent == core() || in_core(new_parent));
    if (from_core && !to_core) {
        return name(id) + " is in Core, and what is in Core stays there";
    }
    if (!from_core && to_core && current != kNoParent) {
        return "Only an instance with no parent can go into Core";
    }
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO2a],[CO5],[CO9]"`
Expected: PASS.

- [ ] **Step 7: Run the whole suite**

Run: `./build/sandbox`
Expected: all pass. A failure in a project or history test that moves instances means the crossing rule fired on something outside Core. Check `from_core` and `to_core` against that test's tree.

- [ ] **Step 8: Commit**

```bash
git add src/engine_core/Ecs.hpp src/engine_core/Ecs.cpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/DataModelPlace.cpp sandbox/core_tests.cpp
git commit -m "Tag what is under Core, and keep anything from crossing into or out of it"
```

---

### Task 3: Core survives New, Open, Play, and Stop, and is never saved

**Files:**
- Modify: `src/engine_core/Project.cpp:848-870` (`clear_world`)
- Modify: `src/engine_core/DataModelPlace.cpp` (`capture_place_unlocked`, `restore_place_unlocked`, `authored_tree`)
- Modify: `src/engine_core/DataModelState.hpp:168-169` (`place_slots`)
- Modify: `src/engine_core/DataModel.cpp:560-567` (`destroy` keeps captured slots off the free list during play)
- Modify: `sandbox/core_tests.cpp`

**Interfaces:**
- Consumes: `core()`, `core_holds()` (Tasks 1, 2).

- [ ] **Step 1: Write the failing tests**

Add `#include "Project.hpp"` to `core_tests.cpp`, and append:

```cpp
TEST_CASE("CO1 New and Open leave Core's instances with their ids, names, and children", "[CO1][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId inner = add_folder(game, "Inner", tools);

    engine_core::Project::reset_place(game);
    REQUIRE(game.core() == core);
    REQUIRE(game.alive(tools));
    REQUIRE(game.parent(inner) == tools);
    REQUIRE(game.name(inner) == "Inner");

    engine_core::Project reopened = engine_core::Project::load(dir.path, game);
    REQUIRE(game.core() == core);
    REQUIRE(game.alive(inner));
    REQUIRE(game.parent(tools) == core);
    REQUIRE(game.get_children(0).back() == core);
}

TEST_CASE("CO2 what changes in Core during play is still there after Stop", "[CO2]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId place = add_folder(game, "Place", game.scene_service("Workspace"));
    game.start_simulation();
    game.set_name(tools, "Renamed");
    const InstanceId made = add_folder(game, "Made", core);
    game.set_name(place, "PlayName");
    const InstanceId play_only = add_folder(game, "PlayOnly", game.scene_service("Workspace"));
    game.stop_simulation();
    REQUIRE(game.name(tools) == "Renamed");
    REQUIRE(game.alive(made));
    REQUIRE(game.parent(made) == core);
    REQUIRE(game.in_core(made));
    // The place outside Core is restored as before.
    REQUIRE(game.name(place) == "Place");
    REQUIRE_FALSE(game.alive(play_only));
    REQUIRE(game.get_children(0).back() == core);
}

TEST_CASE("CO2b a Core instance made after a captured one is destroyed in play survives Stop", "[CO2b]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId doomed = add_folder(game, "Doomed", game.scene_service("Workspace"));
    game.start_simulation();
    game.destroy(doomed);
    const InstanceId made = add_folder(game, "Made", core);
    game.stop_simulation();
    REQUIRE(game.alive(made));
    REQUIRE(game.parent(made) == core);
    REQUIRE(game.alive(doomed));
    REQUIRE(game.name(doomed) == "Doomed");
}

TEST_CASE("CO3 Core is never saved and never makes the place unsaved", "[CO3][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    REQUIRE_FALSE(project.unsaved());
    const std::uint64_t before = engine_core::Project::place_fingerprint(game);
    add_folder(game, "Tools", game.core());
    REQUIRE_FALSE(project.unsaved());
    REQUIRE(engine_core::Project::place_fingerprint(game) == before);
    project.save();

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    REQUIRE(other.get_children(other.core()).empty());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO1],[CO2],[CO2b],[CO3]"`
Expected: CO1 fails, because `reset_place` destroyed `tools`. CO2 and CO2b fail, because Stop retired `made`. CO3 fails on `unsaved()` or the fingerprint.

- [ ] **Step 3: `clear_world` skips Core**

In `Project.cpp` `clear_world`, change the collection:

```cpp
// Destroys every live instance, parented or not, but the services, which go
// back to their defaults, and Core with all it holds, which stays as it is.
// Clears the root's extras.
void clear_world(DataModel& world) {
    std::vector<InstanceId> ids;
    world.for_each_instance([&world, &ids](DataModel& object) {
        if (!object.is_service() && !world.core_holds(object.id())) {
            ids.push_back(object.id());
        }
    });
```

- [ ] **Step 4: Saving skips Core**

In `DataModelPlace.cpp` `authored_tree`, in the edit-mode loop over `first_child(parent_id)`, skip Core and so its subtree:

```cpp
            for (InstanceId child = first_child(parent_id); child != 0; child = next_sibling(child)) {
                const DataModel* object = instance(child);
                // Core is the studio's, not the place's: it and all it holds are never saved.
                if (object == nullptr || child == core()) {
                    continue;
                }
```

The play-mode branch reads the snapshot, which Step 5 keeps Core out of.

- [ ] **Step 5: Capture and restore leave Core alone**

`DataModelState.hpp`, after `PlaceSnapshot place;`:

```cpp
    // place_slots[index]: a captured instance owns slot index. While the
    // simulation runs, destroying one keeps its slot off the free list, so no
    // new instance, such as one made in Core, takes it before Stop restores it.
    std::vector<bool> place_slots;
```

In `capture_place_unlocked`, leave Core and its subtree out, and mark the captured slots:

```cpp
    shot.root_children = child_ids(0);
    const InstanceId core_id = core();
    shot.root_children.erase(std::remove(shot.root_children.begin(), shot.root_children.end(), core_id),
                             shot.root_children.end());
    const std::uint32_t count = slot_count();
    shot.instances.reserve(count);
    state_->place_slots.assign(count, false);
    for (std::uint32_t index = 0; index < count; ++index) {
        Slot& part = state_->slots[index];
        if (!part.alive || part.instance == nullptr) {
            continue;
        }
        // Core is outside the place: Stop leaves it as play left it.
        if (core_holds(make_instance_id(part.generation, index))) {
            continue;
        }
        state_->place_slots[index] = true;
        ...existing body...
```

Add `#include <algorithm>` to DataModelPlace.cpp if it is missing.

In `restore_place_unlocked`, keep Core's live slots and relink its subtree after the place's:

```cpp
    // Core and what it holds stay as they are, links included.
    const InstanceId core_id = core();
    std::vector<std::pair<InstanceId, std::vector<InstanceId>>> core_links;
    if (core_id != 0) {
        std::vector<InstanceId> walk{core_id};
        for (std::size_t at = 0; at < walk.size(); ++at) {
            std::vector<InstanceId> children = child_ids(walk[at]);
            walk.insert(walk.end(), children.begin(), children.end());
            core_links.emplace_back(walk[at], std::move(children));
        }
    }

    for (std::uint32_t index = 0; index < state_->slots.size(); ++index) {
        Slot& part = state_->slots[index];
        if (!part.alive) {
            continue;
        }
        const InstanceId id = make_instance_id(part.generation, index);
        if (ids.count(id) == 0 && !core_holds(id)) {
            retire_slot(index, true);
        }
    }
    for (const PlaceRecord& record : place.instances) {
        restore_record(record);
    }
    clear_hierarchy();
    link_children(0, place.root_children);
    for (const PlaceRecord& record : place.instances) {
        link_children(record.id, record.children);
    }
    if (core_id != 0) {
        link_children(0, {core_id});
        for (const auto& [parent, children] : core_links) {
            link_children(parent, children);
        }
    }
```

The `core_links` block goes before the retire loop, while the tree is still linked. The retire loop's condition gains `&& !core_holds(id)`. The relink goes after the place's relink.

In `DataModel::destroy` (DataModel.cpp around :563), replace the free-list push:

```cpp
    if (part->generation != kMaxGeneration) {
        ++part->generation;
        // A captured instance's slot waits for Stop, which brings it back there.
        const bool captured = state_->simulation_running && index < state_->place_slots.size() &&
                              state_->place_slots[index];
        if (!captured) {
            state_->free_list.push_back(index);
        }
    }
```

`rebuild_free_list` at the end of `restore_place_unlocked` already puts every dead slot back.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO1],[CO2],[CO2b],[CO3]"`
Expected: PASS.

- [ ] **Step 7: Run the whole suite**

Run: `./build/sandbox`
Expected: all pass, including the project, history, and play/stop tests.

- [ ] **Step 8: Commit**

```bash
git add src/engine_core/Project.cpp src/engine_core/DataModelPlace.cpp src/engine_core/DataModelState.hpp src/engine_core/DataModel.cpp sandbox/core_tests.cpp
git commit -m "Keep Core through New, Open, Play, and Stop, and out of every save"
```

---

### Task 4: Nothing under Core enters undo history

**Files:**
- Modify: `src/engine_services/ChangeHistoryService.hpp:195-201` (private `forget_core`)
- Modify: `src/engine_services/ChangeHistoryService.cpp:184-228` (`note`, `apply_waypoint`, new `forget_core`)
- Modify: `src/engine_core/DataModel.cpp:551-553` (`destroy`'s capture)
- Modify: `sandbox/core_tests.cpp`

**Interfaces:**
- Consumes: `core_holds()` (Task 2).

- [ ] **Step 1: Write the failing tests**

Add `#include "ChangeHistoryService.hpp"` and append:

```cpp
TEST_CASE("CO4 changes under Core record no history", "[CO4]") {
    SimRole role;
    engine_core::Game game;
    game.history().reset_waypoints();
    const InstanceId tools = add_folder(game, "Tools", game.core());
    game.history().end_gesture();
    game.set_name(tools, "Renamed");
    game.history().end_gesture();
    const InstanceId other = add_folder(game, "Other", game.core());
    game.set_parent(tools, other);
    game.history().end_gesture();
    game.destroy(tools);
    game.history().end_gesture();
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE_FALSE(game.history().is_recording_in_progress());
}

TEST_CASE("CO4b making an instance and putting it in Core leaves no undo step, and the next edit stands alone",
          "[CO4b]") {
    SimRole role;
    engine_core::Game game;
    game.history().reset_waypoints();
    // As a plugin does: made with no parent, named, then put in Core, with no gesture between.
    const InstanceId tool = add_folder(game, "Tool", engine_core::DataModel::kNoParent);
    game.set_name(tool, "Dragger");
    game.set_parent(tool, game.core());
    REQUIRE_FALSE(game.history().is_recording_in_progress());
    // The user's next edit is its own step, and undoing it leaves the tool alone.
    const InstanceId part = add_folder(game, "Part", game.scene_service("Workspace"));
    game.history().end_gesture();
    REQUIRE(game.history().can_undo().first);
    game.history().undo();
    REQUIRE_FALSE(game.alive(part));
    REQUIRE(game.alive(tool));
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("CO4c undoing a step recorded before an instance went into Core does not touch it", "[CO4c]") {
    SimRole role;
    engine_core::Game game;
    game.history().reset_waypoints();
    const InstanceId part = add_folder(game, "Part", game.scene_service("Workspace"));
    game.history().end_gesture();
    game.set_parent(part, engine_core::DataModel::kNoParent);
    game.history().end_gesture();
    game.set_parent(part, game.core());
    game.history().end_gesture();
    while (game.history().can_undo().first) {
        game.history().undo();
    }
    REQUIRE(game.alive(part));
    REQUIRE(game.parent(part) == game.core());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO4],[CO4b],[CO4c]"`
Expected: CO4 fails on `can_undo`. CO4b fails on `is_recording_in_progress` or on `alive(tool)` after undo. CO4c fails with `part` destroyed or moved.

- [ ] **Step 3: Implement**

`ChangeHistoryService.hpp`, private section, after `void push_or_coalesce(Mutation mutation);`:

```cpp
    // Drops what the open recording holds about instances now in Core. An
    // implicit recording left empty is cancelled, so it does not wait to merge
    // into the next gesture.
    void forget_core();
```

`ChangeHistoryService.cpp` `note`, after the first `if (!enabled_ || applying_ != 0 || game_ == nullptr) { return; }`:

```cpp
    // Core is the studio's, not the place's: what happens there is never an
    // undo step, and what entered it leaves the open recording.
    if (game_->core_holds(mutation.id)) {
        forget_core();
        return;
    }
```

After `push_or_coalesce`, add:

```cpp
void ChangeHistoryService::forget_core() {
    if (!recording_) {
        return;
    }
    std::vector<Mutation>& list = recording_->mutations;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [this](const Mutation& mutation) { return game_->core_holds(mutation.id); }),
               list.end());
    if (recording_->implicit && list.empty()) {
        const std::string id = recording_->id;
        finish_recording(id, FinishRecordingOperation::Cancel);
    }
}
```

Add `#include <algorithm>` if it is missing. In `apply_waypoint`, skip Core in both loops:

```cpp
    if (inverse) {
        for (std::size_t index = waypoint.mutations.size(); index > 0; --index) {
            const Mutation& mutation = waypoint.mutations[index - 1];
            // A step recorded before its instance went into Core no longer reaches it.
            if (!game_->core_holds(mutation.id)) {
                game_->apply_history(mutation, true);
            }
        }
    } else {
        for (const Mutation& mutation : waypoint.mutations) {
            if (!game_->core_holds(mutation.id)) {
                game_->apply_history(mutation, false);
            }
        }
    }
```

In `DataModel::destroy`, the capture before the instance goes:

```cpp
    std::optional<AuthoredRecord> captured;
    if (state_->history && state_->history->wants_mutation() && !core_holds(id)) {
        captured = capture_record(id, true);
    }
```

That is needed because `record_destroyed` notes the mutation after the instance is gone, when `core_holds` can no longer see it.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO4],[CO4b],[CO4c]"`
Expected: PASS.

- [ ] **Step 5: Run the whole suite**

Run: `./build/sandbox`
Expected: all pass, history tests included.

- [ ] **Step 6: Commit**

```bash
git add src/engine_services/ChangeHistoryService.hpp src/engine_services/ChangeHistoryService.cpp src/engine_core/DataModel.cpp sandbox/core_tests.cpp
git commit -m "Record no history under Core, and drop what entered it from the open step"
```

---

### Task 5: Game scripts cannot see Core, and Scripts in Core run in the plugin VM

**Files:**
- Modify: `src/engine_core/ScriptBindings.hpp:229` (`ScriptBindings` gains `hidden_from_play`)
- Modify: `src/engine_core/ScriptBindings.cpp` (`instance_service` :602, `instance_children` :507, `instance_find` :527, `instance_wait_child` :562, `instance_index` child fallback :368)
- Modify: `src/engine_core/ScriptRuntime.hpp` (public `start_core_scripts`; private `core_pending_`, `core_scripts_`, `note_core`)
- Modify: `src/engine_core/ScriptRuntime.cpp` (`step_tools` :179, `on_moved` :277, `on_script_enabled` :322, `on_script_destroyed` :332, `update_tools_open` :685)
- Modify: `sandbox/core_tests.cpp`

**Interfaces:**
- Consumes: `core()`, `core_holds()`, `in_core()` (Tasks 1, 2).
- Produces: `void ScriptRuntime::start_core_scripts();`, SimulationThread, outside any Lua call: registers each queued Script in Core as a plugin and unregisters each that left, was disabled, or was destroyed. Task 7 calls it.

- [ ] **Step 1: Write the failing tests**

Add `#include "Script.hpp"` and, in the helper namespace, a `has_line` helper:

```cpp
bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}
```

Append:

```cpp
TEST_CASE("CO6 a game script cannot reach Core, while a plugin and the command line can", "[CO6]") {
    ScriptRig rig;
    add_script(rig.game, "Probe", R"(
        print("service", pcall(function() return game:GetService("Core") end))
        print("index", pcall(function() return game.Core end))
        print("find", game:FindFirstChild("Core"))
        local listed = false
        for _, child in game:GetChildren() do
            if child.Name == "Core" then listed = true end
        end
        print("listed", listed)
        print("wait", game:WaitForChild("Core", 0.05))
    )");
    rig.game.start_simulation();
    rig.frames(10, 0.02);
    const auto output = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output, "find\tnil\n"));
    REQUIRE(has_line(output, "listed\tfalse\n"));
    REQUIRE(has_line(output, "wait\tnil\n"));
    bool service_refused = false;
    bool index_refused = false;
    for (const auto& line : output.lines) {
        service_refused = service_refused || line.text.rfind("service\tfalse", 0) == 0;
        index_refused = index_refused || line.text.rfind("index\tfalse", 0) == 0;
    }
    REQUIRE(service_refused);
    REQUIRE(index_refused);
    rig.game.stop_simulation();

    rig.runtime.run_chunk(R"(print("console", game:GetService("Core").Name, game:FindFirstChild("Core") ~= nil))");
    rig.frames(1);
    REQUIRE(has_line(rig.runtime.drain_output(), "console\tCore\ttrue\n"));
}

TEST_CASE("CO7 a Script in Core runs in the plugin VM, in edit mode and through Play, Stop, and New", "[CO7]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, rig.game.core(), "Tool", R"(
        print("tool started")
        game:GetService("RunService").Heartbeat:Connect(function()
            _G.ticks = (_G.ticks or 0) + 1
        end)
    )");
    rig.frames(1);
    REQUIRE(has_line(rig.runtime.drain_output(), "tool started\n"));
    REQUIRE(rig.runtime.is_plugin(script.id()));

    rig.game.start_simulation();
    rig.frames(2);
    rig.game.stop_simulation();
    engine_core::Project::reset_place(rig.game);
    rig.frames(2);
    REQUIRE(rig.runtime.is_plugin(script.id()));
    REQUIRE_FALSE(has_line(rig.runtime.drain_output(), "tool started\n"));
}

TEST_CASE("CO7b disabling or destroying a Script in Core stops it, and enabling it starts it again", "[CO7b]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, rig.game.core(), "Tool", R"(print("tool started"))");
    rig.frames(1);
    REQUIRE(rig.runtime.is_plugin(script.id()));
    script.set_enabled(false);
    rig.frames(1);
    REQUIRE_FALSE(rig.runtime.is_plugin(script.id()));
    rig.runtime.drain_output();
    script.set_enabled(true);
    rig.frames(1);
    REQUIRE(rig.runtime.is_plugin(script.id()));
    REQUIRE(has_line(rig.runtime.drain_output(), "tool started\n"));
    const InstanceId id = script.id();
    rig.game.destroy(id);
    rig.frames(1);
    REQUIRE_FALSE(rig.runtime.is_plugin(id));
}

TEST_CASE("CO7c a plugin that puts a Script into Core starts it on the next step", "[CO7c]") {
    ScriptRig rig;
    add_script(rig.game, rig.game.core(), "Maker", R"(
        local made = Instance.new("Script")
        made.Name = "Made"
        made.Source = "print('made ran')"
        made.Parent = script.Parent
        print("maker done")
    )");
    rig.frames(1);
    const auto first = rig.runtime.drain_output();
    REQUIRE(has_line(first, "maker done\n"));
    rig.frames(1);
    const auto second = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE((has_line(first, "made ran\n") || has_line(second, "made ran\n")));
}
```

`run_chunk` is the command line's entry point (ScriptRuntime.hpp:133). `Script::set_enabled` exists, and `Source` is writable from Lua (LuaSource.cpp:101).

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO6],[CO7],[CO7b],[CO7c]"`
Expected: CO6 fails, because the play script finds and lists Core. CO7, CO7b, and CO7c fail, because nothing starts a Script in Core.

- [ ] **Step 3: Hide Core from the play VM**

In `ScriptBindings.hpp`, inside `struct ScriptBindings`, add:

```cpp
    // A game script never sees Core or what is in it. The command line and plugins do.
    static bool hidden_from_play(lua_State* state, ScriptRuntime& runtime, InstanceId id);
```

In `ScriptBindings.cpp`:

```cpp
bool ScriptBindings::hidden_from_play(lua_State* state, ScriptRuntime& runtime, InstanceId id) {
    const ScriptRuntime::Vm* vm = runtime.vm_from(state);
    return vm != nullptr && vm->kind == ScriptRuntime::VmKind::Play && runtime.game_->core_holds(id);
}
```

`instance_service`, after `const InstanceId found = ...;`:

```cpp
        if (found != 0 && hidden_from_play(state, *runtime, found)) {
            luaL_error(state, "%s is not available to game scripts", name);
        }
```

`instance_children`, in the loop:

```cpp
        for (InstanceId child : children) {
            if (hidden_from_play(state, *runtime, child)) {
                continue;
            }
```

`instance_find`, after `find_first_child`:

```cpp
        if (child == 0 || hidden_from_play(state, *runtime, child)) {
            lua_pushnil(state);
        } else {
```

`instance_wait_child`, the first lookup:

```cpp
        const InstanceId child = runtime->game_->find_first_child(ud->id, wanted);
        if (child != 0 && !hidden_from_play(state, *runtime, child)) {
```

`instance_index`, the child fallback (`if (field == nullptr) { const InstanceId child = ...`):

```cpp
            const InstanceId child = runtime->game_->find_first_child(object->id(), key != nullptr ? key : "");
            if (child != 0 && !hidden_from_play(state, *runtime, child)) {
```

- [ ] **Step 4: Start Scripts that enter Core**

`ScriptRuntime.hpp`, public, near `register_plugin`:

```cpp
    // Scripts in Core run in the plugin VM, as plugins: one that enters Core
    // while Enabled starts, and one that leaves, is disabled, or is destroyed
    // stops. The script host's hooks queue them, and this registers or
    // unregisters what is queued. step_tools calls it first, and tools_open()
    // is true while something waits. SimulationThread, outside any Lua call.
    void start_core_scripts();
```

Private members:

```cpp
    // Scripts in Core waiting for start_core_scripts, and those it registered.
    std::vector<InstanceId> core_pending_;
    std::unordered_set<InstanceId> core_scripts_;
    // Queues every Script in id's subtree that start_core_scripts must look at.
    void note_core(InstanceId id);
```

Add `#include <unordered_set>` if it is missing.

`ScriptRuntime.cpp`:

```cpp
void ScriptRuntime::note_core(InstanceId id) {
    if (game_ == nullptr) {
        return;
    }
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        if (dynamic_cast<Script*>(game_->instance(next)) != nullptr &&
            (game_->core_holds(next) || core_scripts_.count(next) != 0)) {
            core_pending_.push_back(next);
        }
        for (InstanceId child = game_->first_child(next); child != 0; child = game_->next_sibling(child)) {
            pending.push_back(child);
        }
    }
    update_tools_open();
}

void ScriptRuntime::start_core_scripts() {
    if (game_ == nullptr || core_pending_.empty()) {
        return;
    }
    std::vector<InstanceId> pending;
    pending.swap(core_pending_);
    for (InstanceId id : pending) {
        auto* script = dynamic_cast<Script*>(game_->instance(id));
        const bool want = script != nullptr && script->enabled() && game_->core_holds(id);
        const bool have = core_scripts_.count(id) != 0;
        if (want && !have) {
            if (register_plugin(id)) {
                core_scripts_.insert(id);
            }
        } else if (!want && have) {
            core_scripts_.erase(id);
            unregister_plugin(id);
        }
    }
    update_tools_open();
}
```

`on_moved`, first lines:

```cpp
void ScriptRuntime::on_moved(InstanceId id) {
    // Core's Scripts run whether or not the place plays. Nothing moves out of
    // Core, so a move anywhere else costs no walk.
    if (game_ != nullptr && game_->core_holds(id)) {
        note_core(id);
    }
    if (game_ == nullptr || !game_->simulation_running() || play_.closing) {
        return;
    }
```

`on_script_enabled`, first lines:

```cpp
void ScriptRuntime::on_script_enabled(Script& script, bool enabled) {
    if (game_ != nullptr && (game_->core_holds(script.id()) || core_scripts_.count(script.id()) != 0)) {
        note_core(script.id());
        return;
    }
```

`on_script_destroyed`, first line, before the existing body:

```cpp
    core_scripts_.erase(script.id());
```

The plugin threads it owned are already stopped by the existing `kill_owned(plugin_, script.id(), 0)`. `register_plugin` and `unregister_plugin` drop dead roots on their next call.

`step_tools`, right after `if (game_ == nullptr) { return; }`:

```cpp
    start_core_scripts();
```

`update_tools_open`:

```cpp
void ScriptRuntime::update_tools_open() {
    tools_open_.store(console_.state != nullptr || plugin_.state != nullptr || !core_pending_.empty(),
                      std::memory_order_relaxed);
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO6],[CO7],[CO7b],[CO7c]"`
Expected: PASS.

- [ ] **Step 6: Run the whole suite**

Run: `./build/sandbox`
Expected: all pass, PL1–PL10 included, since plugins registered by hand outside Core are unchanged.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/ScriptBindings.hpp src/engine_core/ScriptBindings.cpp src/engine_core/ScriptRuntime.hpp src/engine_core/ScriptRuntime.cpp sandbox/core_tests.cpp
git commit -m "Hide Core from game scripts, and run the Scripts in it as plugins"
```

---

### Task 6: Core renders like Workspace, with no physics

**Files:**
- Modify: `src/engine_core/DataModelState.hpp:95-100` (`core_render_query`)
- Modify: `src/engine_core/DataModel.cpp:57-64` (build it), `:851-862` (`for_each_rendered` walks both)
- Modify: `src/engine_core/SnapshotPump.cpp:76,201,251`
- Modify: `sandbox/core_tests.cpp`

**Interfaces:**
- Consumes: `in_core()` (Task 2), `ecs::InCore` (Task 2).

- [ ] **Step 1: Write the failing test**

Add `#include "PhysicsObject.hpp"`, `#include "SnapshotPump.hpp"`, and `#include <algorithm>`, and append:

```cpp
TEST_CASE("CO8 a GameObject in Core has a snapshot row, and a PhysicsObject in Core never simulates", "[CO8]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& shown = game.create_game_object();
    game.set_name(shown.id(), "Shown");
    game.set_parent(shown.id(), game.core());
    std::vector<InstanceId> rendered;
    game.for_each_rendered([&](const engine_core::GameObject& object) { rendered.push_back(object.id()); });
    REQUIRE(std::find(rendered.begin(), rendered.end(), shown.id()) != rendered.end());

    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE(pump.find(shown.id()) != nullptr);

    engine_core::PhysicsObject& body = game.create<engine_core::PhysicsObject>();
    game.set_parent(body.id(), game.core());
    std::vector<InstanceId> bodies;
    game.physics_bodies(bodies);
    REQUIRE(bodies.empty());
}
```

`physics_bodies` (DataModel.cpp:800) lists what `body_query` matches, so an empty list means the physics world never sees the body. The pump is driven as `camera_tests.cpp` CAM3 drives it.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO8]"`
Expected: FAIL. `rendered` does not hold `shown`.

- [ ] **Step 3: Implement**

`DataModelState.hpp`, after `render_query`:

```cpp
    // Rendered GameObjects in Core: the same, with InCore. Core draws as Workspace does.
    flecs::query<> core_render_query;
```

`DataModel.cpp`, after `world.render_query = ...build();`:

```cpp
    world.core_render_query = world.ecs.query_builder<>()
                                  .with<ecs::Instance>()
                                  .in()
                                  .with<ecs::InCore>()
                                  .with<Matrix4>()
                                  .inout_none()
                                  .cached()
                                  .build();
```

`for_each_rendered` walks both:

```cpp
void DataModel::for_each_rendered(const std::function<void(const GameObject&)>& fn) const {
    for (const flecs::query<>* query : {&state_->render_query, &state_->core_render_query}) {
        ecs_iter_t it = ecs_query_iter(ecs_world(), query->c_ptr());
        while (ecs_query_next(&it)) {
            const auto* owners = static_cast<const ecs::Instance*>(ecs_field_w_size(&it, sizeof(ecs::Instance), 0));
            for (std::int32_t i = 0; i < it.count; ++i) {
                if (const GameObject* object = game_object(owners[i].id)) {
                    fn(*object);
                }
            }
        }
    }
}
```

`SnapshotPump.cpp`:
- `:76` `has_row`: `return game.in_workspace(id) || game.in_core(id) || (game.in_lighting(id) && is_light(game.instance(id)));`. Update its comment to "a GameObject or DirectionalLight in Workspace or Core, or any light under Lighting".
- `:201`: `set_row_prefab(*inst, game.in_workspace(change.id) || game.in_core(change.id) ? object->prefab_guid() : kNoPrefab);`
- `:251`: `const bool lit = !game.in_workspace(id) && !game.in_core(id) && dynamic_cast<const Light*>(instance) != nullptr;`. The comment becomes "A light GameObject in Workspace or Core already has its row from the query."

`body_query` keeps `InWorkspace` alone, which is what keeps physics out of Core.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO8]"`
Expected: PASS.

- [ ] **Step 5: Run the whole suite**

Run: `./build/sandbox`
Expected: all pass, prefab render and dense views tests included.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/DataModelState.hpp src/engine_core/DataModel.cpp src/engine_core/SnapshotPump.cpp sandbox/core_tests.cpp
git commit -m "Draw what is in Core as Workspace draws, with no physics"
```

---

### Task 7: The built-in plugins live in Core

**Files:**
- Modify: `src/ide/PluginLoader.hpp:31-44`, `src/ide/PluginLoader.cpp:44-81`
- Modify: `src/ide/IdeLayoutProject.cpp:654,714` (the `load_plugins()` calls after New and Open)
- Modify: `sandbox/core_tests.cpp`

Do not edit `src/ide/IdeLayout.cpp`: the user has uncommitted changes there, and its `load_plugins` needs no change.

**Interfaces:**
- Consumes: `core()` (Task 1), `start_core_scripts()` (Task 5).
- Produces: `PluginLoader::load(DataModel&, ScriptRuntime&, files)`, same signature. It returns how many of its Scripts are registered plugins once it returns.

- [ ] **Step 1: Write the failing test**

Add `#include "ide/PluginLoader.hpp"` and append:

```cpp
TEST_CASE("CO10 the built-in plugins load into Core, and New and Open keep them", "[CO10][project]") {
    ScriptRig rig;
    TempDir dir;
    engine_core::Project project = engine_core::Project::create(dir.path, rig.game);
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, {ide::PluginFile{"Hello", "print('hello')"}}) == 1);
    const InstanceId plugin = loader.loaded()[0];
    REQUIRE(rig.game.parent(plugin) == rig.game.core());
    REQUIRE(rig.runtime.is_plugin(plugin));
    REQUIRE_FALSE(rig.game.history().can_undo().first);

    engine_core::Project::reset_place(rig.game);
    engine_core::Project reopened = engine_core::Project::load(dir.path, rig.game);
    rig.frames(1);
    REQUIRE(rig.game.alive(plugin));
    REQUIRE(rig.runtime.is_plugin(plugin));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO10]"`
Expected: FAIL. The loaded Script's parent is not Core.

- [ ] **Step 3: Implement**

`PluginLoader.hpp`, replace the class comment and method comment:

```cpp
// Runs plugin files in the plugin VM, each as a Script in Core: not in the
// explorer, not saved, not an undo step, and kept through New, Open, Play, and
// Stop, so the studio loads them once.
class PluginLoader {
public:
    // SimulationThread. Destroys what the last load made, then makes one
    // Script per file in Core and starts them. Returns how many run as plugins.
    std::size_t load(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                     const std::vector<PluginFile>& files);
```

`PluginLoader.cpp`: delete `RecordingGuard` and its anonymous namespace, and replace `load`:

```cpp
std::size_t PluginLoader::load(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                               const std::vector<PluginFile>& files) {
    // In Core, so none of this is the user's edit.
    for (engine_core::InstanceId id : loaded_) {
        if (game.alive(id)) {
            game.destroy(id);
        }
    }
    loaded_.clear();
    const engine_core::InstanceId core = game.core();
    for (const PluginFile& file : files) {
        engine_core::Script& script = game.create<engine_core::Script>();
        game.set_name(script.id(), file.name);
        script.set_source(file.source);
        game.set_parent(script.id(), core);
        loaded_.push_back(script.id());
    }
    scripts.start_core_scripts();
    std::size_t registered = 0;
    for (engine_core::InstanceId id : loaded_) {
        if (scripts.is_plugin(id)) {
            ++registered;
        }
    }
    return registered;
}
```

Remove `#include "ChangeHistoryService.hpp"` if nothing else in the file uses it.

`IdeLayoutProject.cpp`: delete the `load_plugins();` line in `new_place` (:654) and in the open path (:714). Core keeps the plugins.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[CO10],[SC14],[SC20]"`
Expected: PASS. SC14 ("loading the built-in plugins is not an edit") now checks Task 4's purge, and SC20 checks a bad file beside a good one.

- [ ] **Step 5: Run every test program**

Run: `cmake --build build --parallel && ./build/sandbox && (cd build && ctest -C Release)`
Expected: the sandbox passes, and 13/13 ctest programs pass.

- [ ] **Step 6: Run the studio**

Run: `make run`
Expected: the studio opens. The Game Explorer shows no Core. In a scene view, hold the right mouse button and use WASD: the camera flies. Use File > New, then try the camera again: it still flies, because the plugin was not reloaded and still runs from Core. Quit.

- [ ] **Step 7: Commit**

```bash
git add src/ide/PluginLoader.hpp src/ide/PluginLoader.cpp src/ide/IdeLayoutProject.cpp sandbox/core_tests.cpp
git commit -m "Load the built-in plugins into Core once, at startup"
```
