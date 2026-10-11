# Collision groups

Date: 2026-10-11. Status: approved design, not yet planned.

## Goal

Let scripts control which physics bodies interact, as Roblox's collision
groups do, with the Whitelist/Blacklist group types of the user's
CollisionGroupManager module built in. It must hold for welded assemblies,
PlayerControllers, held (bone) sides, CanCollide, and rays.

## Decisions

| Question | Decision |
| --- | --- |
| Where a body names its group | `CollisionGroup`, a saved string property on PhysicsBase (so PhysicsObject, PlayerController, Brush). Default `"Default"`. Any name may be set at any time. |
| Where groups live | `PhysicsService`, a script service. Groups are registered at runtime by scripts; nothing about them is saved. It starts each run with only `Default`, and Stop clears it. |
| A body whose group is not registered | Behaves as `Default` until a script registers that name; its filter then updates. |
| Group types | Built in. `Blacklist` (the default type): collides with every group except those listed. `Whitelist`: collides with no group except those listed. |
| How Box3D filters | Shape filter bits (64-bit category and mask), not the custom filter callback: no per-pair cost. |
| Group limit | 62. Bits 62 and 63 are reserved for queries. Roblox's limit is 32. |
| Rays | `RaycastParams.CollisionGroup` (default `"Default"`), as in Roblox. Studio's selection rays ignore groups. |
| Out of scope | A Studio collision groups editor, a Properties dropdown, RenameCollisionGroup, saving groups with the place, collision events. |

## 1. `CollisionGroup`

A saved `string` property on PhysicsBase, default `"Default"`, in the
Physics group of Properties beside Anchored and CanCollide. Writing it marks
the body's filter dirty (`kDirtyFilter`). No validation: an unregistered or
empty name behaves as `Default`. Names are case-sensitive.

## 2. Whether two groups collide

PhysicsService keeps, per registered group, its type, and a set of explicit
pair settings: `(a, b) -> bool`, stored once per unordered pair, where `a`
may equal `b`.

`collidable(a, b)`:

1. An explicit setting for `{a, b}` decides.
2. Else: true only when both groups are `Blacklist`.

So a new group never touches a `Whitelist` group unless the pair is set to
collide, and two `Blacklist` groups collide unless set not to. A group's
"list" (as `GetRegisteredCollisionGroups` reports it, and as the user's
module calls CollisionList) is derived: for a `Blacklist` group, the groups
it is explicitly set not to collide with; for a `Whitelist` group, the
groups it is explicitly set to collide with. Changing a group's type keeps
its explicit settings.

`Default` is always registered, as `Blacklist`, and cannot be unregistered;
its type may be changed.

## 3. `PhysicsService` API

Scripts get it with `game:GetService("PhysicsService")`. Not in the tree,
not saved. Every method runs on the simulation thread.

| Method | Behavior |
| --- | --- |
| `RegisterCollisionGroup(name: string, collisionType: string?)` | `collisionType` is `"Blacklist"` (default) or `"Whitelist"`; anything else raises. An already registered name: no change, not even to its type. More than `GetMaxCollisionGroups()` groups raises `"Too many collision groups (the most is 62)"`. An empty name raises. |
| `UnregisterCollisionGroup(name: string)` | Removes the group and every explicit setting naming it. Its bodies behave as `Default`. `Default` raises. An unknown name does nothing. |
| `SetCollisionGroupCollisionType(name: string, collisionType: string)` | Changes the type. Unknown name raises `Unknown collision group "<name>"; did you forget to call RegisterCollisionGroup?`. |
| `CollisionGroupSetCollidable(name1: string, name2: string, collidable: boolean)` | Records the explicit setting for the pair. Both must be registered, else the same unknown-group error. |
| `CollisionGroupSetCollidableBulk(name: string, names: {string}, collidable: boolean)` | `CollisionGroupSetCollidable` for each of `names`. |
| `CollisionGroupsAreCollidable(name1: string, name2: string): boolean` | `collidable` (section 2). An unregistered name counts as `Default`. |
| `IsCollisionGroupRegistered(name: string): boolean` | |
| `GetRegisteredCollisionGroups(): {{name: string, type: string, list: {string}}}` | In registration order, `Default` first. |
| `GetMaxCollisionGroups(): number` | 62. |

Each change bumps the service's revision.

## 4. Filter bits

- Each registered group has a bit index, 0 to 61, for as long as it is
  registered. `Default` is bit 0. An unregistered group's bit is reused.
- `kGroundBit` = bit 62, `kRayBit` = bit 63.
- A shape of a body in group G (G resolved to `Default` if unregistered):
  - `categoryBits` = bit(G).
  - CanCollide true: `maskBits` = the bits of every group G collides with,
    plus `kGroundBit` and `kRayBit`.
  - CanCollide false: `maskBits` = `kRayBit` alone. It touches nothing,
    whatever its group.
- Terrain chunk shapes are in `Default`.
- Queries (Box3D: a query sees a shape when the query's mask has the
  shape's category and the shape's mask has the query's category):
  - A ray with a collision group C: `categoryBits` = `kRayBit`, `maskBits` =
    the bits of every group C collides with.
  - Studio selection rays (no group): `categoryBits` = `kRayBit`,
    `maskBits` = all.
  - A PlayerController's ground probe: `categoryBits` = `kGroundBit`,
    `maskBits` = the bits of every group the controller's group collides
    with. It stands only on what it collides with; a CanCollide-false
    controller already finds no ground.

This replaces the ghost category added for CanCollide (`kSolidBits`,
`kGhostBits`, `kQueryBits` in PhysicsWorld.cpp); behavior with only
`Default` registered is unchanged.

## 5. Applying changes

- `PhysicsWorld::sync` compares PhysicsService's revision with the one it
  last applied. When it moved, every body's shapes and every terrain chunk
  get `b3Shape_SetFilter(shape, filter, true)`: contacts are made again, so
  a pair that stopped colliding lets go at once. These changes are rare,
  so one pass over every shape is fine.
- A body's own `CollisionGroup` or `CanCollide` write (`kDirtyFilter`)
  re-filters only its shapes.
- A shape made later takes the current filter (`shape_filter(object)`).
- Stop clears PhysicsService, which bumps its revision, so the stopped
  world goes back to everything in `Default`.

## 6. Assemblies

An assembly's Box3D body carries every member's shapes, and filters are
per shape: each member collides by its own `CollisionGroup`, as in Roblox,
where the property is per part. Shapes on one body never collide with each
other, so a group set not to collide with itself cannot pull a weld apart.
Anchored assemblies, held (bone) sides, and PlayerController-rooted
assemblies all make their member shapes through the same builders, so they
need nothing more. A PlayerController that is not its assembly's root does
not probe for ground (unchanged); the root controller's probe uses its own
group.

## 7. Rays

`RaycastParams.CollisionGroup`: string, default `"Default"`, read and
written from Lua; an unregistered name counts as `Default`. `Workspace:Raycast`
passes it to the physics world (`RayFilter::collision_group`, empty for
Studio's own rays, which see every group). A CanCollide-false body is still
hit when its group matches.

## Testing

Sandbox (`[physics][collision-group]` unless noted):

- `collidable`: default Blacklist pairs collide; a Whitelist group touches
  only what it is set to; an explicit setting beats the types both ways;
  changing a type keeps explicit settings; `GetRegisteredCollisionGroups`
  lists derived `list`s.
- API errors: unknown group, bad type, unregistering `Default`, the 63rd
  group, empty name.
- Two boxes in groups set not to collide pass through each other; set back
  to collide, they rest on each other.
- A body naming a group before it is registered behaves as `Default`; once
  registered as a Whitelist group, it falls through a Default floor.
- Changing `CollisionGroup` during play takes effect at the next step.
- A group set not to collide with itself: two welded members stay one
  body; two separate bodies in it pass through each other.
- A welded assembly with members in two groups: each member collides by
  its own group (one rests on a floor its partner passes through).
- A PlayerController stands only on what its group collides with.
- `RaycastParams.CollisionGroup` filters hits; Studio's raycast hits
  every group; a CanCollide-false body in a matching group is hit.
- Stop clears the groups: after Stop, every body behaves as `Default`.
- CanCollide's tests (BP2, BP2b, BP2c, P40 to P42) still pass.
- Properties: `Physics:CollisionGroup` on PhysicsObject, PlayerController,
  and Brush.
