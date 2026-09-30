# Scene Camera Design

2026-09-30 · builds on the plugin VM (`ScriptRuntime::register_plugin`, tests PL1–PL10), the Camera instance, and the scene view (`GameView`).

## Goal

Fly the scene view's camera in edit mode, the way Roblox Studio does: WASD moves relative to where the camera faces, Q and E move down and up, and holding the right mouse button in a scene view locks the pointer where it is and turns the camera by the mouse's motion. The camera control is a built-in Luau plugin, and what it needs from the engine is added as the Roblox API it mirrors: `Workspace.CurrentCamera`, `UserInputService.MouseBehavior`, `UserInputService:GetMouseDelta()`, and `RunService:IsRunning()`.

## Decisions

| Question | Decision |
| --- | --- |
| Which camera moves | The Camera instance a scene view is configured to show, reached through `workspace.CurrentCamera`. |
| Who sets `CurrentCamera` | The IDE: a mouse press in a scene view, or a pick in its camera combo box, sets it to that view's configured Camera. |
| Is `CurrentCamera` saved or undoable | Neither. It is session state, nil after a load or a new place, and nil when its Camera is destroyed. |
| Does a script's write to `CurrentCamera` change a view | No. The link runs from view to property only. |
| Are camera moves undoable | No. A Camera's Transform writes are never undo steps (a Camera is a viewpoint, not content). They still mark the place changed, so the camera's position saves with the place. This holds for any Camera move, including one from Properties. |
| Input in edit mode | `UserInputService` is always active; the edit-mode tool step dispatches it when no play session is open. |
| Mouse lock API | Roblox's: `Enum.MouseBehavior` { `Default`, `LockCenter`, `LockCurrentPosition` } as `UserInputService.MouseBehavior`. |
| Mouse delta API | `UserInputService:GetMouseDelta()` → Vector2, raw motion since the last step, scaled by `MouseDeltaSensitivity` (default 1). |
| How the pointer locks | A JadeFX Scene pointer lock, carried out by the GLFW host with `GLFW_CURSOR_DISABLED` plus `GLFW_RAW_MOUSE_MOTION` where supported, and asked for by the focused `GameView`. |
| Where built-in plugins live | `resources/plugins/*.luau`, loaded by the IDE into unparented Scripts and registered at startup and after every load or new place. |
| WASD without the right button held | Allowed while the scene view has keyboard focus, as in Studio. |
| During play | The camera plugin does nothing; it checks `RunService:IsRunning()`. |
| Speed modifiers (Shift, wheel) | Out of scope. |

## Architecture

### 1. `Workspace.CurrentCamera` (`engine_core`, `engine_services`)

1. Workspace gains an instance property `CurrentCamera`, readable and writable from Lua, typed to Camera (a non-Camera write raises). It is a plain `lua_property`, not a saved one: the project writer skips it and the reader never sets it.
2. Writes to it do not record history and do not mark the place changed, whoever makes them.
3. It becomes nil when the referenced Camera is destroyed, and when `Project::load` or `Project::reset_place` rebuilds the tree (both go through `clear_world`, which clears it).
4. `GameView` sets it (through the engine's simulation-thread call, as other IDE writes do) on a mouse press in the view and when its combo box picks a camera. A view with no configured camera leaves it alone.

### 2. Input in edit mode (`engine_services/UserInputService`, `engine_core/ScriptRuntime`)

1. `UserInputService` accepts posts whenever a ScriptRuntime is attached: `attach` activates it, Test and Stop re-activate it (which clears what was queued), and `detach` deactivates it.
2. `ScriptRuntime::step_tools`, when `!open_`, calls the service's `dispatch` before firing Heartbeat, so plugin handlers for `InputBegan` / `InputChanged` / `InputEnded` run in edit mode and `IsKeyDown` reflects held keys. In play, dispatch stays in `fire_phase(PreAnimation)`.
3. Key and button state resets on Test, on Stop, and on focus loss, so no key stays stuck across a transition.

### 3. Mouse lock and delta (`engine_services/UserInputService`, `engine_datatypes/Enum`, `runner/GameView`, JadeFX)

1. `Enum.MouseBehavior` is added with `Default`, `LockCenter`, `LockCurrentPosition`.
2. `UserInputService` holds `MouseBehavior` (default `Default`) and `MouseDeltaSensitivity` (default 1, clamped ≥ 0) as Lua properties. C++ reads the requested behavior through a thread-safe accessor, since `GameView` reads it on the UI thread.
3. `post_mouse_delta(dx, dy)` is a new post: an `InputChanged` MouseMovement record whose `Delta` is the motion and whose position is the unchanged mouse location. `GetMouseDelta()` returns the sum of the `Delta` of every MouseMovement record in the latest dispatch, multiplied by the sensitivity; a step with no motion returns (0, 0).
4. JadeFX gains a pointer lock on `Scene`: `setPointerLocked(bool)`, `isPointerLocked()`, and `takePointerDelta(dx, dy)`. `Stage` bridges it to its host, as it bridges the clipboard. While locked, the GLFW host:
   - hides and captures the pointer (`GLFW_CURSOR_DISABLED`, raw motion if `glfwRawMouseMotionSupported()`);
   - turns cursor moves into pointer deltas instead of moves, so hover and hit-testing stay put;
   - reports button presses and scrolls at the point where the lock began;
   - ignores cursor-shape changes, which would otherwise put the mode back to normal;
   - on unlock, restores the normal cursor at the point where the lock began, and re-applies the current shape.
   Losing the window's focus ends the lock.
   This is needed because `GlfwHost::setCursor` resets the input mode to `GLFW_CURSOR_NORMAL` on every cursor-shape change, which would silently undo a lock applied from outside JadeFX.
5. `GameView`, each paint, compares the requested `MouseBehavior` with its scene's lock:
   - Lock requested and the view focused: `scene->setPointerLocked(true)`.
   - `Default` requested, or the view not focused: `scene->setPointerLocked(false)`.
   - While locked, `takePointerDelta` goes to `post_mouse_delta`.
   - A lock this view held that the scene dropped (the window lost focus) sets `MouseBehavior` back to `Default`, so a plugin never believes the lock holds when it does not. `post_focus_lost` does the same.
   - `LockCenter` behaves as `LockCurrentPosition` does: the pointer comes back where it was.

### 4. `RunService:IsRunning()` (`engine_services/RunService`, `ScriptBindings`)

Returns true while a play session is open (the play VM exists), false in edit mode and while stopped. Paused play still counts as running.

### 5. Built-in plugins (`ide/PluginLoader`, `resources/plugins`)

1. `resources/plugins/` holds one `.luau` file per built-in plugin. The existing copy of `resources/` next to the executable carries it.
2. `PluginLoader` (IDE side) reads each file, creates an unparented `Script` named after the file with that `Source`, and calls `register_plugin` on the simulation thread. Unparented, the Scripts are not saved, not listed in the Game Explorer, and the creation records no history.
3. It runs at startup and again after `new_place` and `open_project_at`, since both rebuild the tree and invalidate every instance id. Before reloading it unregisters what it registered.
4. A file that cannot be read or fails to compile prints an error to the console; the other plugins still load.

### 6. The camera plugin (`resources/plugins/SceneCamera.luau`)

- `InputBegan` MouseButton2 → `MouseBehavior = LockCurrentPosition`; `InputEnded` MouseButton2 → `Default`.
- On Heartbeat(dt), when `not RunService:IsRunning()` and `workspace.CurrentCamera` is set:
  - **Turn** (only while locked): yaw about world Y by `-delta.X * sensitivity`, pitch about the camera's right axis by `-delta.Y * sensitivity`, pitch clamped to ±89°. Yaw and pitch are kept in plugin state, read from the camera's orientation when the camera changes.
  - **Move**: W/S along `LookVector`, D/A along `RightVector`, E/Q along world Y; the sum normalized, times speed, times dt.
  - The result is written to `cam.Transform` as `Matrix4.new(position) * Matrix4.fromOrientation(pitch, yaw, 0)`.
- Keys only reach `UserInputService` from a focused scene view, so typing in the script editor does not move the camera.

## Error handling

- `CurrentCamera` nil, or a Camera destroyed mid-flight: the plugin skips the step.
- A plugin that errors at load or in a handler reports to the console like any script error; the others keep running.
- The window losing focus while locked (Alt-Tab) restores the cursor via the focus-lost path and resets `MouseBehavior`.

## Testing

Sandbox tests (the existing `sandbox/` harness):

- `CurrentCamera`: set and read; a non-Camera write raises; nil after the Camera is destroyed; nil after `reset_place`; not written by the project writer; no history record from a write.
- Edit-mode input: a posted key fires a plugin `InputBegan` on the next `step_tools`; `IsKeyDown` true while held; state cleared by focus loss.
- Mouse delta: two `post_mouse_delta` calls before a step sum; the next step returns zero; sensitivity scales it; the mouse location does not move.
- `MouseBehavior`: a plugin write is visible through the C++ accessor; focus loss resets it to `Default`.
- `RunService:IsRunning()` false in edit mode, true in play, false after Stop.
- The camera plugin, loaded from its file and driven by posts: W held for a step moves along `LookVector`; a locked delta turns the camera; pitch clamps at 89°; nothing moves during play.

JadeFX tests: the Scene lock records its state, calls the bridge once per change, adds up deltas only while locked, resets on take, and ends on window focus loss.

Manual check in the IDE: right-drag in the scene view hides and locks the cursor, turns the camera, and releases it back where it started; WASDQE fly the camera; Alt-Tab while locked frees the cursor.
