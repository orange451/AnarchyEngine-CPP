# Dragger Brainstorm

2026-10-02 · Status: design approved section by section in brainstorming. Not yet a spec. The next steps are to write `2026-10-02-dragger-design.md` from this, self-review it, have it reviewed, then write the plan with writing-plans.

## Goal

A `Dragger` instance. Parented under a PVInstance, it binds to it and draws translate handles at it: three axis arrows and three plane squares. Dragging a handle moves the PVInstance. It works in edit mode, where the studio's Move tool drives it, and in play, where games create their own. Roblox Studio's Move tool and Unity's or Unreal's translate gizmo are the model.

## Decisions

| Question | Decision |
| --- | --- |
| Who is it for | Both the studio and games. One instance class serves edit mode and play. |
| What a drag does | Moves the parent itself and fires events. Scripts can react to it, but don't have to. |
| First-version features | Axis arrows, plane handles, snap `Increment`, World/Local `Space`, hover highlight. |
| Where the logic runs | Simulation thread, engine side (approach A). Rejected: hit-testing in GameView on the UI thread (splits drag logic, events, and undo across threads), and Luau (plugins do not run in play). |
| Studio integration | A built-in plugin, `MoveTool.luau`, parents a non-Archivable Dragger under the selection. |
| Hiding tool instances | New `Archivable` property on every instance. Hidden: Lua reads and writes it, and the Properties panel never shows it. |
| Multi-select | Out of the first version, but kept possible by two guarantees (see Future). |

## Design

### 1. Instance API (`engine_instances/Dragger.{hpp,cpp}`)

A `DataModel` subclass like `SoundEmitter`. It is active only when its parent is a PVInstance and it is under `game`. It reads its position from the parent's `transform()` every step, so it follows the parent. Under any other parent it draws nothing and ignores input. Re-parenting rebinds it.

| Name | Type | Default | Notes |
| --- | --- | --- | --- |
| `Space` | `Enum.DraggerSpace` { World, Local } | World | Saved (`lua_saved_enum`). Local follows the parent's rotation. |
| `Increment` | number ≥ 0 | 0 | Saved. Snap step in studs, 0 for none. Snaps the offset from the drag's start, not the absolute position. A negative or non-finite value is refused. |
| `Dragging` | boolean | false | Read-only, not saved. |

Events use `Enum.DraggerHandle` { X, Y, Z, XY, YZ, XZ }:
- `DragBegan(handle)`
- `Dragged(handle, offset: Vector3)`: `offset` is the total world move since the drag began, after snapping. It fires after the parent has moved.
- `DragEnded(handle)`

Rules:
- One drag at a time in the place. Where handles overlap, the nearest to the camera wins.
- Only the left button drags. A drag ends on release, focus loss, a pointer lock starting, or destruction of the Dragger or its parent.
- If a script moves the parent mid-drag, the next mouse move overrides it, since the drag stays anchored to where it started.
- `Instance.new("Dragger")` works. Add it to the `Instance.new` factories in `ScriptRuntime` so the class stays linked.

### 2. Rays, hit-testing, drag math

`Camera` gets `ViewportSize` (Vector2 in points, read-only, not saved). GameView writes it on resize and whenever a press sets `CurrentCamera`, so the current camera's size is always that of the view last clicked. Draggers make rays from `workspace.CurrentCamera`: its Transform, `FieldOfView`, and `ViewportSize`, plus the mouse position in `InputRecord::position` (points from the view's top-left). With no CurrentCamera, or with the pointer locked, Draggers ignore input.

`engine_core/DraggerMath.{hpp,cpp}` has no GL and no DataModel:
- `viewport_ray(camera, fov, size, point)`: a world ray. It must match `runner::Perspective` (RenderMath: column-major, right-handed, Y up, looking down -Z).
- `handle_scale(camera, fov, size, origin)`: the world length that is a fixed pixel size at `origin`. Arrows are about 100 px long.
- `pick(...)`: an arrow is hit within 8 px of its projected segment. A plane square is hit where the ray meets its plane inside the square. Squares win over arrows. Hidden handles can't be picked: an arrow within about 11° of the view direction, or a plane seen nearly edge-on.
- `handle_mesh(row, camera, fov, size)`: the triangles to draw (see 3). It shares the size and visibility code with `pick`, so what is drawn is exactly what can be clicked.

Drag math. At drag start, store the parent's Transform and the starting hit.
- Arrow: the closest point between the mouse ray and the axis line through the start position. Offset = (t − t0) × axis.
- Plane: the ray hit on the plane through the start position, minus the starting hit.
- Snap each offset component to `Increment` along the Dragger's own axes. Local axes are the parent's rotation columns with scale removed.
- Result: the start Transform with its translation plus the offset. Rotation and scale are kept.
- A ray parallel to the axis or plane, or a solution past the far plane, keeps the previous offset. Never NaN, never a jump to infinity.

### 3. Rendering

This follows how selection outlines work: GameView builds world-space geometry each frame, hands it to `Renderer`, and a pass after the scene draws it.

**Depends on branch `physics-collision-outlines`** (commit `cd71d73`, not merged to main as of this writing). It adds `Renderer::setOutlines`/`outlinePass`, `GameView::collectOutlines`, and `resources/shaders/pipeline/outline.{vert,frag}`. Merge it first, or build the handle pass without it.

- `VisualSnapshot` (`engine_core/SnapshotPump.hpp`) gains `draggers`, one row per active Dragger: origin, three unit axes, hovered handle, active handle. It is filled on the simulation thread.
- Hover is computed on the simulation thread on every mouse move, with the same `pick` that starts drags.
- `handle_mesh` builds colored triangles:
  - Arrow shafts are camera-facing quads, since macOS core GL caps line width at 1 px. Tips are 8-sided cones. X is red, Y green, Z blue.
  - Plane squares sit at 25–40% of the arrow length along both of their axes, semi-transparent, colored by their normal's axis.
  - Hovered handles brighten. During a drag the active handle turns yellow and the others dim.
- Each GameView sizes the handles for its own camera and size.
- `Renderer::setHandles(vertices, count)` and `handlePass` run after the outline pass. Depth test is off and blending is straight alpha (`glBlendFuncSeparate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ZERO, ONE)`, as the grid and outline passes use). Planes draw first, then arrows. The pass reuses the outline pass's VAO/VBO pattern and is guarded by `CanDraw`. Shaders go in `resources/shaders/pipeline/handle.{vert,frag}`.
- Handles draw in edit mode and in play.

### 4. Input, undo, studio integration

**`engine_core/DraggerWorld.{hpp,cpp}`** works like `AudioWorld` and `PhysicsWorld`. A class tag, `bool dragger() const`, marks Dragger entities so the world finds those under `game`.
- `UserInputService::dispatch` (`engine_services/UserInputService.cpp`, its per-record loop) gets a hook that runs before each record fires to Lua. `DraggerWorld` installs it.
- The hook handles left-button down, mouse move, and left-button up: hover, then start, move, or end a drag. Records it uses get `processed = true`, so scripts and the SceneCamera plugin see `gameProcessed`.
- The same path serves edit mode (`ScriptRuntime::step_tools` → dispatch when no play session is open) and play (dispatch in `fire_phase(PreAnimation)`).
- `post_focus_lost` and a pointer lock starting end an active drag.

Undo (`engine_services/ChangeHistoryService`):
- Edit mode: drag start calls `try_begin_recording("Move")`, which returns null if a recording is already open, and then the drag joins that one. Release commits it. A drag that never moved cancels it. Per-move writes go through the parent's normal `set_transform`, so the drag is one undo step.
- Play: no recording, as play writes work today.
- A Camera parent records nothing, since `Camera::transform_in_history()` is false. That is expected.
- While a `PhysicsObject` is dragged in play, zero its velocity on every write with `set_velocity({})` and `set_angular_velocity({})`.

**`Archivable`**, on every instance: boolean, default true. Hidden: Lua reads and writes it, and Properties never lists it. When false:
- the project writer skips the instance and its subtree
- creating, changing, reparenting, or destroying it records no history and doesn't mark the place changed
- the Explorer hides it

Setting `Archivable` itself records no history. The engine has no `Clone` yet; when it gets one, Clone should skip non-Archivable instances as Roblox does.

**`resources/plugins/MoveTool.luau`**, loaded by `ide/PluginLoader` like `SceneCamera.luau`:
- On `Selection.SelectionChanged`, if the first selected instance is a PVInstance, it parents a Dragger with `Archivable = false` under it. On any other selection change it destroys that Dragger.
- `Space` and `Increment` are constants at the top of the file: World and 1 stud.
- When `RunService:IsRunning()`, it removes its Dragger and makes none. After Stop it rebuilds from the selection.

Out of scope: picking objects by clicking in the viewport (no viewport picking exists; selection comes from the Explorer), multi-select dragging, rotate and scale handles, a toolbar.

### Future: group dragging

Multi-select becomes a change of about 20 lines in `MoveTool.luau`. The plugin keeps one Dragger, as pivot, on the first selected instance. At `DragBegan` it stores the start Transform of every other selected instance. On `Dragged(handle, offset)` it sets each one to its start Transform plus `offset`. Two guarantees in this design make that work, and tests DR19–DR20 hold them:
1. **`offset` is total since the drag began, not a per-move delta.** Every follower moves by the same snapped amount, with no drift.
2. **The edit-mode recording stays open until the `DragEnded` handlers have run**, so Transform writes from Lua handlers during a drag join the drag's undo step.

Handles at the group's bounding-box center would later be an optional `PivotOffset` property (Matrix4, relative to the parent). It is purely additive.

## Tests

The sandbox Catch2 suite gets `sandbox/dragger_tests.cpp` (DR, AR), and `plugin_tests.cpp` gets MT.

- DR1 The center ray is the camera's LookVector, and a corner ray matches the `Perspective` frustum corner.
- DR2 `handle_scale` gives the same pixel length at distances 5 and 500.
- DR3 An arrow is picked at 8 px and missed at 9 px. A plane square beats an arrow where they overlap.
- DR4 A near-parallel arrow and an edge-on plane are neither picked nor meshed.
- DR5 An axis drag stays on its axis and doesn't depend on where along the arrow it was grabbed.
- DR6 A plane drag stays in its plane.
- DR7 Snapping rounds each component, and along the rotated axes in Local space.
- DR8 A parallel ray keeps the previous offset, with no NaN.
- DR9 Active under a GameObject, PhysicsObject, or Camera. Inactive under a Folder or unparented, with no snapshot row.
- DR10 Press, moves, and release move the parent and fire DragBegan, Dragged (correct offset), and DragEnded in order. `Dragging` is true during the drag.
- DR11 Used records are `processed`. Input that misses every handle is not.
- DR12 Edit: one drag is one undo step, and undo restores the start. A press and release with no motion records nothing.
- DR13 Play: no history, and a dragged PhysicsObject ends at zero velocity.
- DR14 Focus loss, or destroying the Dragger or its parent, ends the drag and fires DragEnded.
- DR15 With two overlapping Draggers, the nearer wins.
- DR16 Rotation and scale survive a drag.
- DR17 With no CurrentCamera, or the pointer locked, input is ignored.
- DR18 `Space`, `Increment`, and `Archivable` refuse bad values.
- DR19 Offsets stay total since the drag began, including when `Increment` changes mid-drag.
- DR20 A Lua `Dragged` handler that moves a second object lands in the same undo step.
- AR1 The project writer skips a non-Archivable instance and its subtree.
- AR2 Create, change, reparent, and destroy of a non-Archivable instance record no history and don't mark the place changed.
- AR3 The Explorer hides it. Properties never lists `Archivable`.
- AR4 Lua reads and writes it.
- MT1 Selecting a PVInstance makes exactly one non-Archivable Dragger under it. Changing or clearing the selection removes it.
- MT2 A non-PVInstance selection makes none.
- MT3 Test removes the Dragger, and Stop rebuilds it.
- `scene-render-check`: a Dragger draws red, green, and blue pixels along its arrows, with no GL error. GL errors are fatal in JadeFX.
- By hand in the studio: drag each arrow and plane, snapping, Local space, and undo.

## Open items to settle while writing the spec

- **Event arguments.** `DataModel::fire_event(id, name)` passes only the instance. `Dragged(handle, offset)` and the other two events need a variant that takes arguments. Find how `lua_event` handlers receive arguments (GuiBase's `MouseClicked` may have one).
- **Event timing versus guarantee 2.** Confirm whether handlers connected with `Connect` run synchronously inside `fire_event` or are deferred to a later drain in the step. Either way, the recording must commit only after DragEnded's handlers have run.
- **Explorer hiding.** Find where `IdeExplorer` builds its rows, and filter non-Archivable instances there and in its search.
- **History skipping.** Decide where `wants_mutation`/`note` (ChangeHistoryService) check `Archivable`, and where the "place changed" mark is set, so neither fires for non-Archivable instances.
- **Plugin VM writes.** Confirm a plugin's `Instance.new` and parenting go through the same history path, so Archivable alone keeps the Move tool's Dragger out of undo.
