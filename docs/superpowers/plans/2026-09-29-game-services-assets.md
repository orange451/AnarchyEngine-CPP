# Game Services and the Assets Pane Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a hidden, fixed `Assets` tree of GameServices under `game`, six asset classes with file paths and GUID-held references, and a Finder-style Assets pane with Icons, List, and Columns views.

**Architecture:** One table in `engine_core/Containment` names every service and its parent, and one set of rules by class name says what may go where; `DataModel::parent_error` and the project reader both use them. References are stored as the target's GUID (`InstanceRef`) and resolved on read, so project loads, Stop, and undo need no ordering. The studio gets a widget-free `AssetBrowser` model in `studio_core` and an `IdeAssets` pane in `studio` that draws it.

**Tech Stack:** C++17, CMake 3.16 with MSVC 14.23 (Visual Studio 16 generator) on this machine, Luau, JadeFX (`../JadeFX_CPP`), Catch2 v3 for `sandbox`, plain `Expect` programs for studio tests.

**Spec:** `docs/superpowers/specs/2026-09-29-game-services-assets-design.md`

## Global Constraints

- Service classes and order under game: `Workspace`, `Lighting`, `Storage`, `Scripts`, `Assets`; under `Assets`: `Materials`, `Prefabs`, `Meshes`, `Textures`, `Audio`.
- A service's GUID is its class name in lowercase; its Name is its class name.
- Asset classes and their homes: `Material`→`Materials`, `Prefab`→`Prefabs`, `Mesh`→`Meshes`, `Texture`→`Textures`, `Sound`→`Audio`, `Model`→a `Prefab`.
- Messages, verbatim: `<Name> cannot be moved`, `<Name> cannot be renamed`, `<Name> cannot be destroyed`, `Only scene services can be children of game; put <Name> in Workspace`, `Assets holds only Materials, Prefabs, Meshes, Textures, and Audio`, `Textures holds Textures and Folders` (and `Materials holds Materials and Folders`, `Prefabs holds Prefabs and Folders`, `Meshes holds Meshes and Folders`, `Audio holds Sounds and Folders`), `A Prefab holds only Models`, `A Texture must be in Assets.Textures` (same form for Mesh, Sound, Material, Prefab), `A Model must be in a Prefab`, `DiffuseTexture must be a Texture` (same form for every reference), `Path must be relative to the resources folder`.
- Reference property types are the class name with `?`: `Texture?`, `Mesh?`, `Material?`. Their saved default is `null`.
- `Path` is a saved `string` property with default `""`.
- The pane's view is saved in `preferences.json` as `"assetsView"`: `"icons"`, `"list"`, or `"columns"`; default `"icons"`.
- Build warning-free at `/W4` (MSVC) and `-Wall -Wextra`. Do not use `getenv`; do not write a `FLT_MAX` float literal. After restoring a file from a copy, touch it before rebuilding (MSBuild uses timestamps).
- Comment style: short, plain sentences saying what a thing is or why, as the surrounding code does. No `TODO`.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

**Build and test commands (Windows, this machine):**

- Configure once: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- Build one target: `cmake --build build --config Release --target sandbox --parallel`
- Run a sandbox case: `build/Release/sandbox.exe "[GS1]"`
- Run a studio test program: `build/Release/explorer-tests.exe` (prints `FAIL ...` lines and exits non-zero on failure)
- Everything: `make test` (or `cmake --build build --config Release --parallel && cd build && ctest -C Release --output-on-failure`)

On macOS/Linux the executables are in `build/` rather than `build/Release/`.

## Review Focus

1. **A Folder dragged between services carries assets with it.** A Folder holding a Texture dragged from Textures into Workspace, or into Meshes, must be refused whole, with nothing moved. Test: GS5 in Task 3.
2. **A reference whose target is deleted, then the delete undone.** The Material must show the Texture again. Test: GS9 in Task 5.
3. **Opening a project saved before this change, and one whose Assets folder lacks a category.** Both must load, show every category, and write the missing files at the next Save. Tests: GS13 and GS14 in Task 7.
4. **Inserting into a Prefab or a category from the Assets pane with the wrong class.** The explorer's insert path used to call `set_parent` without asking, which would abort. It must refuse with a toast, leave nothing behind, and leave nothing to undo. Test: `insert_refused_leaves_nothing` in Task 8.
5. **Stop after a play session that set references and created assets.** References set during play revert, and assets created during play disappear from the pane. Test: GS10 in Task 5 (engine); the pane rebuilds on `tree_revision`, covered by `pane_follows_tree` in Task 11.

---

## File Structure

Engine (`engine_core`, `engine_services`, `engine_instances`):

- Create `src/engine_core/Containment.hpp`, `Containment.cpp`: the service table, `service_guid`, asset homes, `passes_rule_up`, `placement_error`. No instances, only class names.
- Create `src/engine_core/InstanceRef.hpp`, `InstanceRef.cpp`: a GUID with a resolve cache.
- Modify `src/engine_core/DataModel.hpp`, `DataModel.cpp`: `is_service`, `hidden_in_explorer`, `service()`, the service and placement checks in `parent_error`, `rename_error`, `destroy_error`, `destroy`.
- Modify `src/engine_core/PropertyReflection.hpp`, `.cpp`: `reference_class`, reference JSON, `same_slot` for references.
- Modify `src/engine_core/ScriptBindings.cpp`: reference setters from Luau; `GetService` for any service directly under game.
- Modify `src/engine_core/Project.cpp`: every service in the class registry, `adopt_services`, `check_placement`, `clear_world`.
- Modify `src/engine_services/SceneService.hpp`, `.cpp`: `Service` base.
- Create `src/engine_services/GameService.hpp`, `.cpp`: `GameService`, `Assets`, and the five categories.
- Modify `src/engine_services/Game.hpp`, `.cpp`: build every service from the table.
- Create `src/engine_instances/AssetInstances.hpp`, `.cpp`: `FileAsset`, `Texture`, `Mesh`, `Sound`, `ReferenceAsset`, `Material`, `Model`, `Prefab`.
- Modify `CMakeLists.txt`: the new sources, the new sandbox file, and the new `assets-tests` program.

Studio (`ide`):

- Modify `src/ide/IdeExplorer.cpp`: skip hidden services.
- Modify `src/ide/InsertPopup.cpp`: leave out asset classes.
- Modify `src/ide/IdeLayout.cpp`: the insert host asks `parent_error`; the Assets window entry.
- Modify `src/ide/PropertySheet.cpp`: reference rows, the class check, read-only service rows.
- Modify `src/ide/PropertiesPanel.cpp`: a reference row takes a drop from the Assets pane.
- Modify `src/ide/IdeIcons.cpp`: icon names for the new classes.
- Modify `src/ide/Preferences.hpp`, `.cpp`: `assets_view`.
- Create `src/ide/AssetBrowser.hpp`, `.cpp` (in `studio_core`): navigation, rows, search, sort.
- Create `src/ide/IdeAssets.hpp`, `.cpp` (in `studio`): the pane.
- Modify `src/ide/IdeLayout.hpp`, `src/ide/IdeLayoutProject.cpp`: `make_assets`.

Tests:

- Create `sandbox/game_services_tests.cpp` (GS1–GS16).
- Modify `sandbox/scene_services_tests.cpp`, and any test that counts game's children: 4 becomes 5.
- Modify `tests/ExplorerRenameTest.cpp`, `tests/PropertiesTest.cpp`, `tests/McpTest.cpp`, `tests/PreferencesTest.cpp`.
- Create `tests/AssetBrowserTest.cpp` (in `engine-tests`) and `tests/AssetsPaneTest.cpp` (new `assets-tests`).

Docs:

- Modify `README.md`, `src/engine_core/README.md`, `src/engine_services/README.md`, `src/engine_instances/README.md`.

---

### Task 1: Containment rules by class name

**Files:**
- Create: `src/engine_core/Containment.hpp`
- Create: `src/engine_core/Containment.cpp`
- Create: `sandbox/game_services_tests.cpp`
- Modify: `CMakeLists.txt` (add `src/engine_core/Containment.cpp` to `engine_core`; add `sandbox/game_services_tests.cpp` to `sandbox`)

**Interfaces:**
- Produces:
  - `struct ServiceSpec { const char* class_name; const char* parent_class; };` (`parent_class` null means game)
  - `inline constexpr ServiceSpec kServices[10]`
  - `const ServiceSpec* find_service(std::string_view class_name);`
  - `std::string service_guid(std::string_view class_name);`
  - `const char* asset_home(std::string_view class_name);` (the class an asset must be under, or null)
  - `bool is_asset_class(std::string_view class_name);`
  - `bool passes_rule_up(std::string_view class_name);` (true for `Folder`)
  - `std::optional<std::string> placement_error(std::string_view holder_class, std::string_view child_class, std::string_view child_name);`

- [ ] **Step 1: Write the failing test**

Create `sandbox/game_services_tests.cpp`:

```cpp
// Game services: Assets and its five categories under game, the asset classes
// they hold, and references between assets.

#include "Containment.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace {

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

}  // namespace

TEST_CASE("GS1 the placement rules, by class name", "[GS1]") {
    using engine_core::placement_error;

    // The service table, in the order game and Assets hold them.
    REQUIRE(std::size(engine_core::kServices) == 10);
    REQUIRE(std::string(engine_core::kServices[4].class_name) == "Assets");
    REQUIRE(engine_core::kServices[4].parent_class == nullptr);
    REQUIRE(std::string(engine_core::kServices[5].class_name) == "Materials");
    REQUIRE(std::string(engine_core::kServices[5].parent_class) == "Assets");
    REQUIRE(engine_core::find_service("Textures") != nullptr);
    REQUIRE(engine_core::find_service("Texture") == nullptr);
    REQUIRE(engine_core::service_guid("Textures") == "textures");

    REQUIRE(std::string(engine_core::asset_home("Texture")) == "Textures");
    REQUIRE(std::string(engine_core::asset_home("Sound")) == "Audio");
    REQUIRE(std::string(engine_core::asset_home("Model")) == "Prefab");
    REQUIRE(engine_core::asset_home("Folder") == nullptr);
    REQUIRE(engine_core::passes_rule_up("Folder"));
    REQUIRE_FALSE(engine_core::passes_rule_up("Prefab"));

    // game takes only services.
    REQUIRE(reason(placement_error("Game", "Folder", "Box")) ==
            "Only scene services can be children of game; put Box in Workspace");
    REQUIRE_FALSE(placement_error("Game", "Assets", "Assets"));

    // Assets takes only its categories.
    REQUIRE(reason(placement_error("Assets", "Folder", "Box")) ==
            "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    REQUIRE(reason(placement_error("Assets", "Texture", "Brick")) ==
            "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");

    // A category takes its class and Folders.
    REQUIRE_FALSE(placement_error("Textures", "Texture", "Brick"));
    REQUIRE_FALSE(placement_error("Textures", "Folder", "Bricks"));
    REQUIRE(reason(placement_error("Textures", "Mesh", "Rock")) == "Textures holds Textures and Folders");
    REQUIRE(reason(placement_error("Meshes", "Script", "Main")) == "Meshes holds Meshes and Folders");
    REQUIRE(reason(placement_error("Audio", "Texture", "Brick")) == "Audio holds Sounds and Folders");
    REQUIRE_FALSE(placement_error("Audio", "Sound", "Boom"));
    REQUIRE_FALSE(placement_error("Prefabs", "Prefab", "Crate"));

    // A Prefab takes only Models, and a Model goes only in a Prefab.
    REQUIRE_FALSE(placement_error("Prefab", "Model", "Body"));
    REQUIRE(reason(placement_error("Prefab", "Folder", "Parts")) == "A Prefab holds only Models");
    REQUIRE(reason(placement_error("Workspace", "Model", "Body")) == "A Model must be in a Prefab");
    REQUIRE(reason(placement_error("Prefabs", "Model", "Body")) == "Prefabs holds Prefabs and Folders");

    // Anything else takes anything but an asset.
    REQUIRE_FALSE(placement_error("Workspace", "Folder", "Box"));
    REQUIRE_FALSE(placement_error("GameObject", "Script", "Main"));
    REQUIRE(reason(placement_error("Workspace", "Texture", "Brick")) == "A Texture must be in Assets.Textures");
    REQUIRE(reason(placement_error("Storage", "Material", "Brick")) == "A Material must be in Assets.Materials");
    REQUIRE(reason(placement_error("Model", "Sound", "Boom")) == "A Sound must be in Assets.Audio");
}
```

Add `sandbox/game_services_tests.cpp` to the `add_executable(sandbox ...)` list in `CMakeLists.txt`, after `sandbox/scene_services_tests.cpp`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release --target sandbox --parallel`
Expected: compile error, `Containment.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

Create `src/engine_core/Containment.hpp`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace engine_core {

// What may go where in a place, by class name, so a project read can check its
// plan before it makes any instance. DataModel::parent_error asks the same rules.

// A service: a child of game, or of another service, that is made with the
// world and cannot be moved, renamed, or destroyed. parent_class is null for a
// child of game.
struct ServiceSpec {
    const char* class_name;
    const char* parent_class;
};

// Every service, parents first, in the order their parents hold them.
inline constexpr ServiceSpec kServices[] = {
    {"Workspace", nullptr}, {"Lighting", nullptr},  {"Storage", nullptr},  {"Scripts", nullptr},
    {"Assets", nullptr},    {"Materials", "Assets"}, {"Prefabs", "Assets"}, {"Meshes", "Assets"},
    {"Textures", "Assets"}, {"Audio", "Assets"},
};

// Null for a class that is not a service.
const ServiceSpec* find_service(std::string_view class_name);
// A service's GUID is its class name in lowercase, the same in every place, so
// a place whose files lack one gets the same service on every read.
std::string service_guid(std::string_view class_name);

// The class an asset must be under: its category, or Prefab for a Model. Null
// for a class that is not an asset.
const char* asset_home(std::string_view class_name);
bool is_asset_class(std::string_view class_name);

// A Folder has no rule of its own: what goes in it is decided by the first
// ancestor that is not a Folder.
bool passes_rule_up(std::string_view class_name);

// Why holder_class, as the instance whose rule decides, refuses a child of
// child_class named child_name. Empty when it takes it. "Game" is the root.
std::optional<std::string> placement_error(std::string_view holder_class, std::string_view child_class,
                                           std::string_view child_name);

}  // namespace engine_core
```

Create `src/engine_core/Containment.cpp`:

```cpp
#include "Containment.hpp"

#include <cctype>

namespace engine_core {
namespace {

// A category, the asset class it holds, and that class's plural for messages.
struct Category {
    const char* service;
    const char* asset;
    const char* plural;
};

constexpr Category kCategories[] = {
    {"Materials", "Material", "Materials"}, {"Prefabs", "Prefab", "Prefabs"}, {"Meshes", "Mesh", "Meshes"},
    {"Textures", "Texture", "Textures"},    {"Audio", "Sound", "Sounds"},
};

const Category* category_named(std::string_view service) {
    for (const Category& category : kCategories) {
        if (service == category.service) {
            return &category;
        }
    }
    return nullptr;
}

// "Assets.Textures" for a service under Assets; the class itself otherwise.
std::string service_path(std::string_view class_name) {
    const ServiceSpec* spec = find_service(class_name);
    if (spec == nullptr || spec->parent_class == nullptr) {
        return std::string(class_name);
    }
    return std::string(spec->parent_class) + "." + std::string(class_name);
}

}  // namespace

const ServiceSpec* find_service(std::string_view class_name) {
    for (const ServiceSpec& spec : kServices) {
        if (class_name == spec.class_name) {
            return &spec;
        }
    }
    return nullptr;
}

std::string service_guid(std::string_view class_name) {
    std::string guid(class_name);
    for (char& c : guid) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return guid;
}

const char* asset_home(std::string_view class_name) {
    if (class_name == "Model") {
        return "Prefab";
    }
    for (const Category& category : kCategories) {
        if (class_name == category.asset) {
            return category.service;
        }
    }
    return nullptr;
}

bool is_asset_class(std::string_view class_name) { return asset_home(class_name) != nullptr; }

bool passes_rule_up(std::string_view class_name) { return class_name == "Folder"; }

std::optional<std::string> placement_error(std::string_view holder_class, std::string_view child_class,
                                           std::string_view child_name) {
    if (holder_class == "Game") {
        if (find_service(child_class) != nullptr) {
            return std::nullopt;
        }
        return "Only scene services can be children of game; put " + std::string(child_name) + " in Workspace";
    }
    if (holder_class == "Assets") {
        const ServiceSpec* spec = find_service(child_class);
        if (spec != nullptr && spec->parent_class != nullptr && std::string_view(spec->parent_class) == "Assets") {
            return std::nullopt;
        }
        return std::string("Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    }
    if (const Category* category = category_named(holder_class)) {
        if (child_class == category->asset || child_class == "Folder") {
            return std::nullopt;
        }
        return std::string(category->service) + " holds " + category->plural + " and Folders";
    }
    if (holder_class == "Prefab") {
        if (child_class == "Model") {
            return std::nullopt;
        }
        return std::string("A Prefab holds only Models");
    }
    if (const char* home = asset_home(child_class)) {
        const std::string where = find_service(home) != nullptr ? service_path(home) : std::string("a ") + home;
        return "A " + std::string(child_class) + " must be in " + where;
    }
    return std::nullopt;
}

}  // namespace engine_core
```

Add `src/engine_core/Containment.cpp` to the `add_library(engine_core STATIC ...)` list in `CMakeLists.txt`, after `src/engine_core/Project.cpp`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "[GS1]"`
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/engine_core/Containment.hpp src/engine_core/Containment.cpp sandbox/game_services_tests.cpp
git commit -m "Add the placement rules for services and assets, by class name

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The Service base, GameService, and the Assets tree under game

**Files:**
- Modify: `src/engine_core/DataModel.hpp:136-149` (service queries)
- Modify: `src/engine_core/DataModel.cpp` (around `scene_service`, `parent_error`, `rename_error`, `destroy_error`, `destroy` at the `is_scene_service` check near line 479)
- Modify: `src/engine_services/SceneService.hpp`, `src/engine_services/SceneService.cpp`
- Create: `src/engine_services/GameService.hpp`, `src/engine_services/GameService.cpp`
- Modify: `src/engine_services/Game.hpp`, `src/engine_services/Game.cpp`
- Modify: `CMakeLists.txt` (add `src/engine_services/GameService.cpp` to `engine_services`)
- Modify: `sandbox/scene_services_tests.cpp` and every test that expects 4 children of game
- Test: `sandbox/game_services_tests.cpp`

**Interfaces:**
- Consumes: `kServices`, `find_service`, `service_guid` (Task 1).
- Produces:
  - `virtual bool DataModel::is_service() const` (false by default)
  - `virtual bool DataModel::hidden_in_explorer() const` (false by default)
  - `InstanceId DataModel::service(std::string_view class_name) const` (0 when none)
  - `class Service : public DataModel`; `class SceneService : public Service`; `class GameService : public Service`
  - Classes `Assets`, `Materials`, `Prefabs`, `Meshes`, `Textures`, `Audio` (all `GameService`), registered for Luau under `GameService`, which is under `DataModel`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/game_services_tests.cpp` (add includes `DataModel.hpp`, `Folder.hpp`, `Game.hpp`, `ChangeHistoryService.hpp`, `Contract.hpp`, `GameService.hpp`, `<vector>` at the top):

```cpp
using engine_core::ContractViolation;
using engine_core::DataModel;
using engine_core::Folder;
using engine_core::Game;
using engine_core::InstanceId;

namespace {

std::vector<std::string> child_classes(const DataModel& game, InstanceId parent) {
    std::vector<std::string> out;
    for (InstanceId child : game.get_children(parent)) {
        out.push_back(game.instance(child)->class_name());
    }
    return out;
}

}  // namespace

TEST_CASE("GS2 a new Game holds Assets and its five categories, hidden from the explorer", "[GS2]") {
    Game game;
    REQUIRE(child_classes(game, 0) ==
            std::vector<std::string>{"Workspace", "Lighting", "Storage", "Scripts", "Assets"});
    const InstanceId assets = game.service("Assets");
    REQUIRE(assets != 0);
    REQUIRE(game.parent(assets) == 0);
    REQUIRE(child_classes(game, assets) ==
            std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"});
    for (const engine_core::ServiceSpec& spec : engine_core::kServices) {
        INFO(spec.class_name);
        const InstanceId id = game.service(spec.class_name);
        REQUIRE(id != 0);
        const DataModel* service = game.instance(id);
        REQUIRE(service->is_service());
        REQUIRE(game.name(id) == spec.class_name);
        REQUIRE(game.guid(id) == engine_core::service_guid(spec.class_name));
        const bool game_service = std::string(spec.class_name) == "Assets" || spec.parent_class != nullptr;
        REQUIRE(service->hidden_in_explorer() == game_service);
        REQUIRE(service->is_scene_service() == !game_service);
    }
    // scene_service still finds only the four scene services.
    REQUIRE(game.scene_service("Workspace") == game.service("Workspace"));
    REQUIRE(game.scene_service("Assets") == 0);
    REQUIRE(game.service("Folder") == 0);
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("GS3 a game service cannot be moved, renamed, or destroyed", "[GS3]") {
    Game game;
    const InstanceId assets = game.service("Assets");
    const InstanceId textures = game.service("Textures");
    const InstanceId meshes = game.service("Meshes");
    const InstanceId workspace = game.service("Workspace");

    REQUIRE(reason(game.parent_error(assets, workspace)) == "Assets cannot be moved");
    REQUIRE(reason(game.parent_error(textures, meshes)) == "Textures cannot be moved");
    REQUIRE(reason(game.parent_error(textures, 0)) == "Textures cannot be moved");
    REQUIRE(reason(game.parent_error(textures, DataModel::kNoParent)) == "Textures cannot be moved");
    REQUIRE_FALSE(game.parent_error(textures, assets));
    REQUIRE(reason(game.rename_error(textures, "Images")) == "Textures cannot be renamed");
    REQUIRE_FALSE(game.rename_error(textures, "Textures"));
    REQUIRE(reason(game.destroy_error(assets)) == "Assets cannot be destroyed");
    REQUIRE(reason(game.destroy_error(textures)) == "Textures cannot be destroyed");

    REQUIRE_THROWS_AS(game.set_parent(textures, workspace), ContractViolation);
    REQUIRE_THROWS_AS(game.set_name(assets, "Stuff"), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy(textures), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy_tree(assets), ContractViolation);
    REQUIRE(game.parent(textures) == assets);

    // Undo and Stop keep them, as they keep the scene services.
    SimRole role;
    game.start_simulation();
    game.stop_simulation();
    REQUIRE(child_classes(game, assets).size() == 5);
}
```

Also change every expectation that game has 4 children to 5, including the service list. Find them with:

Run: `grep -rn "size() == 4\|\"Scripts\"})\|kSceneServiceClasses" sandbox tests`

In `sandbox/scene_services_tests.cpp`:
- SS1: `REQUIRE(children.size() == 5);` and loop over the first 4 only (`for (std::size_t index = 0; index < 4; ++index)`); add `REQUIRE(std::string(game.instance(children[4])->class_name()) == "Assets");`.
- SS2: `REQUIRE(game.get_children(0).size() == 5);`.
- SS6: `REQUIRE(rig.game.get_children(0).size() == 5);`.
- SS10: `REQUIRE(child_names(game, 0) == std::vector<std::string>{"Workspace", "Lighting", "Storage", "Scripts", "Assets"});`.
- SS12: `REQUIRE(game.get_children(0).size() == 5);`.

Update any other hit from the grep the same way.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --config Release --target sandbox --parallel`
Expected: compile errors: `GameService.hpp` not found, no member `service`, `is_service`, `hidden_in_explorer`.

- [ ] **Step 3: Implement the DataModel queries**

In `src/engine_core/DataModel.hpp`, replace the block at lines 136-141 with:

```cpp
    // A service (engine_services): made with the world under game or under
    // another service, as Containment's kServices lists them. None can be
    // moved, renamed, or destroyed.
    virtual bool is_service() const { return false; }
    // Workspace, Lighting, Storage, and Scripts: the services scripts run and
    // render under. A Game makes one of each as its first children.
    virtual bool is_scene_service() const { return false; }
    // A game service, and so everything under it, has no row in the Game Explorer.
    virtual bool hidden_in_explorer() const { return false; }
    // The root's child of this scene service class, or 0 when there is none.
    InstanceId scene_service(std::string_view class_name) const;
    // The service of this class, under game or under a service directly under
    // game, or 0 when there is none.
    InstanceId service(std::string_view class_name) const;
```

Also add to the private section of `DataModel` (next to other private helpers):

```cpp
    // The class whose rule decides what goes in parent: its own, or for a
    // Folder, that of the first ancestor that is not a Folder, walking as
    // though moved were already under moved_to. Empty when the walk leaves the tree.
    std::string rule_class(InstanceId parent, InstanceId moved, InstanceId moved_to) const;
    // The first placement rule that id and its descendants would break under new_parent.
    std::optional<std::string> placement_error_for(InstanceId id, InstanceId new_parent) const;
```

In `src/engine_core/DataModel.cpp`, add `#include "Containment.hpp"` with the other includes. Replace `scene_service`, `parent_error`, `rename_error`, and `destroy_error` (lines ~1131-1197) with:

```cpp
InstanceId DataModel::scene_service(std::string_view class_name) const {
    for (InstanceId child = first_child(0); child != 0; child = next_sibling(child)) {
        const DataModel* object = instance(child);
        if (object != nullptr && object->is_scene_service() && class_name == object->class_name()) {
            return child;
        }
    }
    return 0;
}

InstanceId DataModel::service(std::string_view class_name) const {
    // kServices nests one level: a service's parent is game or a child of game.
    for (InstanceId child = first_child(0); child != 0; child = next_sibling(child)) {
        const DataModel* object = instance(child);
        if (object == nullptr || !object->is_service()) {
            continue;
        }
        if (class_name == object->class_name()) {
            return child;
        }
        if (object->is_scene_service()) {
            continue;
        }
        for (InstanceId inner = first_child(child); inner != 0; inner = next_sibling(inner)) {
            const DataModel* nested = instance(inner);
            if (nested != nullptr && nested->is_service() && class_name == nested->class_name()) {
                return inner;
            }
        }
    }
    return 0;
}

std::string DataModel::rule_class(InstanceId parent, InstanceId moved, InstanceId moved_to) const {
    InstanceId at = parent;
    for (std::size_t guard = 0; guard <= kMaxInstances + 1; ++guard) {
        if (at == kNoParent) {
            return {};
        }
        const DataModel* holder = at == 0 ? state_->root : instance(at);
        if (holder == nullptr) {
            return {};
        }
        if (!passes_rule_up(holder->class_name())) {
            return holder->class_name();
        }
        at = at == moved ? moved_to : this->parent(at);
    }
    return {};
}

std::optional<std::string> DataModel::placement_error_for(InstanceId id, InstanceId new_parent) const {
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId at = pending.back();
        pending.pop_back();
        const DataModel* object = instance(at);
        if (object == nullptr) {
            continue;
        }
        const std::string holder = rule_class(at == id ? new_parent : parent(at), id, new_parent);
        if (!holder.empty()) {
            if (std::optional<std::string> error = placement_error(holder, object->class_name(), name(at))) {
                return error;
            }
        }
        for (InstanceId child = first_child(at); child != 0; child = next_sibling(child)) {
            pending.push_back(child);
        }
    }
    return std::nullopt;
}

std::optional<std::string> DataModel::parent_error(InstanceId id, InstanceId new_parent) const {
    if (id == 0) {
        return std::string("game cannot be moved");
    }
    const DataModel* object = instance(id);
    if (object == nullptr || (new_parent != 0 && new_parent != kNoParent && !alive(new_parent))) {
        return std::string("That instance no longer exists");
    }
    const InstanceId current = parent(id);
    if (current == new_parent) {
        return std::nullopt;
    }
    if (object->is_service()) {
        // Game places each one under its table parent once, when it makes the
        // world, and a project read does the same for one its files lack.
        const ServiceSpec* spec = find_service(object->class_name());
        InstanceId home = kNoParent;
        if (spec != nullptr) {
            home = spec->parent_class == nullptr ? 0 : service(spec->parent_class);
            if (spec->parent_class != nullptr && home == 0) {
                home = kNoParent;
            }
        }
        const bool placing = current == kNoParent && home != kNoParent && new_parent == home &&
                             service(object->class_name()) == 0;
        if (!placing) {
            return name(id) + " cannot be moved";
        }
        return std::nullopt;
    }
    if (new_parent == kNoParent) {
        return std::nullopt;
    }
    if (new_parent == id || is_under(id, new_parent)) {
        return "Cannot parent " + name(id) + " to itself or a descendant";
    }
    return placement_error_for(id, new_parent);
}

std::optional<std::string> DataModel::rename_error(InstanceId id, std::string_view new_name) const {
    if (id == 0) {
        return std::nullopt;
    }
    const DataModel* object = instance(id);
    if (object == nullptr) {
        return std::string("That instance no longer exists");
    }
    if (object->is_service() && object->name_ != new_name) {
        return object->name_ + " cannot be renamed";
    }
    return std::nullopt;
}

std::optional<std::string> DataModel::destroy_error(InstanceId id) const {
    if (id == 0) {
        return std::string("game cannot be destroyed");
    }
    const DataModel* object = instance(id);
    if (object == nullptr) {
        return std::string("That instance no longer exists");
    }
    if (object->is_service()) {
        return object->name_ + " cannot be destroyed";
    }
    return std::nullopt;
}
```

`is_under(id, new_parent)` is the existing helper used by the old code; keep using it. In `DataModel::destroy` (near line 479), change `part->instance->is_scene_service()` to `part->instance->is_service()`. Search the file for any other `is_scene_service()` guard on move, rename, or destroy and switch it to `is_service()`:

Run: `grep -n "is_scene_service" src/engine_core/DataModel.cpp src/engine_core/DataModelHistory.cpp src/engine_core/DataModelPlace.cpp`

Only `scene_service()` itself should keep `is_scene_service()`.

- [ ] **Step 4: Implement Service, GameService, and the categories**

Replace the class section of `src/engine_services/SceneService.hpp` with:

```cpp
// A service: made with the world, under game or under another service, as
// Containment's kServices lists it. It lives as long as the world: it cannot
// be moved, renamed, or destroyed, and a script cannot make one.
class Service : public DataModel {
public:
    Service(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    bool is_service() const override { return true; }
    // Paste only: a service takes children, and cannot be cut, renamed, or deleted.
    void context_actions(std::vector<ContextAction>& out) const override;
};

// A child of game that scripts and the explorer see: Workspace, Lighting,
// Storage, or Scripts. A DataModel that is not an Instance, like Game.
class SceneService : public Service {
public:
    using Service::Service;

    bool is_scene_service() const override { return true; }
};
```

Keep `kSceneServiceClasses`, `is_scene_service_class`, and `scene_service_guid` in that header; make `scene_service_guid` return `service_guid(class_name)` (include `Containment.hpp` in `SceneService.cpp`). In `SceneService.cpp`, rename `SceneService::context_actions` to `Service::context_actions`, and register the base for Luau:

```cpp
ANARCHY_LUA_REGISTER(register_scene_service_lua) {
    register_lua_class("Service", "DataModel", nullptr, 0);
    register_lua_class("SceneService", "Service", nullptr, 0);
    register_lua_class("Workspace", "SceneService", nullptr, 0);
    register_lua_class("Storage", "SceneService", nullptr, 0);
    register_lua_class("Scripts", "SceneService", nullptr, 0);
    // So completion offers them to GetService.
    for (const char* name : kSceneServiceClasses) {
        register_lua_service(name);
    }
}
```

Create `src/engine_services/GameService.hpp`:

```cpp
#pragma once

#include "SceneService.hpp"

namespace engine_core {

// A service the Game Explorer does not show: Assets and its five categories.
// The Assets pane is where they are browsed. Scripts reach them as any child,
// as game.Assets.Textures.
class GameService : public Service {
public:
    using Service::Service;

    bool hidden_in_explorer() const override { return true; }
};

// Holds the five asset categories and nothing else.
class Assets : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

// Each category holds its own asset class and Folders, at any depth.
class Materials : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Prefabs : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Meshes : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Textures : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Audio : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

}  // namespace engine_core
```

Create `src/engine_services/GameService.cpp`:

```cpp
#include "GameService.hpp"

#include "LuaApi.hpp"

namespace engine_core {

const char* Assets::class_name() const { return "Assets"; }
const char* Materials::class_name() const { return "Materials"; }
const char* Prefabs::class_name() const { return "Prefabs"; }
const char* Meshes::class_name() const { return "Meshes"; }
const char* Textures::class_name() const { return "Textures"; }
const char* Audio::class_name() const { return "Audio"; }

namespace {

ANARCHY_LUA_REGISTER(register_game_service_lua) {
    register_lua_class("GameService", "Service", nullptr, 0);
    for (const char* name : {"Assets", "Materials", "Prefabs", "Meshes", "Textures", "Audio"}) {
        register_lua_class(name, "GameService", nullptr, 0);
    }
    // GetService finds a service directly under game.
    register_lua_service("Assets");
}

}  // namespace

}  // namespace engine_core
```

`register_lua_class` keeps the `const char*` it is given; the string literals in that initializer list are static, so that is safe. Add `src/engine_services/GameService.cpp` to `add_library(engine_services ...)`.

- [ ] **Step 5: Build every service in Game**

Replace the anonymous-namespace helper and constructor in `src/engine_services/Game.cpp` with:

```cpp
#include "Game.hpp"

#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "GameService.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "SceneService.hpp"

namespace engine_core {
namespace {

const char* const kClassName = "Game";

// Under its table parent, with its fixed GUID.
template <typename T>
void add_service(Game& game) {
    T& service = game.create<T>();
    const ServiceSpec* spec = find_service(service.class_name());
    if (spec == nullptr) {
        contract_fail("a service class is missing from kServices");
    }
    game.set_guid(service.id(), service_guid(service.class_name()));
    game.set_parent(service.id(), spec->parent_class == nullptr ? 0 : game.service(spec->parent_class));
}

}  // namespace

Game::Game() : DataModel(kClassName) {
    // A new place starts with them; there is nothing to undo.
    ChangeHistoryService& changes = history();
    const bool enabled = changes.enabled();
    changes.set_enabled(false);
    // kServices order, parents first.
    add_service<Workspace>(*this);
    add_service<Lighting>(*this);
    add_service<Storage>(*this);
    add_service<Scripts>(*this);
    add_service<Assets>(*this);
    add_service<Materials>(*this);
    add_service<Prefabs>(*this);
    add_service<Meshes>(*this);
    add_service<Textures>(*this);
    add_service<Audio>(*this);
    changes.set_enabled(enabled);
}
```

(Keep `class_name()` and the Luau registration below it unchanged. Add `#include "Contract.hpp"` if `contract_fail` is not already visible.) Update the comment in `Game.hpp` to say game holds the four scene services and Assets.

- [ ] **Step 6: Keep project loads from destroying the new services**

`Project.cpp`'s `clear_world` destroys every instance that is not a scene service, which would now try to destroy Assets and fail the contract. Change its `!object.is_scene_service()` to `!object.is_service()`. Project loads still do not know the Assets classes; Task 7 teaches them. Until then, leave the `[project]` tests out of each run.

- [ ] **Step 7: Run the tests**

Run: `cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "~[project]"`
Expected: all pass, including SS1–SS9, SS13, SS15 with their updated counts, and GS1–GS3. The SS10 change above is for Task 7; the `[project]` cases are expected to fail until then.

- [ ] **Step 8: Commit**

```bash
git add -A src/engine_core src/engine_services sandbox CMakeLists.txt
git commit -m "Add GameService and the Assets tree under game

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Asset classes, and placement checked on every move

**Files:**
- Create: `src/engine_instances/AssetInstances.hpp`, `src/engine_instances/AssetInstances.cpp`
- Modify: `src/engine_core/ScriptBindings.cpp:141-159` (register the creatable classes)
- Modify: `CMakeLists.txt` (add `src/engine_instances/AssetInstances.cpp` to `engine_instances`)
- Test: `sandbox/game_services_tests.cpp`

**Interfaces:**
- Consumes: `DataModel::parent_error` with placement (Task 2).
- Produces: classes `Texture`, `Mesh`, `Sound` (each a `FileAsset`), `Material`, `Model` (each a `ReferenceAsset`), `Prefab`, all `Instance`s creatable by `Instance.new` and `lua_create_instance`. This task gives them no properties; Tasks 4 and 5 add `Path` and the references to these same classes.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/game_services_tests.cpp` (add `#include "AssetInstances.hpp"` and `#include "LuaApi.hpp"`):

```cpp
namespace {

InstanceId make(DataModel& game, const char* klass, const char* name, InstanceId parent) {
    DataModel* object = engine_core::lua_create_instance(game, klass);
    REQUIRE(object != nullptr);
    game.set_name(object->id(), name);
    if (parent != DataModel::kNoParent) {
        game.set_parent(object->id(), parent);
    }
    return object->id();
}

}  // namespace

TEST_CASE("GS4 each category takes its class and Folders", "[GS4]") {
    Game game;
    const InstanceId textures = game.service("Textures");
    const InstanceId brick = make(game, "Texture", "Brick", DataModel::kNoParent);
    const InstanceId rock = make(game, "Mesh", "Rock", DataModel::kNoParent);
    const InstanceId folder = make(game, "Folder", "Walls", textures);

    REQUIRE_FALSE(game.parent_error(brick, textures));
    REQUIRE_FALSE(game.parent_error(brick, folder));
    REQUIRE(reason(game.parent_error(rock, textures)) == "Textures holds Textures and Folders");
    REQUIRE(reason(game.parent_error(rock, folder)) == "Textures holds Textures and Folders");
    REQUIRE(reason(game.parent_error(brick, game.service("Workspace"))) == "A Texture must be in Assets.Textures");
    REQUIRE(reason(game.parent_error(brick, game.service("Assets"))) ==
            "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    REQUIRE(reason(game.parent_error(folder, game.service("Assets"))) ==
            "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    game.set_parent(brick, folder);
    REQUIRE(game.parent(brick) == folder);

    // Prefab and Model.
    const InstanceId crate = make(game, "Prefab", "Crate", game.service("Prefabs"));
    const InstanceId body = make(game, "Model", "Body", crate);
    REQUIRE(game.parent(body) == crate);
    REQUIRE(reason(game.parent_error(folder, crate)) == "A Prefab holds only Models");
    REQUIRE(reason(game.parent_error(body, game.service("Workspace"))) == "A Model must be in a Prefab");
    REQUIRE(reason(game.parent_error(body, game.service("Prefabs"))) == "Prefabs holds Prefabs and Folders");
    // Out of the tree is always allowed.
    REQUIRE_FALSE(game.parent_error(body, DataModel::kNoParent));
}

TEST_CASE("GS5 a folder carries its assets' rules with it", "[GS5]") {
    Game game;
    const InstanceId walls = make(game, "Folder", "Walls", game.service("Textures"));
    const InstanceId inner = make(game, "Folder", "Inner", walls);
    make(game, "Texture", "Brick", inner);

    REQUIRE(reason(game.parent_error(walls, game.service("Workspace"))) == "A Texture must be in Assets.Textures");
    REQUIRE(reason(game.parent_error(walls, game.service("Meshes"))) == "Meshes holds Meshes and Folders");
    REQUIRE_THROWS_AS(game.set_parent(walls, game.service("Meshes")), ContractViolation);
    REQUIRE(game.parent(walls) == game.service("Textures"));

    // A folder of plain instances moves between scene services as before.
    const InstanceId box = make(game, "Folder", "Box", game.service("Workspace"));
    make(game, "Script", "Main", box);
    REQUIRE_FALSE(game.parent_error(box, game.service("Storage")));
    REQUIRE(reason(game.parent_error(box, game.service("Textures"))) == "Textures holds Textures and Folders");

    // A folder out of the tree takes anything; putting it back is checked.
    game.set_parent(walls, DataModel::kNoParent);
    REQUIRE(reason(game.parent_error(walls, game.service("Workspace"))) == "A Texture must be in Assets.Textures");
    REQUIRE_FALSE(game.parent_error(walls, game.service("Textures")));
}

TEST_CASE("GS6 scripts see game services and meet the same rules", "[GS6]") {
    ScriptRig rig;
    add_script(rig.game, "Assets", R"(
        local function refuses(fn, expected)
            local ok, message = pcall(fn)
            return not ok and string.find(message, expected, 1, true) ~= nil
        end
        _G.path = game.Assets.Textures.ClassName == "Textures" and game:GetService("Assets") == game.Assets
        _G.isa = game.Assets:IsA("GameService") and game.Assets:IsA("Service") and not game.Assets:IsA("Instance")
            and workspace:IsA("Service")
        _G.listed = #game:GetChildren() == 5
        _G.no_move = refuses(function() game.Assets.Textures.Parent = workspace end, "Textures cannot be moved")
        _G.no_rename = refuses(function() game.Assets.Name = "Stuff" end, "Assets cannot be renamed")
        _G.no_destroy = refuses(function() game.Assets.Audio:Destroy() end, "Audio cannot be destroyed")
        _G.no_new = refuses(function() Instance.new("Textures") end, "unknown class Textures")
        _G.no_nested = not pcall(function() return game:GetService("Textures") end)

        local brick = Instance.new("Texture")
        brick.Parent = game.Assets.Textures
        _G.placed = brick.Parent == game.Assets.Textures
        _G.no_workspace = refuses(function() brick.Parent = workspace end, "A Texture must be in Assets.Textures")
        local model = Instance.new("Model")
        _G.no_model = refuses(function() model.Parent = workspace end, "A Model must be in a Prefab")
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"path", "isa", "listed", "no_move", "no_rename", "no_destroy", "no_new", "no_nested",
                             "placed", "no_workspace", "no_model"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --config Release --target sandbox --parallel`
Expected: compile error, `AssetInstances.hpp` not found.

- [ ] **Step 3: Implement the classes**

Create `src/engine_instances/AssetInstances.hpp`:

```cpp
#pragma once

#include "DataModel.hpp"

namespace engine_core {

// Assets, kept under game.Assets. Each lives only under its own category, as
// Containment's rules say: a Texture under Textures, a Model only in a Prefab.
// class_name is defined in AssetInstances.cpp so that file, and its Lua
// registration, stays linked.

// An asset that names a file under the project's resources folder.
class FileAsset : public DataModel {
public:
    FileAsset(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
};

class Texture : public FileAsset {
public:
    using FileAsset::FileAsset;
    const char* class_name() const override;
};

class Mesh : public FileAsset {
public:
    using FileAsset::FileAsset;
    const char* class_name() const override;
};

class Sound : public FileAsset {
public:
    using FileAsset::FileAsset;
    const char* class_name() const override;
};

// An asset whose properties point at other assets.
class ReferenceAsset : public DataModel {
public:
    ReferenceAsset(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
};

// A PBR material.
class Material : public ReferenceAsset {
public:
    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;
};

// Joins a Mesh and a Material. Lives only in a Prefab.
class Model : public ReferenceAsset {
public:
    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;
};

// A template made of Models, its only children.
class Prefab : public DataModel {
public:
    Prefab(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override;
};

}  // namespace engine_core
```

Create `src/engine_instances/AssetInstances.cpp`:

```cpp
#include "AssetInstances.hpp"

#include "LuaApi.hpp"

namespace engine_core {

const char* Texture::class_name() const { return "Texture"; }
const char* Mesh::class_name() const { return "Mesh"; }
const char* Sound::class_name() const { return "Sound"; }
const char* Material::class_name() const { return "Material"; }
const char* Model::class_name() const { return "Model"; }
const char* Prefab::class_name() const { return "Prefab"; }

namespace {

ANARCHY_LUA_REGISTER(register_asset_instances_lua) {
    register_lua_class("FileAsset", "Instance", nullptr, 0);
    register_lua_class("Texture", "FileAsset", nullptr, 0);
    register_lua_class("Mesh", "FileAsset", nullptr, 0);
    register_lua_class("Sound", "FileAsset", nullptr, 0);
    register_lua_class("ReferenceAsset", "Instance", nullptr, 0);
    register_lua_class("Material", "ReferenceAsset", nullptr, 0);
    register_lua_class("Model", "ReferenceAsset", nullptr, 0);
    register_lua_class("Prefab", "Instance", nullptr, 0);
}

}  // namespace

}  // namespace engine_core
```

Add `src/engine_instances/AssetInstances.cpp` to `add_library(engine_instances ...)`.

In `src/engine_core/ScriptBindings.cpp`, add `#include "AssetInstances.hpp"` and extend the creatable block:

```cpp
DataModel& create_texture(DataModel& world) { return world.create<Texture>(); }
DataModel& create_mesh(DataModel& world) { return world.create<Mesh>(); }
DataModel& create_sound(DataModel& world) { return world.create<Sound>(); }
DataModel& create_material(DataModel& world) { return world.create<Material>(); }
DataModel& create_model(DataModel& world) { return world.create<Model>(); }
DataModel& create_prefab(DataModel& world) { return world.create<Prefab>(); }
```

and inside `register_creatable_instances`:

```cpp
    register_lua_creatable("Texture", create_texture);
    register_lua_creatable("Mesh", create_mesh);
    register_lua_creatable("Sound", create_sound);
    register_lua_creatable("Material", create_material);
    register_lua_creatable("Model", create_model);
    register_lua_creatable("Prefab", create_prefab);
```

- [ ] **Step 4: Make GetService take any service directly under game**

In `src/engine_core/ScriptBindings.cpp`, in `instance_service` (near line 523), replace the lookup:

```cpp
        // A service directly under game is in the tree: GetService gives the instance itself.
        const InstanceId found = name != nullptr ? runtime->game_->service(name) : 0;
        if (found != 0 && runtime->game_->parent(found) == 0) {
```

Keep the body of that `if` as it was (it pushes `scene` — rename that variable to `found`).

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "[GS4],[GS5],[GS6]"`
Expected: pass. Then `build/Release/sandbox.exe "~[project]"`; expected: pass.

- [ ] **Step 6: Commit**

```bash
git add -A src/engine_instances src/engine_core/ScriptBindings.cpp sandbox CMakeLists.txt
git commit -m "Add the asset classes and check placement on every move

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Path on Texture, Mesh, and Sound

**Files:**
- Modify: `src/engine_instances/AssetInstances.hpp`, `src/engine_instances/AssetInstances.cpp`
- Test: `sandbox/game_services_tests.cpp`

**Interfaces:**
- Produces:
  - `std::optional<std::string> resource_path_error(std::string_view path);`
  - `const std::string& FileAsset::path() const;`
  - `std::optional<std::string> FileAsset::set_path(std::string path);` (SimulationThread; refuses with `Path must be relative to the resources folder`)
  - Saved property `Path`, type `string`, default `""`, on `FileAsset`.

- [ ] **Step 1: Write the failing test**

Append:

```cpp
TEST_CASE("GS7 Path is relative to the resources folder, and saves and undoes", "[GS7]") {
    SimRole role;
    Game game;
    const InstanceId id = make(game, "Texture", "Brick", game.service("Textures"));
    auto& brick = *dynamic_cast<engine_core::Texture*>(game.instance(id));
    REQUIRE(brick.path().empty());

    for (const char* bad : {"/abs/brick.png", "C:/brick.png", "textures\\brick.png", "../brick.png",
                            "textures/../../brick.png"}) {
        INFO(bad);
        REQUIRE(reason(brick.set_path(bad)) == "Path must be relative to the resources folder");
    }
    REQUIRE(brick.path().empty());

    game.history().set_pending_gesture("Set Path");
    REQUIRE_FALSE(brick.set_path("textures/brick.png"));
    game.history().end_gesture();
    REQUIRE(brick.path() == "textures/brick.png");
    engine_core::PropertyBag saved;
    brick.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Path") != nullptr);
    REQUIRE(engine_core::bag_find(saved, "Path")->as_string() == "textures/brick.png");

    game.history().undo();
    REQUIRE(brick.path().empty());
    game.history().redo();
    REQUIRE(brick.path() == "textures/brick.png");

    // A default Path saves nothing.
    const InstanceId other = make(game, "Sound", "Boom", game.service("Audio"));
    engine_core::PropertyBag none;
    game.instance(other)->save_properties(none);
    REQUIRE(none.empty());

    // Stop puts it back.
    game.start_simulation();
    REQUIRE_FALSE(brick.set_path("textures/other.png"));
    game.stop_simulation();
    REQUIRE(brick.path() == "textures/brick.png");
}
```

(`bag_find` is declared in `PropertyBag.hpp`; include it if the file does not already.)

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Release --target sandbox --parallel`
Expected: compile error, no member `path` / `set_path`.

- [ ] **Step 3: Implement**

In `AssetInstances.hpp`, add `#include <optional>`, `<string>`, `<string_view>`, and replace `FileAsset` with:

```cpp
// Why a Path is refused, or empty. A Path is relative to the resources folder,
// with '/' between names: not absolute, no drive letter, no '\', and no "..".
std::optional<std::string> resource_path_error(std::string_view path);

// An asset that names a file under the project's resources folder, as Path.
// Nothing checks that the file exists; nothing loads resources yet.
class FileAsset : public DataModel {
public:
    FileAsset(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const std::string& path() const { return path_; }
    // SimulationThread. Returns why a path is refused, changing nothing.
    std::optional<std::string> set_path(std::string path);

protected:
    void on_reuse() override { path_.clear(); }

private:
    std::string path_;
};
```

In `AssetInstances.cpp`, add `#include "Contract.hpp"`, `<string>`, and:

```cpp
std::optional<std::string> resource_path_error(std::string_view path) {
    const std::string refused = "Path must be relative to the resources folder";
    if (path.empty()) {
        return std::nullopt;
    }
    if (path.front() == '/' || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) {
        return refused;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if (path.substr(start, end - start) == "..") {
            return refused;
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> FileAsset::set_path(std::string path) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (std::optional<std::string> error = resource_path_error(path)) {
        return error;
    }
    if (path == path_) {
        return std::nullopt;
    }
    LuaSlot before;
    before.kind = LuaSlot::Kind::String;
    before.text = path_;
    path_ = std::move(path);
    LuaSlot after;
    after.kind = LuaSlot::Kind::String;
    after.text = path_;
    note_property_change("Path", before, after);
    return std::nullopt;
}
```

(`#include <algorithm>` for `std::min`.) In the anonymous namespace, add the Luau accessors and register `Path` on `FileAsset`:

```cpp
bool read_path(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* asset = dynamic_cast<const FileAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = asset->path();
    return true;
}

bool write_path(DataModel&, DataModel& object, LuaSlot& in) {
    auto* asset = dynamic_cast<FileAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = asset->set_path(in.text)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}
```

and in `register_asset_instances_lua`, replace the `FileAsset` line with:

```cpp
    const LuaField file_fields[] = {
        lua_saved_property("Path", "string", read_path, write_path, "\"\""),
    };
    register_lua_class("FileAsset", "Instance", file_fields, 1);
```

- [ ] **Step 4: Run the test**

Run: `cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "[GS7],[SS13]"`
Expected: pass. (SS13 checks that every class with saved properties saves nothing at its defaults.)

- [ ] **Step 5: Commit**

```bash
git add src/engine_instances/AssetInstances.hpp src/engine_instances/AssetInstances.cpp sandbox/game_services_tests.cpp
git commit -m "Give Texture, Mesh, and Sound a Path under the resources folder

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Reference properties held by GUID; Material's textures and Model's Mesh and Material

**Files:**
- Create: `src/engine_core/InstanceRef.hpp`, `src/engine_core/InstanceRef.cpp`
- Modify: `src/engine_core/PropertyReflection.hpp`, `src/engine_core/PropertyReflection.cpp`
- Modify: `src/engine_core/ScriptBindings.cpp:357` (the setter's type check)
- Modify: `src/engine_instances/AssetInstances.hpp`, `src/engine_instances/AssetInstances.cpp`
- Modify: `CMakeLists.txt` (add `src/engine_core/InstanceRef.cpp` to `engine_core`)
- Test: `sandbox/game_services_tests.cpp`

**Interfaces:**
- Produces:
  - `class InstanceRef { const std::string& guid() const; void set_guid(std::string); InstanceId resolve(const DataModel& world) const; };`
  - `std::string reference_class(std::string_view type);` (`"Texture?"` → `"Texture"`; `""` otherwise)
  - For a reference, a `LuaSlot` has the GUID in `text`, the live target in `id`, and `kind` `Instance` when resolved, else `Nil`.
  - `LuaSlot ReferenceAsset::reference(std::size_t index) const;`
  - `std::optional<std::string> ReferenceAsset::set_reference(std::size_t index, const LuaSlot& value);`
  - Saved properties: `Material.DiffuseTexture`, `NormalTexture`, `RoughnessTexture`, `MetalnessTexture` (`Texture?`); `Model.Mesh` (`Mesh?`), `Model.Material` (`Material?`); default `null`.

- [ ] **Step 1: Write the failing tests**

Append:

```cpp
namespace {

engine_core::LuaSlot read_field(DataModel& game, InstanceId id, const char* property) {
    DataModel* object = game.instance(id);
    const engine_core::LuaField* field = engine_core::lua_class_find(object->class_name(), property);
    REQUIRE(field != nullptr);
    engine_core::LuaSlot slot;
    REQUIRE(field->read(game, *object, slot));
    return slot;
}

bool write_field(DataModel& game, InstanceId id, const char* property, engine_core::LuaSlot slot,
                 std::string* error = nullptr) {
    DataModel* object = game.instance(id);
    const engine_core::LuaField* field = engine_core::lua_class_find(object->class_name(), property);
    REQUIRE(field != nullptr);
    const bool ok = field->write(game, *object, slot);
    if (error != nullptr) {
        *error = slot.error;
    }
    return ok;
}

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

}  // namespace

TEST_CASE("GS8 a reference takes its class, saves as a GUID, and reads the live target", "[GS8]") {
    SimRole role;
    Game game;
    const InstanceId brick = make(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId rock = make(game, "Mesh", "Rock", game.service("Meshes"));
    const InstanceId mat = make(game, "Material", "Wall", game.service("Materials"));

    REQUIRE(engine_core::reference_class("Texture?") == "Texture");
    REQUIRE(engine_core::reference_class("Texture").empty());
    REQUIRE(engine_core::reference_class("number").empty());

    // nil by default, and a default saves nothing.
    REQUIRE(read_field(game, mat, "DiffuseTexture").kind == engine_core::LuaSlot::Kind::Nil);
    engine_core::PropertyBag saved;
    game.instance(mat)->save_properties(saved);
    REQUIRE(saved.empty());

    std::string error;
    REQUIRE_FALSE(write_field(game, mat, "DiffuseTexture", instance_slot(rock), &error));
    REQUIRE(error == "DiffuseTexture must be a Texture");
    REQUIRE(write_field(game, mat, "DiffuseTexture", instance_slot(brick)));
    const engine_core::LuaSlot read = read_field(game, mat, "DiffuseTexture");
    REQUIRE(read.kind == engine_core::LuaSlot::Kind::Instance);
    REQUIRE(read.id == brick);
    REQUIRE(read.text == game.guid(brick));

    game.instance(mat)->save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "DiffuseTexture")->as_string() == game.guid(brick));

    // A load names the target by GUID, which need not exist yet.
    Game other;
    const InstanceId copy = make(other, "Material", "Wall", other.service("Materials"));
    std::string load_error;
    REQUIRE(other.instance(copy)->load_property("DiffuseTexture", engine_core::JsonValue::string("zzzz"),
                                                load_error));
    REQUIRE(load_error.empty());
    const engine_core::LuaSlot dangling = read_field(other, copy, "DiffuseTexture");
    REQUIRE(dangling.kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(dangling.text == "zzzz");
    // It saves back as it was read.
    engine_core::PropertyBag kept;
    other.instance(copy)->save_properties(kept);
    REQUIRE(engine_core::bag_find(kept, "DiffuseTexture")->as_string() == "zzzz");
    // Once an instance holds that GUID, the reference finds it.
    const InstanceId late = make(other, "Texture", "Late", other.service("Textures"));
    other.set_guid(late, "zzzz");
    REQUIRE(read_field(other, copy, "DiffuseTexture").id == late);

    // null clears.
    REQUIRE(other.instance(copy)->load_property("DiffuseTexture", engine_core::JsonValue(), load_error));
    REQUIRE(read_field(other, copy, "DiffuseTexture").text.empty());

    // Model's two references.
    const InstanceId crate = make(game, "Prefab", "Crate", game.service("Prefabs"));
    const InstanceId body = make(game, "Model", "Body", crate);
    REQUIRE(write_field(game, body, "Mesh", instance_slot(rock)));
    REQUIRE(write_field(game, body, "Material", instance_slot(mat)));
    REQUIRE_FALSE(write_field(game, body, "Material", instance_slot(brick), &error));
    REQUIRE(error == "Material must be a Material");
}

TEST_CASE("GS9 a reference to a destroyed asset reads nil, and undo brings it back", "[GS9]") {
    SimRole role;
    Game game;
    const InstanceId brick = make(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId mat = make(game, "Material", "Wall", game.service("Materials"));
    game.history().set_pending_gesture("Set DiffuseTexture");
    REQUIRE(write_field(game, mat, "DiffuseTexture", instance_slot(brick)));
    game.history().end_gesture();
    REQUIRE(game.history().can_undo().second == "Set DiffuseTexture");

    game.history().set_pending_gesture("Delete");
    game.destroy_tree(brick);
    game.history().end_gesture();
    REQUIRE(read_field(game, mat, "DiffuseTexture").kind == engine_core::LuaSlot::Kind::Nil);

    game.history().undo();
    REQUIRE(game.alive(brick));
    REQUIRE(read_field(game, mat, "DiffuseTexture").id == brick);

    game.history().undo();
    REQUIRE(read_field(game, mat, "DiffuseTexture").kind == engine_core::LuaSlot::Kind::Nil);
    game.history().redo();
    REQUIRE(read_field(game, mat, "DiffuseTexture").id == brick);
}

TEST_CASE("GS10 Stop restores references and drops assets made in play", "[GS10]") {
    ScriptRig rig;
    DataModel& game = rig.game;
    const InstanceId brick = make(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId mat = make(game, "Material", "Wall", game.service("Materials"));
    REQUIRE(write_field(game, mat, "DiffuseTexture", instance_slot(brick)));
    add_script(game, "Play", R"(
        local wall = game.Assets.Materials.Wall
        _G.reads = wall.DiffuseTexture == game.Assets.Textures.Brick
        local made = Instance.new("Texture")
        made.Name = "Made"
        made.Parent = game.Assets.Textures
        wall.NormalTexture = made
        wall.DiffuseTexture = nil
        _G.refused = not pcall(function() wall.RoughnessTexture = workspace end)
        _G.done = wall.NormalTexture == made and wall.DiffuseTexture == nil
    )");
    game.start_simulation();
    rig.frames(1, 0.05);
    for (const char* name : {"reads", "refused", "done"}) {
        bool value = false;
        INFO(name);
        INFO(rig.runtime.last_error());
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
    game.stop_simulation();
    REQUIRE(read_field(game, mat, "DiffuseTexture").id == brick);
    REQUIRE(read_field(game, mat, "NormalTexture").kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(game.get_children(game.service("Textures")).size() == 1);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Release --target sandbox --parallel`
Expected: compile error, `reference_class` not declared.

- [ ] **Step 3: InstanceRef**

Create `src/engine_core/InstanceRef.hpp`:

```cpp
#pragma once

#include "types.hpp"

#include <atomic>
#include <string>

namespace engine_core {

class DataModel;

// A saved reference to another instance, held by the target's GUID, so a
// project load, Stop, or undo can set it before the target exists, and a GUID
// no instance holds is kept as it was read. resolve caches the id it found.
class InstanceRef {
public:
    const std::string& guid() const { return guid_; }
    void set_guid(std::string guid);
    // The live instance holding the GUID, or 0. Never the root.
    InstanceId resolve(const DataModel& world) const;

private:
    std::string guid_;
    mutable std::atomic<InstanceId> cached_{0};
};

}  // namespace engine_core
```

Create `src/engine_core/InstanceRef.cpp`:

```cpp
#include "InstanceRef.hpp"

#include "DataModel.hpp"

namespace engine_core {

void InstanceRef::set_guid(std::string guid) {
    guid_ = std::move(guid);
    cached_.store(0, std::memory_order_relaxed);
}

InstanceId InstanceRef::resolve(const DataModel& world) const {
    if (guid_.empty()) {
        return 0;
    }
    const InstanceId cached = cached_.load(std::memory_order_relaxed);
    if (cached != 0 && world.alive(cached) && world.guid(cached) == guid_) {
        return cached;
    }
    const std::optional<InstanceId> found = world.find_guid(guid_);
    const InstanceId id = found && *found != 0 ? *found : 0;
    cached_.store(id, std::memory_order_relaxed);
    return id;
}

}  // namespace engine_core
```

Add `src/engine_core/InstanceRef.cpp` to `engine_core` in `CMakeLists.txt`.

- [ ] **Step 4: Reference JSON and comparison**

In `src/engine_core/PropertyReflection.hpp`, extend the top comment to mention references, and add:

```cpp
// The class a reference property holds, as its type names it: "Texture?" gives
// "Texture". Empty for any other type. A reference's slot carries the target's
// GUID in text, and the live target, if any, in id.
std::string reference_class(std::string_view type);
```

In `PropertyReflection.cpp`, add `#include "LuaApi.hpp"` (for `lua_class_known`) and:

```cpp
std::string reference_class(std::string_view type) {
    if (type.size() < 2 || type.back() != '?') {
        return {};
    }
    const std::string base(type.substr(0, type.size() - 1));
    return lua_class_known(base.c_str()) ? base : std::string();
}
```

In `slot_to_json`, before the final `else`:

```cpp
    } else if (!reference_class(type).empty() &&
               (slot.kind == LuaSlot::Kind::Instance || slot.kind == LuaSlot::Kind::Nil)) {
        out = slot.text.empty() ? JsonValue() : JsonValue::string(slot.text);
```

In `slot_from_json`, before the final `else`:

```cpp
    } else if (!reference_class(type).empty()) {
        if (value.is_null()) {
            out.kind = LuaSlot::Kind::Nil;
            out.text.clear();
        } else if (value.is_string() && valid_guid(value.as_string())) {
            // Named by GUID; the class resolves it when read.
            out.kind = LuaSlot::Kind::Instance;
            out.id = 0;
            out.text = value.as_string();
        } else {
            error = label + " must be a GUID or null";
            return false;
        }
```

(`valid_guid` is in `DataModel.hpp`; include it.) In `same_slot`, change the `Nil` and `Instance` cases:

```cpp
    case LuaSlot::Kind::Nil:
        // A reference that resolves to nothing still names its GUID.
        return a.text == b.text;
    case LuaSlot::Kind::Signal:
        return true;
```

```cpp
    case LuaSlot::Kind::Instance:
        return a.id == b.id && a.text == b.text;
```

- [ ] **Step 5: Luau writes to a reference**

In `src/engine_core/ScriptBindings.cpp` `instance_newindex` (line ~357), extend the instance branch's condition:

```cpp
        } else if (type == "Instance" || type == "Instance?" || type == "DataModel" || type == "DataModel?" ||
                   !reference_class(type).empty()) {
```

(`#include "PropertyReflection.hpp"` if it is not included.) The class check happens in the property's write, which sets `slot.error`; the existing code already turns `slot.error` into the Luau error.

- [ ] **Step 6: ReferenceAsset, Material, and Model**

In `AssetInstances.hpp`, add `#include "InstanceRef.hpp"`, `#include <array>`, `#include <cstddef>`, and replace `ReferenceAsset`, `Material`, and `Model` with:

```cpp
// One reference property: its name and the class it holds.
struct ReferenceSpec {
    const char* property;
    const char* klass;
};

// An asset whose saved properties are references to other assets, held by
// GUID (InstanceRef). Its subclass lists them once, in reference_specs.
class ReferenceAsset : public DataModel {
public:
    static constexpr std::size_t kMaxReferences = 4;

    ReferenceAsset(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    // The GUID in text; the live target, if any, in id, with kind Instance, else Nil.
    LuaSlot reference(std::size_t index) const;
    // SimulationThread. nil clears. A live instance of the property's class is
    // stored by GUID; one of another class is refused. A slot naming a GUID
    // (a load, Stop, or undo) is stored as it is.
    std::optional<std::string> set_reference(std::size_t index, const LuaSlot& value);

protected:
    virtual const ReferenceSpec* reference_specs(std::size_t& count) const = 0;
    void on_reuse() override;

private:
    std::array<InstanceRef, kMaxReferences> refs_;
};

// A PBR material: DiffuseTexture, NormalTexture, RoughnessTexture, and
// MetalnessTexture, each a Texture or nil.
class Material : public ReferenceAsset {
public:
    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;

protected:
    const ReferenceSpec* reference_specs(std::size_t& count) const override;
};

// Joins a Mesh and a Material. Lives only in a Prefab.
class Model : public ReferenceAsset {
public:
    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;

protected:
    const ReferenceSpec* reference_specs(std::size_t& count) const override;
};
```

In `AssetInstances.cpp`, add `#include <iterator>` (for `std::size`) and:

```cpp
namespace {

constexpr ReferenceSpec kMaterialRefs[] = {
    {"DiffuseTexture", "Texture"},
    {"NormalTexture", "Texture"},
    {"RoughnessTexture", "Texture"},
    {"MetalnessTexture", "Texture"},
};

constexpr ReferenceSpec kModelRefs[] = {
    {"Mesh", "Mesh"},
    {"Material", "Material"},
};

}  // namespace

const ReferenceSpec* Material::reference_specs(std::size_t& count) const {
    count = std::size(kMaterialRefs);
    return kMaterialRefs;
}

const ReferenceSpec* Model::reference_specs(std::size_t& count) const {
    count = std::size(kModelRefs);
    return kModelRefs;
}

LuaSlot ReferenceAsset::reference(std::size_t index) const {
    LuaSlot slot;
    if (index >= kMaxReferences) {
        return slot;
    }
    slot.text = refs_[index].guid();
    slot.id = refs_[index].resolve(*this);
    slot.kind = slot.id != 0 ? LuaSlot::Kind::Instance : LuaSlot::Kind::Nil;
    return slot;
}

std::optional<std::string> ReferenceAsset::set_reference(std::size_t index, const LuaSlot& value) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    std::size_t count = 0;
    const ReferenceSpec* specs = reference_specs(count);
    if (index >= count || index >= kMaxReferences) {
        contract_fail("no such reference");
    }
    const ReferenceSpec& spec = specs[index];
    const std::string refused = std::string(spec.property) + " must be a " + spec.klass;
    std::string guid;
    if (value.kind == LuaSlot::Kind::Instance && value.id != 0 && alive(value.id)) {
        const DataModel* target = instance(value.id);
        if (target == nullptr || !lua_class_inherits(target->class_name(), spec.klass)) {
            return refused;
        }
        guid = this->guid(value.id);
    } else if (value.kind == LuaSlot::Kind::Nil) {
        guid = value.text;
    } else if (value.kind == LuaSlot::Kind::Instance && !value.text.empty()) {
        guid = value.text;
    } else if (value.kind == LuaSlot::Kind::Instance) {
        return std::string("That instance no longer exists");
    } else {
        return refused;
    }
    if (guid == refs_[index].guid()) {
        return std::nullopt;
    }
    const LuaSlot before = reference(index);
    refs_[index].set_guid(std::move(guid));
    note_property_change(spec.property, before, reference(index));
    return std::nullopt;
}

void ReferenceAsset::on_reuse() {
    for (InstanceRef& ref : refs_) {
        ref.set_guid(std::string());
    }
}
```

And the Luau accessors, in the file's anonymous namespace, with registration:

```cpp
template <std::size_t Index>
bool read_reference(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* asset = dynamic_cast<const ReferenceAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    out = asset->reference(Index);
    return true;
}

template <std::size_t Index>
bool write_reference(DataModel&, DataModel& object, LuaSlot& in) {
    auto* asset = dynamic_cast<ReferenceAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = asset->set_reference(Index, in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}
```

In `register_asset_instances_lua`, replace the `Material` and `Model` lines with:

```cpp
    const LuaField material_fields[] = {
        lua_saved_property("DiffuseTexture", "Texture?", read_reference<0>, write_reference<0>, "null"),
        lua_saved_property("NormalTexture", "Texture?", read_reference<1>, write_reference<1>, "null"),
        lua_saved_property("RoughnessTexture", "Texture?", read_reference<2>, write_reference<2>, "null"),
        lua_saved_property("MetalnessTexture", "Texture?", read_reference<3>, write_reference<3>, "null"),
    };
    register_lua_class("Material", "ReferenceAsset", material_fields, 4);
    const LuaField model_fields[] = {
        lua_saved_property("Mesh", "Mesh?", read_reference<0>, write_reference<0>, "null"),
        lua_saved_property("Material", "Material?", read_reference<1>, write_reference<1>, "null"),
    };
    register_lua_class("Model", "ReferenceAsset", model_fields, 2);
```

The registration order of `Texture`, `Mesh`, and `Material` does not matter: `reference_class` asks `lua_class_known` when a value is converted, not at registration.

- [ ] **Step 7: Run the tests**

Run: `cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "[GS8],[GS9],[GS10],[SS13]"`
Expected: pass. Then `build/Release/sandbox.exe "~[project]"`; expected: pass.

If GS9's undo of the destroy does not restore the reference, check that `capture_record` for the Material is not involved (it is not destroyed) and that `InstanceRef::resolve` finds the revived Texture by GUID; the revive keeps the GUID through `apply_record_fields`.

- [ ] **Step 8: Commit**

```bash
git add -A src/engine_core src/engine_instances sandbox CMakeLists.txt
git commit -m "Hold asset references by GUID: Material's textures, Model's Mesh and Material

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Completion and analysis know the new classes

**Files:**
- Modify: `src/engine_core/AnalysisDefinitions.cpp` (only if the test shows a gap)
- Test: `sandbox/analysis_tests.cpp`

**Interfaces:**
- Consumes: the Luau registrations from Tasks 2, 3, and 5.

- [ ] **Step 1: Write the failing test**

Look at how an existing case in `sandbox/analysis_tests.cpp` checks that a script analyzes clean (search for a case that asserts no diagnostics for `game.Lighting.Brightness`). Add a case the same way with this script, and require no errors or warnings:

```lua
local tex: Texture = Instance.new("Texture")
tex.Path = "textures/brick.png"
local mat = Instance.new("Material")
mat.DiffuseTexture = tex
local same: Texture? = mat.DiffuseTexture
local assets = game:GetService("Assets")
local textures = game.Assets.Textures
local model = Instance.new("Model")
model.Mesh = nil
model.Material = mat
```

And a second script that must report a type error on the assignment line: `mat.DiffuseTexture = workspace`.

- [ ] **Step 2: Run it**

Run: `cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "[analysis]"` (use the tag the file's cases use).
Expected: the first passes if `to_luau_type("Texture?")` already gives `Texture?`, which is likely since `Instance?` works. If it fails, read `to_luau_type` in `AnalysisDefinitions.cpp` and make a `Name?` type whose `Name` is a registered class map to the Luau optional of that class, as it does for `Instance?`.

- [ ] **Step 3: Commit**

```bash
git add sandbox/analysis_tests.cpp src/engine_core/AnalysisDefinitions.cpp
git commit -m "Type-check scripts that use assets and their references

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Projects save, load, and check the Assets tree

**Files:**
- Modify: `src/engine_core/Project.cpp` (class registry near line 88; `adopt_scene_services` near line 524; `clear_world` near line 763; `build` near line 790; `reset_scene_service` near line 71)
- Test: `sandbox/game_services_tests.cpp`, `sandbox/scene_services_tests.cpp` (SS10, SS11 file lists)

**Interfaces:**
- Consumes: `kServices`, `find_service`, `service_guid`, `passes_rule_up`, `placement_error` (Task 1); `DataModel::service`, `is_service` (Task 2); asset classes (Tasks 3–5).

- [ ] **Step 1: Write the failing tests**

Append (add `#include "Project.hpp"`, `<filesystem>`, `<fstream>`):

```cpp
namespace {

void write_text(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string file_of(const char* klass, const char* guid, const char* name, const std::string& extra = "") {
    return std::string("{\n  \"class\": \"") + klass + "\",\n  \"id\": \"" + guid + "\",\n  \"Name\": \"" + name +
           "\"" + extra + "\n}\n";
}

// A place saved before Assets existed: only Workspace and a Folder in it.
void write_old_place(const std::filesystem::path& root) {
    write_text(root / "project.json",
               "{\"format\": 1, \"name\": \"Old\", \"engine\": \"engine_core\", \"tree\": {\"src\": \"src\"}, "
               "\"resources\": {\"root\": \"resources\"}}\n");
    write_text(root / "src" / "init.json", file_of("Game", "root0", "Old"));
    const std::filesystem::path workspace = root / "src" / "Workspace.workspace";
    write_text(workspace / "init.json", file_of("Workspace", "workspace", "Workspace"));
    write_text(workspace / "Box.cccc.json", file_of("Folder", "cccc", "Box"));
}

}  // namespace

TEST_CASE("GS11 the Assets tree and its references round-trip through a project", "[GS11][project]") {
    SimRole role;
    TempDir dir;
    namespace fs = std::filesystem;
    std::string brick_guid;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        DataModel& game = project.datamodel();
        const InstanceId walls = make(game, "Folder", "Walls", game.service("Textures"));
        const InstanceId brick = make(game, "Texture", "Brick", walls);
        brick_guid = game.guid(brick);
        REQUIRE_FALSE(dynamic_cast<engine_core::Texture*>(game.instance(brick))->set_path("textures/brick.png"));
        const InstanceId mat = make(game, "Material", "Wall", game.service("Materials"));
        REQUIRE(write_field(game, mat, "DiffuseTexture", instance_slot(brick)));
        const InstanceId crate = make(game, "Prefab", "Crate", game.service("Prefabs"));
        const InstanceId body = make(game, "Model", "Body", crate);
        REQUIRE(write_field(game, body, "Material", instance_slot(mat)));
        project.save();
    }
    REQUIRE(fs::is_directory(dir.path / "src" / "Assets.assets"));
    REQUIRE(fs::exists(dir.path / "src" / "Assets.assets" / "init.json"));
    REQUIRE(fs::exists(dir.path / "src" / "Assets.assets" / "Audio.audio.json"));
    REQUIRE(fs::is_directory(dir.path / "src" / "Assets.assets" / "Textures.textures"));

    engine_core::Project loaded = engine_core::Project::load(dir.path);
    DataModel& game = loaded.datamodel();
    const InstanceId assets = game.service("Assets");
    REQUIRE(child_classes(game, assets) ==
            std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"});
    const InstanceId brick = *game.find_guid(brick_guid);
    REQUIRE(game.name(game.parent(brick)) == "Walls");
    REQUIRE(dynamic_cast<engine_core::Texture*>(game.instance(brick))->path() == "textures/brick.png");
    const InstanceId mat = game.find_first_child(game.service("Materials"), "Wall");
    REQUIRE(read_field(game, mat, "DiffuseTexture").id == brick);
    const InstanceId body = game.find_first_child(game.find_first_child(game.service("Prefabs"), "Crate"), "Body");
    REQUIRE(read_field(game, body, "Material").id == mat);

    // Nothing differs from disk, and a second save writes nothing.
    REQUIRE_FALSE(loaded.unsaved());
    loaded.save();
    REQUIRE(loaded.last_save().written.empty());
}

TEST_CASE("GS12 a reference to a missing GUID loads, reads nil, and saves unchanged", "[GS12][project]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        project.save();
    }
    const std::filesystem::path mat_file =
        dir.path / "src" / "Assets.assets" / "Materials.materials" / "Wall.eeee.json";
    // Materials may be a leaf file until it has children; the load reads either.
    std::filesystem::remove(dir.path / "src" / "Assets.assets" / "Materials.materials.json");
    write_text(dir.path / "src" / "Assets.assets" / "Materials.materials" / "init.json",
               file_of("Materials", "materials", "Materials"));
    const std::string bytes = file_of("Material", "eeee", "Wall", ",\n  \"DiffuseTexture\": \"gone\"");
    write_text(mat_file, bytes);
    engine_core::Project project = engine_core::Project::load(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId mat = *game.find_guid("eeee");
    REQUIRE(read_field(game, mat, "DiffuseTexture").kind == engine_core::LuaSlot::Kind::Nil);
    project.save();
    std::ifstream in(mat_file, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(after.find("\"DiffuseTexture\": \"gone\"") != std::string::npos);
}

TEST_CASE("GS13 a place saved before Assets loads with the whole tree made", "[GS13][project]") {
    SimRole role;
    TempDir dir;
    write_old_place(dir.path);
    engine_core::Project project = engine_core::Project::load(dir.path);
    DataModel& game = project.datamodel();
    REQUIRE(child_classes(game, 0) ==
            std::vector<std::string>{"Workspace", "Lighting", "Storage", "Scripts", "Assets"});
    REQUIRE(child_classes(game, game.service("Assets")).size() == 5);
    REQUIRE(project.unsaved());
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "src" / "Assets.assets"));
    project.save();
    REQUIRE(std::filesystem::exists(dir.path / "src" / "Assets.assets" / "Textures.textures.json"));
    REQUIRE_FALSE(project.unsaved());
}

TEST_CASE("GS14 an Assets folder that lacks a category gets it", "[GS14][project]") {
    SimRole role;
    TempDir dir;
    write_old_place(dir.path);
    const std::filesystem::path assets = dir.path / "src" / "Assets.assets";
    write_text(assets / "init.json", file_of("Assets", "assets", "Assets"));
    write_text(assets / "Textures.textures.json", file_of("Textures", "textures", "Textures"));
    engine_core::Project project = engine_core::Project::load(dir.path);
    DataModel& game = project.datamodel();
    REQUIRE(child_classes(game, game.service("Assets")) ==
            std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"});
    REQUIRE(game.guid(game.service("Textures")) == "textures");
}

TEST_CASE("GS15 a file that breaks a placement rule fails the load and names the file", "[GS15][project]") {
    SimRole role;
    auto expect_failure = [](const std::function<void(const std::filesystem::path&)>& setup,
                             const std::string& expected) {
        TempDir dir;
        write_old_place(dir.path);
        setup(dir.path);
        try {
            engine_core::Project::load(dir.path);
            FAIL("the load should refuse: " << expected);
        } catch (const engine_core::ProjectError& error) {
            INFO(error.what());
            REQUIRE(std::string(error.what()).find(expected) != std::string::npos);
        }
    };
    const std::filesystem::path src("src");
    expect_failure(
        [&](const std::filesystem::path& root) {
            write_text(root / src / "Workspace.workspace" / "Brick.ffff.json", file_of("Texture", "ffff", "Brick"));
        },
        "Brick.ffff.json: A Texture must be in Assets.Textures");
    expect_failure(
        [&](const std::filesystem::path& root) {
            const std::filesystem::path assets = root / src / "Assets.assets";
            write_text(assets / "init.json", file_of("Assets", "assets", "Assets"));
            write_text(assets / "Loose.gggg.json", file_of("Folder", "gggg", "Loose"));
        },
        "Loose.gggg.json: Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    expect_failure(
        [&](const std::filesystem::path& root) {
            const std::filesystem::path textures = root / src / "Assets.assets" / "Textures.textures";
            write_text(root / src / "Assets.assets" / "init.json", file_of("Assets", "assets", "Assets"));
            write_text(textures / "init.json", file_of("Textures", "textures", "Textures"));
            write_text(textures / "Rock.hhhh.json", file_of("Mesh", "hhhh", "Rock"));
        },
        "Rock.hhhh.json: Textures holds Textures and Folders");
    expect_failure(
        [&](const std::filesystem::path& root) {
            write_text(root / src / "Workspace.workspace" / "Textures.textures.json",
                       file_of("Textures", "textures", "Textures"));
        },
        "Textures must be a child of Assets with GUID textures");
}
```

(Add `#include <functional>` and `<iterator>`.)

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "[GS11],[GS12],[GS13],[GS14],[GS15]"`
Expected: failures: the load reports `unknown class Assets` or `only a scene service can be a child of game`.

- [ ] **Step 3: Every service in the class registry**

In `Project.cpp`, add `#include "AssetInstances.hpp"`, `#include "Containment.hpp"`, `#include "GameService.hpp"`. Rename `reset_scene_service` to `reset_service` (same body; update its comment to "A service back at its class defaults...") and replace `existing_scene_service` with:

```cpp
// A world has one of each service, made with it, so the factory hands back that
// one, at its defaults, as if it were new.
template <std::size_t Index>
DataModel& existing_service(DataModel& world) {
    DataModel* service = world.instance(world.service(kServices[Index].class_name));
    if (service == nullptr) {
        contract_fail("a Game holds every service");
    }
    reset_service(*service);
    return *service;
}
```

In `class_registry()`, replace the four `existing_scene_service` lines with one per service, then add the asset classes after `TestTriangle`:

```cpp
        out.push_back({kServices[0].class_name, existing_service<0>});
        out.push_back({kServices[1].class_name, existing_service<1>});
        out.push_back({kServices[2].class_name, existing_service<2>});
        out.push_back({kServices[3].class_name, existing_service<3>});
        out.push_back({kServices[4].class_name, existing_service<4>});
        out.push_back({kServices[5].class_name, existing_service<5>});
        out.push_back({kServices[6].class_name, existing_service<6>});
        out.push_back({kServices[7].class_name, existing_service<7>});
        out.push_back({kServices[8].class_name, existing_service<8>});
        out.push_back({kServices[9].class_name, existing_service<9>});
```

```cpp
        out.push_back({"Texture", [](DataModel& world) -> DataModel& { return world.create<Texture>(); }});
        out.push_back({"Mesh", [](DataModel& world) -> DataModel& { return world.create<Mesh>(); }});
        out.push_back({"Sound", [](DataModel& world) -> DataModel& { return world.create<Sound>(); }});
        out.push_back({"Material", [](DataModel& world) -> DataModel& { return world.create<Material>(); }});
        out.push_back({"Model", [](DataModel& world) -> DataModel& { return world.create<Model>(); }});
        out.push_back({"Prefab", [](DataModel& world) -> DataModel& { return world.create<Prefab>(); }});
```

Add `static_assert(std::size(kServices) == 10, "one registry entry per service");` above `class_registry`.

- [ ] **Step 4: adopt_services and check_placement**

Replace `adopt_scene_services()` with:

```cpp
    // Each node's parent index. The root's is 0.
    std::vector<std::size_t> parents() const {
        std::vector<std::size_t> parent(nodes_.size(), 0);
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            for (std::size_t child : nodes_[index].children) {
                parent[child] = index;
            }
        }
        return parent;
    }

    // game and Assets hold their services, as kServices lists them. A service
    // the files lack is made at its defaults. Each service's GUID is fixed, so
    // every read of the same files gives the same tree, and nothing is written
    // until the next save.
    void adopt_services() {
        const std::vector<std::size_t> parent = parents();
        for (std::size_t index = 1; index < nodes_.size(); ++index) {
            PlanNode& node = nodes_[index];
            if (const ServiceSpec* spec = find_service(node.class_name)) {
                const std::string guid = service_guid(node.class_name);
                const bool home = spec->parent_class == nullptr
                                      ? parent[index] == 0
                                      : parent[index] != 0 && nodes_[parent[index]].class_name == spec->parent_class;
                if (!home || node.guid != guid) {
                    fail(node.props_path + ": " + node.class_name + " must be a child of " +
                         (spec->parent_class != nullptr ? spec->parent_class : "game") + " with GUID " + guid);
                }
                // The name is fixed too. A file that says otherwise is read as the service.
                node.name = node.class_name;
                continue;
            }
            if (parent[index] == 0) {
                fail(node.props_path + ": only a scene service can be a child of game");
            }
            for (const ServiceSpec& service : kServices) {
                if (node.guid == service_guid(service.class_name)) {
                    fail(node.props_path + ": GUID " + node.guid + " is reserved for " + service.class_name);
                }
            }
        }
        // Each service, found or made, in table order under its parent, before
        // any other child that parent has.
        std::unordered_map<std::string, std::size_t> at;
        std::unordered_map<std::size_t, std::vector<std::size_t>> services_of;
        for (const ServiceSpec& spec : kServices) {
            const std::size_t holder = spec.parent_class == nullptr ? 0 : at.at(spec.parent_class);
            std::size_t found = 0;
            for (std::size_t child : nodes_[holder].children) {
                if (nodes_[child].class_name == spec.class_name) {
                    found = child;
                }
            }
            if (found == 0) {
                PlanNode made;
                made.guid = service_guid(spec.class_name);
                made.class_name = spec.class_name;
                made.name = spec.class_name;
                made.doc = JsonValue::object();
                made.doc.set("class", JsonValue::string(spec.class_name));
                made.doc.set("id", JsonValue::string(made.guid));
                made.doc.set("Name", JsonValue::string(spec.class_name));
                made.made = true;
                found = nodes_.size();
                nodes_.push_back(std::move(made));
            }
            at[spec.class_name] = found;
            services_of[holder].push_back(found);
        }
        for (auto& [holder, services] : services_of) {
            std::vector<std::size_t> children = services;
            for (std::size_t child : nodes_[holder].children) {
                if (find_service(nodes_[child].class_name) == nullptr) {
                    children.push_back(child);
                }
            }
            nodes_[holder].children = std::move(children);
        }
    }

    // Every instance where the placement rules allow it, checked before any is made.
    void check_placement() {
        const std::vector<std::size_t> parent = parents();
        for (std::size_t index = 1; index < nodes_.size(); ++index) {
            const PlanNode& node = nodes_[index];
            if (find_service(node.class_name) != nullptr) {
                continue;
            }
            std::size_t holder = parent[index];
            while (holder != 0 && passes_rule_up(nodes_[holder].class_name)) {
                holder = parent[holder];
            }
            const std::string holder_class = holder == 0 ? std::string("Game") : nodes_[holder].class_name;
            if (std::optional<std::string> error = placement_error(holder_class, node.class_name, node.name)) {
                fail(node.props_path + ": " + *error);
            }
        }
    }
```

In `read()`, replace `adopt_scene_services();` with:

```cpp
        adopt_services();
        check_placement();
```

(`<unordered_map>` is likely already included; include it if not.) The old code set `nodes_[0].children = services` exactly; the new code keeps the services first and, for the root, there are no other children because the loop above already failed on them.

- [ ] **Step 5: clear_world and order_children_as**

In `clear_world`, change `!object.is_scene_service()` to `!object.is_service()`, and reset every service:

```cpp
    for (const ServiceSpec& spec : kServices) {
        if (DataModel* service = world.instance(world.service(spec.class_name))) {
            reset_service(*service);
        }
    }
```

In `order_children_as`, right after the existing `if (id == 0) { return; }`, add:

```cpp
    // Assets holds only its categories, made with the world in table order.
    if (const DataModel* holder = world.instance(id);
        holder != nullptr && holder->is_service() && !holder->is_scene_service()) {
        return;
    }
```

Only Assets reaches this: a category is a GameService too, but its children are ordinary assets, so narrow the check to `id == world.service("Assets")` instead of the class test above if a category's children stop reordering from disk (GS11's second save writing nothing covers it).

Search for any other `kSceneServiceClasses` or `is_scene_service` use in `Project.cpp` and decide by meaning: a check that means "is a service" becomes `is_service`/`find_service`; one that means "renders or runs scripts" stays.

Run: `grep -n "kSceneServiceClasses\|is_scene_service\|scene_service_guid\|is_scene_service_class" src/engine_core/Project.cpp`

- [ ] **Step 6: Update SS10 and SS11 expectations**

SS10 expects the files `Lighting.lighting.json`, `Storage.storage.json`, and `Scripts.scripts.json` after the save; add `REQUIRE(fs::exists(dir.path / "src" / "Assets.assets" / "init.json"));`. SS11's first case now reads `GUID storage is reserved for Storage` (unchanged). Its second case expects `Lighting must be a child of game with GUID lighting` (unchanged format).

- [ ] **Step 7: Run all sandbox tests**

Run: `cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe`
Expected: all pass, including every P-series project test. If a P-series test fails because it counts written files after a first save (the Assets files are new), update the count and say so in the commit message.

- [ ] **Step 8: Commit**

```bash
git add src/engine_core/Project.cpp sandbox
git commit -m "Save, load, and check the Assets tree in projects

A project saved before Assets loads with the whole tree made, and the next
Save writes it. A file that breaks a placement rule fails the load and says
which file.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: The Game Explorer hides GameServices; Insert leaves out assets and asks first

**Files:**
- Modify: `src/ide/IdeExplorer.cpp:1005-1016` (`read_hierarchy`)
- Modify: `src/ide/InsertPopup.cpp:106` (`showAt`)
- Modify: `src/ide/IdeLayout.cpp:167-189` (`host.insert`), `src/ide/CutSet.hpp`, `src/ide/CutSet.cpp` (`insert_instance`)
- Modify: `src/ide/IdeIcons.cpp` (`IconFileOverride`)
- Test: `tests/ExplorerRenameTest.cpp`

**Interfaces:**
- Consumes: `DataModel::hidden_in_explorer`, `is_asset_class`, `parent_error`.
- Produces: `bool ide::insert_offers(const std::string& class_name);` in `InsertPopup.hpp`, true for a class the explorer's Insert list shows.

- [ ] **Step 1: Write the failing tests**

In `tests/ExplorerRenameTest.cpp`, add these functions and call them from `main` next to the others (follow the file's pattern: each test is a function that builds a `Rig` and uses `Expect`):

```cpp
void hidden_services_have_no_rows() {
    Rig rig;
    rig.frame(0);
    Expect(rig.cell("Workspace") != nullptr, "Workspace has a row");
    Expect(rig.cell("Assets") == nullptr, "Assets has no row");
    Expect(rig.cell("Textures") == nullptr, "Textures has no row");
    // A Folder in Textures is under a hidden service, so it has no row either.
    engine_core::Folder& walls = rig.game.create<engine_core::Folder>();
    rig.game.set_name(walls.id(), "Walls");
    rig.game.set_parent(walls.id(), rig.game.service("Textures"));
    rig.frame(1);
    Expect(rig.cell("Walls") == nullptr, "a Folder in Textures has no row");
}

void insert_list_leaves_out_assets() {
    Expect(ide::insert_offers("Folder"), "Insert offers Folder");
    Expect(ide::insert_offers("Script"), "Insert offers Script");
    for (const char* klass : {"Texture", "Mesh", "Sound", "Material", "Prefab", "Model"}) {
        Expect(!ide::insert_offers(klass), "Insert leaves out asset classes");
    }
}
```

Also check the filter: after `hidden_services_have_no_rows`' setup, type `Walls` into the filter field the way the file's existing filter test does, and `Expect(rig.cell("Walls") == nullptr, "the filter never shows a hidden row")`.

For the insert path, the body of `IdeLayout`'s `host.insert` lambda moves into a function in `CutSet.hpp`/`CutSet.cpp` (beside `move_set`, which the explorer tests already call), so it can be tested without an engine thread:

```cpp
// Makes class_name for the explorer's or the Assets pane's insert, under asked,
// or under Workspace when asked is the root. 0, with error set, when the place
// is full or refuses the class there; nothing is left behind then.
engine_core::InstanceId insert_instance(engine_core::DataModel& world, const std::string& class_name,
                                        engine_core::InstanceId asked, std::string& error);
```

Add this test to `tests/ExplorerRenameTest.cpp` and call it from `main`:

```cpp
void insert_refused_leaves_nothing() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    engine_core::Game game;
    std::size_t before = 0;
    game.for_each_instance([&before](engine_core::DataModel&) { ++before; });
    std::string error;
    const engine_core::InstanceId made = ide::insert_instance(game, "Folder", game.service("Assets"), error);
    Expect(made == 0, "Assets refuses a Folder");
    Expect(error == "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio", "and says why");
    std::size_t after = 0;
    game.for_each_instance([&after](engine_core::DataModel&) { ++after; });
    Expect(after == before, "nothing is left behind");
    Expect(game.get_children(game.service("Assets")).size() == 5, "Assets is unchanged");

    error.clear();
    const engine_core::InstanceId texture = ide::insert_instance(game, "Texture", game.service("Textures"), error);
    Expect(texture != 0 && error.empty() && game.parent(texture) == game.service("Textures"), "a Texture goes in");
    const engine_core::InstanceId top = ide::insert_instance(game, "Folder", 0, error);
    Expect(game.parent(top) == game.service("Workspace"), "the root still means Workspace");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}
```

If `for_each_instance` visits only GameObjects (its comment says "Visits live GameObjects only"), count with `game.room_left()` before and after instead: the same value means nothing was left alive.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Release --target explorer-tests --parallel && build/Release/explorer-tests.exe`
Expected: `FAIL Assets has no row`, and a compile error for `insert_offers`.

- [ ] **Step 3: Implement**

In `IdeExplorer::read_hierarchy`, inside the child loop, skip hidden services before recording the child:

```cpp
                if (const engine_core::DataModel* object = root_.instance(child);
                    object != nullptr && object->hidden_in_explorer()) {
                    // A game service, and everything under it, is shown in the Assets pane.
                    continue;
                }
```

Place it right after the `seen_` check. The filter works on the captured snapshot, so hidden rows never reach it.

In `InsertPopup.hpp`, declare:

```cpp
// True for a class the explorer's Insert list shows: one Instance.new makes
// that is not an asset, since assets go in the Assets pane.
bool insert_offers(const std::string& class_name);
```

In `InsertPopup.cpp`, define it (include `Containment.hpp` and `LuaApi.hpp`):

```cpp
bool insert_offers(const std::string& class_name) {
    return engine_core::lua_creatable_known(class_name.c_str()) && !engine_core::is_asset_class(class_name);
}
```

and in `showAt`, after `engine_core::lua_creatable_names(all_);`:

```cpp
        all_.erase(std::remove_if(all_.begin(), all_.end(),
                                  [](const std::string& name) { return !insert_offers(name); }),
                   all_.end());
```

In `CutSet.cpp`, define `insert_instance` (include `LuaApi.hpp` and `<optional>`):

```cpp
engine_core::InstanceId insert_instance(engine_core::DataModel& world, const std::string& class_name,
                                        engine_core::InstanceId asked, std::string& error) {
    // game holds only services, so an insert at the top goes into Workspace.
    const engine_core::InstanceId parent = asked == 0 ? world.scene_service("Workspace") : asked;
    if (parent == engine_core::DataModel::kNoParent || (parent != 0 && !world.alive(parent))) {
        error = "That instance no longer exists";
        return 0;
    }
    if (world.room_left() == 0) {
        error = engine_core::InstanceCapacityError().what();
        return 0;
    }
    engine_core::DataModel* created = engine_core::lua_create_instance(world, class_name.c_str());
    if (created == nullptr) {
        error = "Cannot make a " + class_name;
        return 0;
    }
    if (std::optional<std::string> refused = world.parent_error(created->id(), parent)) {
        // Made out of the tree, so destroying it leaves nothing behind.
        world.destroy(created->id());
        error = std::move(*refused);
        return 0;
    }
    world.set_parent(created->id(), parent);
    return created->id();
}
```

In `IdeLayout.cpp` `host.insert`, replace the body of the `on_simulation` lambda with:

```cpp
                std::string error;
                const engine_core::InstanceId made = insert_instance(world, class_name, asked, error);
                CloseGesture(world);
                if (result) {
                    result->id.store(made, std::memory_order_relaxed);
                    result->error = std::move(error);
                    result->done.store(true, std::memory_order_release);
                }
```

A refused insert records a create and a destroy in the open gesture. Check with the explorer test that `game.history().can_undo()` is the same before and after a refused insert; if it is not, start the gesture only once `parent_error` passes (ask it with a scratch check before creating: `lua_creatable_known(class_name)` plus `placement_error(rule class of parent, class_name, class_name)` through a new `DataModel::placement_error_for_class(parent, class_name)`), rather than destroying afterwards. The explorer shows `result->error` through `host.notice` already (see `finish_insert`).

In `IdeIcons.cpp` `IconFileOverride`, add:

```cpp
    if (class_name == "Assets" || class_name == "Audio") {
        return "AssetFolder.png";
    }
    if (class_name == "Materials") {
        return "AssetFolderMaterial.png";
    }
    if (class_name == "Meshes") {
        return "AssetFolderMesh.png";
    }
    if (class_name == "Prefabs") {
        return "AssetFolderPrefab.png";
    }
    if (class_name == "Textures") {
        return "AssetFolderTexture.png";
    }
    if (class_name == "Prefab") {
        return "ModelAlt.png";
    }
```

`Texture`, `Mesh`, `Sound`, `Material`, and `Model` already have `<Class>.png` in `resources/icons`.

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --config Release --target explorer-tests studio-tests --parallel && build/Release/explorer-tests.exe && build/Release/studio-tests.exe`
Expected: no `FAIL` lines, exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/ide tests
git commit -m "Hide game services from the explorer and keep assets out of Insert

The insert path now asks parent_error, so a refused insert says why and
leaves nothing behind.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Properties shows and edits references

**Files:**
- Modify: `src/ide/PropertySheet.cpp:187-203` (`property_kind_for`), `:266` (service rows), `:340-362` (pre-checks)
- Modify: `src/ide/PropertiesPanel.cpp` (a reference row takes a drop)
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Consumes: `reference_class` (Task 5); `ReferenceAsset` properties.
- Produces: `inline constexpr const char* ide::kInstanceDragFormat = "application/x-anarchy-instances";` in `PropertySheet.hpp` — a drag's data: instance ids in decimal, joined by `,`. Task 11's pane writes it; Properties reads it.

- [ ] **Step 1: Write the failing tests**

In `tests/PropertiesTest.cpp` (add `#include "AssetInstances.hpp"` and `#include "LuaApi.hpp"`), add these and call them from `main` after `TestR6ParentReference()`:

```cpp
InstanceId MakeAsset(Game& game, const char* klass, const char* name, InstanceId parent) {
    DataModel* object = engine_core::lua_create_instance(game, klass);
    game.set_name(object->id(), name);
    game.set_parent(object->id(), parent);
    return object->id();
}

const ide::PropertyRow* RowNamed(const ide::PropertySheet& sheet, const char* name) {
    for (const ide::PropertyRow& row : sheet.rows) {
        if (row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

ide::PropertyEdit RefEdit(const char* property, InstanceId id) {
    return {property, ide::PropertyKind::Ref, {"", false, 0, {}, id}, -1};
}

void TestReferenceRows() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Game game;
    const InstanceId brick = MakeAsset(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId rock = MakeAsset(game, "Mesh", "Rock", game.service("Meshes"));
    const InstanceId wall = MakeAsset(game, "Material", "Wall", game.service("Materials"));

    const ide::PropertySheet before = ide::read_sheet(game, {wall});
    const ide::PropertyRow* row = RowNamed(before, "DiffuseTexture");
    Expect(row != nullptr && row->kind == ide::PropertyKind::Ref, "a reference is a Ref row");
    Expect(row != nullptr && row->writable && row->value.nil_ref(), "it starts nil and writable");

    const ide::EditResult set = ide::apply_edit(game, {wall}, RefEdit("DiffuseTexture", brick));
    Expect(set.written == 1 && !set.rejected, "a Texture is taken");
    const ide::PropertySheet after = ide::read_sheet(game, {wall});
    row = RowNamed(after, "DiffuseTexture");
    Expect(row != nullptr && row->label == "Brick", "the row shows the Texture's name");
    Expect(row != nullptr && row->path == "Game.Assets.Textures.Brick", "and its path on hover");

    const ide::EditResult wrong = ide::apply_edit(game, {wall}, RefEdit("DiffuseTexture", rock));
    Expect(wrong.rejected && wrong.error == "DiffuseTexture must be a Texture", "a Mesh is refused, saying why");
    Expect(wrong.written == 0, "a refusal writes nothing");

    const ide::EditResult cleared =
        ide::apply_edit(game, {wall}, RefEdit("DiffuseTexture", engine_core::DataModel::kNoParent));
    Expect(cleared.written == 1, "nil clears");
    row = RowNamed(ide::read_sheet(game, {wall}), "DiffuseTexture");
    Expect(row != nullptr && row->value.nil_ref(), "the row is nil again");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

void TestServiceRowsReadOnly() {
    Game game;
    const ide::PropertySheet sheet = ide::read_sheet(game, {game.service("Textures")});
    const ide::PropertyRow* name = RowNamed(sheet, "Name");
    const ide::PropertyRow* parent = RowNamed(sheet, "Parent");
    Expect(name != nullptr && !name->writable, "a game service's Name is read-only");
    Expect(parent != nullptr && !parent->writable, "a game service's Parent is read-only");
}
```

The root's Name is `Game` in a new place, so the path starts `Game.`. If `ref_path` names the root differently, match what `TestR6ParentReference` shows for a path. If `set_thread_role` is not reachable from this file, include `DataModel.hpp`'s thread-role header the sandbox's `SimRole` uses (`support.hpp` includes it through `DataModel.hpp`).

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Release --target properties-tests --parallel && build/Release/properties-tests.exe`
Expected: `FAIL` on the DiffuseTexture row (it is not shown, since `Texture?` is not a known kind).

- [ ] **Step 3: Implement**

In `property_kind_for`, extend the reference branch:

```cpp
    } else if (type_name == "Instance" || type_name == "Instance?" || type_name == "DataModel" ||
               type_name == "DataModel?" || !engine_core::reference_class(type_name).empty()) {
        out = PropertyKind::Ref;
```

(include `PropertyReflection.hpp`). At line ~266, make every service's Name and Parent read-only:

```cpp
            // A service keeps its name and its place.
            if (object->is_service() && (row.name == "Name" || row.name == "Parent")) {
```

In the pre-check loop of the edit (line ~350), add a class check for references other than Parent:

```cpp
        } else if (edit.kind == PropertyKind::Ref && !edit.value.nil_ref()) {
            const std::string klass = engine_core::reference_class(target.field.type_name);
            const DataModel* picked = world.instance(edit.value.ref);
            if (!klass.empty() && (picked == nullptr ||
                                   !engine_core::lua_class_inherits(picked->class_name(), klass.c_str()))) {
                error = edit.property + " must be a " + klass;
            }
        }
```

Place it as a third branch after the `Name` branch, so `Parent` keeps its own check. `target.field` is the `LuaField` for that instance; if the `Target` struct names it differently, use its name.

In `PropertiesPanel.cpp`, where a reference row's pick button (`view->pick`) is made (line ~601), let it take a drop of instances:

```cpp
            view->pick->setOnDragOver([](jadefx::DragEvent& event) {
                if (event.dragboard != nullptr && event.dragboard->has(kInstanceDragFormat)) {
                    event.acceptTransferModes(jadefx::TransferMode::Link);
                    event.consume();
                }
            });
            view->pick->setOnDragDropped([weak_self, weak_view](jadefx::DragEvent& event) {
                auto self = weak_self.lock();
                auto view = weak_view.lock();
                if (!self || !view || event.dragboard == nullptr) {
                    return;
                }
                const std::string ids = event.dragboard->get(kInstanceDragFormat);
                const std::size_t comma = ids.find(',');
                const std::string first = ids.substr(0, comma);
                if (first.empty()) {
                    return;
                }
                PropertyEdit edit;
                edit.property = view->row.name;
                edit.kind = PropertyKind::Ref;
                edit.value.ref = static_cast<engine_core::InstanceId>(std::stoul(first));
                self->submit(self->sheet.ids, edit);
                event.setDropCompleted(true);
                event.consume();
            });
```

Use the names this file actually has for the weak pointers, the row, the current sheet's ids, and `submit` (they appear in the `on_pick` lambda just above: follow it). Only a reference row other than `Parent` takes the drop; skip installing the handlers when `row.name == "Parent"`.

In `PropertySheet.hpp`, declare `inline constexpr const char* kInstanceDragFormat = "application/x-anarchy-instances";` with a comment: "A drag of instances: their ids in decimal, joined by commas. The Assets pane writes it; a reference row in Properties takes it."

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --config Release --target properties-tests --parallel && build/Release/properties-tests.exe`
Expected: no `FAIL`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/ide/PropertySheet.hpp src/ide/PropertySheet.cpp src/ide/PropertiesPanel.cpp tests/PropertiesTest.cpp
git commit -m "Show and edit asset references in Properties

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: AssetBrowser, the pane's model

**Files:**
- Create: `src/ide/AssetBrowser.hpp`, `src/ide/AssetBrowser.cpp`
- Create: `tests/AssetBrowserTest.cpp`
- Modify: `CMakeLists.txt` (add `src/ide/AssetBrowser.cpp` to `STUDIO_CORE_SOURCES`; add `tests/AssetBrowserTest.cpp` to `engine-tests`)
- Modify: `src/ide/Preferences.hpp`, `src/ide/Preferences.cpp`; `tests/PreferencesTest.cpp`

**Interfaces:**
- Consumes: `DataModel`, `service()`, `asset_home`, `is_asset_class`, `ReferenceAsset`, `FileAsset`.
- Produces (used by Task 11):

```cpp
namespace ide {

enum class AssetView { Icons, List, Columns };
const char* asset_view_name(AssetView view);            // "icons", "list", "columns"
bool asset_view_from(std::string_view name, AssetView& out);

struct AssetRow {
    engine_core::InstanceId id = 0;
    std::string name;
    std::string class_name;
    std::string path;        // Path for Texture, Mesh, Sound; empty otherwise
    std::string where;       // search_rows only: names from the folder shown down to it, joined by '/'
    int depth = 0;           // List view: 0 for a child of the folder shown
    bool opens = false;      // a Folder or Prefab: double-click opens it
    bool expanded = false;   // List view only
    bool operator==(const AssetRow&) const;
};

enum class AssetSort { Name, Kind, Path };

class AssetBrowser {
public:
    explicit AssetBrowser(engine_core::DataModel& world);

    // The folder shown: a category, a Folder, or a Prefab under Assets. Starts
    // at Materials, the first category.
    engine_core::InstanceId folder() const;
    // Opens id, pushing the current folder on back. False when id cannot be opened
    // (dead, not under Assets, or not a category, Folder, or Prefab).
    bool open(engine_core::InstanceId id);
    bool back();
    bool forward();
    bool can_back() const;
    bool can_forward() const;
    // Assets down to the folder shown, with names: {Assets, Textures, Walls}.
    std::vector<std::pair<engine_core::InstanceId, std::string>> crumbs() const;
    // The five categories, in order.
    std::vector<AssetRow> categories() const;

    // Icons view: the folder's children in sibling order.
    std::vector<AssetRow> children() const;
    // List view: the folder's children, sorted, with expanded Folders and Prefabs' children under them.
    std::vector<AssetRow> list_rows() const;
    void set_expanded(engine_core::InstanceId id, bool expanded);
    void set_sort(AssetSort sort, bool descending);
    AssetSort sort() const;
    bool descending() const;
    // Columns view: one column per level from the categories to the folder shown.
    std::vector<std::vector<AssetRow>> columns() const;

    // A non-empty search replaces every view with the matches under the folder:
    // name contains the text, ignoring case; where holds "Walls/Brick" from the folder.
    void set_search(std::string text);
    const std::string& search() const;
    std::vector<AssetRow> search_rows() const;

    // Moves back to a live folder when the one shown was destroyed, or left
    // Assets. Returns true when anything the views show may have changed since
    // the last call (the tree moved or the folder changed).
    bool refresh();

    // The class New <kind> makes in the folder shown: Material in Materials or
    // a Folder under it, Model in a Prefab, and so on. Empty when there is none.
    std::string new_kind() const;
};

}  // namespace ide
```

Every method reads the world under a `DataModelLock` Read taken by the caller (the pane takes it once per frame and calls several methods); document that on the class: "Callers hold the world's read lock."

- [ ] **Step 1: Write the failing tests**

Create `tests/AssetBrowserTest.cpp`. Like the other `engine-tests` files, it exposes `int RunAssetBrowserTests()`; declare it at the top of `tests/LuaEngineTest.cpp` with the other `Run...Tests` declarations, and add `gFailures += RunAssetBrowserTests();` to its `main` after `RunColorLiteralsTests()`.

```cpp
#include "ide/AssetBrowser.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"

#include <cstdio>
#include <string>
#include <vector>

// The Assets pane's model: navigation, each view's rows, sort, and search.
namespace {

using engine_core::InstanceId;

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

InstanceId Make(engine_core::DataModel& game, const char* klass, const char* name, InstanceId parent) {
    engine_core::DataModel* object = engine_core::lua_create_instance(game, klass);
    game.set_name(object->id(), name);
    game.set_parent(object->id(), parent);
    return object->id();
}

std::vector<std::string> Names(const std::vector<ide::AssetRow>& rows) {
    std::vector<std::string> out;
    for (const ide::AssetRow& row : rows) {
        out.push_back(row.name);
    }
    return out;
}

void TestNavigates() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    engine_core::Game game;
    ide::AssetBrowser browser(game);
    Expect(browser.folder() == game.service("Materials"), "starts in the first category");
    Expect(Names(browser.categories()) ==
               std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"},
           "the five categories, in order");
    const InstanceId walls = Make(game, "Folder", "Walls", game.service("Textures"));
    const InstanceId brick = Make(game, "Texture", "Brick", walls);
    Expect(browser.open(game.service("Textures")), "opens a category");
    Expect(browser.open(walls), "opens a Folder");
    const auto crumbs = browser.crumbs();
    Expect(crumbs.size() == 3 && crumbs[0].second == "Assets" && crumbs[2].second == "Walls", "crumbs from Assets");
    Expect(Names(browser.children()) == std::vector<std::string>{"Brick"}, "a folder's children");
    Expect(browser.back() && browser.folder() == game.service("Textures"), "back");
    Expect(browser.forward() && browser.folder() == walls, "forward");
    Expect(!browser.can_forward(), "nothing after the newest");
    Expect(!browser.open(game.service("Workspace")), "Workspace is not under Assets");
    Expect(!browser.open(brick), "a Texture does not open");
    // The folder shown is destroyed: refresh falls back to its nearest live ancestor.
    game.destroy_tree(walls);
    browser.refresh();
    Expect(browser.folder() == game.service("Textures"), "falls back to the category");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

void TestListsSortsAndSearches() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    engine_core::Game game;
    const InstanceId textures = game.service("Textures");
    const InstanceId b = Make(game, "Folder", "b", textures);
    Make(game, "Texture", "Deep", b);
    const InstanceId c = Make(game, "Texture", "c", textures);
    dynamic_cast<engine_core::Texture*>(game.instance(c))->set_path("textures/c.png");
    Make(game, "Texture", "a", textures);

    ide::AssetBrowser browser(game);
    browser.open(textures);
    Expect(Names(browser.children()) == std::vector<std::string>{"b", "c", "a"}, "icons keep sibling order");
    Expect(Names(browser.list_rows()) == std::vector<std::string>{"a", "b", "c"}, "list sorts by name");
    browser.set_expanded(b, true);
    const std::vector<ide::AssetRow> open = browser.list_rows();
    Expect(Names(open) == std::vector<std::string>{"a", "b", "Deep", "c"}, "an open folder lists its children");
    Expect(open.size() == 4 && open[2].depth == 1 && open[1].opens && open[1].expanded, "under it, one deeper");
    browser.set_sort(ide::AssetSort::Name, true);
    Expect(Names(browser.list_rows()) == std::vector<std::string>{"c", "b", "Deep", "a"}, "descending");
    browser.set_sort(ide::AssetSort::Path, false);
    Expect(browser.list_rows().back().name == "c", "by path, an empty path first");

    browser.set_search("DEE");
    const std::vector<ide::AssetRow> found = browser.search_rows();
    Expect(found.size() == 1 && found[0].name == "Deep" && found[0].where == "b/Deep", "search ignores case");
    browser.set_search("");

    browser.open(b);
    const auto columns = browser.columns();
    Expect(columns.size() == 3, "a column per level: categories, Textures, b");
    Expect(columns.size() == 3 && Names(columns[2]) == std::vector<std::string>{"Deep"}, "the last is the folder");
    Expect(browser.new_kind() == "Texture", "New Texture in a Folder under Textures");
    const InstanceId crate = Make(game, "Prefab", "Crate", game.service("Prefabs"));
    browser.open(crate);
    Expect(browser.new_kind() == "Model", "New Model in a Prefab");
    browser.open(game.service("Audio"));
    Expect(browser.new_kind() == "Sound", "New Sound in Audio");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

}  // namespace

int RunAssetBrowserTests() {
    gFailures = 0;
    TestNavigates();
    TestListsSortsAndSearches();
    return gFailures;
}
```
In `tests/PreferencesTest.cpp`, add: a fresh `Preferences` reads `assets_view()` as `"icons"`; after `set_assets_view("columns")` and `save`, a new `Preferences` on the same file reads `"columns"`; an unknown stored value reads as `"icons"`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build --config Release --target engine-tests --parallel`
Expected: compile error, `AssetBrowser.hpp` not found.

- [ ] **Step 3: Implement AssetBrowser**

Create `src/ide/AssetBrowser.hpp` with the interface above, the comment "What the Assets pane shows, without widgets: the folder, back and forward, crumbs, and each view's rows. Callers hold the world's read lock.", and private members:

```cpp
private:
    AssetRow row_of(engine_core::InstanceId id, int depth) const;
    bool can_open(engine_core::InstanceId id) const;
    void add_list_rows(engine_core::InstanceId parent, int depth, std::vector<AssetRow>& out) const;

    engine_core::DataModel& world_;
    engine_core::InstanceId folder_ = 0;
    std::vector<engine_core::InstanceId> back_;
    std::vector<engine_core::InstanceId> forward_;
    std::unordered_set<engine_core::InstanceId> expanded_;
    AssetSort sort_ = AssetSort::Name;
    bool descending_ = false;
    std::string search_;
    std::uint64_t seen_tree_ = 0;
    engine_core::InstanceId seen_folder_ = 0;
```

Create `src/ide/AssetBrowser.cpp`. The key functions:

```cpp
AssetBrowser::AssetBrowser(engine_core::DataModel& world) : world_(world) {
    folder_ = world_.service(engine_core::kServices[5].class_name);  // Materials
}

bool AssetBrowser::can_open(engine_core::InstanceId id) const {
    const engine_core::DataModel* object = world_.instance(id);
    if (object == nullptr) {
        return false;
    }
    const std::string klass = object->class_name();
    const bool container = klass == "Folder" || klass == "Prefab" ||
                           (object->is_service() && world_.parent(id) == world_.service("Assets"));
    if (!container) {
        return false;
    }
    // Under Assets.
    const engine_core::InstanceId assets = world_.service("Assets");
    for (engine_core::InstanceId at = world_.parent(id); at != engine_core::DataModel::kNoParent;
         at = world_.parent(at)) {
        if (at == assets) {
            return true;
        }
        if (at == 0) {
            return false;
        }
    }
    return false;
}

bool AssetBrowser::open(engine_core::InstanceId id) {
    if (id == folder_ || !can_open(id)) {
        return false;
    }
    back_.push_back(folder_);
    forward_.clear();
    folder_ = id;
    return true;
}

bool AssetBrowser::back() {
    while (!back_.empty()) {
        const engine_core::InstanceId previous = back_.back();
        back_.pop_back();
        if (can_open(previous)) {
            forward_.push_back(folder_);
            folder_ = previous;
            return true;
        }
    }
    return false;
}
```

(`forward()` mirrors `back()`.) `row_of` fills `path` from `dynamic_cast<const engine_core::FileAsset*>` and `opens` from `klass == "Folder" || klass == "Prefab"`. `list_rows` collects the folder's children, sorts them by `sort_` (name compare case-insensitive; kind by class name then name; path then name; `descending_` reverses), and recurses into expanded containers, with `depth + 1`, sorting each level the same way. `columns()` walks from the folder up to Assets, collecting the chain, then returns `categories()` followed by each level's `children()` for the chain below the category. `search_rows()` walks the folder's subtree depth-first in sibling order and keeps rows whose lowercased name contains the lowercased text, setting `where` to the names from the folder down to the row, joined by `/`. `new_kind()`:

```cpp
std::string AssetBrowser::new_kind() const {
    const engine_core::DataModel* object = world_.instance(folder_);
    if (object == nullptr) {
        return {};
    }
    if (std::string(object->class_name()) == "Prefab") {
        return "Model";
    }
    // The category above: its asset class is the one whose home it is.
    engine_core::InstanceId at = folder_;
    while (at != 0 && at != engine_core::DataModel::kNoParent) {
        const engine_core::DataModel* here = world_.instance(at);
        if (here != nullptr && here->is_service()) {
            for (const char* klass : {"Material", "Prefab", "Mesh", "Texture", "Sound"}) {
                if (std::string(engine_core::asset_home(klass)) == here->class_name()) {
                    return klass;
                }
            }
            return {};
        }
        at = world_.parent(at);
    }
    return {};
}
```

`refresh()` falls back to the nearest ancestor that still opens. A destroyed folder has no parent to walk up to, so the browser remembers, each time the folder changes, the chain of ancestors from the category down (`std::vector<engine_core::InstanceId> chain_;`, a private member), and walks it from the end:

```cpp
// The ancestors of folder_ from its category down, not counting folder_.
void AssetBrowser::remember_chain() {
    chain_.clear();
    const engine_core::InstanceId assets = world_.service("Assets");
    for (engine_core::InstanceId at = world_.parent(folder_);
         at != assets && at != 0 && at != engine_core::DataModel::kNoParent; at = world_.parent(at)) {
        chain_.insert(chain_.begin(), at);
    }
}

bool AssetBrowser::refresh() {
    if (!can_open(folder_)) {
        engine_core::InstanceId fallback = world_.service("Materials");
        for (auto it = chain_.rbegin(); it != chain_.rend(); ++it) {
            if (can_open(*it)) {
                fallback = *it;
                break;
            }
        }
        folder_ = fallback;
        remember_chain();
    }
    const std::uint64_t tree = world_.tree_revision();
    const bool changed = tree != seen_tree_ || folder_ != seen_folder_;
    seen_tree_ = tree;
    seen_folder_ = folder_;
    return changed;
}
```

Call `remember_chain()` at the end of the constructor and whenever `open`, `back`, or `forward` changes `folder_`, and declare it private in the header.

In `Preferences.hpp` add:

```cpp
    // The Assets pane's view: "icons", "list", or "columns". "icons" when none
    // was chosen or the file holds another value.
    std::string assets_view() const;
    void set_assets_view(const std::string& view);
```

and in `Preferences.cpp`:

```cpp
std::string Preferences::assets_view() const {
    const engine_core::JsonValue* view = root_.find("assetsView");
    if (view != nullptr && view->is_string()) {
        const std::string& text = view->as_string();
        if (text == "icons" || text == "list" || text == "columns") {
            return text;
        }
    }
    return "icons";
}

void Preferences::set_assets_view(const std::string& view) {
    root_.set("assetsView", engine_core::JsonValue::string(view));
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --config Release --target engine-tests studio-tests --parallel && build/Release/engine-tests.exe && build/Release/studio-tests.exe`
Expected: exit 0, no `FAIL`.

- [ ] **Step 5: Commit**

```bash
git add -A src/ide/AssetBrowser.hpp src/ide/AssetBrowser.cpp src/ide/Preferences.hpp src/ide/Preferences.cpp tests CMakeLists.txt
git commit -m "Add AssetBrowser, the Assets pane's model, and remember its view

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: The Assets pane

**Files:**
- Create: `src/ide/IdeAssets.hpp`, `src/ide/IdeAssets.cpp`
- Modify: `src/ide/IdeLayout.hpp` (`make_assets`, `assets_window_`), `src/ide/IdeLayout.cpp` (window entry), `src/ide/IdeLayoutProject.cpp` (`make_assets`)
- Modify: `CMakeLists.txt` (add `src/ide/IdeAssets.cpp` to `STUDIO_SOURCES`; new `assets-tests` program with `tests/AssetsPaneTest.cpp`, linked to `studio`, added to the warnings list and the ctest list)
- Create: `tests/AssetsPaneTest.cpp`
- Modify: `resources/themes/*.css` only if the pane needs a new `--ide-*` color (prefer existing ones: reuse the explorer's selection and row colors).

**Interfaces:**
- Consumes: `AssetBrowser` (Task 10), `ExplorerHost` (existing: `run`, `run_many`, `enabled`, `notice`, `insert`, `rename`, `move`), `icon_view(class_name)`, `kInstanceDragFormat` (Task 9), `Preferences::assets_view`.
- Produces:

```cpp
namespace ide {

// What the Assets pane needs from the studio: the explorer's actions, and the
// view it last showed, to keep in preferences.json.
struct AssetsHost {
    ExplorerHost actions;
    std::function<std::string()> saved_view;
    std::function<void(const std::string&)> save_view;
};

class IdeAssets : public IdePane {
public:
    IdeAssets(engine_core::DataModel& world, AssetsHost host);

    AssetBrowser& browser();
    AssetView view() const;
    void setView(AssetView view);
    // Opens id when it can be opened; for tests and the path bar.
    bool openFolder(engine_core::InstanceId id);
    // The widget showing id in the current view, or null. For tests.
    jadefx::Node* itemNode(engine_core::InstanceId id) const;
    // Starts renaming id in place. For tests and the slow second click.
    void beginRename(engine_core::InstanceId id);

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;
};

}  // namespace ide
```

- [ ] **Step 1: Write the failing tests**

Create `tests/AssetsPaneTest.cpp`, modeled on `tests/ExplorerRenameTest.cpp`: a `Rig` with a `Game`, an `ExplorerHost` that applies actions straight to the game and records `notices`, `inserts`, `renames`, `moves`, and `batches` as that file's `Rig` does (its `move` calls `ide::move_set`), an `IdeAssets` in a headless `jadefx::Scene` of 640×400, and helpers `frame(at)`, `clickItem(id, at, clicks)`, `rightClickEmpty(at)`, `menuItem(label)`. Tests, each a function called from `main`:

- `starts_in_saved_view`: `saved_view` returns `"list"`; after one frame, `view() == AssetView::List`.
- `navigates`: click the sidebar item "Textures" (find it by label); `browser().folder() == game.service("Textures")`; double-click a Folder "Walls" (`clickItem(walls, t, 2)`); folder is Walls; click the "Assets" crumb... crumbs are `Assets › Textures › Walls`; clicking "Textures" goes up; the back button goes back to Walls.
- `three_views_same_folder`: with Textures open holding Brick and Rock textures, for each view set by the toggle buttons, `itemNode(brick) != nullptr` and `itemNode(rock) != nullptr`; `save_view` was called with `"icons"`, `"list"`, `"columns"` in turn.
- `selection_is_shared`: clicking Brick sets `game.selection().get() == {brick}`; Ctrl+click Rock adds it; `game.selection().set({rock})` shows Rock selected on the next frame (its node has the `selected` style class).
- `new_folder_and_kind`: right-click on empty space in Textures; the menu has "New Folder" and "New Texture"; choosing "New Texture" calls `host.insert("Texture", textures, ...)`; after the insert completes, the new item is selected and in rename.
- `rename_delete_cut_paste`: begin rename on Brick, type "Stone", Enter: `renames` has `{brick, "Stone"}`. Select Brick, press Delete: `batches` has `{"Delete", {brick}}`. Cut and Paste go through `host.run_many`/`host.run` with Cut and Paste.
- `refused_drop_says_why`: call the pane's drop handler for moving `walls` (holding a Texture) onto the Meshes sidebar item; `notices` has `Meshes holds Meshes and Folders`, and Walls did not move.
- `search_filters`: type "bri" in the search field; the view shows only matches; × clears.
- `pane_follows_tree`: create a Texture in the open folder directly in the game; after a frame, `itemNode` finds it; destroy the open folder; after a frame, the pane shows its category.
- `insert_through_pane`: choosing New Folder inside a Prefab calls `host.insert("Folder", crate, ...)`; the rig's insert calls `ide::insert_instance`, and `notices` gets `A Prefab holds only Models`.

Write each with concrete `Expect` calls on the values named.

- [ ] **Step 2: Run to verify failure**

Run: `cmake -S . -B build && cmake --build build --config Release --target assets-tests --parallel`
Expected: compile error, `IdeAssets.hpp` not found.

- [ ] **Step 3: Implement the pane**

Structure of `IdeAssets` (in `IdeAssets.cpp`), built in the constructor the way `IdeSearch` builds its header and body:

- A `jadefx::BorderPane` filling the pane (`Fill(*column)`), with:
  - Top: an `HBox` toolbar: back and forward `jadefx::Button`s (`‹`, `›`), a `jadefx::HBox` of crumb `jadefx::Button`s separated by `›` labels, a spacer, three toggle buttons (icons `Grid.png`, `hbox.png`, `vbox.png` through `icon_graphic`, tooltips "Icons", "List", "Columns") in a `jadefx::ToggleGroup`, and a search `jadefx::TextField` with a × `jadefx::Label` as the explorer's filter has.
  - Left: the sidebar, a `VBox` of an "Assets" heading `Label` and one row `HBox` (icon + label) per category. Hidden in Columns view.
  - Center: a `jadefx::ScrollPane` whose content is rebuilt by the current view:
    - Icons: a `jadefx::FlowPane(8, 8)` of tiles: a `VBox` 72 wide with `icon_view(class)` scaled to 40×40 and a `Label` of the name, centered.
    - List: a `VBox` whose first child is a header `HBox` of three `Button`s ("Name", "Kind", "Path"; a click calls `browser().set_sort(...)`, toggling `descending` when the same column is clicked again, with `▲`/`▼` after the active one), then one `HBox` row per `list_rows()` entry: indent `depth * 16`, a disclosure `Label` (`▸`/`▾`) for `opens` rows whose click calls `set_expanded`, the icon, then three labels with fixed widths 45% / 20% / 35% of the viewport.
    - Columns: an `HBox` of `VBox` columns 180 wide, one per `columns()` entry, each a list of row `HBox`es (icon, name, and `›` for rows that open); the row in each column that is on the path to the folder has the `assets-on-path` class; after them, a preview `VBox` for a single selected asset: a 64×64 icon, name, class, and its `Path` or each reference's name (read through `lua_class_find(...)->read` and `ref_label`).
  - Bottom: a status `Label`: `5 items`, or `5 items · Brick selected`.
- Each item node (tile, list row, column row) gets `getProperties()["asset-id"] = id` and the style class `assets-item`, and handlers:
  - `setOnMouseClicked`: button 0 with `clickCount == 2` and the row `opens`: `openFolder(id)`; button 0 once: select (shortcut toggles, shift extends from the last clicked item in the current row order), and if the item was already the only selection and 0.5 s or more passed since the previous click on it, begin rename after the double-click window, as `IdeExplorer::poll_clicks` does.
  - `setOnContextMenuRequested`: select the item if it is not selected, then show a `jadefx::ContextMenu`... use the same menu mechanism `IdeExplorer::show_menu` uses, with Rename, Cut, Paste, Delete (enabled per `host.actions.enabled`), running `host.actions.run_many` for Delete and Cut on the whole selection and `host.actions.run` for Paste on the item when it `opens`, else on the folder.
  - `setOnDragDetected`: `startDragAndDrop(TransferMode::Move | TransferMode::Link)`, and put `kInstanceDragFormat` with the selected ids (the dragged item's id alone when it is not selected), comma-joined.
  - For items that open, sidebar rows, and crumbs: `setOnDragOver` accepts `Move` when the dragboard has `kInstanceDragFormat`; `setOnDragDropped` parses the ids and asks, under a read lock, `world.parent_error(id, target)` for each; on the first refusal, `host.actions.notice(*error)` and stop; else `host.actions.move(ids, target)`.
- The empty area of the center has `setOnContextMenuRequested` with New Folder, New *kind* (from `browser().new_kind()`, omitted when empty), and Paste (into the folder). New calls `host.actions.insert(klass, folder, result)`, keeps the `InsertResult`, and in `layoutChildren` when `result->done`, selects the new id and calls `beginRename(id)`; when `result->error` is set, `host.actions.notice(result->error)`.
- Rename: a `jadefx::TextField` with class `assets-rename` laid over the item's label, all text selected, as `IdeExplorer::begin_rename` does; Enter calls `host.actions.rename(id, text)` when the text is not empty and differs, Escape or focus loss drops it.
- Keys (`handleKey`): Delete/Backspace deletes the selection; Enter on a single selected item renames it; Cmd/Ctrl+Up goes up one level; Cmd/Ctrl+[ and ] go back and forward; Escape clears the selection. Consume only what the pane handles.
- `layoutChildren`: take `DataModelLock lock(world_, DataModelLock::Read, kFrameLockWait)` (use the constant the explorer uses); if it is not owned, keep the last widgets and return. Call `browser_.refresh()`; if it returned true, or the selection revision changed, or the view or search or sort changed since the last build, rebuild the center and the crumbs from the browser, then release the lock. Mark selected items with the `selected` style class from `world_.selection().get()`.
- Style: add a stylesheet string for `.assets-item`, `.assets-item.selected`, `.assets-on-path`, `.assets-sidebar`, reusing existing theme variables the explorer uses for rows and selection (search `IdeExplorer.cpp` and `IdeTheme.cpp` for the variable names).

In `IdeLayout.hpp`, declare `std::shared_ptr<IdePane> make_assets();` and `WindowEntry* assets_window_ = nullptr;`. In `IdeLayoutProject.cpp`:

```cpp
std::shared_ptr<IdePane> IdeLayout::make_assets() {
    AssetsHost host;
    host.actions = explorer_host_;
    host.saved_view = [this] { return preferences_.assets_view(); };
    host.save_view = [this](const std::string& view) {
        preferences_.set_assets_view(view);
        std::string error;
        preferences_.save(error);
    };
    auto pane = jadefx::make<IdeAssets>(runner_.simulation().datamodel(), std::move(host));
    pane->setIconFile("AssetFolder.png");
    return pane;
}
```

The constructor currently builds `ExplorerHost host` as a local; keep a copy in a new member `ExplorerHost explorer_host_;` (declared in `IdeLayout.hpp`) right after it is filled, so `make_assets` can use the same actions. In `IdeLayout.cpp`, after the terminal entry:

```cpp
    assets_window_ = &keep_closed("Assets", "AssetFolder.png", [this] { return make_assets(); });
    // In with the console, as a project browser docks under the scene.
    assets_window_->home = terminal_window_->home;
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --config Release --target assets-tests studio-tests --parallel && build/Release/assets-tests.exe && build/Release/studio-tests.exe`
Expected: exit 0, no `FAIL`. `StudioLayoutTest` may list the Window menu's entries; if it fails only because "Assets" is new, add it to its expected list.

- [ ] **Step 5: Commit**

```bash
git add -A src/ide tests CMakeLists.txt resources/themes
git commit -m "Add the Assets pane: Icons, List, and Columns views

Window > Assets docks it beside the console. It browses the Assets tree,
makes and renames folders and assets, moves them by drag or cut and paste,
and shares the selection with the explorers and Properties.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: MCP, the READMEs, and a look at the running studio

**Files:**
- Modify: `tests/McpTest.cpp`
- Modify: `README.md`, `src/engine_core/README.md`, `src/engine_services/README.md`, `src/engine_instances/README.md`

- [ ] **Step 1: Write the MCP test**

In `tests/McpTest.cpp`, next to the existing tree-listing and move tests, add a check that the tool that lists the tree includes `Assets` and its five categories, and that the tool that sets a parent (or a property `Parent`) on a Texture to `Workspace` returns an error whose text contains `A Texture must be in Assets.Textures`. Use the exact tool names and call helpers the file already uses for the existing scene-service refusal (search the file for `cannot be moved`).

- [ ] **Step 2: Run it**

Run: `cmake --build build --config Release --target mcp-tests --parallel && build/Release/mcp-tests.exe`
Expected: pass. If a tool writes `Parent` without asking `parent_error` first, fix the tool to ask and return the message, as the scene-services commit did.

- [ ] **Step 3: Update the READMEs**

In `README.md`, "The place" section: after the list of four scene services, add a paragraph describing `Assets` and its categories as the spec's Goal does (a hidden tree; what each category holds; that no one can move, rename, or destroy them; that scripts reach them as `game.Assets.Textures`), the six asset classes with their properties, and that a reference is kept by GUID and reads `nil` when its target is gone. In "The studio" section, add a paragraph for Window > Assets describing the pane as the spec's item 26 does. In "Projects", note `src/Assets.assets/`. Keep the README's voice: short declarative sentences, no marketing.

In `src/engine_core/README.md`, add a line for `Containment` and `InstanceRef`, and that a saved property typed `Class?` is a reference held by GUID. In `src/engine_services/README.md` and `src/engine_instances/README.md`, list the new classes.

- [ ] **Step 4: Full test run**

Run: `make test` (or the cmake/ctest commands in the header)
Expected: every suite passes. Copy the ctest summary line into the report.

- [ ] **Step 5: Run the studio and look**

Use the `run` skill to launch the studio. Open Window > Assets. Make a Folder and a Texture in Textures, a Material in Materials, set the Material's DiffuseTexture by dragging the Texture onto its row in Properties, and switch through the three views. Take a screenshot of each view and look at them: the sidebar, crumbs, toggle, tiles, list columns, and column view with preview. Fix anything that is clipped, overlapping, or unreadable in the Light and Dark themes before committing.

- [ ] **Step 6: Commit**

```bash
git add README.md src/engine_core/README.md src/engine_services/README.md src/engine_instances/README.md tests/McpTest.cpp src
git commit -m "Document game services and the Assets pane, and cover them over MCP

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
