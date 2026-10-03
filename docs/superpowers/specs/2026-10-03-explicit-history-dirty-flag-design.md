# Explicit Change History and the Dirty Flag

2026-10-03 · Builds on the Dragger (`2026-10-03-dragger-design.md`) and the RenderStepped window (`2026-10-03-renderstepped-window-design.md`), both on master.

## Goal

The studio asks to save when the place has not changed in any way the user would call an edit: flying the scene camera is enough. The cause is that "unsaved" is a key-by-key diff of the files a save would write, and the camera's Transform is one of those keys.

This change makes two rules:

1. **ChangeHistoryService records a mutation only inside a recording that someone explicitly opened.** A write that no recording covers, from Lua or from C++, is not an undo step. Plugins get the same `ChangeHistoryService` API that Roblox plugins have, so they can open recordings of their own.
2. **The place is dirty when ChangeHistoryService has had an update since the last save or load:** a committed recording, an undo, or a redo. The diff goes away. Flying the camera, a plugin's per-frame writes, and a console command that does not open a recording never dirty the place, though a save still writes what they changed.

## Decisions

| Question | Decision |
| --- | --- |
| What enters history | Only mutations made while a recording is open. The implicit gesture (a recording that opened itself on the first unrecorded write) is gone. |
| Who opens recordings | Every IDE command, in C++. A plugin, the console, or an MCP `run_lua` chunk opens its own through the Lua `ChangeHistoryService`, or its writes stay unrecorded. |
| What dirties the place | A recording committed to the edit stack with at least one mutation, an undo, a redo, and the few `src`-changing writes that bypass history (`set_guid`, `set_extra_property`). Edit-then-undo is dirty. |
| What cleans it | A save that wrote every file, and a rebuild (load, new place, reset). Nothing in the IDE. |
| Changes from Disk | Applying them leaves the flag as it found it. Clean stays clean. |
| The camera | A Camera's Transform is an ordinary property: inside a recording it is an undo step. SceneCamera's writes are outside any recording, so flying is neither undone nor dirty, but the position is in the save set and a save writes it. |
| Console and MCP `run_lua` | Not wrapped. A chunk's writes are saved on the next save but are neither undoable nor dirty unless the chunk opens a recording. |
| Lua API | `game:GetService("ChangeHistoryService")` with Roblox's methods and signals, one-to-one with the C++ API. `SetEnabled` is left out: a plugin that leaves history off breaks every other tool, and the C++ sites that need it scope it. |
| Flying during a drag | Accepted as is. A camera write made while a drag's recording is open joins that recording, so undoing the Move also puts the camera back. Flying with the right button or WASD while dragging with the left is the only way to hit it. |
| Out of scope | Per-plugin undo stacks, a save point that survives undo past it, history for text editors (they keep their own). |

## Architecture

### 1. ChangeHistoryService records only inside a recording (`engine_services/ChangeHistoryService`)

- `note()` returns at once when no recording is open. `wants_mutation()` is true only while one is open (and history is enabled, and no undo is applying), so mutators skip capturing before and after values for an unrecorded write. Flying the camera no longer builds a `PropertyValue` per frame.
- Removed: `open_implicit`, `set_pending_gesture`, `end_gesture`, `pending_`, `Recording::implicit`, `default_gesture`, `forget_core`'s "cancel an empty implicit recording" branch. The IDE's two `CloseGesture` helpers go with them.
- Unchanged: `try_begin_recording` refuses while another recording is open; `set_waypoint` commits the open one; play recordings go to the session stack and are dropped on Stop; `seal_edit_recording` at play start; the limits and trimming.
- `Core` writes inside a recording are still dropped, as today.

### 2. The dirty flag (`engine_services/ChangeHistoryService`)

```cpp
// True since the last mark_saved when a recording committed a mutation to the
// edit stack, an undo or redo ran, or a writer called mark_dirty. Any thread
// may read it, so a UI can refresh its title without taking the lock.
bool dirty() const;
void mark_dirty();
void mark_saved();
```

An `std::atomic<bool>`. Set by:

- `finish_recording(..., Commit)` when the waypoint it pushes is non-empty and goes on the edit stack. A play recording does not dirty: Stop puts the place back.
- `undo()` and `redo()` when they move a waypoint on the edit stack. Undoing a play waypoint does not dirty.
- `mark_dirty()`, called by `DataModel::set_guid` and `DataModel::set_extra_property` next to their `mark_authored_dirty`. These change a file without entering history.

Cleared by `mark_saved()`, called from:

- `Project::save_tree`, after every file is written. A write that fails leaves the flag set.
- `Rebuild::finish` in `Project.cpp`, next to `reset_waypoints()` and `clear_authored_dirty()`. Load, `create`, `reset_place`, `save_as`, and `adopt` all end there.

`Project::apply_disk` reads `dirty()` before its "Changes from Disk" recording and writes it back after. Not touched: `reset_waypoints()`.

The `authored_dirty` id set and `authored_revision()` stay as they are. They drive the incremental save and the script editor's `reapply`, not the title.

### 3. IDE commands open recordings explicitly (`ide/`)

A guard in `ide/IdeLayoutInternal.hpp`:

```cpp
// Opens a recording for one IDE command and commits it when the command
// returns, by any path. Does nothing when another recording is open or
// history is off, as try_begin_recording does.
class ScopedRecording {
public:
    ScopedRecording(engine_core::DataModel& world, std::string name);
    ~ScopedRecording();  // finish_recording(Commit) when it opened one
};
```

Each site that set a pending gesture and closed it with `CloseGesture` becomes one `ScopedRecording` around the writes, with the same name:

| File | Names |
| --- | --- |
| `IdeLayoutEditing.cpp` | Delete, Copy, Duplicate, Cut, Paste (two sites), Move, Rename, Add Model, Set `<prop>` / Clear `<prop>` |
| `IdeLayoutProject.cpp` | Add as GameObject, Import Models / Import Assets |
| `IdeLayout.cpp` (host.insert) | Insert `<class>` |
| `McpTools.cpp` | Insert `<class>`, Delete, Import Assets |

Already explicit and unchanged: the dragger's Move, Properties' Set `<prop>`, Edit Script, Edit CSS, Replace in Scripts, Type Text, Changes from Disk. Their `end_gesture()` calls before `try_begin_recording` are deleted (`DraggerWorld.cpp`, `PropertySheet.cpp`, `Project.cpp`).

### 4. The Lua `ChangeHistoryService` (`engine_core/ScriptBindings`, `LuaApi`)

`game:GetService("ChangeHistoryService")` resolves like `RunService` and `UserInputService`: a service object, not an instance in the tree. Available in the plugin, console, and play VMs; a recording opened during play goes to the session stack, as the C++ API does.

Methods, each a direct call to the C++ method of the same name:

| Lua | C++ |
| --- | --- |
| `TryBeginRecording(name: string, displayName: string?) → string?` | `try_begin_recording` |
| `FinishRecording(id: string, operation: Enum.FinishRecordingOperation)` | `finish_recording` |
| `IsRecordingInProgress(id: string?) → boolean` | `is_recording_in_progress` |
| `SetWaypoint(name: string)` | `set_waypoint` |
| `Undo()`, `Redo()` | `undo`, `redo` |
| `GetCanUndo() → boolean, string`, `GetCanRedo() → boolean, string` | `can_undo`, `can_redo` |
| `ResetWaypoints()` | `reset_waypoints` |

Signals, each wired to the C++ `HistorySignal` of the same name: `OnUndo(name)`, `OnRedo(name)`, `OnRecordingStarted(name, displayName)`, `OnRecordingFinished(name, displayName, id, operation)`.

`Enum.FinishRecordingOperation` has `Commit` and `Cancel`, registered where `Enum.MouseBehavior` is.

A plugin's RenderStepped and Heartbeat handlers run inside the render window, which already admits DataModel writes; history is DataModel state, so the same rule covers these calls. The plan confirms this against `ScriptRuntime` before wiring the binding.

### 5. The camera (`engine_instances/Camera`, `resources/plugins/SceneCamera.luau`)

- `GameObject::transform_in_history()` and Camera's override are deleted. `DataModel::set_transform` always calls `record_transform`, which marks the save set and notes to history; history then decides by whether a recording is open. Camera's header comment changes to match.
- SceneCamera.luau and MoveTool.luau are unchanged. Flying while a drag's recording is open puts the camera's writes into that recording; that is accepted (see Decisions).

### 6. `Project::unsaved()` and the IDE (`engine_core/Project`, `ide/IdeLayout`)

- `Project::unsaved()` returns `game_->history().dirty()`. The key-by-key plan and compare are deleted. `Project::place_fingerprint` is deleted unless a test depends on it for something other than the title.
- `IdeLayout::refresh_modified` reads `dirty()` and `editors_unflushed()` and refreshes the title when their OR changes. Deleted: `seen_revision_`, `modified_checked_at_`, `saved_fingerprint_`, `place_modified_`, `kModifiedCheckInterval`, the `force` parameter, and `IdeLayout::mark_saved` (its five callers are each preceded by a rebuild or a save, which clear the flag in the engine; the title refresh happens on the next frame).
- `has_unsaved_changes()` is `dirty() || editors_unflushed()`.

### 7. Console and MCP (`ide/IdeConsole`, `ide/McpTools`)

- `run_chunk` is not wrapped. The MCP server's instructions and `run_lua`'s description say so: `set_property`, `create_instance`, `delete_instance`, `import_assets`, `write_script`, and `edit_script` are undo steps; `run_lua` is not unless the chunk opens a recording through `ChangeHistoryService`.
- MCP `undo` is unchanged.

## Behaviour changes

- Retyping an old value counts as dirty. Undoing back to the saved state counts as dirty.
- Only flying the camera, then closing: no prompt, and the view is not kept. Any save for another reason writes it.
- A console command or plugin that edits the place outside a recording: saved on the next save, but no undo step and no prompt. Before this change the console's writes were an implicit undo step.
- Any future write path that bypasses history and changes `src` must call `mark_dirty()`. The diff no longer catches it.
- A place that load repaired (missing GUIDs filled in) opens clean. Before, the diff reported it dirty.
- Flying the camera during a drag puts the camera's moves into the drag's undo step.

## Tests

Sandbox history tests (`sandbox/history_tests.cpp`):

- H4 rewritten: two renames inside one explicit recording are one "Rename" step; the same two renames outside a recording are no step and leave the place clean. The `close_gesture` helper goes.
- A write outside any recording is not captured: `can_undo` false, `dirty()` false, and the instance is in `authored_dirty()` so a save writes it.
- A committed recording dirties; a cancelled one does not; an empty one does not.
- Edit, save (`mark_saved`), undo: dirty. Edit, undo, save: clean.
- A play recording commits to the session stack and does not dirty; Stop does not dirty.
- `set_extra_property` and `set_guid` dirty without a recording.

Project tests (`sandbox/project_tests.cpp`):

- `save` clears the flag; a save that fails to write leaves it.
- Load opens clean. Changes from Disk applied to a clean place leave it clean; applied to a dirty place leave it dirty.
- A source-only edit inside a recording dirties.

Camera tests (`sandbox/scene_camera_tests.cpp`, `sandbox/camera_tests.cpp`):

- SC11 rewritten: a Camera's Transform set inside a recording is an undo step and dirties; set outside one it is neither, and after `save` and load the position comes back.
- A drag of a non-viewing Camera is one Move step and dirties.

Lua binding tests (`sandbox/history_tests.cpp` or a new `sandbox/history_lua_tests.cpp`):

- A console chunk that sets a property is not an undo step; one that wraps it in `TryBeginRecording`/`FinishRecording` is, named as given, and `OnRecordingStarted`/`OnRecordingFinished` fire.
- `FinishRecording` with `Cancel` puts the value back.
- `GetCanUndo` returns the name; `Undo()` fires `OnUndo`.

IDE tests (`tests/`): the existing Explorer rename, delete, paste, and Properties tests keep passing with explicit recordings; the title test (if any) reads the flag.
