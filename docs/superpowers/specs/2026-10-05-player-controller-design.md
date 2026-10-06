# PlayerController Design

2026-10-05 · builds on the Box3D physics (`2026-09-30-box3d-physics-design.md`): `PhysicsObject`, `PhysicsWorld`, the 240 Hz physics substep.

## Goal

A new instance, `PlayerController`: a rigid body for a walking character. It acts like a PhysicsObject (a dynamic Box3D body that pushes and is pushed, moves its GameObject, is driven by scripts through Velocity), but:

- it never rotates, except for a yaw that scripts set;
- its collider is an upright cylinder with zero contact friction, so it slides along walls;
- it hovers `StepHeight` above the ground, so it walks up anything that tall without a separate step-up routine;
- its own `Friction` slows it only while it stands on ground;
- it reports `OnGround` and `IsSliding`.

Reference: Box3D's character documentation (`docs/character.md`) and its rigid-body character sample (`samples/sample_character.cpp`, a rotation-locked dynamic body with zero-friction shapes and a shape-cast ground probe).

## Decisions

| Question | Decision |
| --- | --- |
| Rigid body or Box3D's character mover | A dynamic rigid body. The mover (`b3World_CastMover`, `b3SolvePlanes`) lives outside the simulation, so it neither pushes nor is pushed, unlike a PhysicsObject. |
| Class tree (Lua) | A new abstract `PhysicsBase` under PVInstance holds the shared properties. `PhysicsObject` and `PlayerController` both sit under it. PhysicsObject's own properties do not leak into PlayerController, which a Lua subclass of PhysicsObject would cause. |
| Collider | A 16-sided cylinder hull (Box3D has no round cylinder), radius `Radius`, from the hover gap above the feet up to `Height`. The hover gap is StepHeight capped at `Height - kMinSize`, so the cylinder always has length. Contact friction 0, restitution 0, all three rotation axes locked (`b3MotionLocks`), sleep off. |
| Why a cylinder | Its flat bottom makes StepHeight exact: an edge up to StepHeight passes under the collider and is climbed, any taller one meets a vertical side and blocks. A capsule's round bottom rides up over edges somewhat above StepHeight. Accepted costs: slight bumps sliding along walls as facet corners pass, and a flat top that can catch on ceiling edges. |
| Height | Ground to top of head. The hover gap is inside it. |
| Origin | Transform's position is the feet: the point on the ground below the collider's center. No Prefab recentering (`shape_center` does not apply). |
| Rotation | Only yaw about Y. Any written Transform, from a script, Properties, or a driven GameObject moved by something else, keeps its position and yaw and loses tilt and scale. |
| Scale | Radius and Height are not multiplied by the GameObject's Scale (`shape_scale` does not apply). |
| Ground probe shape | A flat puck: one ring of 16 points at 0.95 × Radius, rounding radius 0, cast straight down with `b3World_CastShape`. A capsule cast (2 points) is cheaper, but its round bottom reads tilted normals at ledge edges. The puck is one cast per controller per substep; the cost is negligible for a handful of players. |
| How scripts move it | By writing `Velocity`, as on a PhysicsObject. There is no MoveDirection or acceleration property. Friction pulls toward the ground's velocity and works against script writes every substep; scripts that need exact walking speed compensate. |
| Friction model | While OnGround, horizontal velocity relative to the ground decays by `exp(-Friction * dt)`. Friction is per second. |
| Hover direction | Always vertical, never along the ground normal, so nothing pushes a standing controller sideways and it does not creep down a walkable slope. |
| Mass | Applies to collisions with other bodies and to the impulses the controller puts into its ground. Hover and friction act on velocity and are independent of Mass. |
| MaxSlope units | Degrees. |
| Out of scope | Acceleration or MoveDirection, crouching (changing Height during play works but is not designed for), custom gravity, collision events, a configurable spring stiffness. |

## Properties

### PhysicsBase (abstract; not creatable, not in the Insert menu)

Moved unchanged from PhysicsObject: names, types, defaults, validation, and saved form stay what they are, so existing places load as before.

| Property | Type | Default |
| --- | --- | --- |
| Transform | Matrix4 | identity |
| Velocity | Vector3 | (0, 0, 0) |
| Anchored | boolean | false |
| Mass | number | 1, at least kMinMass |
| LinearDamping | number | 0, not below 0 |
| GameObject | GameObject? | nil |

### PhysicsObject

Its own fields, unchanged: AngularVelocity, AngularDamping, Friction, Bounciness, Shape, Size, Mesh.

### PlayerController

| Property | Type | Default | Rule |
| --- | --- | --- | --- |
| Friction | number | 8 | Not below 0. Ground friction per second. |
| Radius | number | 0.5 | At least kMinSize. |
| Height | number | 2 | At least kMinSize. The character's total height, ground to top of head; StepHeight never changes it. |
| StepHeight | number | 0.4 | Not below 0. Keeps the value written; where it would leave the collider shorter than kMinSize, the collider and the hover use `Height - kMinSize` instead. |
| MaxSlope | number | 45 | Degrees, clamped to [0, 89]. Slider 0–89. |
| OnGround | boolean | false | Read-only, not saved, not in history. |
| IsSliding | boolean | false | Read-only, not saved, not in history. |

A value that is not finite is refused, as on PhysicsObject. Each property is clamped only against its own limits, never against another property, so a place loads and scripts write the same result in any order. Radius, Height, and StepHeight mark the shape dirty; Friction and MaxSlope are read every step and need no dirty bit.

When Anchored, the controller is a static cylinder: no probe, no hover, no friction, and OnGround and IsSliding stay false.

## Each physics step

In `PhysicsWorld::Impl::step`, after the dirty changes are pushed into bodies and before `b3World_Step`, a controller pass runs for every unanchored PlayerController that has a body.

Below, StepHeight means the capped hover gap: `min(StepHeight, Height - kMinSize)`.

Constants (internal, in `PhysicsWorld.cpp`):

| Constant | Value | Meaning |
| --- | --- | --- |
| kProbeSides | 16 | Points in the puck's ring. |
| kProbeWidth | 0.95 | Puck radius over Radius. |
| kProbeNarrowest | 0.6 | Smallest puck radius over Radius when retrying. |
| kProbeSkin | 0.01 | How far above the collider's bottom face the puck starts. |
| kSnapMin | 0.1 | Least probe reach below the feet. |
| kHoverFrequency | 6 Hz | Hover spring frequency, damping ratio 1. |
| kRisingSpeed | 0.1 | Upward speed relative to the ground, in units per second, above which the controller counts as rising. |

### 1. Probe

The puck starts `kProbeSkin` above the collider's bottom face, centered under the body, and is cast straight down. It travels `kProbeSkin + StepHeight + max(StepHeight, kSnapMin)`: to the feet, then on by StepHeight (at least kSnapMin), so the controller stays stuck to stairs and slopes going down.

The cast keeps the closest hit and ignores the controller's own shape. If the puck overlaps a shape at its start (fraction 0), it is cast again narrower, in steps of 0.1 × Radius, down to kProbeNarrowest × Radius. If it still starts overlapping, there is no ground this step.

From the hit:

- `gap`: the distance from the collider's bottom face down to the hit, plus B3_LINEAR_SLOP, since Box3D stops a cast that far short. At rest it is StepHeight.
- `normal`: the hit normal.
- the ground body, and `ground_velocity`: `b3Body_GetWorldPointVelocity` at the hit point (zero for a static body).

### 2. Classify

- **Rising**: `velocity.y - ground_velocity.y > kRisingSpeed`, and either the controller is launched, or `gap > StepHeight + kProbeSkin` and `velocity.y > kRisingSpeed` (it is itself going up, not left behind by ground dropping away): a Velocity write since the last step added more than kRisingSpeed upward to what the body had. Launched clears when it stops rising or loses the ground. With no hit, the controller is in the air and none of the cases below apply.
- **OnGround**: a hit, the angle between `normal` and +Y is at most MaxSlope, not rising, and it reaches the ground: it was OnGround last step, or `gap <= StepHeight + fall + kProbeSkin`, where `fall` is how far it drops toward the ground this step. A falling controller is not OnGround until the step it reaches its hover height.
- **IsSliding**: a hit, that angle is above MaxSlope, and not rising.
- Otherwise both are false.

### 3. Hover (only when OnGround)

At or above the hover height (`gap >= StepHeight`), the controller goes there at once: it is moved down by `gap - StepHeight` and its vertical velocity becomes the ground's. A landing stops dead, and walking down stairs or a slope keeps to the ground with no lag. Only below it, when an edge has passed under the cylinder, does the spring lift it smoothly:

A critically damped spring at kHoverFrequency drives `gap` toward StepHeight, on vertical velocity relative to the ground. Integrated implicitly, as the pogo in Box3D's `samples/mover.cpp` is:

```
omega = 2π · kHoverFrequency
v_rel = velocity.y - ground_velocity.y
v_rel = (v_rel - omega² · dt · (gap - StepHeight)) / (1 + 2 · omega · dt + omega² · dt²)
velocity.y = ground_velocity.y + v_rel
```

Box3D adds this step's gravity after, so the pass also adds `-kGravity * dt` back. When the ground body is dynamic, it gets the controller's weight, the impulse `Mass * kGravity * dt` along Y at the hit point, and nothing else: standing on a box presses it down, and standing on one end of a plank tips it. The spring's own change is not handed to the ground. Doing that couples the two bodies into a loop that rings once the ground is about 8 times lighter than the controller (found in review: a Mass 80 player on a Mass 1 crate or controller).

Because the collider's bottom sits StepHeight above the feet, any edge up to StepHeight passes under it; once the puck is over that edge the probe hits its top and the spring lifts the controller onto it.

### 3b. Overhangs

Before any of this, every contact on the controller whose push on it points downward (a ceiling, or the underside of a ramp) takes out the part of its horizontal velocity that goes into that contact. With zero contact friction, walking into a sloped underside would otherwise press the controller into the ground; instead it stops there as at a wall.

### 4. Friction (only when OnGround)

```
h_rel = (velocity - ground_velocity) with its Y set to 0
dh = h_rel · (exp(-Friction · dt) - 1)
velocity += dh
```

The ground feels no reaction to friction. A script's Velocity write is how a controller pushes off, and Box3D sees no push for it, so friction's reaction alone would drag the ground forward under a walking controller (a light crate slid along with the player in review). A moving platform still carries the controller, because friction pulls toward the platform's velocity.

### 5. Sliding and air

When IsSliding or in the air, there is no hover and no friction. On a steep surface the cylinder's bottom edge rests on it with zero contact friction, and gravity slides it down.

### 6. After `b3World_Step`

The body writes Transform and Velocity as every body does (`store_simulated`, with the driven GameObject's scale kept). The controller also stores OnGround and IsSliding (`store_ground`). These writes fire no Changed and record no history.

Jumping needs nothing special: a script sets Velocity.Y upward. That write adds speed upward, so the controller counts as launched and the spring lets go at once; without that, the spring's damping would take a quarter of the jump in the first steps. A script that writes Velocity each frame keeping its Y adds nothing upward and launches nothing.

## Code structure

1. **`engine_instances/PhysicsBase.hpp/.cpp`** (new). The shared state, its setters, `driven_game_object`, the Dirty bits, `take_dirty`, `store_simulated(transform, velocity)`, and the Lua fields for the six shared properties. Registered with `register_lua_class("PhysicsBase", "PVInstance", ...)` and no factory. `physics_body()` is true.
2. **`engine_instances/PhysicsObject.hpp/.cpp`**. Inherits PhysicsBase and keeps only its own state and fields. `register_lua_class("PhysicsObject", "PhysicsBase", ...)`. Angular velocity moves to a PhysicsObject-only store call.
3. **`engine_instances/PlayerController.hpp/.cpp`** (new). Its fields, as above. `set_transform` overrides the base one to keep only position and yaw. `store_ground(bool on_ground, bool sliding)`. `register_lua_class("PlayerController", "PhysicsBase", ...)`, the same suited parents as PhysicsObject.
4. **`engine_core/PhysicsWorld.cpp`**. Works on `PhysicsBase&` wherever it handles any body: reconcile, first-in-tree-order, push, pull, follow_driven. It branches on the class only for:
   - `make_shape`: a PlayerController gets the cylinder hull from the existing Cylinder point builder, with friction 0, restitution 0, rotation locks, and sleep off. Density is `Mass / volume`, as for every shape.
   - `push`: a PlayerController pushes no angular damping or angular velocity, and writes poses with yaw only.
   - the controller pass, as a new function between push and `b3World_Step`.
   - `shape_center` and `shape_scale` return the origin and 1 for a PlayerController.
   - `collision_outline` draws the cylinder, plus one line from the feet up to the collider's bottom so the hover gap shows.
5. **Factories and Studio**. `ScriptBindings.cpp` (`register_lua_creatable`) and `Project.cpp` (load) create PlayerController. `ClassOrder.hpp` places it next to PhysicsObject. `IdeIcons.cpp` gives it PhysicsObject's default icon. `GameView.cpp` draws its outline when selected.
6. **Docs**. `engine_instances/README.md` lists PhysicsBase and PlayerController.

## Testing

In `sandbox/physics_tests.cpp`, stepping a real world headlessly:

- **Properties**: defaults; clamping (Height to at least kMinSize, MaxSlope to [0, 89], non-finite refused; writing StepHeight 3 on a Height 2 controller leaves Height 2 and StepHeight 3, and its collider is kMinSize tall at the top); a save and load round trip; `IsA("PhysicsBase")` true for both classes and `IsA("PhysicsObject")` false for a PlayerController; OnGround and IsSliding refuse writes and are not saved.
- **PhysicsObject unchanged**: every existing physics test passes as is.
- **Hover**: dropped onto a floor, the controller settles with gap within 1% of StepHeight and OnGround true.
- **Steps**: walking at a ledge 0.9 × StepHeight tall climbs it; at 1.1 × StepHeight it is blocked.
- **Slopes**: on a 30° ramp with Velocity zero it does not creep; on a 60° ramp IsSliding is true and it slides down.
- **Friction**: horizontal speed decays as `exp(-Friction · t)` within tolerance; on a kinematic platform moving sideways it is carried along; in the air the speed does not decay.
- **Jump**: an upward Velocity.Y write clears OnGround, and the controller is not pulled back down.
- **Wall**: pushing diagonally into a wall keeps the speed along it.
- **No rotation**: after a spinning box hits it, its rotation is unchanged; a tilted Transform written by a script keeps only its yaw.
- **Ground reaction**: standing on one end of a dynamic plank on a pivot tips it.

In the studio: one playtest through the anarchy MCP with a controller walking up steps and a slope, with one studio open, quit right after.
