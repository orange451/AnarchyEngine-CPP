# Dragger Phase 1: Prerequisites and Archivable

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cache Core's id, keep Core edits from counting as place changes, and add `Archivable`, a property Properties never lists, that keeps an instance and its subtree out of saves.

**Architecture:** `DataModel::core()` keeps the id it found in `State` and checks it with `alive()`. `mark_authored_dirty` skips what Core holds. `Archivable` is a plain `bool` on `DataModel`, a Lua property on every instance, marked `hidden` so `read_sheet` leaves it out unless `with_hidden` (MCP) asks. `authored_tree` skips a non-archivable instance with its subtree.

**Tech Stack:** C++17, Luau, Catch2 sandbox, the properties-tests program.

**Spec:** `docs/superpowers/specs/2026-10-03-dragger-design.md` (Prerequisites, section 1)

## Global Constraints

- Build: `cmake --build build --parallel`. Sandbox: `./build/sandbox "[TAG]"`. Properties: `./build/properties-tests`. All: `(cd build && ctest -C Release)`.
- No new warnings in touched files. Comments in the codebase's style.
- `Archivable` is hidden from the Properties panel only. MCP (`read_sheet(..., true)`), scripts, and the command line see it.

## Review Focus

- A non-archivable instance under a saved one: the save must leave the whole subtree out, and the next disk sync (`apply_disk`) must not destroy the live instance (AR2b).
- Setting `Archivable` changes what a save writes, so it must make `unsaved()` true when the instance was on disk (AR2).
- An undo that revives a non-archivable instance brings it back archivable (records do not carry it). Accepted; noted.

---

### Task 1: Core's id is cached, and Core edits are not place edits

**Files:** `src/engine_core/DataModelState.hpp` (`core_id`), `src/engine_core/DataModel.cpp` (`core()`, `mark_authored_dirty`), `sandbox/core_tests.cpp`.

- [ ] **Step 1: Failing tests** — append to `sandbox/core_tests.cpp`:

```cpp
TEST_CASE("PR1 core() is Core's id through New, Open, Play, and Stop", "[PR1][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    const InstanceId core = game.core();
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    engine_core::Project::reset_place(game);
    REQUIRE(game.core() == core);
    engine_core::Project reopened = engine_core::Project::load(dir.path, game);
    game.start_simulation();
    REQUIRE(game.core() == core);
    game.stop_simulation();
    REQUIRE(game.core() == core);
}

TEST_CASE("PR2 a change in Core leaves the place's authored revision alone", "[PR2]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId tools = add_folder(game, "Tools", game.core());
    const std::uint64_t before = game.authored_revision();
    game.set_name(tools, "Renamed");
    add_folder(game, "More", tools);
    REQUIRE(game.authored_revision() == before);
}
```

- [ ] **Step 2:** Build and run `[PR1],[PR2]`. Expected: PR1 passes already (it pins behavior the cache must keep); PR2 fails on the revision.

- [ ] **Step 3: Implement.** `DataModelState.hpp`, after `place_slots`/`history_held`:

```cpp
    // Core's id once found. Core cannot move or be destroyed, so it holds.
    InstanceId core_id = 0;
```

`DataModel.cpp`:

```cpp
InstanceId DataModel::core() const {
    // Game makes Core once; until then, and in a DataModel that is not a Game, there is none.
    if (state_->core_id == 0 || !alive(state_->core_id)) {
        state_->core_id = service(kCoreClass);
    }
    return state_->core_id;
}
```

`mark_authored_dirty`, after the existing early return:

```cpp
    // Core is not the place: what changes there is never saved.
    if (core_holds(id)) {
        return;
    }
```

- [ ] **Step 4:** Run `[PR1],[PR2]` then `./build/sandbox`. Expected: all pass.
- [ ] **Step 5: Commit** `Cache Core's id, and leave Core's changes out of the place's revision`.

---

### Task 2: Archivable

**Files:** `src/engine_core/DataModel.hpp` (`archivable_`, accessors), `src/engine_core/DataModel.cpp` (Lua property), `src/engine_core/DataModelPlace.cpp` (`authored_tree`), `sandbox/archivable_tests.cpp` (new), `CMakeLists.txt`.

**Produces:** `bool DataModel::archivable() const;`, `void DataModel::set_archivable(bool);`.

- [ ] **Step 1: Failing tests** — create `sandbox/archivable_tests.cpp` (add it to the sandbox sources):

```cpp
// Archivable: whether a save writes an instance.

#include "support.hpp"

#include "Folder.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::InstanceId;

InstanceId add_folder(engine_core::DataModel& game, const char* name, InstanceId parent) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder.id();
}

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const auto& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("AR1 Archivable is true by default, and Lua reads and writes it", "[AR1]") {
    ScriptRig rig;
    add_script(rig.game, "Probe", R"(
        local folder = Instance.new("Folder")
        print("default", folder.Archivable)
        folder.Archivable = false
        print("set", folder.Archivable)
    )");
    rig.game.start_simulation();
    rig.frames(1);
    const auto output = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output, "default\ttrue\n"));
    REQUIRE(has_line(output, "set\tfalse\n"));
}

TEST_CASE("AR2 a save leaves out a non-archivable instance and its subtree, and Stop still restores it",
          "[AR2][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId kept = add_folder(game, "Kept", workspace);
    const InstanceId gone = add_folder(game, "Gone", workspace);
    add_folder(game, "Inner", gone);
    project.save();
    REQUIRE_FALSE(project.unsaved());
    game.set_archivable(gone, false);
    REQUIRE(project.unsaved());
    project.save();

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    REQUIRE(other.find_first_child(other.scene_service("Workspace"), "Kept") != 0);
    REQUIRE(other.find_first_child(other.scene_service("Workspace"), "Gone") == 0);

    game.start_simulation();
    game.set_name(gone, "Renamed");
    game.stop_simulation();
    REQUIRE(game.alive(gone));
    REQUIRE(game.name(gone) == "Gone");
    REQUIRE(game.alive(kept));
}

TEST_CASE("AR2b a disk sync does not destroy a live non-archivable instance", "[AR2b][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const InstanceId gone = add_folder(game, "Gone", game.scene_service("Workspace"));
    game.set_archivable(gone, false);
    project.save();
    project.apply_disk();
    REQUIRE(game.alive(gone));
}
```

`set_archivable` is a DataModel method taking `(InstanceId, bool)`, the shape `set_name` has. Use the real `find_first_child` signature from DataModel.hpp.

- [ ] **Step 2:** Build. Expected: compile errors, no `set_archivable`.

- [ ] **Step 3: Implement.** `DataModel.hpp` (public, near the name and guid accessors):

```cpp
    // Whether a save writes this instance and its subtree. Not saved itself,
    // not undone, and true for a new instance. The place capture keeps a
    // non-archivable instance, so Stop restores it like any other.
    bool archivable(InstanceId id) const;
    void set_archivable(InstanceId id, bool archivable);
```

and a private `bool archivable_ = true;` beside `name_`. `DataModel.cpp`:

```cpp
bool DataModel::archivable(InstanceId id) const {
    const DataModel* object = instance(id);
    return object == nullptr || object->archivable_;
}

void DataModel::set_archivable(InstanceId id, bool archivable) {
    DataModel* object = instance(id);
    if (object == nullptr || object->archivable_ == archivable) {
        return;
    }
    object->archivable_ = archivable;
    // What a save writes changed: the parent's folder gains or loses it.
    mark_authored_dirty(id);
    mark_authored_dirty(parent(id));
    emit_property("Archivable");
}
```

Use the property-changed emit the class already uses for its own reflected fields (see `Workspace::set_current_camera`'s `emit_property`); if `emit_property` is a member of the instance, call it on `object`. Register in `register_datamodel_lua` (fields count 5), with readers in the anonymous namespace there:

```cpp
bool read_lua_archivable(DataModel& world, DataModel& object, LuaSlot& out) {
    out.kind = LuaSlot::Kind::Bool;
    out.flag = world.archivable(object.id());
    return true;
}

bool write_lua_archivable(DataModel& world, DataModel& object, LuaSlot& in) {
    if (in.kind != LuaSlot::Kind::Bool) {
        in.error = "Archivable must be true or false";
        return false;
    }
    world.set_archivable(object.id(), in.flag);
    return true;
}
```

```cpp
        lua_property("Archivable", "boolean", true, read_lua_archivable, write_lua_archivable),
```

`DataModelPlace.cpp` `authored_tree`, edit-mode loop: skip a child that is not archivable, as Core is skipped:

```cpp
                if (object == nullptr || child == core() || !object->archivable_) {
                    continue;
                }
```

- [ ] **Step 4:** Run `[AR1],[AR2],[AR2b]`, then `./build/sandbox`. Expected: pass. If AR2b fails, `apply_disk` treats a live instance missing from the authored tree as deleted on disk; make its walk skip non-archivable instances the same way, and say so in the ledger.
- [ ] **Step 5: Commit** `Add Archivable: a save leaves out an instance that is not archivable`.

---

### Task 3: Properties never lists Archivable

**Files:** `src/engine_core/LuaApi.hpp` (`LuaField::hidden`, `lua_hidden`), `src/engine_core/DataModel.cpp` (mark Archivable), `src/ide/PropertySheet.cpp` (`read_sheet` skips hidden unless `with_hidden`), `tests/PropertiesTest.cpp`.

- [ ] **Step 1: Failing test** — in `tests/PropertiesTest.cpp`, near the `read_sheet` unit checks (:999), add:

```cpp
    {
        const ide::PropertySheet panel = ide::read_sheet(rig.game, ids);
        Expect(panel.find("Archivable") == nullptr, "the panel never lists Archivable");
        const ide::PropertySheet mcp = ide::read_sheet(rig.game, ids, true);
        Expect(mcp.find("Archivable") != nullptr, "MCP still reads Archivable");
    }
```

- [ ] **Step 2:** Build and run `./build/properties-tests`. Expected: "the panel never lists Archivable" fails.

- [ ] **Step 3: Implement.** `LuaApi.hpp`, in `LuaField` after `shown_when_items`:

```cpp
    // Never a row in the Properties panel, though scripts, the command line,
    // and MCP read and write it. Archivable is one.
    bool hidden = false;
```

and after `lua_shown_when`:

```cpp
inline LuaField lua_hidden(LuaField field) {
    field.hidden = true;
    return field;
}
```

Wrap the Archivable property: `lua_hidden(lua_property("Archivable", ...))`. In `read_sheet`, where a candidate field becomes a row, skip it when `field.hidden && !with_hidden`.

- [ ] **Step 4:** Run `./build/properties-tests`, `./build/sandbox`, and `(cd build && ctest -C Release)`. Expected: all pass.
- [ ] **Step 5: Commit** `Keep Archivable out of the Properties panel`.
