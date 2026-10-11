# Joint constraints

Date: 2026-10-11. Status: approved design, not yet planned. Builds on
Assemblies (`2026-10-11-assemblies-design.md`) and NoCollisionConstraint.

## Goal

Constraints that join two assemblies with a moving Box3D joint, so a user can
build a drivable car (a chassis, four wheels with suspension, a motor, and
steering) and hinges, sliders, springs, ropes, and ball joints, in Anarchy
Studio. Engine distances are units.

## Decisions

| Question | Decision |
| --- | --- |
| Classes | `RevoluteConstraint`, `PrismaticConstraint`, `DistanceConstraint`, `SphericalConstraint`, `WheelConstraint`, under a new abstract `JointConstraint`, under the existing `Constraint`. |
| Naming | Box3D's. Classes and properties are named after Box3D's joints and fields, not Roblox's. |
| Shared properties | `Constraint` already holds `PVInstance1`, `PVInstance2`, `Enabled`, and `Active` for every constraint, old and new. `JointConstraint` adds only `CollideConnected`. |
| Assemblies | A joint joins nothing: each end stays in its own assembly, and the joint links the two assemblies' bodies. |
| Ends | PVInstance1 is the base (Box3D's body A), PVInstance2 the moving part (body B). Order matters for joints. |
| Frame | One frame, held where placed: the joint sits at PVInstance2's pivot, captured when the joint starts, so nothing moves at Play. A DistanceConstraint uses both ends' pivots as its two points. |
| Axis | The frame's Y axis (the engine is Y-up, and a Cylinder stands along Y). |
| Units | Angles in degrees, angular speeds in degrees per second, lengths in units. |
| When it runs | Joints exist only in play. `Active` reads the same in the editor and in play. |
| Not in scope | Box3D's motor, parallel, filter, and weld joints; breakable joints (joint events); editor handles for limits; moving joint-connected assemblies together in the editor; a PlayerController or Terrain end. |

## 1. Classes

### `JointConstraint` (abstract)

`engine_instances/JointConstraint.{hpp,cpp}` (its own files: five classes and
their properties would double Constraint.cpp). Derives from
`Constraint`; `rigid()` false, `joins()` false. Lua class `JointConstraint`,
base `Constraint`, no factory.

| Property | Type | Saved | Default | Meaning |
| --- | --- | --- | --- | --- |
| `CollideConnected` | boolean | yes | false | Whether the two ends' bodies collide with each other. |

Each joint class adds its own saved properties below. Every one is a saved
registry property, takes effect live in play (§3), and is checked as §4 says.
Read-only properties are not saved, fire no Changed, and read 0 outside play
or while the joint is inactive.

Each class is creatable (`register_lua_creatable`): `Instance.new`, paste, the
Insert list (suited under Workspace and any PVInstance, as the other
constraints), MCP `create_instance`. Each is added to `Project.cpp`'s
`class_registry`, `ClassOrder.hpp`, `InsertPopup.cpp`, LuaApi docs, the
README, and gets an icon in `resources/icons/` with its ATTRIBUTION line.

### `RevoluteConstraint` (`b3RevoluteJoint`)

Body B turns about the axis.

| Property | Type | Default | Box3D field |
| --- | --- | --- | --- |
| `EnableSpring` | boolean | false | `enableSpring` |
| `Hertz` | number | 1 | `hertz` |
| `DampingRatio` | number | 0.7 | `dampingRatio` |
| `TargetAngle` | number (deg) | 0 | `targetAngle` |
| `EnableLimit` | boolean | false | `enableLimit` |
| `LowerAngle` | number (deg) | -45 | `lowerAngle` |
| `UpperAngle` | number (deg) | 45 | `upperAngle` |
| `EnableMotor` | boolean | false | `enableMotor` |
| `MotorSpeed` | number (deg/s) | 0 | `motorSpeed` |
| `MaxMotorTorque` | number | 10000 | `maxMotorTorque` |
| `Angle` | number (deg), read-only | | `b3RevoluteJoint_GetAngle` |

### `PrismaticConstraint` (`b3PrismaticJoint`)

Body B slides along the axis and does not turn.

| Property | Type | Default | Box3D field |
| --- | --- | --- | --- |
| `EnableSpring` | boolean | false | `enableSpring` |
| `Hertz` | number | 1 | `hertz` |
| `DampingRatio` | number | 0.7 | `dampingRatio` |
| `TargetTranslation` | number (units) | 0 | `targetTranslation` |
| `EnableLimit` | boolean | false | `enableLimit` |
| `LowerTranslation` | number (units) | -1 | `lowerTranslation` |
| `UpperTranslation` | number (units) | 1 | `upperTranslation` |
| `EnableMotor` | boolean | false | `enableMotor` |
| `MotorSpeed` | number (units/s) | 0 | `motorSpeed` |
| `MaxMotorForce` | number | 10000 | `maxMotorForce` |
| `Translation` | number (units), read-only | | `b3PrismaticJoint_GetTranslation` |

### `DistanceConstraint` (`b3DistanceJoint`)

Holds the distance between PVInstance1's pivot and PVInstance2's pivot: a rod
when `EnableSpring` is off, a spring when on, a rope with a limit.

| Property | Type | Default | Box3D field |
| --- | --- | --- | --- |
| `Length` | number (units) | 0 | `length`. 0 means the distance when the joint starts. |
| `EnableSpring` | boolean | false | `enableSpring` |
| `Hertz` | number | 1 | `hertz` |
| `DampingRatio` | number | 0.7 | `dampingRatio` |
| `EnableLimit` | boolean | false | `enableLimit` |
| `MinLength` | number (units) | 0 | `minLength` |
| `MaxLength` | number (units) | 10 | `maxLength` |
| `EnableMotor` | boolean | false | `enableMotor` |
| `MotorSpeed` | number (units/s) | 0 | `motorSpeed` |
| `MaxMotorForce` | number | 10000 | `maxMotorForce` |
| `CurrentLength` | number (units), read-only | | `b3DistanceJoint_GetCurrentLength` |

Box3D's lower and upper spring forces are left at their defaults (unbounded).

### `SphericalConstraint` (`b3SphericalJoint`)

Body B turns freely about the pivot.

| Property | Type | Default | Box3D field |
| --- | --- | --- | --- |
| `EnableSpring` | boolean | false | `enableSpring` (pulls back to the placed rotation) |
| `Hertz` | number | 1 | `hertz` |
| `DampingRatio` | number | 0.7 | `dampingRatio` |
| `EnableConeLimit` | boolean | false | `enableConeLimit` |
| `ConeAngle` | number (deg) | 45 | `coneAngle`, about the axis |
| `EnableTwistLimit` | boolean | false | `enableTwistLimit` |
| `LowerTwistAngle` | number (deg) | -45 | `lowerTwistAngle` |
| `UpperTwistAngle` | number (deg) | 45 | `upperTwistAngle` |
| `EnableMotor` | boolean | false | `enableMotor` |
| `MotorVelocity` | Vector3 (deg/s, world axes) | 0,0,0 | `motorVelocity` |
| `MaxMotorTorque` | number | 10000 | `maxMotorTorque` |

Box3D's `targetRotation` stays identity: the spring pulls back to the rotation
as placed.

### `WheelConstraint` (`b3WheelJoint`)

PVInstance1 is the chassis, PVInstance2 the wheel. The wheel spins about its
own Y; the suspension travels, and steering turns, along the chassis's up
(§2, Wheel frame).

| Property | Type | Default | Box3D field |
| --- | --- | --- | --- |
| `EnableSuspension` | boolean | true | `enableSuspensionSpring` |
| `SuspensionHertz` | number | 4 | `suspensionHertz` |
| `SuspensionDampingRatio` | number | 0.7 | `suspensionDampingRatio` |
| `EnableSuspensionLimit` | boolean | false | `enableSuspensionLimit` |
| `LowerSuspensionLimit` | number (units) | -0.5 | `lowerSuspensionLimit` |
| `UpperSuspensionLimit` | number (units) | 0.5 | `upperSuspensionLimit` |
| `EnableSpinMotor` | boolean | false | `enableSpinMotor` |
| `SpinMotorSpeed` | number (deg/s) | 0 | `spinSpeed` |
| `MaxSpinTorque` | number | 10000 | `maxSpinTorque` |
| `EnableSteering` | boolean | false | `enableSteering` |
| `SteeringHertz` | number | 5 | `steeringHertz` |
| `SteeringDampingRatio` | number | 0.7 | `steeringDampingRatio` |
| `TargetSteeringAngle` | number (deg) | 0 | `targetSteeringAngle` |
| `MaxSteeringTorque` | number | 10000 | `maxSteeringTorque` |
| `EnableSteeringLimit` | boolean | false | `enableSteeringLimit` |
| `LowerSteeringLimit` | number (deg) | -30 | `lowerSteeringLimit` |
| `UpperSteeringLimit` | number (deg) | 30 | `upperSteeringLimit` |
| `SpinSpeed` | number (deg/s), read-only | | `b3WheelJoint_GetSpinSpeed` |

With steering off, Box3D holds the wheel facing forward.

### Defaults

Box3D's own defaults leave every max torque and force at 0, so turning a motor
on would do nothing. The defaults above are chosen so that enabling a motor or
steering on parts of the default Mass (50) moves them, and so that a car of
default-Mass parts rides on its suspension. The implementation may retune the
numbers (not the names) if the car test (§5) shows they do not; any change is
written back here.

## 2. Activity and frames

### When a joint is active

A `JointConstraint` is **Active** when all hold:

- `Enabled` is true, and the constraint is in Workspace.
- Both ends resolve (Assemblies' existing rules: an Attachment or Bone stands
  for its holder, a GameObject for the physics node that drives it).
- The two ends are in **different assemblies**.
- Each end's assembly has physics (a body to join): a lone Camera or plain
  GameObject end is inactive.
- Not both assemblies are anchored.
- Neither end's node, nor its assembly's root, is a PlayerController (its
  movement code would fight the joint). A Terrain end never resolves, as today.

`Assemblies` computes this in edit mode and in play alike (as `AssemblyRoot`),
so `Active` shows in the editor whether a joint will hold. Joints never join
assemblies, change roots, or take part in the rigid rules.

### What it connects

Each end's whole assembly: the joint is made between the two assemblies' Box3D
bodies (a compound's body for a welded assembly). Anchoring is each
assembly's own: an anchored base is static, so a wheel on an anchored stand
spins in place. A body a hand holds (kinematic) may be an end.

### The frame

Pivots come from `Assemblies::end_pivot` (scale removed). The engine axis is Y;
Box3D's joints use other axes, so the joint's world frame `W` maps them:

| Class | Box3D axis | `W` |
| --- | --- | --- |
| Revolute | turns about frame Z | PVInstance2's pivot × (−90° about X), so Box3D's Z is the pivot's Y |
| Prismatic | slides along frame A's X | PVInstance2's pivot × (+90° about Z), so Box3D's X is the pivot's Y |
| Spherical | cone about frame A's Z, twist about frame B's Z | as Revolute |
| Wheel | spins about frame B's Z, travels and steers along frame A's X | see Wheel frame |
| Distance | points only | point A = PVInstance1's pivot position, point B = PVInstance2's |

Box3D's local frames are `W` in each body's frame: `localFrameA = bodyA⁻¹ · W`,
`localFrameB = bodyB⁻¹ · W`, so the two frames coincide when the joint starts
and nothing moves.

**Wheel frame.** `W`'s origin is the wheel's pivot. Its Z is the wheel pivot's
Y (the spin axis). Its X is the chassis pivot's Y made perpendicular to that
spin axis and normalized (the suspension and steering axis); if that is
shorter than 0.001 (the wheel lies flat, its axle pointing up), the chassis
pivot's X is used instead, made perpendicular the same way. Its Y is Z × X.

### Capture and rebuild

The frame is captured when the joint starts: at Play, or when it becomes
active during play (made by a script, `Enabled` turned on, an end set, an
assembly change that newly separates its ends). At capture the engine records
`W` relative to each end's **member** (node frame), not body, and a `Length`
of 0 becomes the distance between the points then.

When an end's assembly body is remade in play (a weld added or removed, a
compound rebuilt, the body made again for a shape change), the joint is made
again from those recorded member offsets: no jump. Turning `Enabled` off and
on, or changing an end, captures afresh from where things are now.

## 3. Physics

`Assemblies` produces `joints()`: a sorted, reserved vector of active joints
(constraint id, the two ends' nodes), rebuilt when the assembly revision
moves, as `no_collide_pairs()` is.

`PhysicsWorld` keeps a joint record per active constraint (its `b3JointId`, the
captured member offsets, the bodies it was made between, the settings
revision it applied) and, each sync in play:

1. Compares `joints()` with its records: destroys joints no longer listed,
   creates new ones (capture, §2), and remakes any whose either body changed.
2. Applies settings whose revision moved. Every `JointConstraint` setter bumps
   a settings revision on the constraint; the record applies the class's
   settings through Box3D's setters (`b3RevoluteJoint_SetMotorSpeed`,
   `b3RevoluteJoint_SetLimits`, `b3WheelJoint_SetTargetSteeringAngle`,
   `b3Joint_SetCollideConnected`, …) and wakes both bodies
   (`b3Joint_WakeBodies`), so a parked car answers its throttle.
3. After the step, writes each joint's read-only value (`Angle`,
   `Translation`, `CurrentLength`, `SpinSpeed`) into its constraint, as a
   plain field.

Joints are destroyed before the bodies they join, and all of them at Stop
(the play world goes; Stop restores every property as it does now).

A place with no JointConstraint in Workspace runs none of this. A sync where
nothing changed allocates nothing and touches only the revision compare.

## 4. Values

Every setter returns why it refused, changing nothing:

- **Refused:** NaN or infinity anywhere; a negative `Hertz`, `DampingRatio`,
  `SuspensionHertz`, `SuspensionDampingRatio`, `SteeringHertz`,
  `SteeringDampingRatio`, max torque or force, `Length`, `MinLength`, or
  `MaxLength`.
- **Clamped:** Revolute's `LowerAngle` and `UpperAngle` and Spherical's twist
  angles to ±178.2° (Box3D's ±0.99π); `ConeAngle` to 0..90 (Box3D's cone is at most π/2).
- **Order:** a lower limit above its upper is saved as written; the joint uses
  the smaller as lower, as Box3D's setters do. So the two can be typed in
  either order.

All properties save, load, undo, Copy/Paste, and Duplicate as registry
properties do. Ends inside a copied set are remapped to the copies, as Weld's
are, so a duplicated car keeps its joints.

## 5. Editor

Nothing simulates in the editor. Dragging moves only what is selected; select
the Model to move a whole car. A selected joint is drawn by `ConstraintLines`
as the others are (a line between its ends), plus a short line along its axis
at the frame `W` (for a Wheel, the spin axis and the suspension axis).

## 6. Testing

Release sandbox tests, each written failing first:

- **Activity:** inactive for the same assembly, both anchored, a
  PlayerController end, a Terrain end, `Enabled` off, outside Workspace;
  `Active` reads the same in the editor as in play.
- **Held where placed:** with gravity off, no body moves on the first step of
  play, for each class. A compound remade in play (a weld added to the
  chassis) makes no jump.
- **Revolute:** the motor reaches `MotorSpeed`; the limit stops it; the spring
  returns it to `TargetAngle`.
- **Prismatic:** the motor slides it; the limit stops it.
- **Distance:** a spring settles a hanging weight below its length; a rod holds
  its length; `Length` 0 holds the placed distance.
- **Spherical:** the cone limit holds a swinging weight.
- **Wheel:** the suspension sags under the chassis's weight; the spin motor
  turns the wheel at `SpinMotorSpeed`; steering reaches `TargetSteeringAngle`.
- **Live:** a script writing `MotorSpeed` mid-play takes effect and wakes a
  sleeping body; Stop restores the values.
- **Car:** a chassis on four wheels on a floor drives forward under its spin
  motors, stays upright, and turns when its front wheels steer.
- **Values:** one refusal per rule in §4; clamping; lower/upper in either
  order.
- **Saving:** save and load keep every property; duplicating a car remaps its
  joints' ends.
- **Lua:** completion lists the classes and their properties.
