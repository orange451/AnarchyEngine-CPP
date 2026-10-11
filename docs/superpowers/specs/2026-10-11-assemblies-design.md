# Assemblies, WeldConstraint, and RigidConstraint

2026-10-11 · builds on Box3D physics (`2026-09-30-box3d-physics-design.md`),
edit-mode physics (`2026-10-06-edit-mode-physics-raycast-design.md`), Custom
shape pieces (`2026-10-06-custom-shape-decomposition-design.md`), and
`Attachment`/`Bone`.

## Goal

A fast, easy, intuitive way to attach objects together.

- An **assembly** is a group of PVInstances that move as one rigid thing. It
  is internal: nobody inserts one; constraints and existing links make it.
- Two physics bodies in one assembly act as if welded. A PVInstance with no
  physics rides along with zero mass.
- If any member is anchored, the whole assembly is anchored.
- Every assembly has a **root**, chosen by fixed rules, so the same layout
  always picks the same root. The root may change at any time.
- `WeldConstraint` holds two PVInstances where they were placed in the editor.
- `RigidConstraint` snaps its sub's pivot onto its parent's pivot, live in the
  editor, and holds it there.

Engine distances are units.

## Decisions

| Question | Decision |
| --- | --- |
| How welded bodies simulate | One Box3D body per assembly, carrying every physics member's shapes at its offset (a runtime compound). Not weld joints: Box3D's welds flex in chains, and every joint costs solver time. |
| Where assemblies are computed | A new engine_core unit, `Assemblies`, owned by the DataModel. Physics, the editor, and Lua read the same result. |
| When assemblies exist | Always, in edit mode and in play. Only physics compounding is play-only; edit mode keeps one body per PhysicsBase, as now. |
| Pivot | A PVInstance's frame: position and rotation, scale removed. There is no separate Pivot property. A physics node's frame comes from its GameObject (§2). |
| Can an Attachment be the root | Never. An Attachment or Bone is not a member; it stands for the object it is attached to. |
| Root order | Anchored, then PlayerController, then has physics, then higher Mass, then the larger body, then lower GUID. |
| Unique id for the tie-break | The GUID (byte-wise compare): a physics node's PhysicsBase's, a plain node's own. Stable across save, load, Stop, undo, and cut. Script-made objects get random GUIDs, so two equal-mass script-made parts may pick different roots on different runs. |
| Two anchored members | One assembly with one anchored root. Roblox splits them; we do not. |
| RigidConstraint direction | Set by the root, not by which end is PVInstance1: the root never moves. The end on the root's side is the parent, the other the sub, and the sub moves. (Changed 2026-10-11 at the user's request; it was PVInstance1 the parent.) |
| RigidConstraint in the editor | Live: the sub always sits on the parent's pivot. It snaps in the same call as the edit that moved it, so one undo reverts both. |
| WeldConstraint in the editor | Does nothing. Objects move freely; the offset is captured when the weld becomes active in play. |
| A member's Transform written during play | The whole assembly moves at once, keeping every offset. |
| Velocity of a member | The assembly's center-of-mass velocity, read and written through any physics member, as Roblox's `AssemblyLinearVelocity`. |
| Terrain | Not a member in v1. A constraint with a Terrain end is inactive. |
| Duplicate / Copy / Paste / instance files | References that point inside the copied set are remapped to the copies, for one whole user action. Instance files carry GUIDs so inserting one remaps too. |
| A RigidConstraint to a Bone (a hand) | The Bone's side is always the parent, and the two sides stay apart, each its own assembly: the hand moves under its GameObject as the pose changes. In play what it holds is a kinematic body moved to the hand each step (§5, Held by a hand). Added 2026-10-11. |
| Out of scope | Terrain ends; breakable welds; hinges, ropes, and other non-rigid constraints; a root-priority override; a weld-selection command; `AssemblyMass` and other read-only assembly queries beyond `AssemblyRoot`. |

## 1. Classes

### `Constraint` (abstract)

`engine_instances/Constraint.{hpp,cpp}`. A `DataModel` subclass, not a
PVInstance (it has no Transform), registered as Lua class `Constraint`, base
`Instance`, with no factory. Its entity carries a new `ecs::Constraint` tag,
with a cached query for constraints in Workspace, as `physics_bodies()` has.

| Property | Type | Saved | Default | Meaning |
| --- | --- | --- | --- | --- |
| `PVInstance1` | `PVInstance?` | yes, by GUID | nil | One end. Order does not matter (§3 direction rule). |
| `PVInstance2` | `PVInstance?` | yes, by GUID | nil | The other end. |
| `Enabled` | boolean | yes | true | Off disconnects the constraint without deleting it. |
| `Active` | boolean | no, read-only | | Whether it is holding anything now (§2). Fires no Changed. |

`PVInstance1` and `PVInstance2` are written through `set_instance_reference(...,
"PVInstance", ...)`, as `WireframeAdornment.Adornee` is. Both ends may name the
same instance; the constraint is then inactive.

A constraint may live anywhere. `register_suited_parents` ranks it under
`Workspace` and any PVInstance in the Insert list, as Attachment's are.

### `WeldConstraint` and `RigidConstraint`

Both derive from `Constraint`, add no properties, and are creatable
(`register_lua_creatable`, so `Instance.new`, paste, the Insert list, and MCP
`create_instance` make them). Both are added to `Project.cpp`'s
`class_registry`, or saved places would not load.

## 2. The assembly model

### Nodes

An assembly is a connected group of **nodes**. A node is one of:

- **A physics node**: a `PhysicsBase` (PhysicsObject, Brush, PlayerController)
  in Workspace that wins its GameObject claim, together with the GameObject it
  drives (`driven_game_object()`), and with any PhysicsBase that lost the claim
  to that GameObject (it has no shapes and is written like the GameObject). A
  weld to any of them means the same thing. A PhysicsBase that drives no
  GameObject is a physics node by itself.
- **A plain node**: any other member-eligible PVInstance in Workspace: a
  GameObject no PhysicsBase drives, a Camera, a PointLight, or a SpotLight.

Never members: Attachments and Bones (they resolve to their holder, below),
Terrain, a Dragger, and anything outside Workspace.

The GameObject claim rule (one PhysicsBase per GameObject, the first in
depth-first Workspace order wins, the rest warn once) becomes one function that
`Assemblies` and `PhysicsWorld` both call. Its behavior does not change. It
writes a reserved per-slot array instead of today's per-sync map.

### A node's frame

Every place below that needs a node's pose uses its **frame**:

- A physics node with a driven GameObject: the GameObject's position and
  rotation (upright, turned only about Y, for a PlayerController), never the
  PhysicsBase's own Transform. While stopped, the PhysicsBase's Transform lags
  its GameObject until the next physics sync, and a write to it is thrown away
  there (edit-mode tests E11 and E12), so it is not the truth.
- A PhysicsBase with no driven GameObject: its own Transform.
- A plain node: its own Transform.

Each is taken without scale.

The **mover** of a node is the instance whose Transform the editor writes to
place it: a physics node's driven GameObject, else its PhysicsBase, else the
plain node itself.

### Ends

An end of a constraint resolves to a node and a **pivot** (a frame):

1. **An `Attachment` or `Bone`.** Its holder is the first ancestor that is not an
   Attachment. The holder must be a PVInstance that resolves to a node (rules 2
   and 3), or the end is unresolved. A Folder, Workspace, or Terrain holder
   leaves the end unresolved. The pivot is the Attachment's frame, computed as
   Attachment does, from the holder node's frame (not a lagging PhysicsBase
   Transform): `frame × Offset` in Local, and `translation(frame) × Offset` in
   World. Attachments between it and the holder chain the same way.
2. **A PhysicsBase, or a GameObject a physics node drives.** It resolves to that
   physics node, and the pivot is the node's frame.
3. **Any other member-eligible PVInstance.** It resolves to its own node, and
   the pivot is the node's frame.
4. **Anything else.** The end is unresolved.

Ends are resolved with `InstanceRef::resolve` plus an alive and class check,
never through a `LuaSlot`, so resolving allocates nothing.

### When a constraint is active

A constraint is **Active** when all hold:

- `Enabled` is true, and the constraint is in Workspace.
- Both ends resolve, and to two different nodes.
- For a RigidConstraint, it survives the rigid rules below.

Rigid rules, applied in order:

1. **Forest.** Rigid constraints may not form a loop. RigidConstraints are
   taken in GUID order; one whose two ends other rigid constraints already
   join is inactive. The surviving rigid edges form a forest of trees.
2. **Play only: already joined.** All active WeldConstraints are joined first.
   Then each rigid edge, in GUID order, is inactive if its two nodes are
   already in one assembly by then. Otherwise it joins them.
3. **Direction: the root never moves.** Once assemblies and roots are known,
   every node gets its hop count from its root over all active edges. In each
   rigid tree, the node with the fewest hops (the root order breaks a tie) is
   its anchor and stays; every edge of the tree points away from it. The end
   nearer the anchor is the edge's parent, the other its sub. **Follow order**
   lists each parent's edges before its subs' edges, ties in GUID order.

An assembly is a connected group of nodes under active constraints. A node no
active constraint touches is an assembly of one, and behaves exactly as today.

### Root

Members are compared in this order; the first difference decides:

1. **Anchored** wins: a physics node whose `Anchored` is true.
2. **A PlayerController** wins. The controller code assumes its body's origin
   is its feet, so a heavier crate must never become the root of a player
   carrying it.
3. **A physics node** wins over a plain node.
4. **Higher Mass** wins (`PhysicsBase::mass()`). A plain node has no Mass.
5. **The larger body** wins: the volume of the box around its solid (a Brush's
   faces, a PhysicsObject's Size, or a Hull's or a Custom's Mesh bounds), times
   its GameObject's Scale cubed. Mass is a total that defaults to the same on
   every body, so this is what usually decides between two bodies. A change to
   Size, Shape, Mesh, a Brush's faces, or a linked GameObject's Scale moves the
   assembly revision.
6. **Lower GUID** wins, as a byte-wise string compare (GUIDs are not all hex):
   the PhysicsBase's for a physics node, the instance's own for a plain node.

The root depends only on members' properties and GUIDs, never on the order
things were made, loaded, or linked.

### `AssemblyRoot`

A read-only, unsaved property on every PVInstance (`PVInstance?`), registered
with PVInstance's Lua class in `GameObject.cpp` through `lua_property(...,
writable = false, ...)`.

- It reads the root's PhysicsBase for an assembly with physics, or the root
  PVInstance for an assembly without.
- A lone physics node reads its PhysicsBase; its driven GameObject reads the
  same.
- An Attachment or Bone reads its holder's root, or nil when it has no holder.
- Terrain, a Dragger, and anything outside Workspace read nil.
- It reads the same in the editor and in play: in the editor it shows what the
  root will be.

## 3. `Assemblies` (engine_core)

`engine_core/Assemblies.{hpp,cpp}`, owned by DataModel state. It is the only
code that groups nodes. It is a cache keyed by its own **assembly revision**,
an atomic counter any thread may read (as `tree_revision()` is). It rebuilds
lazily when a reader finds the revision has moved.

### What moves the revision

Only events that can change an assembly:

- **Constraints:** a write to `PVInstance1`, `PVInstance2`, or `Enabled`. A
  Constraint created, destroyed, or entering or leaving Workspace.
- **Linked slots.** A slot is linked when it is a constraint end, an Attachment
  on an end's walk to its holder, a holder, a member of an assembly with more
  than one node, or a GameObject more than one PhysicsBase claims. A linked
  slot linked, unlinked, reparented, or destroyed moves the revision.
- **PhysicsBases:** one entering or leaving Workspace, or a `GameObject` write,
  when the GameObject it drives (before or after) is linked. `Anchored` and
  `Mass` writes on a linked PhysicsBase.
- **Stop** (`world_generation()`).

Renames never move it, and nothing outside Workspace does. A script that
spawns uncontested bodies every frame never moves it.

### What a rebuild records

A rebuild walks the Workspace constraints query and the claimed PhysicsBases,
runs a union-find over slots, and records:

- per assembly: its members, its root, whether it is anchored, and whether it
  has physics;
- per slot: its assembly, and the flags the hooks read in O(1):
  - `linked` (above);
  - `member` (in an assembly with more than one node);
  - `rigid` (a node or Attachment on either side of an active RigidConstraint);
- per constraint: whether it is Active;
- the rigid edges in follow order, with each node's child edges in a compact
  per-slot list, so a downstream walk needs no search.

Arrays are indexed by slot and reserved for `kMaxInstances` at construction.
A rebuild allocates nothing. An end whose GUID resolves to nothing is kept in
an unresolved list. It is resolved again only after something that can bring a
GUID back: load, undo or redo, Stop's restore, paste, or `set_guid`. That is
one pass over the slots for all of them, not a linear `find_guid` per end per
rebuild.

### Suspending

A **suspend scope** (an RAII counter on DataModel state) makes every assembly
hook return at once. These hold it:

- **Project build and Rebuild, and Changes from Disk.** Nothing follows when
  it closes. The stopped-tick pass covers it.
- **Undo, redo, and cancel applying an edit waypoint.** The place they write
  back is already consistent. During play, undo and redo of play waypoints do
  not suspend the play hooks.
- **Stop's restore** (`restore_place_unlocked`), which runs the checked setters
  while `simulation_running` is still true. Nothing follows; the revision moves
  through `world_generation`.
- **Paste, Duplicate, and instance-file inserts, including the remap (§6).**
  When the outermost one closes: one rebuild and one follow pass, inside the
  action's recording.

A hook also returns at once when the written slot is not `linked` (its flags
are current or the revision would have moved), when it is outside Workspace,
or when Workspace holds no Constraint. A follow pass or play move never starts
another from its own writes.

The Properties panel re-reads its rows when the assembly revision moves, so
`Active` and `AssemblyRoot` stay current though they fire no Changed.

## 4. In the editor

Edit mode keeps one Box3D body per PhysicsBase; raycasts, selection, and
outlines are unchanged. Assemblies still exist (for `Active`, `AssemblyRoot`,
and the visualization). WeldConstraints move nothing.

### RigidConstraint follow

For an active RigidConstraint:

- `F_parent` is the parent end's pivot and `F_sub` the sub end's (§2), the
  ends as the direction rule sets them.
- `L = frame(sub node)⁻¹ × F_sub` is the sub's pivot in its node's frame.
- **Target.** The sub node's frame must become `F_parent × L⁻¹`. Its mover is
  written to that frame, keeping the mover's own scale.
  - If the sub end is a World-space Attachment, the target is matched by
    translation alone, and the mover keeps its rotation.
  - A PlayerController's target is made upright first.
- **Converges in one write.** The follow compares the mover's frame against
  that target (after those adjustments), not `F_sub` against `F_parent`. It
  writes only when they differ by more than 1e-5 units, or the matching
  rotation. A target the mover cannot reach exactly is still a fixed point, so
  a consistent place writes nothing.
- **What the write does.** The mover is written through its checked setter
  (`set_pv_transform`). That records history when a recording is open, fires
  Changed, and marks the place dirty, like any edit.
- **Carried along.** The mover's descendants that keep their own world
  Transform (GameObjects, Cameras, Lights; not Attachments, Bones, or driven
  PhysicsBases) move by the same rigid delta, as MoveTool carries them. A
  descendant that is itself a sub then follows its own edge.

### When the follow runs

A **pass** starts at a node N, synchronously, at the end of:

- **A checked Transform write to a `rigid` slot.** That is a SpatialObject
  through `DataModel::apply_transform`, or a PhysicsBase with no driven
  GameObject through `set_transform`. While stopped, a write to a driven
  PhysicsBase moves nothing (E12) and starts no pass.
- **A write to a `rigid` Attachment's `Offset`, `OffsetSpace`, or `Transform`.**
  N is its holder's node.
- **The end of a suspend scope, or a rebuild found by a hook.** The pass then
  covers every edge.

A pass started at N handles:

1. N's own incoming rigid edge, if N is a sub. A sub moved on its own snaps
   straight back.
2. Every edge downstream of N, in follow order. Moving the car moves the wheel,
   which moves the hubcap.

Writes made by a pass start no pass. Since it all happens in the call that made
the edit, it lands in that edit's recording. Drag the car and the wheel follows,
and one undo reverts both. Setting an end in Properties snaps the sub in
the same undo step.

Each stopped tick also runs one pass over every edge, after `physics_.sync`.
It is a safety net for a write that bypassed the hooks, and writes nothing in a
consistent place.

### Exceptions

- **The view camera.** While stopped, the follow neither moves nor follows the
  camera the Scene View looks through (`workspace.CurrentCamera`). SceneCamera
  flies it every frame. Its constraints still count for `Active` and
  `AssemblyRoot`, and still act in play.
- **A Brush face edit.** `Brush::apply` recentres the solid: it moves the
  Transform one way and the faces the other, so nothing moves in the world.
  That is a pivot change, not a move. In the editor, a Brush that is a rigid
  sub is snapped after it like any sub, so its geometry shifts by the
  recentre. In play, see §5.

### MoveTool

`resources/plugins/MoveTool.luau` changes in two ways:

- **Dragging.** It does not write an Attachment or Bone whose parent is in the
  moved list. Its pose comes from that parent. Writing it would solve a new
  `Offset` when the follow snaps the parent back.
- **DragEnded.** It calls `place()`, so the handles return to the selection
  after a sub snaps back.

## 5. In play

### When play starts

`Assemblies` captures every assembly's body frame and member offsets in
`start_simulation`, before any script runs:

- The body frame is the root node's frame.
- A member's offset is `bodyFrame⁻¹ × frame(member)`.

`PhysicsWorld` builds compound bodies from these at its first sync with
`simulation_running`, of any kind (a Raycast's included).

From Play on, a write to a member of an assembly with more than one node
follows this section, including before the first step. That supersedes E13
for such members; a single-node assembly behaves exactly as today.

### Bodies

Each assembly with physics is one Box3D body:

- **Type.** Static if any member is anchored, else dynamic.
- **Shapes.** Each physics node's shapes are built as today, then placed at its
  offset:
  - hulls and pieces through `b3CreateTransformedHullShape`;
  - boxes through `b3MakeTransformedBoxHull`;
  - sphere and capsule centers transformed;
  - an anchored Custom's mesh from transformed points.

  Each member's hull is cached in member space after its first build, so
  placing it again needs no quickhull. Whether a Custom uses its mesh or its
  pieces follows the **assembly's** anchoring, in make, in keep's
  made-without-pieces check, and for outlines.
- **Mass.** Each node keeps its own density (`Mass / volume`).
  `b3Body_ApplyMassFromShapes` runs once per change, so the body's mass is the
  sum of its members' Mass and its center of mass is weighted.
- **Damping.** The body uses the root's `LinearDamping` and `AngularDamping`. A
  damping write to a non-root member does not change it.
- **Identity.**
  - Body userData is the root's id.
  - Each shape's userData is its own PhysicsBase's id, as now.
  - Each physics member's slot records which body it is on.
- **PlayerController root.** It keeps its controller setup: upright motion
  locks, no sleep, and the hover probe. A root change to or from a
  PlayerController rebuilds the body, keeping its velocity, so the body's
  origin is always the controller's feet.

An assembly with no physics has no body. Its members still move together when
one is moved.

### Write-back

After each step, for every moved body, every member is written from the body's
pose times its offset. Nothing fires Changed or records history, as physics
writes now:

- **A PhysicsBase:** `store_simulated`, keeping its own scale, with the body's
  center-of-mass velocity. A PhysicsObject also gets the angular velocity, and
  so now does a Brush.
- **Its driven GameObject, and every plain SpatialObject member** (GameObject,
  Camera, PointLight, SpotLight): `write_simulated_transform`, keeping its own
  scale.
- **A PhysicsBase that lost its claim:** `store_simulated`.

Attachments, Bones, SoundEmitters, BillboardGuis, and WireframeAdornments
already follow their parents or adornees. A static body emits no move events,
so its members are written when it is built and when it is teleported.

`Engine::step_physics` brings `Assemblies` up to date before
`integrate_simulated`. `integrate_simulated` then skips every `member` slot by
its flag, so its legacy velocity integration never fights the weld.

`note()` keeps one pending Transform invalidation per slot (a per-slot queued
bit cleared by the drain). Riders times substeps cannot overflow the queue,
and `apply_live` reads the live Transform anyway.

### A member moved during play

Any checked Transform write to a `member` slot, through any path, moves the
whole assembly in the same call: a script, Properties, MCP, a worker command,
or a render-window script.

- The new body frame is `frame(written) × offset(member)⁻¹`, made upright when
  the root is a PlayerController.
- Every other member is written from it at once, silently, through the
  write-back stores. Only the written member fires Changed. With a
  PlayerController root, the written member is also re-placed from the
  upright frame, so it may not land exactly where written.
- The body is marked to teleport there at the next sync. Raycasts sync first,
  so they see it.

This replaces `follow_driven`'s per-sync compare for driven GameObjects that
are members. A Brush recentre (§4) is not a move: it adjusts that member's
offset by the recentre and re-places only its shapes.

### Velocity

All assembly velocities are center-of-mass velocities.

- **Reading.** `Velocity` on any physics member gives the body's linear
  velocity. `AngularVelocity` gives its angular velocity.
- **Writing.**
  - A write through any member of an assembly with more than one node is
    stored silently on every physics member.
  - It also records one pending body velocity for the assembly, applied at the
    next sync, so the last write wins.
  - A PlayerController root's jump detection reads that pending value,
    whichever member was written.
- **Applying.** Velocities are always applied with
  `b3Body_SetLinearVelocity` and `b3Body_SetAngularVelocity`, after
  `b3Body_ApplyMassFromShapes`. Never through `b3BodyDef.linearVelocity`,
  which is the origin's velocity.
- **At Play start.** A compound body takes its root's authored `Velocity` and
  `AngularVelocity` as its center-of-mass velocities.

### Membership changes during play

A constraint that is enabled, disabled, destroyed, retargeted, or moved in or
out of Workspace changes membership. So does a member leaving Workspace.
Bodies are updated incrementally:

- **Split.** The piece holding the old root keeps the body, its frame, and its
  members' shapes. Box3D keeps its velocity consistent as its center of mass
  moves. Each departing piece gets a new body, with velocity
  `v + ω × (c_new − c_old)` and the same angular velocity. Only the departing
  members' shapes are destroyed.
- **Merge.** The body of the side whose root wins takes the other side's
  shapes at their new offsets. Linear momentum is kept (`Σ mᵢvᵢ / Σ mᵢ`), and
  the angular velocity is the mass-weighted mean.
- **Anchoring change.** When the assembly's anchoring changes, every Custom
  member's shapes are remade (mesh or pieces) before `b3Body_SetType`. A result
  that becomes anchored has zero velocity. One that becomes unanchored starts
  at rest.
- **WeldConstraint.** It captures its offset from where things are when it
  becomes active.
- **RigidConstraint.** When it becomes active during play, the sub's side (the
  assembly the sub was in) moves rigidly so its pivot lands on the parent's,
  then the two join.
- **Attachment Offset.** A write to a rigid end's `Offset` during play
  re-snaps that edge. Only the sub side's members get new offsets, and only
  their shapes are re-placed, with mass recomputed once. The parent side's
  shapes and contacts are untouched; the moved shapes lose theirs for a step.

Members whose assembly did not change keep their body, offsets, and contacts.

### PlayerController in an assembly

- **Control.** It runs only for an unanchored assembly whose root is a
  PlayerController.
- **Contacts.** The controller's contact filtering (walls, unclimb) reads only
  contacts on its own cylinder shape, so a carried crate's contacts do not
  stop the player.
- **Ground load.** The weight pushed into dynamic ground is the body's mass
  (`b3Body_GetMass`).
- **A non-root PlayerController** runs no control and stores `OnGround` and
  `IsSliding` false.

### Raycasts and terrain

- `ray_hit` maps a hit through the **shape's** userData. `Raycast` and
  `mouse.Target` report the member that was hit, not the root. A Brush face
  hit uses that member's Transform.
- A PlayerController's ground probe already skips its own body, so it skips
  its whole assembly.
- Terrain collider interest, for each **dynamic** assembly, is asked around
  every physics member's origin (body pose × offset), not just the body's.
  Static assemblies ask for none, as today.

### Test accessors

`PhysicsWorld`'s test accessors take any member's PhysicsBase id:

- `has_body`, `body_mass`, `body_key`, and `body_position` report the member's
  compound body;
- `shape_count`, `shape_frictions`, `shape_key`, and `shapes_made` report only
  that member's own shapes;
- `body_count` counts Box3D bodies.

### Stop

Stop restores the place from its snapshot under a suspend scope, as §3 says.
Members return to their authored poses. `world_generation` moves, and
`Assemblies` and `PhysicsWorld` drop their state.

### Held by a hand

A RigidConstraint with an end that is a `Bone` posing a bone of its
GameObject is a **bone edge**: a hand holding something. Added 2026-10-11.

- **Pivot.** The Bone's pivot is the bone where it is posed: its GameObject's
  frame times the bone's posed global, with the Bone's Offset in it, scale
  removed. An Attachment under the Bone measures from that.
- **Direction and joining.** The Bone's end is always the parent, whatever
  the root rule says, and the edge joins nothing: the held thing (with
  whatever is welded to it) is its own assembly, never in the GameObject's,
  and in no rigid tree. Its AssemblyRoot is its own.
- **Editor.** The held thing follows as any rigid sub does (§4), and also
  when the pose changes: every Bone under the GameObject is linked, so an
  Offset written to one higher up the arm moves it, and so does an
  Animator's `StepAnimations` while stopped.
- **Play.** What a hand holds is flagged held. Its body (its own, or its
  assembly's one body) is kinematic while held: gravity and contacts do not
  move it. Before each Box3D step its velocity is set to reach the hand over
  that step (`b3Body_SetTargetTransform`), so it pushes what it swings into.
  After each step, and after the frame's scripts, it is written where the
  hand is now, as a body write-back (no Changed, no history), so what is drawn
  is on the hand; its body follows a step behind. Its GameObject is not
  jumped to by its body while held. Let go (the constraint disabled or gone),
  it is anchored or dynamic again, at rest.

## 6. Copy, Paste, Duplicate, and instance files keep their links

- **GUIDs carried.** `CopiedNode` gains its source's GUID. Instance files write
  it as an optional per-node `"guid"` key. The format stays version 1, and old
  files without it still load.
- **Scope of the remap.** It covers one **user action**:
  - every group of one Duplicate together;
  - each target of a Paste on its own, including held-cut extra targets;
  - one instance-file insert.

  The `paste_copies` calls of that action share one source-GUID-to-copy map,
  covering every node of every subtree, not just the roots.
- **The remap.** After all groups are built, every saved reference property
  (any `X?` field) on a built instance whose GUID names a source in the map is
  rewritten to that source's copy. It is written through the property's own
  write, so the action's recording keeps it, and Undo then Redo brings back
  copies that still point at each other. References to anything outside the
  set stay as they were. This covers `PVInstance1`, `PVInstance2`,
  `PhysicsBase.GameObject`, and `WireframeAdornment.Adornee`.
- **A RigidConstraint copied with one end only.** If one end was remapped and
  the other, named, stays outside the set, that outside end is cleared.
  Otherwise the copy would join the original, and the root rules would snap
  one of the two onto the other.
- **Bulk.** The whole action runs under one suspend scope. There is one rebuild
  and one follow pass at its end, inside its recording.

## 7. Studio

- **Explorer and Insert order.** Both classes rank in the Attachment cluster
  of `ClassOrder.hpp` (currently 10).
- **Icons.** New 32px icons, `WeldConstraint.png` and `RigidConstraint.png`, in
  the recent style, with entries in `resources/icons/ATTRIBUTION.md`.
- **Insert descriptions** (`InsertPopup.cpp`):
  - WeldConstraint: "Holds two objects together where they are."
  - RigidConstraint: "Snaps one object's pivot onto another's and holds it there."
- **Properties.**
  - `PVInstance1` and `PVInstance2` are reference rows, picked as any reference
    is, in a `Constraint` group, with `Enabled` and a read-only `Active`.
  - `AssemblyRoot` shows read-only on PVInstances.
  - The panel re-reads when the assembly revision moves.
- **Visualization.** While a constraint is selected, the Scene View:
  - draws a line between its two pivots, with a small marker at each end;
  - outlines both ends' collision shapes, where they have them;
  - draws an inactive constraint gray.

  It is drawn in GL (`GameView` tool lines and outlines), so MCP screenshots
  show it.
- **Docs.**
  - `LuaApi.cpp` symbol docs for the new members.
  - The `engine_instances` README, fixing its out-of-date PVInstance list.
- **Tests.** `tests/LuauCompleteTest.cpp`'s hard-coded Insert list is already
  out of date at HEAD. It is rewritten to the current ClassOrder, with
  RigidConstraint and WeldConstraint in the Attachment cluster.
- **Concurrent edits.** The main checkout has uncommitted MoveTool edits from
  other work. This branch's MoveTool change is kept small so the two merge
  cleanly.

## Testing

Written first, layer by layer. New sandbox IDs continue from the current last
ones and carry an `[assembly]` tag.

**Assemblies (no physics stepping):**

- **Root rules.** Each is checked with both creation orders and both
  constraint directions:
  - anchored beats mass;
  - a PlayerController beats a heavier body;
  - physics beats plain;
  - higher Mass wins;
  - lower GUID breaks a tie.
- **Attachments.** An Attachment is never the root, and a weld to an
  Attachment joins its holder. An Attachment under a Folder or directly in
  Workspace is unresolved: Active is false and nothing moves.
- **Physics nodes.** A GameObject and its PhysicsObject are one node, and a
  losing PhysicsBase belongs to its GameObject's node.
- **Active is false** for:
  - an end missing;
  - Enabled off;
  - an end outside Workspace;
  - a Terrain end;
  - the same node on both ends;
  - a second rigid parent;
  - a rigid loop;
  - in play, a rigid edge already joined by welds.
- **AssemblyRoot** reads the same in the editor and in play, and nil outside
  Workspace.
- **Revision.** A rename and an uncontested spawned body do not move the
  assembly revision.

**Physics (play):**

- Two welded bodies fall as one and keep the offset they were placed at. The
  body's mass is the sum.
- A plain GameObject, a Camera, and a Light welded to a body ride along.
- Welding to an anchored body anchors the whole assembly.
- Writing a non-root member's Transform moves the whole assembly at once,
  including before the first step. Other members fire no Changed.
- Writing Velocity on any member sets the assembly's, and reading it through
  another member returns it.
- Disabling a weld mid-flight splits it; each piece keeps its velocity at its
  center of mass, checked on a spinning assembly.
- Un-anchoring the anchored member of an assembly with a Custom makes the
  Custom collide as pieces.
- A RigidConstraint connected by script snaps the sub's side, then holds it.
- Sliding an end Attachment's Offset re-places only the sub side.
- Clipping a welded Brush does not move the other members.
- A raycast reports the member it hit.
- A PlayerController carrying a heavier welded crate stays root, still hovers,
  and stays upright when the crate's Transform is written tilted.
- Stop restores every member, also after a script enabled a RigidConstraint or
  slid an end's Offset.
- With assemblies warm, the step does not allocate. A counting `operator new`
  is added to the sandbox for this, run in a place without Terrain.

**Editor:**

- A RigidConstraint sub snaps when connected, and undo puts it back.
- Moving the parent moves the sub, and one undo reverts both. This holds with
  ends picked as GameObjects and as PhysicsObjects.
- Dragging the sub alone snaps it back. When its end is its own Attachment, the
  part's Transform and the Attachment's Offset are both unchanged after the
  drag.
- A Scene View drag of a selected driven PhysicsObject moves no sub (E12).
- A chain follows in order. A sub's child GameObject comes along.
- A World-space Attachment sub is matched by position only.
- A PlayerController sub under a tilted parent pivot settles in one write.
- Flying a CurrentCamera that is a sub moves the camera and nothing else.
- A WeldConstraint moves nothing while editing.

**Copy, Paste, and Duplicate:**

- Duplicating two welded parts and their WeldConstraint, selected as siblings,
  welds the copies to each other. After Undo and Redo they are still welded.
- Duplicating a constraint alone still points at the originals.
- Duplicating a part with a RigidConstraint child whose sub is outside clears
  the copy's sub end.
- `PhysicsObject.GameObject` remaps the same way.
- An instance file saved with a welded pair and inserted welds the inserted
  copies.

**Studio:**

- Insert list order.
- Save and load round-trip both classes.
- Properties picks `PVInstance1`, and refreshes `Active` when another
  constraint makes this one redundant.

**Live check.** One MCP run of a welded object falling, with a screenshot of
a selected constraint's visualization.
