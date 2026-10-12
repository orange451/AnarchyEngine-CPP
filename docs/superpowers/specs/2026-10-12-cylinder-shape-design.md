# Cylinder shape

Date: 2026-10-12. Status: design agreed in conversation, spec awaiting review.
Builds on the physics world (`PhysicsWorld`), Assemblies, and joint
constraints (`2026-10-11-joint-constraints-design.md`).

## Goal

A PhysicsObject whose Shape is Cylinder collides as a true cylinder, not as a
polygon. A cylinder rolls as smoothly and as straight as a sphere does, and it
rests on its side or on a cap without creeping. The first user is the H1Z1
car's wheels, but the shape is general: barrels, logs, pipes, coins.

Engine distances are units.

## Why

Box3D has no cylinder shape. `b3CreateCylinder` only tessellates one into a
hull of at most 32 sides. The engine builds a Cylinder from four hulls of 16
sides, each turned a quarter of a side, so together they roll as one of 64
sides (`kRoundSides`, `kCylinderLayers` in `PhysicsWorld.cpp`).

Measured on the H1Z1 car (a probe in a sandbox test, the car driven straight
for about 120 units on a flat floor, 5 Hz suspension):

| Wheel collider | Sideways drift | Body roll (RMS) |
| --- | --- | --- |
| Sphere | 0.1 | 0.000° |
| Layered cylinder, 4 × 16 sides | 2.2 | 0.55° |
| One hull of 32 sides | 3.3 | 0.05° |
| Crowned hulls (three tread profiles) | 1.4 to 14 | 0.004° to 0.006° |
| Ring of capsules round the rim | 0.6 balanced, 2.4 to 4.7 off-balance | 0.003° |

The cause is how a polygon touches the ground. Each time the wheel tips onto
its next edge, its contact points change features. Box3D's warm-starting then
starts each new point from nothing, and the solver gets a small impulse. Those
impulses add up to rocking and to steering. More sides make the impulses
smaller and more frequent, not gone. A true cylinder has no edges on its
tread. Its contact points stay the same two points (the two ends of the line
it touches along) the whole way round, so warm-starting carries through.

A true cylinder's tread is everywhere exactly its radius from the axis. So
unlike the crowned hulls and the capsule ring, leaning moves the contact
across the tread without changing the rolling radius.

## Decisions

| Question | Decision |
| --- | --- |
| Where | A new shape type in Box3D, `b3_cylinderShape`, with exact contact code. Not a wheel-only feature in WheelConstraint (that was option 1: a cast wheel). |
| Box3D changes | Our own new file, `cmake/box3d/cylinder.c`, plus one patch to Box3D's files, applied when Box3D is fetched. The pin stays a clean upstream commit. |
| Shape | `b3Cylinder { center1, center2, radius }`: the centres of its two caps, and its radius. Shaped like `b3Capsule`. |
| Pairs | Cylinder against sphere, capsule, hull, mesh (one triangle at a time), and cylinder. Not against a compound or a height field: the engine makes neither. |
| Contact | Exact on the side and caps. A separation search on the cylinder's core (the cylinder shrunk by a small margin, as a Box3D capsule is a segment plus a radius) gives the normal and depth. Contact points come from the cylinder's feature facing the other shape (§3). |
| Queries | Ray casts are exact. Overlaps, shape casts, closest points, the mover, time of impact, and sensors use a stand-in: a 32-sided hull (`b3CreateCylinder`, its 2 rings × 32 points) kept with the shape, so Box3D's own hull code answers them (§4). |
| Engine use | A Cylinder PhysicsObject makes one `b3_cylinderShape`. Under non-uniform scale across its round axes it would be elliptical, which a true cylinder cannot be, so it stays the layered hulls. |
| Users | Nothing changes in Lua or Properties. Shape = Cylinder just collides correctly. |
| Not in scope | A true Cone; the PlayerController's cylinder (it never rolls; its hull and ground probe stay); a cylinder as a Box3D compound child or against a height field; exact distance code for queries; Box3D's own debug draw (the engine draws outlines itself); sending the change upstream (welcome later, not required). |

## 1. The shape

In `include/box3d/types.h` (patch):

```c
/// A solid cylinder: two flat caps joined by a round side.
typedef struct b3Cylinder
{
	/// Local centre of the first cap
	b3Vec3 center1;
	/// Local centre of the second cap
	b3Vec3 center2;
	/// The radius of the caps
	float radius;
} b3Cylinder;
```

`b3_cylinderShape` joins `b3ShapeType` before `b3_shapeTypeCount`. `b3Shape`'s
union gains `b3Cylinder cylinder` (`shape.h`).

New API, declared in `collision.h` and `box3d.h` (patch), defined in
`cylinder.c` (ours) unless noted:

| Function | What it does |
| --- | --- |
| `b3CreateCylinderShape(body, def, cylinder)` | As `b3CreateCapsuleShape` (in `shape.c`, patch). |
| `b3Shape_GetCylinder`, `b3Shape_SetCylinder` | As the capsule's (in `shape.c`, patch). |
| `b3ComputeCylinderMass(cylinder, density)` | Exact mass, centre, and inertia of a solid cylinder about any axis direction. |
| `b3ComputeCylinderAABB(cylinder, transform)` | Exact: each cap disc's extent, combined. |
| `b3RayCastCylinder(cylinder, input)` | Exact: ray against the infinite tube clipped to the caps, and against each cap disc. A ray starting inside reports overlap as `b3RayCastCapsule` does. |
| `b3MakeCylinderProxy(cylinder, buffer)` | The stand-in point cloud (§4). |
| `b3CollideCylinderAndSphere`, `b3CollideCylinderAndCapsule`, `b3CollideHullAndCylinder`, `b3CollideCylinders`, `b3CollideTriangleAndCylinder` | The manifolds (§3). |

A cylinder is not sphere-swept: its proxy radius and contact radius are 0.

The table's `b3MakeCylinderProxy` is `b3MakeCylinderHull(cylinder)` instead:
the stand-in hull (§4), which the shape keeps beside its `b3Cylinder`
(`shape->cylinderHull`, in the world's hull database). `b3SupportDistance`
(§3.1) is added to `distance.c` (patch) and declared in `collision.h`.

## 2. Plumbing in Box3D (patch)

Every place that switches on a shape's type handles the cylinder:

| File | What |
| --- | --- |
| `shape.c` | Create, destroy, AABB, mass, extent, ray cast, proxy, closest point, overlap, sensor overlap, get/set. About 50 cases; most are one line calling into `cylinder.c`. |
| `contact.c` | Register the pairs: `(cylinder, sphere)`, `(cylinder, capsule)`, `(hull, cylinder)`, `(mesh, cylinder)`, `(cylinder, cylinder)`. Dispatch each to its manifold function. Widen the assertion in `b3CreateContact` that a second shape is a sphere, capsule, or hull. |
| `mesh_contact.c` | Dispatch a triangle against a cylinder. A cylinder's contact radius is 0. |
| `world_snapshot.c`, `recording.c`, `recording_ops.inl`, `recording_replay.c` | Save and load a cylinder's bytes, and record and replay `CreateCylinderShape` and `ShapeSetCylinder`, as for a capsule. |
| `physics_world.c`, `sensor.c` | Their few type switches. |

Box3D's debug-shape hook (`createDebugShape`) is handed a cylinder as its
stand-in hull, so a debug renderer draws a 32-sided prism. The engine does not
use Box3D's debug draw; this only keeps Box3D complete.

### Patch mechanics

- `cmake/box3d/cylinder.patch` is a `git diff` against the pinned commit. The
  root `CMakeLists.txt` applies it right after `FetchContent_Populate(box3d)`.
- The apply is guarded: when the patch already applies in reverse, it is
  skipped. A fresh fetch patches once; re-configuring does not patch twice.
- When the patch does not apply (a pin bump moved Box3D), configuring fails
  with a message naming the patch. It never builds unpatched.
- `cmake/box3d/CMakeLists.txt` adds `cylinder.c` to the `box3d` target, with
  Box3D's `src` as a private include directory, and the same flags (C11,
  `c11_shim.h`, `/O2` in every configuration).
- A worktree that copies main's `build/_deps/box3d-src` copies it patched,
  which is what it needs (`FULLY_DISCONNECTED` never refetches).

## 3. Contact

Each manifold function takes the two shapes and the transform between them,
and fills a `b3LocalManifold` with at most 4 points. Points within the
speculative distance (`B3_SPECULATIVE_DISTANCE`) are kept, as for every other
pair.

### 3.1 Normal and depth

- **The core:** the cylinder shrunk by a margin on every side,
  `margin = min(B3_SPECULATIVE_DISTANCE, ¼ · min(radius, halfHeight))` (0.02
  units for any wheel-sized cylinder). The true shape is the core grown by the
  margin: its side and caps are exact, and its rims are rounded by the margin.
  Box3D treats a capsule the same way (a segment plus a radius).
- **GJK on support functions:** `b3SupportDistance` (§1) runs Box3D's own
  simplex solver with support functions instead of point clouds. The core's
  support is `centre ± (halfHeight − margin)·axis + (radius − margin)·unit(d −
  (d·axis)·axis)`, the cap chosen by the sign of `d·axis`. The other shape's
  core is its points (a hull, a triangle, a capsule's segment), or another
  cylinder's core. The distance between cores, less both margins and any
  capsule radius, is the separation; the normal points between the closest
  points. It stays exact and smooth while the shapes overlap by less than the
  margin, which covers resting and rolling contact.
- **Deeper overlap** (the cores themselves overlap: a body spawned inside
  another, a fast impact) falls back to Box3D's own pair functions, with the
  cylinder's stand-in hull (§4) in its place: `b3CollideHulls`,
  `b3CollideHullAndCapsule`, `b3CollideTriangleAndHull`. Contact is faceted
  only then, and only until the bodies push apart.
- **A sphere** needs no search: the closest point on the solid cylinder, exact
  at any depth.

### 3.2 The cylinder's feature

From the normal, the cylinder offers the feature it presents to the other
shape:

| Normal against the axis | Feature | Points |
| --- | --- | --- |
| Within 10° of the axis | The near cap: a polygon of 16 points on its rim, fixed in the cylinder's frame | up to 16, reduced to 4 |
| Otherwise | The side line: the segment of the tread nearest the other shape, from one cap's rim to the other's | 2 |

The side line covers leaning and edge contact too. As a wheel leans, one end
of its line rises away and drops out past the speculative distance, and the
other end is the rim point it rests on. So there is no separate rim mode to
switch into.

### 3.3 Clipping and points

- The other shape's feature: a hull's face most against the normal, the
  triangle, a capsule's segment (or its nearest point when not parallel), or
  a sphere's nearest point.
- The cylinder's feature is clipped to the other's face (side planes,
  Sutherland–Hodgman), as Box3D clips hull against hull.
- Each point keeps its separation along the normal. More than 4 points reduce
  as Box3D's hull pairs reduce them: deepest first, then farthest, then the
  largest area.

### 3.4 Feature IDs

A point's `b3FeaturePair` names the cylinder's feature, not where on the round
surface it is:

- **Side line:** one ID for each end (cap 1, cap 2), whatever the angle round
  the axis. A rolling wheel's two points keep their IDs all the way round, so
  warm-starting never resets. This is what makes it smooth.
- **Cap:** the cap (1 or 2) and the rim sample (0 to 15), fixed in the
  cylinder's frame. A resting cylinder's points keep their IDs.
- **The other shape's half:** its face or edge index, as in the hull pairs.
- **Order:** Box3D solves a manifold's points one after another, favouring the
  first a little. On a spinning cylinder that is a steady torque about its
  rolling direction, which its spin turns into yaw: a free wheel crept 3.7 cm
  in 20 units, and the H1Z1 car turned 29°. Box3D's own capsule does the same.
  So the side line's two ends swap order every 1/128 of a turn, and the bias
  cancels as it rolls. The ids stay with their ends.

### 3.5 Against a triangle

`b3CollideTriangleAndCylinder` follows `b3CollideTriangleAndHull`. It treats
the triangle as a one-face hull, fills the manifold's `feature`,
`triangleNormal`, and `squaredDistance`, and honours `triangleFlags`, so
`mesh_contact.c`'s ghost-collision reduction keeps a cylinder rolling across
the seams between triangles from catching on internal edges. Terrain is mesh
shapes, so this is the wheel-on-terrain path.

## 4. Queries

Box3D's distance, shape cast, overlap, mover, time-of-impact, and sensor code
take a shape as `b3ShapeProxy` (points plus a radius) or as a hull. A cylinder
shape keeps a stand-in hull, `b3CreateCylinder` with 32 sides placed on its
axis, in the world's hull database: its 64 points are the proxy, and its faces
let `b3ShapeCastHull`, `b3OverlapHull`, and `b3CollideMoverAndHull` answer for
it. The stand-in is at most `radius·(1 − cos(π/32))` (0.5 % of the radius)
inside the true surface.

Using the proxy for these queries is acceptable because none of them makes
the rolling contact:

- the PlayerController's ground probe (`b3World_CastShape`) and feet overlap
  (`b3World_OverlapShape`) when standing on a cylinder;
- `Touched` points (`b3Shape_GetClosestPoint`) and sensors (a CanCollide-off
  cylinder);
- continuous collision for fast bodies.

Ray casts (`b3World_CastRay`, `b3Shape_RayCast`: picking, the move tools,
scripts' raycasts) use `b3RayCastCylinder`, which is exact.

## 5. Engine

In `PhysicsWorld.cpp`:

- **The shape:** a Cylinder PhysicsObject makes one `b3CreateCylinderShape`
  (its cap centres at ±Size.y/2 along local Y, radius Size.x/2, at the body's
  scale; `placed_point` for an assembly member, as the capsule does).
- **Mass:** its volume (π r² h) shares Mass as every shape's does, through
  `shape_density`, so Massless and a disabled PlayerController's rule hold.
- **Elliptical:** when the body's scale differs between X and Z by more than
  1e-4 (an elliptical cylinder), it stays the layered hulls exactly as today.
  `kRoundSides`' comment is corrected: a hull's limit is 128 edges, and a
  prism has 3 per side.
- **Outline:** `collision_outline` draws a Cylinder from its true shape (two
  rim circles and four side lines) instead of the hull's edges.
- **Unchanged:** the Cone, the PlayerController's cylinder, and Shape = Hull
  or Custom.

## 6. Testing

Sandbox tests (Catch2, `sandbox/`), in Debug and Release.

### Box3D-level (`cylinder_shape_tests.cpp`)

1. **Mass:** `b3ComputeCylinderMass` matches the formulas for mass, centre,
   and inertia about the axis and across it, within 1e-5 relative.
2. **Bounds:** `b3ComputeCylinderAABB` holds the exact shape, turned at 0°,
   30°, and 90°, within 1e-5.
3. **Ray casts:** `b3RayCastCylinder` against the side, a cap, the rim edge, a
   miss, a start inside, and along the axis, against analytic answers within
   1e-5.
4. **Each pair's manifold,** against known poses:
   - cylinder on its side on a box: 2 points, normal up, depth right;
   - on its cap: 4 points;
   - on a sphere, on a capsule, crossed with another cylinder;
   - on one triangle, and on a triangle's shared edge.

### World-level (`physics_tests.cpp`, `cylinder_contact_tests.cpp`)

5. **Rolling:** a cylinder of radius 0.5 rolled 20 units across a box floor.
   Its centre's height varies by under 0.1 mm, it drifts sideways under 1 mm,
   and its axis tilts under 0.01°.
6. **Rolling over seams:** the same across a triangle mesh floor whose
   triangles are 1 unit across. Its height never jumps more than 0.5 mm at a
   seam.
7. **Resting on its side:** on a 5° slope with Friction 1, it rolls down. On
   flat ground it moves under 0.1 mm in 10 s and sleeps.
8. **Resting on a cap:** it does not creep, and sleeps.
9. **A coin:** a short cylinder spun on its rim settles onto a cap.
10. **Stacking:** logs stacked three high, cylinder on cylinder, hold for 10 s.
11. **Queries:**
    - `b3World_CastRay` hits a cylinder exactly;
    - a PlayerController stands and walks on a lying cylinder;
    - a CanCollide-off cylinder fires `Touched`;
    - a fast cylinder does not tunnel through a thin wall.
12. **Elliptical:** a Cylinder under a GameObject scaled (2, 1, 1) still makes
    layered hulls.
13. **Determinism:** the same rolling run twice gives identical poses.
14. **Speed:** 200 cylinders tumbling in a bin take no more physics time per
    step than the layered hulls do today (both measured in the same test).

### The car

15. **H1Z1 car:** driven straight for 120 units on cylinder wheels, it drifts
    under 0.5 units, with body roll under 0.05° RMS (spheres: 0.1 units and
    0.000°). The probe runs in the sandbox against a copy of the project.

### Build

16. **Patch:**
    - a fresh configure fetches Box3D, applies the patch, and builds;
    - re-configuring does not patch twice;
    - a deliberately broken patch fails configure with the patch named.
17. **Suite:** the full sandbox suite stays green.

## Risks

- **Switching features:** a cylinder tipping between resting on a cap and on
  its side swaps the cap polygon for the side line. The 10° threshold and the
  cap's fixed rim samples keep the deepest point steady across the switch;
  tests 8 and 9 watch for jitter there.
- **Deep overlap:** past the margin, contact is the stand-in hull's, faceted
  like today's polygon, until the bodies push apart. Resting and rolling
  contact never go that deep.
- **Pin bumps:** every Box3D update means reapplying the patch to about ten
  Box3D files. `cylinder.c` is ours and moves untouched. If Box3D gains its
  own cylinder upstream, we move to it and drop the patch.
