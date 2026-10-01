# Box3D Physics Design

2026-09-30 · builds on the play loop's fixed physics substep (`Engine::step_physics`), the flecs scope tags (`InWorkspace`), `InstanceRef`, and the Enum global.

## Goal

Rigid-body physics during play, simulated by [Box3D](https://github.com/erincatto/box3d). A new instance, `PhysicsObject`, is one rigid body. Every PhysicsObject inside Workspace is in the simulation; one outside it is not. A PhysicsObject can name a `GameObject`, which it then moves. The simulation steps with the game loop, before Heartbeat.

## Decisions

| Question | Decision |
| --- | --- |
| Box3D version | FetchContent, pinned to commit `9f998c86` (HEAD on 2026-09-27; v0.1.0 is the only tag and 29 commits older). MIT. |
| Who includes Box3D | `engine_core/PhysicsWorld.cpp` only. Linked PRIVATE to `engine_core`, as flecs is. |
| C runtime | Box3D's top-level CMake forces `/MT`. The project uses the default `/MD`, so after `FetchContent_MakeAvailable` the build sets `MSVC_RUNTIME_LIBRARY` on `box3d` to `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`. |
| Threads | Single-threaded (`workerCount` 1, no task callbacks). |
| Step rate | Each engine physics substep (240 Hz) is one `b3World_Step(physics_dt, 1)`, before Heartbeat. Equivalent in the solver to Box3D's recommended 60 Hz × 4 substeps. |
| When it simulates | Only during play. Edit mode and pause step nothing. |
| Is PhysicsObject a GameObject | No. It inherits `DataModel`; it has its own Transform but does not render and has no Prefab. |
| Which Transform places the body | When a body is created with `GameObject` set, the GameObject's Transform seeds it. With no GameObject, `PhysicsObject.Transform` does. After each step the body writes both. A script write to either teleports the body. |
| Anchoring | An `Anchored` boolean maps to `b3_staticBody`. In Box3D a zero-mass dynamic body is not static; it is unsupported ("unexpected behavior"), so Mass never anchors. |
| Mass | Shape density is set to `Mass / volume`, so Box3D derives a consistent inertia. Mass ≤ 0 (or not finite) on write is clamped to 0.001. |
| AngularFactor | Dropped. Box3D has only per-world-axis on/off locks, no fractional factor. `AngularDamping` is added instead. |
| Shapes | Box, Sphere, Capsule, Hull. Box3D has no cylinder primitive; meshes collide only on static bodies. |
| Hull point source | A `Mesh?` property on PhysicsObject, independent of the GameObject's visuals. Shown in Properties only when Shape is Hull. |
| Several PhysicsObjects on one GameObject | The first eligible one in depth-first tree order gets a body. The rest get none, and each warns once. If the winner leaves or retargets, the next takes over on the next step. |
| Does the GameObject have to be in Workspace | No. Only the PhysicsObject does. |
| Gravity | Fixed (0, -9.81, 0). `Workspace.Gravity` is out of scope. |
| Do physics writes fire `Changed` | No. As in Roblox, and as the existing velocity integration does: they update the render snapshot only. Script and Properties writes fire `Changed` as usual. |
| Scale in a Matrix4 | Ignored for the body pose. Writing back a GameObject's Transform keeps that GameObject's existing scale. |
| Out of scope | Collision events (`Touched`), joints, raycasts, collider debug drawing, `Workspace.Gravity`, kinematic bodies, motion locks. |

## Architecture

### 1. Build (`CMakeLists.txt`)

1. `FetchContent_Declare(box3d GIT_REPOSITORY https://github.com/erincatto/box3d.git GIT_TAG 9f998c862d54c03a633ecea3831937385c78b532)`, then `FetchContent_MakeAvailable`. Samples, tests, and benchmarks are already skipped when Box3D is not the top-level project.
2. Override the C runtime as in the decisions table. `BOX3D_DOUBLE_PRECISION` stays OFF, so `b3Pos` is `b3Vec3`.
3. `target_link_libraries(engine_core PRIVATE box3d::box3d)`. Box3D's include directory is added as SYSTEM so the engine's warnings do not report on it.

### 2. `PhysicsObject` (`engine_instances/PhysicsObject.{hpp,cpp}`)

A `DataModel` subclass registered as `PhysicsObject`, base class `Instance`. `Instance.new("PhysicsObject")` makes one, and the studio's insert list offers it. `Containment` allows it anywhere an Instance may go.

Every property is a saved registry property (`lua_saved_property`): DataModel saves, loads, undoes, and restores it at Stop.

| Property | Type | Default | Box3D |
| --- | --- | --- | --- |
| Transform | Matrix4 | identity | body position and rotation |
| Velocity | Vector3 | (0, 0, 0) | linear velocity |
| AngularVelocity | Vector3 | (0, 0, 0) | angular velocity, world space, rad/s |
| Anchored | boolean | false | static when true, dynamic when false |
| Mass | number | 1 | density = Mass / shape volume |
| Friction | number | 0.6 | surface material friction; not below 0 |
| Bounciness | number | 0 | restitution; 0 to 1 slider, not below 0 |
| LinearDamping | number | 0 | linear damping; not below 0 |
| AngularDamping | number | 0 | angular damping; not below 0 |
| Shape | Enum.PhysicsShape | Box | which shape the body has |
| Size | Vector3 | (1, 1, 1) | see below; each component clamped to at least 0.01 |
| Mesh | Mesh? | nil | Hull's point source; visible only when Shape is Hull |
| GameObject | GameObject? | nil | the GameObject this body moves |

What `Size` means for each Shape:

- **Box**: full extents. `b3MakeBoxHull(Size / 2)`.
- **Sphere**: diameter `Size.X`. Radius `Size.X / 2`.
- **Capsule**: along local Y, total height `Size.Y`, diameter `Size.X`. The segment between the cap centers is `max(Size.Y - Size.X, 0)` long; the radius is `Size.X / 2`.
- **Hull**: the Mesh's vertices are scaled per axis so their bounding box is `Size`, centered on the body's origin, then passed to `b3CreateHull(points, count, B3_MAX_HULL_VERTICES)`. `b3CreateHull` fails past 128 vertices, faces, or edges, so a mesh with more than 128 distinct vertices is first reduced to support points: for each of 128 directions spread evenly over the sphere (a Fibonacci lattice), the vertex farthest along it, with duplicates dropped. If Mesh is nil, its AMESH cannot be read, or the hull still cannot be built, the body gets a Box of `Size` and the PhysicsObject warns once: `PhysicsObject <name>: Hull fell back to Box (<reason>)`. During play, a Mesh with a session copy (MeshShapes) builds from the copy, as the Scene View draws it.

Non-finite numbers and vectors are refused with a message, as Light's setters refuse them. Every setter records which parts of the body it changed (a dirty mask: Pose, Velocity, Material, Shape, Mass, Type, Damping) so `PhysicsWorld` can apply only those.

PhysicsObject's entity carries a new flecs tag, `ecs::PhysicsBody`, set when the instance is made and cleared on release.

### 3. `PhysicsWorld` (`engine_core/PhysicsWorld.{hpp,cpp}`)

Owns the `b3WorldId` and a table from InstanceId to body record. A body record holds the `b3BodyId`, its `b3ShapeId`, the GameObject it drives (by id), and the pose it last wrote to that GameObject.

Pools are reserved at construction (the engine README: the step does not allocate). The table holds at most `kMaxInstances` entries.

**Lifetime.** The DataModel start hook calls `PhysicsWorld::begin()`, which creates the world and reconciles. The stop hook calls `end()`, which destroys the world before the place is restored. `Engine::step_physics` calls `integrate_simulated` and then `PhysicsWorld::step(dt)`.

**`step(dt)`**, on SimulationThread under the step lock:

1. **Reconcile.** Walk a cached flecs query, `PhysicsBody` with `InWorkspace`, into a reserved list of eligible ids. Resolve each one's GameObject reference. Where several eligible ids name one GameObject, the first in depth-first tree order (compared by walking ancestor chains) is the winner; each loser warns once until it wins or stops being a loser. Destroy the bodies of ids that are no longer eligible or winners. Create bodies for new winners and for PhysicsObjects with no GameObject. A body whose GameObject changed is destroyed and created again.
2. **Create.** A body def gets its type from Anchored, its pose from the GameObject's Transform if there is one, otherwise from PhysicsObject.Transform (rotation from the upper 3×3 with scale removed, via `b3MakeQuatFromMatrix`), its velocities, and its damping. One shape follows, with friction, restitution, and density from Mass. `userData` holds the InstanceId.
3. **Push edits.** For each body, apply the PhysicsObject's dirty mask and clear it:
   - Pose: `b3Body_SetTransform`.
   - Velocity: `b3Body_SetLinearVelocity` and `b3Body_SetAngularVelocity`.
   - Material: `b3Shape_SetFriction` and `b3Shape_SetRestitution`.
   - Shape: destroy the shape and create it again, then re-apply density.
   - Mass: `b3Shape_SetDensity(…, true)`.
   - Type: `b3Body_SetType`.
   - Damping: the two damping setters.

   Then compare the driven GameObject's Transform, bitwise, with the pose last written to it. If it differs, a script moved it, so teleport the body there.
4. **Step.** `b3World_Step(world, dt, 1)`.
5. **Write back.** Iterate `b3World_GetBodyEvents`. For each moved body, write PhysicsObject Transform, Velocity, and AngularVelocity, and the winner's GameObject Transform with its scale kept. Each write goes through a simulation store, below, and the record keeps the pose it wrote.

**Simulation store.** These writes change the stored value and call `note(id, VisualField::Transform, WriteOrigin::Simulation)` for GameObjects, as `integrate_simulated` does. They emit no `Changed` and record no history, and they do not set the dirty mask. PhysicsObject gets a narrow friend or internal entry point for this; GameObject uses its existing component store.

### 4. Enum-typed properties (`engine_datatypes/Enum`, `engine_core/LuaApi`, `PropertyReflection`, `ide/PropertySheet`, MCP tools)

1. `Enum.PhysicsShape` is added: `Box` 0, `Sphere` 1, `Capsule` 2, `Hull` 3.
2. `LuaSlot::Kind::Enum` is added, carrying the `EnumType*` and the value. A registered property whose type name is `Enum.<Type>` reads and writes this kind. A write accepts the EnumItem, its name, or its value, as `check_enum_arg` does, and refuses anything else with a message naming the type.
3. Saved as the item's name: `"Shape": "Sphere"`. Loading an unknown name is a load error for that property, reported as other bad values are.
4. `PropertyKind::Enum` in the property sheet is a dropdown of the type's items, in value order. The MCP property tools accept the same forms scripts do.
5. Script analysis declares the property as `EnumItem`, as other enum values are typed.

### 5. Conditional visibility (`engine_core/LuaApi`, `ide/PropertySheet`)

1. `LuaField` gains an optional display rule: a sibling property name and an enum value the property must have to be shown. `lua_shown_when(field, "Shape", PhysicsShape::Hull)` is the helper.
2. The property sheet hides a field whose rule fails and re-lays out when the named property changes. A hidden property keeps its value, saves, loads, and is readable and writable from scripts and MCP.

## Testing

Written first, layer by layer.

**`sandbox/physics_tests.cpp`** (engine, no window), IDs P1–P12:

- **P1** A dynamic Box above an Anchored Box falls, then rests on it within a tolerance after 3 simulated seconds.
- **P2** An Anchored PhysicsObject's Transform does not change while stepping.
- **P3** Moving a PhysicsObject out of Workspace removes its body (its Transform stops changing); moving it back resumes.
- **P4** Moving a Folder that contains a PhysicsObject out of Workspace removes the body.
- **P5** A PhysicsObject with GameObject set starts at the GameObject's Transform, and after stepping both Transforms match. The GameObject's scale is kept.
- **P6** Two PhysicsObjects naming one GameObject: the first in tree order drives it, and the other has no body and warns once. Destroying the first hands the GameObject to the second.
- **P7** A script write to PhysicsObject.Transform during play teleports the body; so does a write to the driven GameObject's Transform.
- **P8** A script write to Velocity during play changes the body's velocity.
- **P9** Stop restores every PhysicsObject and GameObject property to the authored values, and a second Test simulates from them again.
- **P10** Physics write-back fires no `Changed` on PhysicsObject or GameObject.
- **P11** Hull from a Mesh built with `AddBox` collides like a Box of the same Size. A nil Mesh falls back to Box and warns once.
- **P12** Changing Shape, Size, Mass, and Anchored during play rebuilds or updates the body.

**`tests/LuaEngineTest.cpp`**: `Enum.PhysicsShape` assignment by item, name, and number. A wrong-typed value raises an error naming `PhysicsShape`. Writing Mass ≤ 0 is clamped, and a non-finite value raises.

**`tests/PropertiesTest.cpp`**: Shape shows as a dropdown of four items. Mesh is hidden for Box and shown for Hull. Project save and load round-trip `"Shape": "Hull"` and the Mesh reference.

**Live check**: launch the studio with MCP. Put an Anchored floor and a few dynamic PhysicsObjects driving GameObjects with Prefabs in Workspace, press Test, and screenshot that they fall and settle.
