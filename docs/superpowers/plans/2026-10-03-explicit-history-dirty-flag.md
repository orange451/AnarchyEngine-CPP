# Explicit Change History and the Dirty Flag Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ChangeHistoryService records only inside a recording someone explicitly opened, the place is "unsaved" exactly when history has had an update since the last save or load, and Lua gets the `ChangeHistoryService` API.

**Architecture:** The implicit gesture (a recording that opened itself on the first unrecorded write) is removed; every IDE command opens its own recording through a small RAII guard. An atomic dirty flag on `ChangeHistoryService` replaces the key-by-key diff in `Project::unsaved()` and the fingerprint bookkeeping in `IdeLayout`. `Camera` stops being a special case: its Transform records like any other property, and flying the view is simply a write no recording covers. The Lua service is a thin binding over the existing C++ methods and `HistorySignal`s.

**Tech Stack:** C++ (CMake, Catch2 v3 for `sandbox`, hand-rolled harnesses for the `tests/` suites), Luau.

**Spec:** `docs/superpowers/specs/2026-10-03-explicit-history-dirty-flag-design.md`

## Global Constraints

- The Mac toolchain is Xcode 13 / libc++ 13. Use nothing from the standard library newer than what that ships.
- Match the surrounding code: comments are plain sentences that say what the code does or why, at the density the file already has. No comment that only restates the line under it.
- History records a mutation only while a recording is open. Nothing may reintroduce a recording that opens itself.
- Recording names shown in the undo menu stay as they are today: Delete, Copy, Duplicate, Cut, Paste, Move, Rename, Add Model, Set `<part>` / Clear `<part>`, Add as GameObject, Import Models / Import Assets / Import Sounds / Import Textures, Insert `<class>`, Edit Script, Edit CSS, Replace in Scripts, Type Text, Changes from Disk, Set `<property>`.
- The dirty flag is cleared only by `Project::save_tree` and `Rebuild::finish`. Nothing in `src/ide/` clears it.
- `resources/plugins/SceneCamera.luau` and `resources/plugins/MoveTool.luau` are not edited.
- Lua API names are Roblox's: `TryBeginRecording`, `FinishRecording`, `IsRecordingInProgress`, `SetWaypoint`, `Undo`, `Redo`, `GetCanUndo`, `GetCanRedo`, `ResetWaypoints`, `OnUndo`, `OnRedo`, `OnRecordingStarted`, `OnRecordingFinished`, `Enum.FinishRecordingOperation.Commit` / `.Cancel`. `SetEnabled` is not bound.
- Build: `cmake --build build --parallel --target <target>`. Run a sandbox test from the repo root: `./build/sandbox "[TAG]"`. Run an IDE suite from the repo root: `./build/<suite>`. Everything: `make test`.
- Work on a branch, not on `master`. Commit messages are one sentence in the repo's style (see `git log --oneline`), ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Three refinements of the spec, decided while planning

1. **The flag is set when a mutation enters an edit-mode recording, not when the recording commits.** A cancelled recording, or one that commits nothing, puts the flag back to what it was when the recording opened. Every outcome the spec lists is unchanged (commit dirties, cancel does not, empty does not). The difference is a recording that is never finished, as a plugin that errors after `TryBeginRecording` leaves: the place still reads dirty, so the save prompt is not lost.
2. **`Project::place_fingerprint` stays.** Three sandbox tests (CO-series in `core_tests.cpp`, SC10 and SC22 in `scene_camera_tests.cpp`) use it to prove a write does not change what a save would write. Only the IDE's use of it goes. The spec allows this ("unless a test depends on it for something other than the title").
3. **`DataModel::set_archivable` joins `set_guid` and `set_extra_property`** as a write that changes a file without entering history, so it also marks the place dirty. The spec's list was by example.

## Review Focus

- **A recording opened and never finished** (a plugin errors after `TryBeginRecording`): edits made while it is open must still make the place read dirty. Test H29, Task 4.
- **An IDE command issued while another recording is open** (a drag in progress): its writes must not be lost or left uncommitted; they join the open recording and commit with it. Test H26, Task 1.
- **Undoing a "Changes from Disk" step on a clean place:** memory now differs from disk, so the place must read dirty. Assertion added to the disk-apply test, Task 4.
- **A save that fails partway:** the place must stay dirty. Assertion added to P17, Task 4.
- **`FinishRecording` with an id that names no open recording, or a bad operation:** the first is a no-op, the second an argument error a `pcall` catches; neither closes or corrupts an open recording. Test HL4, Task 6.

## File Structure

| File | Change |
| --- | --- |
| `src/ide/ScopedRecording.hpp` | Create. The RAII guard IDE commands and MCP tools open a recording with. |
| `src/ide/IdeLayoutInternal.hpp`, `IdeLayoutEditing.cpp`, `IdeLayoutProject.cpp`, `IdeLayout.cpp`, `McpTools.cpp` | Replace pending-gesture/`CloseGesture` pairs with `ScopedRecording`. |
| `src/engine_services/ChangeHistoryService.hpp/.cpp` | Remove the implicit gesture; add the dirty flag; register the Lua class's signals. |
| `src/engine_core/DraggerWorld.cpp`, `src/ide/PropertySheet.cpp`, `src/engine_core/Project.cpp` | Drop `end_gesture()` calls; `Project` clears and restores the flag and `unsaved()` reads it. |
| `src/engine_core/DataModel.hpp/.cpp` | `set_transform` always records; `note_unrecorded_edit` for the three history-bypassing writers. |
| `src/engine_instances/GameObject.hpp`, `Camera.hpp` | Delete `transform_in_history`. |
| `src/ide/IdeLayout.hpp/.cpp`, `IdeLayoutProject.cpp` | Title follows the flag; fingerprint, revision, and throttle members go. |
| `src/engine_datatypes/Enum.hpp/.cpp` | `Enum.FinishRecordingOperation`. |
| `src/engine_core/LuaApi.hpp/.cpp`, `ScriptBindings.hpp/.cpp`, `ScriptRuntime.hpp/.cpp` | The Lua service: host signals, methods, docs. |
| `src/ide/McpServer.cpp`, `src/engine_services/README.md` | Say what is and is not an undo step. |
| `sandbox/support.hpp` | `begin_step` / `end_step` test helpers. |
| `sandbox/*.cpp`, `tests/*.cpp` | Migrate off the implicit gesture; new tests per task. |
| `sandbox/history_lua_tests.cpp`, `CMakeLists.txt` | Create the Lua binding tests and add them to the `sandbox` target. |

---

### Task 1: IDE commands open their own recordings

Behaviour-preserving. The implicit gesture still exists after this task; each IDE command just stops depending on it.

**Files:**
- Create: `src/ide/ScopedRecording.hpp`
- Modify: `src/ide/IdeLayoutInternal.hpp:146` (delete `CloseGesture`, include the new header)
- Modify: `src/ide/IdeLayoutEditing.cpp` (sites at 128/134, 256/263, 295/301, 345/356, 379/383, 432/439, 449/452, 475/477, 585/588, 599/602)
- Modify: `src/ide/IdeLayoutProject.cpp` (85/96, 360/366)
- Modify: `src/ide/IdeLayout.cpp:167-168`
- Modify: `src/ide/McpTools.cpp` (522, 963/972, 990/992, 1064-1072)
- Test: `sandbox/history_tests.cpp`

**Interfaces:**
- Produces: `ide::ScopedRecording(engine_core::DataModel& world, std::string name)`; destructor commits. Used by every later task that touches an IDE command.

- [ ] **Step 1: Write the failing tests**

Add to the end of `sandbox/history_tests.cpp`, and `#include "ide/ScopedRecording.hpp"` beside the existing `ide/InputRouter.hpp` include:

```cpp
TEST_CASE("H25 a ScopedRecording is one named step, committed when it leaves scope", "[H25][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    {
        ide::ScopedRecording step(game, "Rename");
        REQUIRE(game.history().is_recording_in_progress());
        game.set_name(part.id(), "A");
        game.set_name(part.id(), "B");
    }
    REQUIRE_FALSE(game.history().is_recording_in_progress());
    REQUIRE(game.history().can_undo().second == "Rename");
    game.history().undo();
    REQUIRE(game.name(part.id()) == "Brick");
}

TEST_CASE("H26 a ScopedRecording inside an open recording joins it and leaves it open", "[H26][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    const std::optional<std::string> drag = game.history().try_begin_recording("Move");
    REQUIRE(drag.has_value());
    {
        // As an IDE command issued while a drag is in progress.
        ide::ScopedRecording step(game, "Rename");
        game.set_name(part.id(), "A");
    }
    REQUIRE(game.history().is_recording_in_progress(*drag));
    game.history().finish_recording(*drag, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(game.history().can_undo().second == "Move");
    game.history().undo();
    REQUIRE(game.name(part.id()) == "Brick");
}
```

Add `#include <optional>` to the file's includes.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: the build fails, `ide/ScopedRecording.hpp` not found.

- [ ] **Step 3: Create the guard**

`src/ide/ScopedRecording.hpp`:

```cpp
#pragma once

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"

#include <optional>
#include <string>
#include <utility>

namespace ide {

// One IDE command's undo step. Opens a recording, and commits it when the
// command returns, by any path. When another recording is already open, or
// history is off, it opens nothing and the writes go where they would have.
class ScopedRecording {
public:
    ScopedRecording(engine_core::DataModel& world, std::string name) : history_(world.history()) {
        // An implicit recording left open would refuse this one. Goes with the implicit gesture.
        history_.end_gesture();
        id_ = history_.try_begin_recording(std::move(name));
    }

    ~ScopedRecording() {
        if (id_) {
            history_.finish_recording(*id_, engine_core::FinishRecordingOperation::Commit);
        }
    }

    ScopedRecording(const ScopedRecording&) = delete;
    ScopedRecording& operator=(const ScopedRecording&) = delete;

private:
    engine_core::ChangeHistoryService& history_;
    std::optional<std::string> id_;
};

}  // namespace ide
```

`end_gesture()` commits only an *implicit* recording, so H26's explicit "Move" recording is left alone and `try_begin_recording` returns nullopt, which is the behaviour H26 asserts.

- [ ] **Step 4: Run the two tests**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[H25],[H26]"`
Expected: both pass.

- [ ] **Step 5: Convert the IDE sites**

In `src/ide/IdeLayoutInternal.hpp`, delete line 146 (`inline void CloseGesture(...)`) and add `#include "ScopedRecording.hpp"` to its includes.

Every site has one of three shapes. The rule for all of them: delete `world.history().set_pending_gesture(X);` and the matching `CloseGesture(world);`, and put `ScopedRecording step(world, X);` where the first write group begins, in a scope that ends where `CloseGesture` was.

**Shape A, straight line** (most sites). `IdeLayoutEditing.cpp` rename, currently:

```cpp
        world.history().set_pending_gesture("Rename");
        world.set_name(id, name);
        CloseGesture(world);
    });
```

becomes:

```cpp
        ScopedRecording step(world, "Rename");
        world.set_name(id, name);
    });
```

Where code that is not part of the edit follows `CloseGesture` (a `world.selection().set(made)`, a `toast_later`, a result stored for the caller), leave it after the guard in the same scope: none of it is a DataModel mutation history records.

Apply Shape A at:

| File:line (pending / close) | Name |
| --- | --- |
| `IdeLayoutEditing.cpp` 295 / 301 | `"Duplicate"` |
| `IdeLayoutEditing.cpp` 345 / 356 | `"Cut"` |
| `IdeLayoutEditing.cpp` 379 / 383 | `"Paste"` |
| `IdeLayoutEditing.cpp` 432 / 439 | `"Paste"` |
| `IdeLayoutEditing.cpp` 449 / 452 | `"Move"` |
| `IdeLayoutEditing.cpp` 475 / 477 | `"Rename"` |
| `IdeLayoutEditing.cpp` 585 / 588 | `"Add Model"` |
| `IdeLayoutEditing.cpp` 599 / 602 | `std::string(target != 0 ? "Set " : "Clear ") + model_part_name(part)` |
| `IdeLayoutProject.cpp` 85 / 96 | `"Add as GameObject"` |
| `IdeLayoutProject.cpp` 360 / 366 | the existing four-way `models ? "Import Models" : ...` expression, unchanged |

**Shape B, a loop that opened the gesture on its first write.** `IdeLayoutEditing.cpp:118-135` (Delete) becomes:

```cpp
        ScopedRecording step(world, "Delete");
        for (std::uint32_t id : ids) {
            // A selected child is already gone with its selected parent.
            if (!world.alive(id)) {
                continue;
            }
            if (std::optional<std::string> error = world.destroy_error(id)) {
                toast_later(this, alive, std::move(*error));
                continue;
            }
            world.destroy_tree(id);
        }
    });
```

Delete the `bool any` declaration above the loop and both of its uses. A recording that ends with no mutations pushes no waypoint, so an all-refused delete still leaves no undo step.

`IdeLayoutEditing.cpp:251-265` (dropping the instances a cut was holding when a copy replaces them) becomes:

```cpp
        runner_.simulation().on_simulation([dropped](engine_core::DataModel& world) {
            ScopedRecording step(world, "Copy");
            for (engine_core::InstanceId id : dropped) {
                if (world.alive(id) && world.parent(id) == engine_core::DataModel::kNoParent) {
                    world.destroy_tree(id);
                }
            }
        });
```

**Shape C, the step must close before later code runs.** `IdeLayout.cpp:163-169` (host.insert). The name was the implicit default; it becomes the name MCP already uses:

```cpp
            [class_name = std::move(class_name), asked = parent, result](engine_core::DataModel& world) {
                std::string error;
                engine_core::InstanceId made = 0;
                {
                    ScopedRecording step(world, "Insert " + class_name);
                    made = insert_instance(world, class_name, asked, error);
                }
                if (result) {
```

`McpTools.cpp`: delete its own `CloseGesture` (lines 521-522, comment included), add `#include "ScopedRecording.hpp"`, and:

`CreateInstance` (963-976) becomes:

```cpp
        engine_core::InstanceId made_id = 0;
        {
            ScopedRecording step(world, "Insert " + class_name);
            DataModel* made = engine_core::lua_create_instance(world, class_name.c_str());
            if (made == nullptr) {
                throw std::runtime_error("Could not create " + class_name + ".");
            }
            if (!name.empty()) {
                world.set_name(made->id(), name);
            }
            world.set_parent(made->id(), parent_id);
            made_id = made->id();
        }
        // An edit while stopped is part of the place, as the explorer's insert is.
        if (!world.simulation_running()) {
            world.capture_place();
        }
        return Brief(world, made_id);
```

The comment above it changes from "Refused before the gesture opens, so a full place leaves nothing pending." to "Refused before the recording opens, so a full place leaves no empty step."

`DeleteInstance` (990-992) becomes:

```cpp
        {
            ScopedRecording step(world, "Delete");
            world.destroy_tree(id);
        }
        return out;
```

`ImportAssets` (1064-1072): the try/catch existed only to close the gesture on a throw; the guard does that:

```cpp
        std::vector<McpImport> imports;
        {
            ScopedRecording step(world, "Import Assets");
            imports = place(world);
        }
```

- [ ] **Step 6: Check nothing in `src/ide` still uses the old pair**

Run: `grep -rnE "set_pending_gesture|CloseGesture" src/ide`
Expected: no output.

- [ ] **Step 7: Build and run every suite**

Run: `make test`
Expected: every suite passes, as before this task.

- [ ] **Step 8: Commit**

```bash
git add src/ide/ScopedRecording.hpp src/ide/IdeLayoutInternal.hpp src/ide/IdeLayoutEditing.cpp src/ide/IdeLayoutProject.cpp src/ide/IdeLayout.cpp src/ide/McpTools.cpp sandbox/history_tests.cpp
git commit -m "$(cat <<'EOF'
Have each IDE command open its own recording, through ScopedRecording

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: History records only inside a recording

The engine change and the test migration are one task: once `end_gesture` is deleted, the compiler lists every caller, and no intermediate state is both green and meaningful.

**Files:**
- Modify: `src/engine_services/ChangeHistoryService.hpp`, `ChangeHistoryService.cpp`
- Modify: `src/ide/ScopedRecording.hpp` (drop the transitional line)
- Modify: `src/ide/PropertySheet.cpp:584`, `src/engine_core/DraggerWorld.cpp:135-137`, `src/engine_core/Project.cpp:2318-2319`
- Modify: `sandbox/support.hpp`
- Modify (migrate): `sandbox/history_tests.cpp` (35 sites), `core_tests.cpp` (13), `game_services_tests.cpp` (12), `prefab_render_tests.cpp` (11), `dense_views_tests.cpp` (9), `project_tests.cpp` (9), `scene_camera_tests.cpp` (7), `light_tests.cpp` (6), `skybox_tests.cpp` (4), `move_tool_tests.cpp` (4), `scene_services_tests.cpp` (4), `gui_tests.cpp` (3), `dragger_tests.cpp` (3), `tests.cpp` (3), `mesh_shapes_tests.cpp` (2), `camera_tests.cpp` (2)
- Modify (migrate): `tests/FindReplaceTest.cpp:409`, `tests/PropertiesTest.cpp:275`, `tests/ScriptTabsTest.cpp:214,239`, `tests/McpTest.cpp:398,460`

**Interfaces:**
- Consumes: `ide::ScopedRecording` from Task 1.
- Produces: `ChangeHistoryService` without `set_pending_gesture`, `end_gesture`; `wants_mutation()` true only while a recording is open. Sandbox helpers `begin_step(engine_core::DataModel&, std::string name = "Edit")` and `end_step(engine_core::DataModel&)`, used by every later task's tests.

- [ ] **Step 1: Add the test helpers**

In `sandbox/support.hpp`, add `#include "ChangeHistoryService.hpp"` and `#include <catch2/catch_test_macros.hpp>` to the includes, and after `workspace_of`:

```cpp
// A test's stand-in for an IDE command: the writes between begin_step and
// end_step are one undo step. A write outside them is not undoable.
inline void begin_step(engine_core::DataModel& game, std::string name = "Edit") {
    REQUIRE(game.history().try_begin_recording(std::move(name)).has_value());
}

// Commits the step begin_step opened.
inline void end_step(engine_core::DataModel& game) { game.history().seal_edit_recording(); }
```

- [ ] **Step 2: Write the failing tests**

In `sandbox/history_tests.cpp`, replace the whole of H4 (lines 115-132) with:

```cpp
TEST_CASE("H4 writes inside one recording coalesce into one named step", "[H4][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    begin_step(game, "Rename");
    game.set_name(part.id(), "A");
    game.set_name(part.id(), "B");
    end_step(game);

    REQUIRE(game.history().can_undo().second == "Rename");
    game.history().undo();
    REQUIRE(game.name(part.id()) == "Brick");
    REQUIRE_FALSE(game.history().can_undo().first);
    game.history().redo();
    REQUIRE(game.name(part.id()) == "B");
}

TEST_CASE("H4b a write outside a recording is not an undo step, but a save writes it", "[H4b][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    game.clear_authored_dirty();

    REQUIRE_FALSE(game.history().wants_mutation());
    game.set_name(part.id(), "A");
    part.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));

    REQUIRE_FALSE(game.history().is_recording_in_progress());
    REQUIRE_FALSE(game.history().can_undo().first);
    const engine_core::AuthoredDirty dirty = game.authored_dirty();
    REQUIRE(std::find(dirty.ids.begin(), dirty.ids.end(), part.id()) != dirty.ids.end());
}
```

`<algorithm>` is already included.

- [ ] **Step 3: Run H4b to see it fail**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[H4b]"`
Expected: FAIL at `REQUIRE_FALSE(game.history().wants_mutation())`, and again at `is_recording_in_progress()`: the rename opened an implicit recording.

- [ ] **Step 4: Remove the implicit gesture from the service**

`src/engine_services/ChangeHistoryService.hpp`:

- Change the class comment to:

```cpp
// Edit undo for DataModel mutations. A mutation is recorded only while a
// recording is open; a write no recording covers is not an undo step. A
// second stack holds waypoints committed during play and is dropped on stop.
// Text keystrokes do not come here.
```

- Change `try_begin_recording`'s comment to `// Null when a recording is already open or history is disabled.` (unchanged) and `set_waypoint`'s to `// Commits the open recording under this name. Does nothing when none is open.`
- Delete these declarations and their comments: `set_pending_gesture`, `end_gesture`, `open_implicit`.
- Replace the comment and declaration of `wants_mutation`:

```cpp
    // True when a mutator should capture: a recording is open, history is on,
    // and no undo is being applied.
    bool wants_mutation() const;
```

- Replace `forget_core`'s comment with `// Drops what the open recording holds about instances now in Core.`
- In `struct Recording`, delete `bool implicit = false;`.
- Delete the member `std::string pending_;`.

`src/engine_services/ChangeHistoryService.cpp`:

- Delete `default_gesture` (lines 12-43), `set_pending_gesture`, `open_implicit`, and `end_gesture`.
- In `try_begin_recording`, delete `recording.implicit = false;`.
- Replace `wants_mutation`, `forget_core`, and `note`:

```cpp
bool ChangeHistoryService::wants_mutation() const {
    return enabled_ && applying_ == 0 && game_ != nullptr && recording_.has_value();
}
```

```cpp
void ChangeHistoryService::forget_core() {
    if (!recording_) {
        return;
    }
    std::vector<Mutation>& list = recording_->mutations;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [this](const Mutation& mutation) { return game_->core_holds(mutation.id); }),
               list.end());
}
```

```cpp
void ChangeHistoryService::note(Mutation mutation) {
    if (!enabled_ || applying_ != 0 || game_ == nullptr || !recording_) {
        return;
    }
    // Core is the studio's, not the place's: what happens there is never an
    // undo step, and what entered it leaves the open recording.
    if (game_->core_holds(mutation.id)) {
        forget_core();
        return;
    }
    push_or_coalesce(std::move(mutation));
}
```

- `set_waypoint` becomes:

```cpp
void ChangeHistoryService::set_waypoint(std::string name) {
    if (!recording_) {
        return;
    }
    recording_->name = name;
    recording_->display_name = std::move(name);
    finish_recording(recording_->id, FinishRecordingOperation::Commit);
}
```

- In `reset_waypoints`, delete `pending_.clear();`.
- If `lua_property_name` was used only by `default_gesture`, delete `#include "PropertyReflection.hpp"`.

- [ ] **Step 5: Remove the engine and IDE callers**

- `src/ide/ScopedRecording.hpp`: delete the comment and the `history_.end_gesture();` line in the constructor.
- `src/ide/PropertySheet.cpp:582-585`: delete `history.end_gesture();` and the comment above the block ("A gesture left open by an earlier edit is its own waypoint, not part of this one.").
- `src/engine_core/DraggerWorld.cpp:135-137`: delete the two comment lines ("An edit that left its undo step open, as the command line's do, is its own step, not part of the drag's: close it first.") and `game.history().end_gesture();`.
- `src/engine_core/Project.cpp:2318-2319`: delete the comment ("An edit still open is its own step; this one is "Changes from Disk".") and `world.history().end_gesture();`.

- [ ] **Step 6: Migrate the sandbox tests**

Build `sandbox`; every remaining use of the removed API is a compile error. Apply these rules at each one. `game` below stands for whatever the test calls its DataModel (`rig.game`, `scene.game`, `move.rig.game`, …).

| Was | Becomes |
| --- | --- |
| `game.history().set_pending_gesture(N);` … writes … `game.history().end_gesture();` | `begin_step(game, N);` … writes … `end_step(game);` |
| writes … `end_gesture();` and a later line undoes that step or reads `can_undo()` for it | `begin_step(game);` before the writes, `end_step(game);` where `end_gesture()` was. Pass the name the test asserts if it asserts one (`"Move"`, `"Rename"`, `"Delete"`). |
| `end_gesture();` that only closes setup writes: it is followed by `reset_waypoints()`, or its comment says it keeps setup out of the next step | Delete the line and any comment that explained it. The setup writes were never recorded. |
| `close_gesture(game)` in `history_tests.cpp` | Delete the helper at line 26, then apply the rows above. |

After the mechanical pass, these tests need their meaning restated, because they tested the implicit gesture itself:

`history_tests.cpp` H1: the create must be inside a step for undo to remove it.

```cpp
    engine_core::Game game;
    begin_step(game, "Insert GameObject");
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    game.set_name(id, "Brick");
    const engine_core::Matrix4 placed = engine_core::matrix4_translation(1.f, 2.f, 3.f);
    part.set_transform(placed);
    end_step(game);
```

`core_tests.cpp` CO4b (lines 204-221): the plugin's writes are outside any recording; the user's edit is its own step.

```cpp
    // As a plugin does: made with no parent, named, then put in Core, outside any recording.
    const InstanceId tool = add_folder(game, "Tool", engine_core::DataModel::kNoParent);
    game.set_name(tool, "Dragger");
    game.set_parent(tool, game.core());
    REQUIRE_FALSE(game.history().is_recording_in_progress());
    // The user's next edit is its own step, and undoing it leaves the tool alone.
    begin_step(game, "Insert Folder");
    const InstanceId part = add_folder(game, "Part", game.scene_service("Workspace"));
    end_step(game);
    REQUIRE(game.history().can_undo().first);
```

`core_tests.cpp` lines 186-200 (writes into Core, each followed by `end_gesture()`): wrap each write in its own `begin_step(game)` / `end_step(game)`, so the test still proves a write to Core *inside a recording* leaves no undo step.

`scene_camera_tests.cpp` SC10 (232, 238) and SC22 (385, 390): delete the four `end_gesture()` lines. `move_tool_tests.cpp:88`: delete `rig.game.history().end_gesture();`.

`project_tests.cpp:2203-2205` becomes:

```cpp
    begin_step(game, "Transform");
    game.game_object(a)->set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));
    end_step(game);
```

(The `unsaved()` assertions around it change in Task 4, not here.)

- [ ] **Step 7: Migrate the four `tests/` files**

- `tests/FindReplaceTest.cpp:408-409`: delete the comment and the `end_gesture()` line.
- `tests/PropertiesTest.cpp:275`: delete the line.
- `tests/McpTest.cpp:397-398` and `:460`: delete the comment and both lines.
- `tests/ScriptTabsTest.cpp:213-214`: delete the comment and the line. Lines 237-240 become:

```cpp
    engine.on_simulation([id](engine_core::DataModel& game) {
        const std::optional<std::string> step = game.history().try_begin_recording("Delete");
        game.destroy(id);
        if (step) {
            game.history().finish_recording(*step, engine_core::FinishRecordingOperation::Commit);
        }
    });
```

Add `#include <optional>` and `#include "ChangeHistoryService.hpp"` if the file lacks them.

- [ ] **Step 8: Check the old API is gone everywhere**

Run: `grep -rnE "end_gesture|set_pending_gesture|close_gesture|CloseGesture|open_implicit|default_gesture" src sandbox tests`
Expected: no output.

- [ ] **Step 9: Build and run every suite**

Run: `make test`
Expected: every suite passes. Where a test fails because a direct write it made is no longer undoable (a `can_undo()` that is now false, an `undo()` that now does nothing), wrap that write: `begin_step`/`end_step` in `sandbox`, or the `try_begin_recording` / `finish_recording` pair shown in Step 7 in `tests/`. Do not change an assertion to match the new result without wrapping, except in the tests Step 6 names.

- [ ] **Step 10: Commit**

```bash
git add -A src sandbox tests
git commit -m "$(cat <<'EOF'
Record history only inside an open recording, and drop the implicit gesture

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: A Camera's Transform is an ordinary property

**Files:**
- Modify: `src/engine_instances/GameObject.hpp:31-33`, `src/engine_instances/Camera.hpp:17,30-31`
- Modify: `src/engine_core/DataModel.cpp:695-699`
- Test: `sandbox/scene_camera_tests.cpp` (SC11), `sandbox/camera_tests.cpp`, `sandbox/move_tool_tests.cpp`

**Interfaces:**
- Consumes: `begin_step` / `end_step` from Task 2.
- Produces: `GameObject` and `Camera` without `transform_in_history()`.

- [ ] **Step 1: Write the failing tests**

Replace SC11 in `sandbox/scene_camera_tests.cpp` (lines 268-286) with:

```cpp
TEST_CASE("SC11 a Camera's Transform is an undo step only inside a recording", "[SC11]") {
    ScriptRig rig;
    engine_core::Camera& camera = add_camera(rig.game);
    rig.game.history().reset_waypoints();
    rig.game.clear_authored_dirty();

    // Flying: no recording is open, so nothing is undoable, but a save writes it.
    for (int step = 1; step <= 50; ++step) {
        camera.set_transform(engine_core::matrix4_translation(0.f, 0.f, static_cast<float>(step)));
    }
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    const engine_core::AuthoredDirty dirty = rig.game.authored_dirty();
    REQUIRE(std::find(dirty.ids.begin(), dirty.ids.end(), camera.id()) != dirty.ids.end());

    // An edit, as a dragger or the Properties panel makes: one step.
    const engine_core::Matrix4 flown = camera.transform();
    begin_step(rig.game, "Move");
    camera.set_transform(engine_core::matrix4_translation(5.f, 0.f, 0.f));
    end_step(rig.game);
    REQUIRE(rig.game.history().can_undo().second == "Move");
    rig.game.history().undo();
    REQUIRE(engine_core::same_matrix4(camera.transform(), flown));
}
```

Add `#include <algorithm>` to the file.

Add to `sandbox/move_tool_tests.cpp`, after MT6, using the next free `MT` number in the file for `MTn`:

```cpp
TEST_CASE("MTn dragging a Camera the view does not look through is one Move step", "[MTn]") {
    MoveRig move;
    // Where MT6's handles sat: the middle of its two parts. The same pixels hit the X arrow.
    engine_core::Camera& shot = move.rig.game.create<engine_core::Camera>();
    move.rig.game.set_name(shot.id(), "Shot");
    move.rig.game.set_parent(shot.id(), move.rig.game.scene_service("Workspace"));
    shot.set_transform(engine_core::matrix4_translation(0, 0, -10));
    move.rig.game.history().reset_waypoints();
    move.rig.game.selection().set({shot.id()});
    move.rig.frames(1);
    move.post(true, 150, 100);
    move.move(160, 100);
    move.move(170, 100);
    move.post(false, 170, 100);
    move.rig.frames(1);
    INFO(move.rig.runtime.last_error());
    REQUIRE(near(move.x_of(shot.id()), 2));
    REQUIRE(move.rig.game.history().can_undo().second == "Move");
    move.rig.game.history().undo();
    move.rig.frames(1);
    REQUIRE(near(move.x_of(shot.id()), 0));
}
```

Add to `sandbox/camera_tests.cpp`, after CAM4. This one pins behaviour that must survive the change; it passes before and after:

```cpp
TEST_CASE("CAM5 a camera flown outside any recording is written by the next save", "[camera][project]") {
    SimRole role;
    TempDir dir;
    const engine_core::Matrix4 flown = engine_core::matrix4_translation(3.f, 4.f, 5.f);
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        Camera* camera = workspace_cameras(project.datamodel())[0];
        camera->set_transform(flown);
        REQUIRE_FALSE(project.datamodel().history().can_undo().first);
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    REQUIRE(engine_core::same_matrix4(workspace_cameras(game)[0]->transform(), flown));
}
```

- [ ] **Step 2: Run them to see SC11 and MTn fail**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[SC11],[MTn],[camera]"`
Expected: SC11 and MTn FAIL at `can_undo().second == "Move"` (a Camera's Transform is not in history, so the recording is empty). CAM5 passes.

- [ ] **Step 3: Remove the special case**

`src/engine_instances/GameObject.hpp`: delete lines 31-33 (the comment and `virtual bool transform_in_history() const { return true; }`).

`src/engine_instances/Camera.hpp`: delete the override and its comment ("Flying the camera is looking around, not editing."). Replace the class comment's last line, "Its Transform writes are not undo steps (transform_in_history), though they save.", with:

```cpp
// Its Transform is an ordinary property: a write inside a recording is an undo
// step. Flying the view writes it outside any recording, so that is not one,
// though a save writes where the camera is.
```

`src/engine_core/DataModel.cpp:695-699`:

```cpp
    target->store_transform(transform);
    record_transform(id, previous, transform);
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[SC11],[MTn],[camera],[history]"`
Expected: all pass.

- [ ] **Step 5: Run every suite and commit**

Run: `make test`
Expected: every suite passes.

```bash
git add src/engine_instances/GameObject.hpp src/engine_instances/Camera.hpp src/engine_core/DataModel.cpp sandbox/scene_camera_tests.cpp sandbox/camera_tests.cpp sandbox/move_tool_tests.cpp
git commit -m "$(cat <<'EOF'
Record a Camera's Transform like any other, so only a recorded move is an undo step

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: The dirty flag, and `Project::unsaved()` reads it

**Files:**
- Modify: `src/engine_services/ChangeHistoryService.hpp`, `ChangeHistoryService.cpp`
- Modify: `src/engine_core/DataModel.hpp` (near 293-316), `DataModel.cpp` (`set_archivable` ~1590, `set_guid` ~1899, `set_extra_property` ~1952, `erase_extra_property` ~1966)
- Modify: `src/engine_core/Project.hpp:156-159`, `Project.cpp` (`Rebuild::finish` 1071, `unsaved` 1481-1508, `apply_disk` 2317-2325, end of `save_tree` ~2931)
- Test: `sandbox/history_tests.cpp`, `sandbox/project_tests.cpp`, `sandbox/archivable_tests.cpp:63`, `sandbox/game_services_tests.cpp:682`, `sandbox/scene_services_tests.cpp:418`, `tests/ConflictsTest.cpp:245`, `tests/SaveConflictTest.cpp`

**Interfaces:**
- Produces, on `ChangeHistoryService`: `bool dirty() const`, `void mark_dirty()`, `void mark_saved()`. `Project::unsaved()` returns `dirty()`. Tasks 5 and 7 read `dirty()`.

- [ ] **Step 1: Write the failing history tests**

Add to `sandbox/history_tests.cpp`:

```cpp
TEST_CASE("H27 the place is dirty once a recording holds a change, and after undo and redo", "[H27][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    REQUIRE_FALSE(game.history().dirty());

    // A write no recording covers does not dirty.
    game.set_name(part.id(), "Loose");
    REQUIRE_FALSE(game.history().dirty());

    begin_step(game, "Rename");
    game.set_name(part.id(), "A");
    end_step(game);
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    REQUIRE_FALSE(game.history().dirty());
    game.history().undo();
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    game.history().redo();
    REQUIRE(game.history().dirty());
}

TEST_CASE("H28 a cancelled recording, and one that commits nothing, leave the place clean", "[H28][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    const std::optional<std::string> cancelled = game.history().try_begin_recording("Rename");
    game.set_name(part.id(), "A");
    game.history().finish_recording(*cancelled, engine_core::FinishRecordingOperation::Cancel);
    REQUIRE(game.name(part.id()) == "Brick");
    REQUIRE_FALSE(game.history().dirty());

    begin_step(game, "Nothing");
    end_step(game);
    REQUIRE_FALSE(game.history().dirty());

    // There and back coalesces to no change at all.
    begin_step(game, "Rename");
    game.set_name(part.id(), "A");
    game.set_name(part.id(), "Brick");
    end_step(game);
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE_FALSE(game.history().dirty());
}

TEST_CASE("H29 a recording left open still makes the place dirty", "[H29][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    // As a plugin that errors after TryBeginRecording leaves it.
    REQUIRE(game.history().try_begin_recording("Stuck").has_value());
    game.set_name(part.id(), "A");
    REQUIRE(game.history().dirty());
}

TEST_CASE("H30 a play recording and Stop do not dirty the place", "[H30][history]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    game.capture_place();

    game.start_simulation();
    begin_step(game, "Play Move");
    part.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));
    end_step(game);
    REQUIRE_FALSE(game.history().dirty());
    game.history().undo();
    REQUIRE_FALSE(game.history().dirty());

    game.stop_simulation();
    REQUIRE_FALSE(game.history().dirty());
}

TEST_CASE("H31 writes that change a file without entering history dirty the place", "[H31][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    game.set_extra_property(part.id(), "Note", engine_core::JsonValue::string("kept"));
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    game.erase_extra_property(part.id(), "Note");
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    game.set_guid(part.id(), "abcd");
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    game.set_archivable(part.id(), false);
    REQUIRE(game.history().dirty());

    // Core is not the place.
    game.history().mark_saved();
    engine_core::GameObject& tool = game.create<engine_core::GameObject>();
    game.set_parent(tool.id(), game.core());
    game.set_extra_property(tool.id(), "Note", engine_core::JsonValue::string("x"));
    REQUIRE_FALSE(game.history().dirty());
}
```

H30 starts and stops play the way H6 does (`history_tests.cpp:179-214`); if H6 uses different calls than `start_simulation()` / `stop_simulation()` / `capture_place()`, use H6's.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: the build fails, `dirty` / `mark_saved` are not members of `ChangeHistoryService`.

- [ ] **Step 3: Add the flag to the service**

`ChangeHistoryService.hpp`: add `#include <atomic>`. After `reset_waypoints()`'s declaration:

```cpp
    // True when the place has changed since mark_saved: a mutation entered an
    // edit recording that was not then cancelled or emptied, an undo or redo
    // ran on the edit stack, or a writer called mark_dirty. Play does not
    // dirty: Stop puts the place back. Any thread may read it.
    bool dirty() const { return dirty_.load(std::memory_order_relaxed); }
    // For a write that changes what a save writes without entering history.
    void mark_dirty();
    // The place is what is on disk: a save wrote every file, or it was just built from disk.
    void mark_saved() { dirty_.store(false, std::memory_order_relaxed); }
```

In `struct Recording`, add:

```cpp
        // dirty() when it opened, which a cancel or an empty commit puts back.
        bool was_dirty = false;
```

Add the member `std::atomic<bool> dirty_{false};`.

`ChangeHistoryService.cpp`:

```cpp
void ChangeHistoryService::mark_dirty() {
    dirty_.store(true, std::memory_order_relaxed);
    // What a cancel reverts does not include this write, so it must not put the flag back.
    if (recording_) {
        recording_->was_dirty = true;
    }
}
```

In `try_begin_recording`, beside the other `recording.` assignments: `recording.was_dirty = dirty();`

At the end of `note()`, replacing the final `push_or_coalesce` line:

```cpp
    push_or_coalesce(std::move(mutation));
    // Now, not at the commit: a recording left open must not hide an edit from the save prompt.
    if (!playing()) {
        dirty_.store(true, std::memory_order_relaxed);
    }
```

In `finish_recording`, the two branches become:

```cpp
    const bool edit = !playing();
    if (op == FinishRecordingOperation::Cancel) {
        Waypoint inverse;
        inverse.mutations = std::move(recording.mutations);
        apply_waypoint(inverse, true);
        if (edit && !recording.was_dirty) {
            mark_saved();
        }
    } else if (!recording.mutations.empty()) {
        // ... the existing waypoint push, unchanged ...
    } else if (edit && !recording.was_dirty) {
        mark_saved();
    }
```

In `step()`, after `to.push_back(std::move(waypoint));`:

```cpp
    if (!playing()) {
        dirty_.store(true, std::memory_order_relaxed);
    }
```

- [ ] **Step 4: Mark the three history-bypassing writers**

`DataModel.hpp`, in the private section beside `record_transform`'s declaration:

```cpp
    // A write that changes what a save writes but is never an undo step: it
    // marks the save set and the place dirty. Not during play, not in Core.
    void note_unrecorded_edit(InstanceId id);
```

`DataModel.cpp`, after `mark_authored_dirty`:

```cpp
void DataModel::note_unrecorded_edit(InstanceId id) {
    mark_authored_dirty(id);
    if (id == kNoParent || state_->simulation_running || core_holds(id) || !state_->history) {
        return;
    }
    state_->history->mark_dirty();
}
```

Replace `mark_authored_dirty(id);` with `note_unrecorded_edit(id);` in `set_guid`, `set_extra_property`, and `erase_extra_property`. In `set_archivable`, replace the first of its two calls (`mark_authored_dirty(id);`) with `note_unrecorded_edit(id);` and leave the parent's.

In `DataModel.hpp`, change the comment on `set_extra_property` from "Marks the instance dirty. Not recorded in undo history." to "Marks the instance and the place dirty. Not an undo step."

- [ ] **Step 5: Run the history tests**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[history]"`
Expected: H27-H31 and every earlier H test pass.

- [ ] **Step 6: Write the failing project tests**

In `sandbox/project_tests.cpp`, lines 2206-2208 become:

```cpp
    REQUIRE(project.unsaved());
    // Undoing an edit is itself a change since the last save.
    game.history().undo();
    REQUIRE(project.unsaved());
```

At the end of that same TEST_CASE, after the assertion now at line ~2238 (`REQUIRE_FALSE(project.unsaved());` following the second `apply_disk()`), add:

```cpp
    // Undoing what came from disk leaves the place different from disk.
    REQUIRE(game.history().can_undo().second == "Changes from Disk");
    game.history().undo();
    REQUIRE(project.unsaved());
```

Add two test cases after it, numbered with the next free `P` numbers for `Pa` and `Pb`:

```cpp
TEST_CASE("Pa changes from disk leave a dirty place dirty, and a save cleans it", "[Pa][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, workspace_of(game), "A").id();
    const InstanceId b = add_part(game, workspace_of(game), "B").id();
    project.save();
    REQUIRE_FALSE(project.unsaved());

    begin_step(game, "Move");
    game.game_object(b)->set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));
    end_step(game);
    REQUIRE(project.unsaved());

    edit_key(dir.path / leaf(game, a), "Transform", translated(2, 2, 2));
    project.apply_disk();
    REQUIRE(project.unsaved());

    project.save();
    REQUIRE_FALSE(project.unsaved());
}

TEST_CASE("Pb an edit to a script's Source alone dirties the place", "[Pb][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Script& script = add_script(game, workspace_of(game), "Main", "print(1)\n");
    project.save();
    REQUIRE_FALSE(project.unsaved());

    begin_step(game, "Edit Script");
    script.set_source("print(2)\n");
    end_step(game);
    REQUIRE(project.unsaved());
}
```

In P17 ("a save that fails partway leaves the last save on disk"): put the edit it makes before the failing save inside `begin_step(game, "Edit")` / `end_step(game)`, and after the save that throws add `REQUIRE(project.unsaved());`.

Flip three assertions whose setup is a load that fills in what the files lacked, or a write that is not an undo step:

- `sandbox/game_services_tests.cpp:682`: `REQUIRE(project.unsaved());` → `REQUIRE_FALSE(project.unsaved());`, with the comment `// A load that filled in what the files lacked opens clean; the next save still writes them.` The `project.save()` and file assertions after it stay.
- `sandbox/scene_services_tests.cpp:418`: the same change and comment.
- `sandbox/archivable_tests.cpp:63` stays `REQUIRE(project.unsaved());`. It now passes through `note_unrecorded_edit`.

- [ ] **Step 7: Run them to see them fail**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[project]"`
Expected: the undo assertions at ~2209 and at the end of that test FAIL (the diff reports clean after an undo and cannot see the flag), as do the two flipped assertions.

- [ ] **Step 8: Make `Project` use the flag**

`Project.hpp:156-159`, the comment and declaration become:

```cpp
    // The place has changed since the last load or save: ChangeHistoryService::dirty().
    bool unsaved() const;
```

`Project.cpp`, replace the whole body of `unsaved()` (1481-1508):

```cpp
bool Project::unsaved() const { return game_->history().dirty(); }
```

`Rebuild::finish`, after `world_.clear_authored_dirty();`:

```cpp
        world_.history().mark_saved();
```

End of `save_tree`, after the `if (!playing) { world.clear_authored_dirty(); }` block:

```cpp
    // Every file is written. During play that is the place as it was at Play,
    // which is all the edit stack has changed.
    world.history().mark_saved();
```

`apply_disk`, the block at 2317-2325 becomes:

```cpp
    if (!compared.actions.empty()) {
        // What comes from disk is on disk: applying it leaves the place as dirty as it was.
        const bool was_dirty = world.history().dirty();
        const std::optional<std::string> recording = world.history().try_begin_recording("Changes from Disk");
        apply_changes(compared, out.loaded);
        if (recording) {
            world.history().finish_recording(*recording, FinishRecordingOperation::Commit);
        }
        if (!was_dirty) {
            world.history().mark_saved();
        }
        world.capture_place();
```

If `merge_keys`, `plan_files`, or a helper is now unused because only `unsaved()` called it, the compiler's unused warning names it; delete only what nothing else calls. `place_fingerprint` stays.

- [ ] **Step 9: Run the sandbox suite**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox`
Expected: all pass. A test that asserted `unsaved()` after a direct write with no recording now needs that write inside `begin_step` / `end_step`.

- [ ] **Step 10: Fix the two IDE tests whose edits were direct writes**

`tests/ConflictsTest.cpp:245-250`, `move_part` becomes:

```cpp
    auto move_part = [&layout](float x, float y, float z) {
        layout.simulation().on_simulation([x, y, z](engine_core::DataModel& game) {
            // As an edit made in the studio: one undo step, so there is something to save.
            const std::optional<std::string> step = game.history().try_begin_recording("Move");
            game.game_object(game.find_first_child(game.scene_service("Workspace"), "Part"))
                ->set_position(engine_core::Vec3{x, y, z});
            if (step) {
                game.history().finish_recording(*step, engine_core::FinishRecordingOperation::Commit);
            }
        });
    };
```

`tests/SaveConflictTest.cpp`: its `move_part(name, value)` lambda gets the same three added lines around its write (the comment, `try_begin_recording("Move")` before, `finish_recording` after). Add `#include "ChangeHistoryService.hpp"` and `#include <optional>` to both files if absent.

- [ ] **Step 11: Run every suite and commit**

Run: `make test`
Expected: every suite passes. In a `tests/` suite, a failure where the studio no longer asks to save after a test's direct write is fixed the same way as Step 10.

```bash
git add -A src sandbox tests
git commit -m "$(cat <<'EOF'
Call the place unsaved when history has changed since the last save or load

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: The studio's title follows the flag

After Task 4 a project's title is already right, through `Project::unsaved()`. This task makes an untitled place use the same flag and removes the bookkeeping the diff needed.

**Files:**
- Modify: `src/ide/IdeLayout.hpp:216-220, 545-551`
- Modify: `src/ide/IdeLayout.cpp:286, 414, 848`
- Modify: `src/ide/IdeLayoutProject.cpp:23-24, 518, 545-590, 653, 712, 750, 931`
- Test: `tests/SaveConflictTest.cpp`

**Interfaces:**
- Consumes: `ChangeHistoryService::dirty()` from Task 4.
- Produces: `IdeLayout::refresh_modified()` (no parameter); `IdeLayout::mark_saved` no longer exists.

- [ ] **Step 1: Write the test**

In `tests/SaveConflictTest.cpp`, right after `expect(!layout.has_unsaved_changes(), "and the place is saved");` (line ~121):

```cpp
    // Flying the view is not an edit: there is nothing to save.
    layout.simulation().on_simulation([](engine_core::DataModel& game) {
        for (engine_core::InstanceId child : game.get_children(game.scene_service("Workspace"))) {
            if (auto* camera = dynamic_cast<engine_core::Camera*>(game.instance(child))) {
                camera->set_transform(engine_core::matrix4_translation(9.f, 9.f, 9.f));
            }
        }
    });
    layout.flushFrame();
    expect(!layout.has_unsaved_changes(), "flying the camera leaves nothing to save");
```

Add `#include "Camera.hpp"` if absent.

- [ ] **Step 2: Run it**

Run: `cmake --build build --parallel --target studio-tests && ./build/studio-tests`
Expected: passes already (Task 4 made a project's answer the flag). It pins the behaviour through the refactor below.

- [ ] **Step 3: Remove the diff's bookkeeping**

`IdeLayout.hpp`: delete the `mark_saved` declaration and its comment (215-216). Replace the `refresh_modified` comment and declaration (217-220) with:

```cpp
    // Puts the title's unsaved mark where the place and the editors say it
    // belongs. Called each frame.
    void refresh_modified();
```

Delete the members `saved_fingerprint_`, `seen_revision_`, `modified_checked_at_`, `place_modified_` and their comments (545-551). Keep `title_modified_`.

`IdeLayoutProject.cpp`: delete `kModifiedCheckInterval` and its comment (23-24). Delete line 518 (`seen_revision_ = ~std::uint64_t{0};`) and the comment above it. Delete `IdeLayout::mark_saved` (545-551). Replace `refresh_modified` and `has_unsaved_changes` (553-589):

```cpp
void IdeLayout::refresh_modified() {
    if (has_unsaved_changes() != title_modified_) {
        update_title();
    }
}

bool IdeLayout::has_unsaved_changes() {
    return runner_.simulation().datamodel().history().dirty() || editors_unflushed();
}
```

Replace each `mark_saved();` call (`IdeLayoutProject.cpp` 653, 712, 750, 931, and `IdeLayout.cpp:286`) with `update_title();`. The engine already cleared the flag at each of those points (a rebuild or a save); what remains is that the title's name may have changed. At `IdeLayout.cpp:285-286` the comment "Whatever the app built before start is the starting point, not an edit." goes, since nothing built outside a recording dirties.

`IdeLayout.cpp:848`:

```cpp
    title_modified_ = has_unsaved_changes();
```

Add `#include "ChangeHistoryService.hpp"` to `IdeLayoutProject.cpp` if absent. Remove `#include <chrono>` from a file only if nothing else there uses it.

- [ ] **Step 4: Check the members are gone**

Run: `grep -rnE "saved_fingerprint_|seen_revision_|modified_checked_at_|place_modified_|kModifiedCheckInterval|mark_saved\(\)" src/ide`
Expected: no output.

- [ ] **Step 5: Run every suite and commit**

Run: `make test`
Expected: every suite passes.

```bash
git add src/ide/IdeLayout.hpp src/ide/IdeLayout.cpp src/ide/IdeLayoutProject.cpp tests/SaveConflictTest.cpp
git commit -m "$(cat <<'EOF'
Mark the title unsaved from history's flag, for a project and an untitled place alike

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: The Lua `ChangeHistoryService`

**Files:**
- Modify: `src/engine_datatypes/Enum.hpp`, `Enum.cpp` (~212, ~221, ~291)
- Modify: `src/engine_core/LuaApi.hpp:157` (`HostSignal`), `LuaApi.cpp` (docs, near 1107)
- Modify: `src/engine_services/ChangeHistoryService.cpp` (class registration)
- Modify: `src/engine_core/ScriptRuntime.hpp` (~436-442), `ScriptRuntime.cpp` (`attach` 118-122, `detach` 143-145, `host_signal` 777-790)
- Modify: `src/engine_core/ScriptBindings.hpp` (65, ~288), `ScriptBindings.cpp` (methods, registration near 1309)
- Create: `sandbox/history_lua_tests.cpp`
- Modify: `CMakeLists.txt:763` (add the new test file after `sandbox/history_tests.cpp`)

**Interfaces:**
- Consumes: the C++ `ChangeHistoryService` API and its four `HistorySignal`s.
- Produces: `game:GetService("ChangeHistoryService")` in every VM. Task 7's MCP test calls it.

- [ ] **Step 1: Write the failing tests**

Create `sandbox/history_lua_tests.cpp`:

```cpp
#include "ChangeHistoryService.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::vector<std::string> lines(engine_core::ScriptRuntime& runtime) {
    std::vector<std::string> out;
    for (const auto& line : runtime.drain_output().lines) {
        out.push_back(line.text);
    }
    return out;
}

bool has(const std::vector<std::string>& all, const std::string& line) {
    return std::find(all.begin(), all.end(), line) != all.end();
}

engine_core::GameObject& brick(ScriptRig& rig) {
    engine_core::GameObject& part = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(part.id(), "Brick");
    rig.game.set_parent(part.id(), workspace_of(rig.game));
    rig.game.history().reset_waypoints();
    return part;
}

}  // namespace

TEST_CASE("HL1 a chunk's write is an undo step only when the chunk records it", "[HL1][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);

    rig.runtime.run_chunk("workspace.Brick.Name = 'Loose'");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.game.name(part.id()) == "Loose");
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE_FALSE(rig.game.history().dirty());

    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Rename Brick")
        workspace.Loose.Name = "Kept"
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
    )");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.game.history().can_undo().second == "Rename Brick");
    REQUIRE(rig.game.history().dirty());
    rig.game.history().undo();
    REQUIRE(rig.game.name(part.id()) == "Loose");
}

TEST_CASE("HL2 FinishRecording with Cancel puts the value back", "[HL2][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Rename")
        workspace.Brick.Name = "Gone"
        history:FinishRecording(id, Enum.FinishRecordingOperation.Cancel)
    )");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.game.name(part.id()) == "Brick");
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE_FALSE(rig.game.history().dirty());
}

TEST_CASE("HL3 the queries name the step and the signals fire with their arguments", "[HL3][history]") {
    ScriptRig rig;
    brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        history.OnRecordingStarted:Connect(function(name, display) print("started", name, display) end)
        history.OnRecordingFinished:Connect(function(name, display, id, op) print("finished", name, display, op) end)
        history.OnUndo:Connect(function(name) print("undo", name) end)
        history.OnRedo:Connect(function(name) print("redo", name) end)
        print("second", history:TryBeginRecording("Outer") ~= nil, history:TryBeginRecording("Inner"))
        history:SetWaypoint("Outer")
        local id = history:TryBeginRecording("Rename", "Rename Brick")
        print("open", history:IsRecordingInProgress(), history:IsRecordingInProgress(id), history:IsRecordingInProgress("0"))
        workspace.Brick.Name = "B"
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
        print("can undo", history:GetCanUndo())
        history:Undo()
        print("can redo", history:GetCanRedo())
        history:Redo()
        history:ResetWaypoints()
        print("after reset", history:GetCanUndo())
    )");
    rig.frames(1);
    INFO(rig.runtime.last_error());
    const std::vector<std::string> all = lines(rig.runtime);
    REQUIRE(has(all, "second\ttrue\tnil\n"));
    REQUIRE(has(all, "open\ttrue\ttrue\tfalse\n"));
    REQUIRE(has(all, "can undo\ttrue\tRename Brick\n"));
    REQUIRE(has(all, "can redo\ttrue\tRename Brick\n"));
    REQUIRE(has(all, "after reset\tfalse\t\n"));
    REQUIRE(has(all, "started\tRename\tRename Brick\n"));
    REQUIRE(has(all, "finished\tRename\tRename Brick\tEnum.FinishRecordingOperation.Commit\n"));
    REQUIRE(has(all, "undo\tRename Brick\n"));
    REQUIRE(has(all, "redo\tRename Brick\n"));
}

TEST_CASE("HL4 a stale id is a no-op and a bad operation is an error a pcall catches", "[HL4][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Rename")
        workspace.Brick.Name = "A"
        history:FinishRecording("no such id", Enum.FinishRecordingOperation.Commit)
        print("still open", history:IsRecordingInProgress(id))
        print("bad op", pcall(function() history:FinishRecording(id, "Sideways") end))
        print("still open", history:IsRecordingInProgress(id))
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
    )");
    INFO(rig.runtime.last_error());
    const std::vector<std::string> all = lines(rig.runtime);
    REQUIRE(std::count(all.begin(), all.end(), "still open\ttrue\n") == 2);
    REQUIRE(std::any_of(all.begin(), all.end(),
                        [](const std::string& line) { return line.rfind("bad op\tfalse", 0) == 0; }));
    REQUIRE(rig.game.history().can_undo().second == "Rename");
    REQUIRE(rig.game.name(part.id()) == "A");
}
```

`lines` reads each output line's text the way `texts` does in `scene_camera_tests.cpp:28`; if that helper reads a differently named member, use the same one.

In `CMakeLists.txt`, add `    sandbox/history_lua_tests.cpp` on the line after `    sandbox/history_tests.cpp` (763).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake -S . -B build && cmake --build build --parallel --target sandbox && ./build/sandbox "[HL1],[HL2],[HL3],[HL4]"`
Expected: HL1 passes its first half and FAILS at `can_undo().second == "Rename Brick"`; the others fail the same way. `last_error()` reads "unknown service".

- [ ] **Step 3: Add the enum**

`Enum.cpp`, after `kMouseBehaviorType` (212):

```cpp
// Roblox's values. It has Append too, which this engine has no use for.
const EnumEntry kFinishRecordingOperations[] = {{"Commit", 0}, {"Cancel", 1}};
const EnumType kFinishRecordingOperationType{"FinishRecordingOperation", kFinishRecordingOperations,
                                             count_of(kFinishRecordingOperations)};
```

Add `&kFinishRecordingOperationType` to the `kTypes` array (~221), and after `mouse_behavior_enum()`'s definition:

```cpp
const EnumType& finish_recording_operation_enum() { return kFinishRecordingOperationType; }
```

`Enum.hpp`, after `mouse_behavior_enum()`:

```cpp
// Commit 0, Cancel 1: engine_core::FinishRecordingOperation's own values.
const EnumType& finish_recording_operation_enum();
```

- [ ] **Step 4: Add the four host signals**

`LuaApi.hpp:157`:

```cpp
enum class HostSignal {
    SelectionChanged = 0,
    Started = 1,
    Stopped = 2,
    Undo = 3,
    Redo = 4,
    RecordingStarted = 5,
    RecordingFinished = 6
};
```

Update the comment on `LuaField::host_signal` (~105) and on `kSignalHost` in `ScriptBindings.hpp` from "with no arguments" to "found by tag through ScriptRuntime::host_signal (HostSignal); its handlers get whatever values the event carries".

`ChangeHistoryService.cpp`, at the end of the file inside `namespace engine_core`:

```cpp
namespace {

// ScriptRuntime adds the methods, since those calls need the script VM, and
// fires these from the C++ signals of the same names.
ANARCHY_LUA_REGISTER(register_change_history_lua) {
    const LuaField fields[] = {
        lua_host_signal("OnUndo", HostSignal::Undo),
        lua_host_signal("OnRedo", HostSignal::Redo),
        lua_host_signal("OnRecordingStarted", HostSignal::RecordingStarted),
        lua_host_signal("OnRecordingFinished", HostSignal::RecordingFinished),
    };
    register_lua_class("ChangeHistoryService", nullptr, fields, static_cast<int>(sizeof(fields) / sizeof(fields[0])));
    register_lua_service("ChangeHistoryService");
}

}  // namespace
```

`ScriptRuntime.hpp`, after `Signal selection_changed_;` and its two neighbours:

```cpp
    // ChangeHistoryService's OnUndo, OnRedo, OnRecordingStarted, and
    // OnRecordingFinished, fired from its C++ signals of the same names.
    Signal history_undo_;
    Signal history_redo_;
    Signal history_started_;
    Signal history_finished_;
    std::uint64_t history_links_[4] = {0, 0, 0, 0};
```

`ScriptRuntime.cpp`, in an anonymous namespace near the top:

```cpp
LuaSlot text_slot(const std::string& text) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::String;
    slot.text = text;
    return slot;
}

LuaSlot operation_slot(FinishRecordingOperation op) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &finish_recording_operation_enum();
    slot.number = static_cast<int>(op);
    return slot;
}
```

(`LuaSlot::Kind`'s enumerators are in `LuaApi.hpp`; use the string and enum kinds as that header spells them. HL3 asserts the values arrive.) Add `#include "ChangeHistoryService.hpp"` and `#include "Enum.hpp"` if absent.

In `attach`, after `game.events().host_signal(&selection_changed_);`:

```cpp
    for (Signal* signal : {&history_undo_, &history_redo_, &history_started_, &history_finished_}) {
        game.events().host_signal(signal);
    }
    ChangeHistoryService& history = game.history();
    history_links_[0] = history.on_undo.connect([this](const std::string& name) {
        game_->events().emit_args(history_undo_.id(), 0, EventArgs{text_slot(name)});
    });
    history_links_[1] = history.on_redo.connect([this](const std::string& name) {
        game_->events().emit_args(history_redo_.id(), 0, EventArgs{text_slot(name)});
    });
    history_links_[2] = history.on_recording_started.connect([this](const std::string& name, const std::string& display) {
        game_->events().emit_args(history_started_.id(), 0, EventArgs{text_slot(name), text_slot(display)});
    });
    history_links_[3] = history.on_recording_finished.connect(
        [this](const std::string& name, const std::string& display, const std::string& id, FinishRecordingOperation op) {
            game_->events().emit_args(history_finished_.id(), 0,
                                      EventArgs{text_slot(name), text_slot(display), text_slot(id), operation_slot(op)});
        });
```

In `detach`, beside `game_->events().release_signal(selection_changed_);`:

```cpp
        ChangeHistoryService& history = game_->history();
        history.on_undo.disconnect(history_links_[0]);
        history.on_redo.disconnect(history_links_[1]);
        history.on_recording_started.disconnect(history_links_[2]);
        history.on_recording_finished.disconnect(history_links_[3]);
        for (Signal* signal : {&history_undo_, &history_redo_, &history_started_, &history_finished_}) {
            game_->events().release_signal(*signal);
        }
```

In `host_signal`, add to the switch:

```cpp
    case HostSignal::Undo:
        return &history_undo_;
    case HostSignal::Redo:
        return &history_redo_;
    case HostSignal::RecordingStarted:
        return &history_started_;
    case HostSignal::RecordingFinished:
        return &history_finished_;
```

- [ ] **Step 5: Add the service kind and the methods**

`ScriptBindings.hpp:65`:

```cpp
inline constexpr const char* kServiceClasses[] = {"RunService", "Selection", "UserInputService",
                                                 "ChangeHistoryService"};
```

In the class's method list, after `selection_set`:

```cpp
    // ChangeHistoryService's methods, each the C++ method of the same name.
    // Raises when no place is attached.
    static ChangeHistoryService& history_service(lua_State* state);
    static int history_try_begin_recording(lua_State* state);
    static int history_finish_recording(lua_State* state);
    static int history_is_recording_in_progress(lua_State* state);
    static int history_set_waypoint(lua_State* state);
    static int history_undo(lua_State* state);
    static int history_redo(lua_State* state);
    static int history_get_can_undo(lua_State* state);
    static int history_get_can_redo(lua_State* state);
    static int history_reset_waypoints(lua_State* state);
```

`ScriptBindings.cpp`, after `selection_set`:

```cpp
ChangeHistoryService& ScriptBindings::history_service(lua_State* state) {
    luaL_checkudata(state, 1, kServiceMeta);
    ScriptRuntime* runtime = runtime_from(state);
    if (runtime == nullptr || runtime->game_ == nullptr) {
        luaL_error(state, "ChangeHistoryService is not available");
    }
    return runtime->game_->history();
}

int ScriptBindings::history_try_begin_recording(lua_State* state) {
    return lua_guard(state, [&] {
        ChangeHistoryService& history = history_service(state);
        const char* name = luaL_checkstring(state, 2);
        const char* display = luaL_optstring(state, 3, "");
        const std::optional<std::string> id = history.try_begin_recording(name, display);
        if (id) {
            lua_pushlstring(state, id->data(), id->size());
        } else {
            lua_pushnil(state);
        }
        return 1;
    });
}

int ScriptBindings::history_finish_recording(lua_State* state) {
    return lua_guard(state, [&] {
        ChangeHistoryService& history = history_service(state);
        const char* id = luaL_checkstring(state, 2);
        const int op = check_enum_arg(state, 3, finish_recording_operation_enum());
        history.finish_recording(id, static_cast<FinishRecordingOperation>(op));
        return 0;
    });
}

int ScriptBindings::history_is_recording_in_progress(lua_State* state) {
    return lua_guard(state, [&] {
        ChangeHistoryService& history = history_service(state);
        std::optional<std::string> id;
        if (!lua_isnoneornil(state, 2)) {
            id = luaL_checkstring(state, 2);
        }
        lua_pushboolean(state, history.is_recording_in_progress(std::move(id)) ? 1 : 0);
        return 1;
    });
}

int ScriptBindings::history_set_waypoint(lua_State* state) {
    return lua_guard(state, [&] {
        history_service(state).set_waypoint(luaL_checkstring(state, 2));
        return 0;
    });
}

int ScriptBindings::history_undo(lua_State* state) {
    return lua_guard(state, [&] {
        history_service(state).undo();
        return 0;
    });
}

int ScriptBindings::history_redo(lua_State* state) {
    return lua_guard(state, [&] {
        history_service(state).redo();
        return 0;
    });
}

namespace {

int push_can(lua_State* state, const std::pair<bool, std::string>& can) {
    lua_pushboolean(state, can.first ? 1 : 0);
    lua_pushlstring(state, can.second.data(), can.second.size());
    return 2;
}

}  // namespace

int ScriptBindings::history_get_can_undo(lua_State* state) {
    return lua_guard(state, [&] { return push_can(state, history_service(state).can_undo()); });
}

int ScriptBindings::history_get_can_redo(lua_State* state) {
    return lua_guard(state, [&] { return push_can(state, history_service(state).can_redo()); });
}

int ScriptBindings::history_reset_waypoints(lua_State* state) {
    return lua_guard(state, [&] {
        history_service(state).reset_waypoints();
        return 0;
    });
}
```

In the registration function, after the `Selection` block (~1313):

```cpp
    // ChangeHistoryService.cpp declares the class, its signals, and the service.
    const LuaField history[] = {
        lua_method("TryBeginRecording", "string", reinterpret_cast<void*>(&ScriptBindings::history_try_begin_recording)),
        lua_method("FinishRecording", "nil", reinterpret_cast<void*>(&ScriptBindings::history_finish_recording)),
        lua_method("IsRecordingInProgress", "boolean",
                   reinterpret_cast<void*>(&ScriptBindings::history_is_recording_in_progress)),
        lua_method("SetWaypoint", "nil", reinterpret_cast<void*>(&ScriptBindings::history_set_waypoint)),
        lua_method("Undo", "nil", reinterpret_cast<void*>(&ScriptBindings::history_undo)),
        lua_method("Redo", "nil", reinterpret_cast<void*>(&ScriptBindings::history_redo)),
        lua_method("GetCanUndo", "boolean", reinterpret_cast<void*>(&ScriptBindings::history_get_can_undo)),
        lua_method("GetCanRedo", "boolean", reinterpret_cast<void*>(&ScriptBindings::history_get_can_redo)),
        lua_method("ResetWaypoints", "nil", reinterpret_cast<void*>(&ScriptBindings::history_reset_waypoints)),
    };
    register_lua_class("ChangeHistoryService", nullptr, history, static_cast<int>(sizeof(history) / sizeof(history[0])));
```

Add `#include "Enum.hpp"` and `#include <optional>` if absent.

The methods take no thread or window check. A plugin's RenderStepped handler runs inside `ScriptRuntime::render_step`'s `WindowScope`, while the render thread holds the DataModel write lock (`ScriptRuntime.cpp:520-540`), and DataModel writes there are already allowed (`DataModel::authorize`, `window_script`). History is DataModel state behind the same lock, so calling it from there is as safe as the property writes it records.

- [ ] **Step 6: Document the members for completion and hover**

`LuaApi.cpp`, after the `Selection` entries (~1109):

```cpp
    add("ChangeHistoryService", "TryBeginRecording",
        "Opens a recording: every change to the place until FinishRecording is one undo step with this name. Returns its "
        "id, or nil when a recording is already open. A change no recording covers is not an undo step.",
        "string?", false, {P("name", "string"), P("displayName", "string?")});
    add("ChangeHistoryService", "FinishRecording",
        "Closes the recording with this id. Commit keeps its changes as one undo step; Cancel puts them back. An id "
        "that names no open recording does nothing.",
        nullptr, false, {P("id", "string"), P("operation", "Enum.FinishRecordingOperation")});
    add("ChangeHistoryService", "IsRecordingInProgress",
        "True while the recording with this id is open, or while any is when id is omitted.", "boolean", false,
        {P("id", "string?")});
    add("ChangeHistoryService", "SetWaypoint", "Commits the open recording under this name. Does nothing when none is open.",
        nullptr, false, {P("name", "string")});
    add("ChangeHistoryService", "Undo", "Undoes the newest step. Does nothing while a recording is open.", nullptr, false, {});
    add("ChangeHistoryService", "Redo", "Redoes the step last undone. Does nothing while a recording is open.", nullptr,
        false, {});
    add("ChangeHistoryService", "GetCanUndo", "Whether there is a step to undo, and its name.", "(boolean, string)", false, {});
    add("ChangeHistoryService", "GetCanRedo", "Whether there is a step to redo, and its name.", "(boolean, string)", false, {});
    add("ChangeHistoryService", "ResetWaypoints",
        "Forgets every undo and redo step, and drops an open recording without putting its changes back.", nullptr,
        false, {});
    add("ChangeHistoryService", "OnUndo", "Fires after an undo. The argument is the step's name.", "Signal", false, {});
    add("ChangeHistoryService", "OnRedo", "Fires after a redo. The argument is the step's name.", "Signal", false, {});
    add("ChangeHistoryService", "OnRecordingStarted", "Fires when a recording opens. The arguments are its name and displayName.",
        "Signal", false, {});
    add("ChangeHistoryService", "OnRecordingFinished",
        "Fires when a recording closes. The arguments are its name, displayName, id, and the Enum.FinishRecordingOperation.",
        "Signal", false, {});
```

- [ ] **Step 7: Run the tests**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[HL1],[HL2],[HL3],[HL4]"`
Expected: all four pass.

- [ ] **Step 8: Run every suite and commit**

Run: `make test`
Expected: every suite passes. If a completion or analysis test lists every service or every enum by name, add `ChangeHistoryService` and `FinishRecordingOperation` to its expected list.

```bash
git add -A src sandbox CMakeLists.txt tests
git commit -m "$(cat <<'EOF'
Give scripts ChangeHistoryService, so a plugin can record its own undo steps

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Say what is an undo step, in MCP and the docs

**Files:**
- Modify: `src/ide/McpServer.cpp:23-29`
- Modify: `src/engine_services/README.md`
- Test: `tests/McpTest.cpp`

**Interfaces:**
- Consumes: the Lua `ChangeHistoryService` from Task 6; `dirty()` from Task 4.

- [ ] **Step 1: Write the failing test**

In `tests/McpTest.cpp`, in the test that already has `server`, `game`, and the script `Workspace.Main` (around line 396), add at its end, using the file's own assertion helper as the neighbouring lines do:

```cpp
    // run_lua is the command line: a write is an undo step only when the chunk records it.
    game.history().reset_waypoints();
    game.history().mark_saved();
    Call(server, "run_lua", R"({"source":"workspace.Main.Name = 'Loose'"})");
    Expect(!game.history().can_undo().first, "a run_lua write outside a recording is not an undo step");
    Expect(!game.history().dirty(), "and does not mark the place unsaved");
    Call(server, "run_lua",
         R"({"source":"local h = game:GetService('ChangeHistoryService') local id = h:TryBeginRecording('Rename Main') workspace.Loose.Name = 'Main' h:FinishRecording(id, Enum.FinishRecordingOperation.Commit)"})");
    Expect(game.history().can_undo().second == "Rename Main", "a chunk that records is one named step");
    Expect(game.history().dirty(), "and marks the place unsaved");

    const std::string instructions = server.instructions();
    Expect(instructions.find("run_lua") != std::string::npos &&
               instructions.find("ChangeHistoryService") != std::string::npos,
           "the instructions say run_lua records only through ChangeHistoryService");
```

If the server exposes its instructions under another name than `instructions()`, read them the way the file's existing instruction assertions do; if nothing in the file reads them, drop the last three lines.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --parallel --target mcp-tests && ./build/mcp-tests`
Expected: the run_lua assertions pass (Tasks 2, 4, and 6 made them true); the instructions assertion FAILS.

- [ ] **Step 3: Change the instructions**

`McpServer.cpp`, the two sentences about edits and `run_lua` become:

```cpp
    "set_property, create_instance, delete_instance, import_assets, write_script, and edit_script are undo steps "
    "in the studio's history, as if made by hand. "
    "run_lua runs Luau against the live place, like the studio's command line: what it changes is saved with the "
    "place, but is an undo step only when the chunk records it with ChangeHistoryService "
    "(TryBeginRecording, then FinishRecording). "
```

- [ ] **Step 4: Document the service**

`src/engine_services/README.md`, add an entry in the list beside `UserInputService`'s:

```markdown
- `ChangeHistoryService`: the place's undo history, as Roblox's `ChangeHistoryService` gives it. A change is an undo step only while a recording is open: the studio opens one around each of its own commands, and a plugin or the command line opens its own with `TryBeginRecording(name)` and closes it with `FinishRecording(id, Enum.FinishRecordingOperation.Commit)`, or `Cancel` to put the changes back. A write no recording covers, such as the built-in SceneCamera flying the view, is saved with the place but cannot be undone and does not mark the place unsaved. The place is unsaved when a recording has held a change, or an undo or redo has run, since the last save or load. `Undo`, `Redo`, `GetCanUndo`, `GetCanRedo`, `SetWaypoint`, `IsRecordingInProgress`, and `ResetWaypoints` are here too, with the signals `OnUndo`, `OnRedo`, `OnRecordingStarted`, and `OnRecordingFinished`.
```

- [ ] **Step 5: Run every suite and commit**

Run: `make test`
Expected: every suite passes.

```bash
git add src/ide/McpServer.cpp src/engine_services/README.md tests/McpTest.cpp
git commit -m "$(cat <<'EOF'
Say in the MCP instructions and the docs that run_lua records only through ChangeHistoryService

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
)"
```

---

## After the last task

Run the studio (`make run`) and check by hand, since no test drives the real window title:

1. Open a project. Fly the camera with the right button and WASD. The title gains no `*`, and closing does not ask to save.
2. Move a part with the Move tool. The title gains `*`. Ctrl+Z puts the part back; the `*` stays.
3. Save. The `*` goes. Reopen the project: the camera is where step 1 left it.
4. In the command line, `workspace.Part.Name = "X"`. No `*`, and Ctrl+Z does not rename it back.
