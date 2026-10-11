# Touched and TouchEnded

Date: 2026-10-11. Status: approved design, not yet planned.

## Goal

Scripts learn when bodies start and stop touching, with the detail Roblox's
CollisionSummaries API gives (where, which way, how fast), through two
signals every body has. Trigger zones (CanCollide-off bodies) work too.

## Decisions

| Question | Decision |
| --- | --- |
| Signals | `Touched` and `TouchEnded` on every PhysicsBase (PhysicsObject, PlayerController, Brush). |
| When | Once when a pair of bodies starts touching, once when it stops. Not every frame. |
| Argument | A `TouchObject` for both signals. |
| Both sides | Each body of the pair fires its own event, naming the other. |
| Trigger zones | A CanCollide-off body fires on overlap (Box3D sensor), and so does the body overlapping it. |
| Opt-out | None: no CanTouch. Every body takes part; scripts filter in the handler. |
| Cost | Only bodies something listens to ask Box3D for contact events. A place with no listeners pays nothing for contacts. |
| When it runs | Only while the place runs, on the simulation thread, after each physics step. |
| Not in scope | Per-frame collision summaries, CanTouch, CanQuery, spatial queries, Terrain's own Touched. |

## 1. `TouchObject`

A read-only Luau value (frozen table, `typeof` reads `"TouchObject"`):

| Field | Type | Touched | TouchEnded |
| --- | --- | --- | --- |
| `Time` | number | The simulation time (seconds since Play) of the step it began. | Of the step it ended. |
| `Instance` | Instance | The other body, or the Terrain it touched. | The same. Still given when the other was destroyed or left Workspace. |
| `Point` | Vector3 | The contact's manifold points, averaged, world space. | The closest point between the two at the step they parted (Box3D distance query between their shapes). |
| `Normal` | Vector3 | Unit, pointing away from this body toward the other. | Along the gap at parting, away from this body. |
| `RelativeVelocity` | Vector3 | The other body's velocity at `Point` minus this body's, both from the start of the step (before the collision was resolved). | The same at the parting step. |

Velocity at a point is `v + w × (Point − center of mass)`. Terrain's is zero.

Trigger zone overlaps: `Point` is the other body's position, `Normal` is
`(0, 0, 0)`, `RelativeVelocity` is exact. When one of the pair was
destroyed or left Workspace before `TouchEnded`: `Point`, `Normal`, and
`RelativeVelocity` are the last ones known from while they touched.

## 2. Pairs, not shapes

Box3D reports per shape pair. A Brush cut into pieces, an unanchored Custom,
or a welded assembly has several shapes on one body; the engine counts
touching shape pairs per **body pair** (the two PhysicsBase ids, or a body
and a Terrain) and fires `Touched` when the count goes from 0 to 1 and
`TouchEnded` when it returns to 0. One step's `Touched` for a pair uses the
first shape pair's contact.

A welded assembly's members never touch each other (one Box3D body). A
member touching something outside its assembly fires on that member: the
member is the PhysicsBase whose shape touched.

## 3. Trigger zones

Every CanCollide-off body's shapes become Box3D sensors. Today they are
ordinary shapes whose mask is the ray bit alone (collision groups spec,
section 4). A sensor never collides, still has mass, and is still hit by
rays (Box3D's ray cast checks only filter bits). Filter bits for a sensor:

- `categoryBits` = its group's bit.
- `maskBits` = every group its group collides with, plus `kRayBit` (not
  `kGroundBit`: a PlayerController still does not stand on it).

So a trigger zone sees only bodies whose groups collide with its own. A
NoCollisionConstraint between a zone and a body keeps that body from
setting it off. Every solid shape enables sensor events, so it can visit
a sensor. Box3D only lets convex shapes visit, so an anchored Custom mesh
and Terrain never set off a zone.

## 4. Who asks for contact events

A body asks when its `Touched` or `TouchEnded` has a connection.
PhysicsWorld enables contact events on that body's shapes
(`b3Shape_EnableContactEvents`) and turns them off when the last
connection goes. Box3D reports a contact when either shape asks, so a body
that is listened to hears about everything it touches, and the other side
fires its own signal too if it is listened to. A pair that was already
touching when the first connection was made fires nothing until it parts
and touches again.

Sensor events are always on (section 3); a zone with no listeners only
costs Box3D's per-sensor overlap query.

## 5. Firing

After `b3World_Step`, PhysicsWorld reads Box3D's contact begin and end
events and sensor begin and end events. It updates the per-body-pair counts
and queues `Touched` or `TouchEnded` for each listened body, with its
`TouchObject`. Pre-step velocities for `RelativeVelocity` are saved before
the step for listened bodies only. The queue goes through the engine's
EventQueue like other instance events, so handlers run on the simulation
thread after the step, in event order.

Stop clears every count; no `TouchEnded` fires for pairs still touching at
Stop.

## Testing

Sandbox (`[physics][touch]`):

- A box falling onto an anchored floor: the box's `Touched` fires once with
  the floor as `Instance`, `Point` on the floor's top, `Normal` (0, -1, 0)
  for the box (pointing down toward the floor), and `RelativeVelocity` the
  floor's speed relative to the box's (up, about the fall speed). Resting
  for 2 s fires nothing more.
- Both sides: a listened floor also fires, with the box as `Instance` and
  `Normal` (0, 1, 0).
- Lifting the box off (teleport) fires `TouchEnded` once on each listener,
  with a `TouchObject`.
- A Brush cut into several pieces landing on a floor fires one `Touched`.
- A welded pair landing fires on the member that hit.
- A trigger zone: a CanCollide-off anchored body; a box falling through it
  fires `Touched` and `TouchEnded` on the zone and on the box, with
  `Normal` (0, 0, 0); the box still falls through. A ray still hits the zone.
- Collision groups and NoCollisionConstraint keep a pair from touching.
- A body nobody listens to asks Box3D for no contact events.
- A body destroyed while touching: the other's `TouchEnded` still names it.
- Lua: `Touched:Connect` receives a `TouchObject` whose fields read as
  above, which cannot be assigned to.
