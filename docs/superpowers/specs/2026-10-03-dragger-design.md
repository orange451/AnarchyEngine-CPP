# Dragger Design

2026-10-03 · Builds on event arguments (`2026-10-02-event-arguments-design.md`) and the Core service (`2026-10-02-core-service-design.md`), both on main. Written from the approved brainstorm (`2026-10-02-dragger-brainstorm.md`) and its revisions; where the two differ, this spec wins.

## Revision (2026-10-03, after phase 6)

This replaces the sections below wherever they differ.

- **A Dragger is a PVInstance with its own Transform,** saved and scriptable. The handles sit there; Local space follows its rotation. It binds to nothing: `Adornee` and the parent rule are gone. It is active anywhere under `game`.
- **A drag moves nothing.** DraggerWorld measures the offset from the Dragger's Transform at the press and fires `DragBegan`, `Dragged(handle, offset)`, and `DragEnded`. Listeners move what they like, the Dragger included. In edit mode the drag is still one "Move" undo step that the listeners' writes join; the physics hold during play is gone.
- **The Move tool** keeps one Dragger in Core at the middle of the box around the selected PVInstances, made when something is selected and destroyed when nothing is. On `DragBegan` it records the start Transform of each selected PVInstance and of its Dragger; on `Dragged` it sets each to start plus offset, so the whole selection moves by the same snapped amount in one undo step. It watches the selected instances' `Changed`, so the handles follow an undo or a Properties edit.
- Tests: DR9 (active anywhere under game), DR10 (events only, nothing moves), DR16 (Local axes from its own rotation), DR22 (a listener moving the Dragger does not change the drag's base), MT1 (handles at the selection's middle), MT6 (one drag moves the whole selection; one undo puts it and the handles back).

## Goal

A `Dragger` instance that binds to a PVInstance and draws translate handles at it: three axis arrows and three plane squares. Dragging a handle moves the PVInstance. It works in edit mode, where the studio's Move tool drives it, and in play, where games create their own. Roblox Studio's Move tool and Unity's translate gizmo are the model.

## Decisions

| Question | Decision |
| --- | --- |
| Who it is for | The studio and games. One class serves edit mode and play. |
| What a drag does | Moves the bound PVInstance itself, and fires `DragBegan`, `Dragged`, and `DragEnded`. |
| Binding | `Adornee` when set, otherwise the parent. Either way it must be a PVInstance under `game`. |
| First version | Axis arrows, plane squares, snap `Increment`, World or Local `Space`, hover highlight. |
| Where the logic runs | The simulation thread: a `DraggerWorld` in engine_core, fed by UserInputService's dispatch. |
| Undo | One step per drag in edit mode, closed at the start of the step after the release. None in play. |
| The studio's Move tool | A built-in plugin in Core. Its Dragger lives in Core with `Adornee` set to the first selected PVInstance. |
| `Archivable` | A hidden property on every instance. False means the project writer skips it and its subtree. Nothing else. |
| Out of scope | Clicking in the viewport to select, moving several objects together, rotate and scale handles, a toolbar. |

## Prerequisites (from the Core review)

1. **Cache Core's id.** `DataModel::core()` scans the root's children on every call, and it is now on hot paths (`core_holds` in every history note and every scope change). `Game`'s constructor stores Core's id in `State`; `core()` returns it. Core cannot move or be destroyed, so the id never goes stale.
2. **Core edits are not place edits.** `mark_authored_dirty` skips an instance `core_holds`, so a Dragger changing every frame never makes the studio re-walk `unsaved()`.

## Architecture

### 1. `Archivable` (`engine_core/DataModel`, `PropertyReflection`, `LuaApi`)

1. Every instance has `Archivable`, a boolean, default true, readable and writable from Lua. It is not saved (an instance that is not archivable is not written at all), records no history, and does not mark the place changed.
2. `LuaField` gains `hidden`: a field the Properties panel never lists. `Archivable` is the first one. Only Properties (`ide/PropertySheet`) skips hidden fields; the MCP tools, scripts, and the command line see them.
3. `authored_tree` skips an instance whose `Archivable` is false, with its subtree, in edit mode. The place capture keeps it: Archivable is about saving, and Stop still restores it.

### 2. `Camera.ViewportSize` (`engine_instances/Camera`, `runner/GameView`)

1. Camera gains `ViewportSize`, a Vector2 in points, read-only from Lua, not saved, not undone, default (0, 0).
2. GameView writes it, through `on_simulation`, when its size changes and in `noteCurrentCamera` (the press that sets `CurrentCamera`). So the current camera's size is always that of the view last pressed in.

### 3. Drag math (`engine_core/DraggerMath.{hpp,cpp}`)

No GL and no DataModel, so every function is unit-tested on its own. All of it matches `runner::Perspective` (RenderMath: column-major, right-handed, Y up, the camera looks down its -Z) with the renderer's near plane, 0.1, and far plane, 1000.

- `struct DraggerFrame { Vec3 origin; Vec3 axes[3]; }`: where the handles sit. World space: the world axes. Local space: the bound PVInstance's rotation columns, normalized.
- `struct ViewInfo { Matrix4 camera; float fov_degrees; Vec2 size; }`.
- `Ray viewport_ray(const ViewInfo&, Vec2 point)`: the world ray through a point in points from the view's top-left.
- `float handle_scale(const ViewInfo&, Vec3 origin)`: world length of one pixel at origin. Arrows are `kArrowPixels` = 100 px long; the cone is the last 20 px; shafts are 3 px wide; plane squares span 25 to 40 percent of the arrow along both of their axes.
- `enum class Handle { None, X, Y, Z, XY, YZ, XZ }`, matching `Enum.DraggerHandle` (X, Y, Z, XY, YZ, XZ).
- `bool handle_visible(const DraggerFrame&, const ViewInfo&, Handle)`: false for an arrow within 11 degrees of the view direction to origin (|dot| > 0.98), and for a plane seen within 6 degrees of edge-on (|dot(normal, view)| < 0.1).
- `Handle pick(const DraggerFrame&, const ViewInfo&, Vec2 point, float* depth)`: plane squares first (the ray meets the plane inside the square), then arrows (the point within `kPickPixels` = 8 px of the projected shaft and cone). Hidden handles are never picked. `depth` is the distance along the ray, for choosing between Draggers.
- `struct DragStart { DraggerFrame frame; Handle handle; Vec3 hit; float along; }` from `begin_drag(frame, view, point, handle)`.
- `std::optional<Vec3> drag_offset(const DragStart&, const ViewInfo&, Vec2 point, double increment)`:
  - Arrow: the closest point between the mouse ray and the axis line through the start origin; offset = (t - t0) along the axis.
  - Plane: the ray hit on the plane through the start origin, minus the start hit.
  - Snap: each component of the offset, measured along the frame's axes, rounds to a multiple of `increment` when it is above 0.
  - Nullopt when the ray runs parallel to the axis or plane (|denominator| < 1e-6) or the solution lies past the far plane. The caller keeps the last offset, so a drag never jumps or turns NaN.
- `void handle_mesh(const DraggerFrame&, const ViewInfo&, Handle hovered, Handle active, std::vector<HandleVertex>& out)`: colored world-space triangles. Arrow shafts are camera-facing quads; cones have 8 sides. X is red, Y green, Z blue; a plane square takes its normal's color at 40 percent alpha. Hovered brightens; during a drag the active handle is yellow and the rest drop to 35 percent alpha. Hidden handles are left out, from the same `handle_visible` that `pick` uses, so what is drawn is what can be grabbed.

### 4. The Dragger instance (`engine_instances/Dragger.{hpp,cpp}`)

A `DataModel` subclass with an ECS tag, `dragger()`, so `DraggerWorld` finds the ones under `game`.

| Name | Type | Default | Notes |
| --- | --- | --- | --- |
| `Adornee` | `PVInstance?` | nil | Saved reference (`InstanceRef`, as SoundEmitter's `Sound`). |
| `Space` | `Enum.DraggerSpace` { World, Local } | World | Saved enum. |
| `Increment` | number | 0 | Saved. Studs; 0 is no snapping. Negative or non-finite is refused. |
| `Dragging` | boolean | false | Read-only, not saved. |

Events, declared with arguments (`lua_event` with `LuaParam`s), `handle` an `EnumItem` of `Enum.DraggerHandle`:
- `DragBegan(handle)`
- `Dragged(handle, offset: Vector3)`: the total world offset since the drag began, after snapping, fired after the target moved.
- `DragEnded(handle)`

The bound target is `Adornee` if set, else the parent, and only when it is a live PVInstance under `game`; otherwise the Dragger is inactive: no row, no picking. `Instance.new("Dragger")` works (added to ScriptRuntime's factories).

`PVInstance` gains `virtual void set_pv_transform(const Matrix4&)`, the class's ordinary Transform setter: GameObject's `set_transform`, PhysicsObject's `set_transform` (its refusal is reported to the console and ends the drag). So a drag's writes fire Changed, mark the snapshot, and record history like any edit. Every GameObject is a PVInstance, so Camera, PointLight, and SpotLight (through `Light`) can be dragged too. DirectionalLight is not: it has a direction, no position.

### 5. Input and drags (`engine_core/DraggerWorld.{hpp,cpp}`, `engine_services/UserInputService`)

1. UserInputService gains `set_filter(std::function<void(InputRecord&)>)`, called in `dispatch` for each record before it updates the key and button state and fires the signal. The filter may set `processed`.
2. `DraggerWorld` owns the filter. The view is `workspace.CurrentCamera` with its Transform, FieldOfView, and ViewportSize; with none, or a ViewportSize of 0, or the pointer locked (`MouseBehavior` not Default), input passes through untouched.
3. **Hover.** On MouseMovement, while no drag runs: pick every active Dragger and keep the nearest hit as hovered. Hover is state on the Dragger (`hovered()`), not an event.
4. **Begin.** MouseButton1 Begin over a handle: the nearest Dragger's drag starts. The record is marked processed. In edit mode, `try_begin_recording("Move")`; a null result (a recording already open) joins it. `Dragging` becomes true and `DragBegan` fires.
5. **Move.** MouseMovement during a drag: `drag_offset`; when it gives a value, the target's Transform becomes the start Transform with that offset added to its translation (rotation and scale kept), and `Dragged` fires. The record is marked processed. A PhysicsObject target in play gets `set_velocity({})` and `set_angular_velocity({})` with each write.
6. **End.** MouseButton1 End, focus loss (`post_focus_lost`), a pointer lock beginning, or the Dragger or its target going away ends the drag: `Dragging` false, `DragEnded` fires, and the release record is marked processed. The recording is not closed here.
7. **Closing the undo step.** At the next `dispatch`, before any record, an ended drag's recording finishes: Commit if the target moved, Cancel if not. Events are deferred, so the handlers of the ending step, and any events they cause in that step, record into the same undo step. A handler that waits (`task.wait`) records outside it. For that one step, any other edit joins the Move's step.
8. One drag at a time, across all Draggers.
9. **Guarantees for group dragging later** (tested now): `offset` is always total since the drag began, never per move; and the recording stays open until the step after `DragEnded`.

### 6. Rendering (`engine_core/SnapshotPump`, `runner/GameView`, `runner/Renderer`)

1. `VisualSnapshot` gains `std::vector<VisualDragger> draggers`: frame, hovered, and active handle per active Dragger, rebuilt at every `take_changes` from DraggerWorld (there are few).
2. GameView builds the mesh with `handle_mesh` for its own camera, FieldOfView, and size, so every view draws the handles at the right size, and hands it to the Renderer.
3. `Renderer::setHandles` and `handlePass`, after the outline pass: depth test off, straight-alpha blending as the grid and outline passes use, planes first then arrows, the outline pass's VAO and VBO pattern, guarded by `CanDraw`. Shaders `resources/shaders/pipeline/handle.{vert,frag}`: per-vertex color, no lighting.
4. Handles draw in edit mode and in play.

### 7. Selection and run-state events (`engine_services/SelectionService`, `RunService`)

The Move tool needs to hear selection and Play/Stop changes, and polling them would cost every frame and drift as the tool grows. These are host signals, as RunService's phase signals are, with no arguments.

1. **`Selection.SelectionChanged`**, Roblox's name. `SelectionService::set` fires it once when the selection actually changes (a set to the same list fires nothing), after the new list is in place, so a handler's `Selection:Get()` sees it.
2. **`RunService.Started`** fires when a play session opens, after the place is captured and `IsRunning()` is true. **`RunService.Stopped`** fires after Stop has restored the place, when `IsRunning()` is false and the tree is the edit tree again. Roblox has neither; plugins there poll `IsRunning()`. Both reach plugins and the command line; game scripts in play see `Stopped` never fire, since their VM closes first.

### 8. The Move tool (`resources/plugins/MoveTool.luau`)

1. A built-in plugin, listed in `kBuiltinPlugins`, so it loads into Core at startup.
2. It connects `Selection.SelectionChanged`, `RunService.Started`, and `RunService.Stopped` to one update: `Adornee` becomes the first selected instance when it is a PVInstance and the place is not running, nil otherwise. It keeps one Dragger in Core, made on first need, and runs the update once at load. `Space` World and `Increment` 1 are constants at the top of the file.

## Tests

Sandbox Catch2, `sandbox/dragger_tests.cpp` unless noted.

- **Prerequisites.** PR1: `core()` is Core's id in a new Game and stays so through New, Open, Play, and Stop. PR2: a Core instance's property change leaves `authored_revision` unchanged.
- **Archivable.** AR1: Lua reads and writes it, default true. AR2: the project writer skips a non-archivable instance and its subtree; Stop still restores it. AR3: Properties' field list never includes it.
- **Viewport.** VP1: ViewportSize is read-only from Lua, unsaved, and a C++ write records no history.
- **Math, DR1–DR8.** DR1: the center ray is the camera's look vector, a corner ray matches the Perspective frustum corner. DR2: `handle_scale` gives the same pixel length at 5 and 500 studs. DR3: an arrow picks at 8 px, misses at 9; a plane square wins over an arrow where both are hit. DR4: a near-parallel arrow and an edge-on plane are neither picked nor meshed. DR5: an axis drag stays on its axis and does not depend on where along the arrow it was grabbed. DR6: a plane drag stays in its plane. DR7: snapping rounds each component, along rotated axes in Local space. DR8: a parallel ray gives nullopt, and no value is NaN.
- **Instance and world, DR9–DR20.** DR9: active with a PVInstance parent or Adornee under game; inactive otherwise, with no row. DR10: press, moves, release move the target and fire DragBegan, Dragged (right offset), DragEnded in order; `Dragging` is true between. DR11: those records are processed; input that misses every handle is not. DR12: edit mode: one drag is one undo step and undo restores the start; a press and release with no motion records nothing. DR13: play: no history; a dragged PhysicsObject ends with zero velocity. DR14: focus loss, destroying the Dragger, or destroying the target ends the drag and fires DragEnded. DR15: two overlapping Draggers: the nearer wins. DR16: rotation and scale survive a drag. DR17: no CurrentCamera, a zero ViewportSize, or a locked pointer: input passes through. DR18: `Space` and `Increment` refuse bad values. DR19: offsets stay total since the drag began, also when `Increment` changes mid-drag. DR20: a Lua `Dragged` handler that moves a second object lands in the same undo step.
- **Rendering.** RD1: an active Dragger has a snapshot row with its frame and states; an inactive one has none. RD2 (`scene-render-check`): a Dragger draws red, green, and blue pixels along its arrows, with no GL error.
- **Events, EV1–EV3** (`game_services_tests.cpp`). EV1: `SelectionChanged` fires once per change and not for a set to the same list; a handler's `Get()` sees the new list. EV2: `Started` fires at Play with `IsRunning()` true; `Stopped` fires after Stop with `IsRunning()` false and the place restored. EV3: a plugin's connections to both outlive a play session.
- **Lights.** DR21: a PointLight and a SpotLight can be dragged; a Dragger bound to a DirectionalLight is inactive.
- **Move tool, MT1–MT3** (`plugin_tests.cpp`). MT1: selecting a PVInstance sets the one Dragger's Adornee to it; another selection retargets it; clearing sets nil. MT2: a non-PVInstance selection sets nil. MT3: during play Adornee is nil; after Stop it follows the selection again. Loading MoveTool leaves no undo step.
- **By hand.** Drag each arrow and plane in the studio, with snapping, in Local space, then undo.

## Plan phases

Each phase ends with passing tests and can merge alone.
1. Prerequisites and `Archivable`.
2. `ViewportSize` and DraggerMath.
3. Dragger instance, DraggerWorld, input, undo, and events.
4. Snapshot rows and the handle pass.
5. Selection and run-state events.
6. The Move tool.
