# Skinned meshes: the skinning foundation

Sub-project 1 of 3. Imported rigged models keep their bones and skin weights,
every GameObject holds its own pose, and bones can be posed from Lua and the
Properties pane, drawn on the GPU without losing instanced batching.

## Goal and sub-projects

Skinned meshes should serve three uses equally: posing by hand in Studio,
playing imported animation clips, and driving bones from Lua. They share one
foundation, so the work is split:

1. **Skinning foundation** (this spec): import keeps bones, a per-GameObject
   pose, GPU skinning, the `Bone` instance, `GetBone`/`AddBone`/`GetBoneNames`.
2. **Studio pose mode**: the selected GameObject's joints are drawn and picked;
   a gizmo on a joint edits (or, through an explicit action, adds) its `Bone`;
   an "Expose all bones" action adds every one.
3. **Animation clips**: the importer extracts clips into an animation asset; an
   `Animator` plays, fades, weights, and loops them, sampling into the pose.
   Tracks match bones by name.

Later, only if wanted: state machines, masks and additive layers, IK,
retargeting, root motion. Also separate: a generic instance that attaches one
PVInstance to an `Attachment` (a sword to a `Bone`); this spec only guarantees
what it will read.

## Decisions already made

- The Prefab → Model(Mesh + Material) and GameObject → Prefab links do not
  change. The Mesh carries its bones; there is no Skeleton asset.
- The pose belongs to each GameObject, not the shared Prefab or Mesh.
- Bones are **not** instances by default. The pose is plain runtime data. A
  `Bone` instance exists only for a bone someone chose to pose, attach to, or
  script, and it is only ever made explicitly: `AddBone`, Insert Object, or a
  Studio action. Nothing creates one implicitly.
- `Bone` extends `Attachment`.
- `Bone.Offset` is **additive**: applied after whatever the animation produced,
  in the bone's local frame.
- Bones are direct children of their GameObject, matched by `Name`, never
  nested to mirror the skeleton. The hierarchy comes from the mesh.
- An attached item's grip offset lives on the item's own `Attachment`, so
  `Bone.Offset` means only "pose this bone".

## 1. Data and import

### AMESH

The format, version, and reader are unchanged; this spec fixes what the bone
fields mean, and `amesh.hpp`'s comment says so:

- `AEBone` `m`/`t` are the bone's **rest transform in mesh space** (not
  relative to its parent). `m` is a 3×3 that may carry scale as well as
  rotation (an FBX root's unit scale, for one); the runtime decomposes it.
- `parent` orders the hierarchy; every bone comes after its parent.
- `cull_radius` is the farthest any vertex it influences (weight > 0) lies from
  the bone's rest origin, in mesh units.
- Skins hold global bone indices (as now).

At load the runtime derives, per bone, the rest local transform
(`parentRest⁻¹ × rest`) as position + rotation + scale, and the inverse-bind
matrix (`rest⁻¹`). These are cached with the mesh in `MeshCache` and in the
datamodel's skeleton table (section 2).

### Importer (`src/ide/ModelImport.cpp`)

When the scene has any bones:

- **One skeleton per file.** Collect every node any mesh's `aiBone` names, add
  every ancestor node up to (not including) the lowest common ancestor's parent,
  and order parents before children. Names are the node names; a duplicate name
  is made unique with the existing `UniqueName`. A bone's rest transform is its
  **bind pose**, `meshNodeGlobal × aiBone::mOffsetMatrix⁻¹`, in the skeleton
  root's space; an ancestor-only node with no `aiBone` uses its node global
  transform. The bind pose is used rather than the scene's current node
  transforms, which some files leave mid-animation.
- **Every AMESH written from that file carries the identical bone table**, so
  bone index *i* is the same bone in every Model of the Prefab, across the
  existing per-material and per-size splits.
- **Skinned vertices stay in bind space**: a mesh with bones is not multiplied
  by its node transform into world space; its vertices are put in the skeleton
  root's space by `meshNodeGlobal` alone, which lines them up with the bind
  poses above, so every skinning matrix is identity at rest.
- **Weights**: the 4 largest per vertex, renormalized to sum to 1. A vertex
  with none is bound to the mesh's own node's bone (or the root) at weight 1.
- **Rigid children**: a mesh without bones whose node sits under a bone node
  (a helmet under `Head`) is bound to that bone at weight 1 per vertex, its
  vertices in bind space, so it follows the pose instead of freezing.
- Meshes not under any bone node are baked as today. A file with no bones
  imports exactly as today.
- The "Skinned meshes came in static" note is removed. The "Animations are not
  imported yet" note stays until sub-project 3.

Physics keeps decomposing in the bind pose, as the custom-shape decomposition
spec already says.

## 2. Pose at runtime (engine_core)

### Skeleton and Pose

- A **skinned GameObject** is one whose Prefab has at least one Model whose
  Mesh has bones. Its skeleton is that bone table; Models are assumed to share
  it. If two Models' tables differ, the first Model's table is the skeleton and
  the others draw unskinned, in bind pose, with one Output warning per Prefab.
- It gets a runtime `Pose` component (flecs) holding, per bone:
  local position, rotation (quaternion), scale; the model-space global matrix;
  and the skinning matrix (`global × inverseBind`). Plus a dirty flag.
- `Pose` is never saved or replicated. It is rebuilt from the rest pose.
- A Prefab change, or a Mesh that loads or reloads, rebuilds the `Pose` for the
  new skeleton.

### Each frame

A new step runs right after the `PreAnimation` phase (the "animation" slot):

1. For each skinned GameObject whose pose is dirty: reset locals to rest.
2. (Sub-project 3: each `Animator` samples its clips into the locals.)
3. For each `Bone` child whose `Name` matches a bone: `local = local × Offset`.
4. Compute globals parent-first, then skinning matrices.

A pose is dirty when its skeleton changed, an `Offset` or `Name` on one of its
`Bone` children changed, a `Bone` child was added or removed, or (later) an
`Animator` is playing. A pose that is not dirty is skipped, so a posed statue
costs nothing per frame.

### Bone : Attachment

| Property | Type | Saved | Meaning |
|---|---|---|---|
| `Name` | string | yes (as every instance) | which bone it handles |
| `Offset` | Matrix4 | yes (inherited) | additive local pose, in the bone's frame |
| `OffsetSpace` | Enum | inherited | ignored by a matched Bone (always the bone's frame); kept for the plain-Attachment fallback |
| `Transform` | Matrix4 | no | `GameObject.Transform × global[bone]`, global already including `Offset` |

- Allowed parents: GameObject only (`register_suited_parents`).
- **Reading `Transform`** computes it from the latest pose, cached for the
  frame. Because `global[bone]` is model space, a GameObject moved later in the
  frame (by physics, by a script) is reflected at the next read. No `Changed`
  fires per frame for an animated bone; `Changed` fires for `Offset` writes as
  for any Attachment.
- **Writing `Transform`** solves for the `Offset` that puts the bone there given
  the current animated pose, as Attachment does; it is undone as the `Offset`
  write it made. A GameObject whose Transform has no inverse refuses it.
- **A Name that matches no bone** (or a non-skinned parent) makes the Bone act
  as a plain Attachment: `Transform = parent frame × Offset`, honoring
  `OffsetSpace`. Output warns once per Bone, again only if it later breaks
  after matching.
- Two Bones with the same Name under one GameObject: the first by child order
  is used, the rest act as plain Attachments, with one warning.

### Frame order for the future attach instance

`scripts → animation step (fills Pose) → physics → attach/constraints (read
Bone.Transform) → render`. Since Bone.Transform is computed on read from a
model-space pose, any order after the animation step sees the current frame's
hand.

## 3. Rendering (runner)

- **Snapshot**: `SnapshotPump` copies the skinning matrices of skinned
  GameObjects into the frame snapshot as 3×4 float rows (48 bytes per bone)
  next to their `VisualInstance` (about 280 KB per frame for 100 × 60-bone
  characters).
- **Bone buffer**: each frame the renderer packs the visible skinned
  instances' matrices back to back into one `GL_TEXTURE_BUFFER` (RGBA32F, 3
  texels per bone), growing only when it must.
- **InstanceData** gains `int boneBase` at vertex slot 15 (116 bytes): the
  first texel of the instance's matrices, or -1 for unskinned.
- **Batching is unchanged**: the batch key stays `slot | lod | mirrored`, so 100
  instances of one Prefab are still one instanced draw per Model, each instance
  reading its own slice.
- **Shaders**: a `SKINNED` variant of `geometry.vert` and `shadow.vert` reads
  locations 5 and 6, blends the 4 matrices (`texelFetch`), and skins the normal
  and tangent with the same blended 3×3. A mesh with bones draws with the
  variant; a static mesh keeps today's shader with no added cost.
- **Culling**: a skinned instance's bounds are the union of its posed bone
  origins, each grown by its `cull_radius`, then put in world space. This
  replaces the padding the culling spec reserved for skinned meshes.
- Transparency and shadow passes use the same buffer and variant.

## 4. Lua and Studio

### Lua

- `GameObject:GetBoneNames() -> {string}`: the skeleton's bone names in bone
  order; empty when not skinned.
- `GameObject:GetBone(name) -> Bone?`: the existing `Bone` child for that name,
  or nil. Never creates one.
- `GameObject:AddBone(name) -> Bone`: makes a `Bone` child for that name and
  returns it. Errors when the skeleton has no such bone, or when a `Bone` for
  it already exists (use `GetBone`). Undoable as any instance creation.
- `Bone.Offset`, `Bone.Transform`, `Bone.OffsetSpace`: inherited.
- `Bone` is `Instance.new`-creatable and documented in `LuaApi.cpp`.

### Studio

- `Bone` is in Insert Object under a GameObject and in `ClassOrder`.
- In Properties, a Bone's `Name` stays a plain string field (a bone-name
  dropdown is sub-project 2's).
- The importer's result: a rigged FBX/glTF imports as today's Prefab, now
  skinned, posed at rest.

## 5. Testing

- **amesh**: rest-local and inverse-bind derivation round-trips for a 3-bone
  chain with rotations.
- **Importer**: a small rigged glTF, built inside the test, split across two
  materials, yields identical bone tables in both AMESH files, weights summing
  to 1, bind-space vertices, and a rigid child bound at weight 1.
- **Pose**: `Offset` is additive over rest; a child bone follows its parent;
  `Bone.Transform` is right under a rotated, scaled GameObject; writing
  `Transform` round-trips to `Offset`; a Name with no matching bone falls back
  to a plain Attachment; a pose with nothing dirty is not rebuilt.
- **Lua**: `GetBone` returns nil before `AddBone` and the Bone after;
  `AddBone` errors for an unknown bone and for a duplicate; undo removes it.
- **Batching**: 100 skinned instances of one Prefab make one run per Model,
  each with its own `boneBase`.
- **Live**: in a running Release Studio over MCP, a rigged model imports, and
  setting a bone's `Offset` visibly bends the mesh and its shadow.

## Out of scope

Pose mode and joint display (2); clips, the animation asset, and `Animator`
(3); the attach instance; IK; blend shapes / morph targets; dual-quaternion
skinning; more than 4 influences; bone LODs (`lod_parent` stays unused);
replicating poses over the network.
