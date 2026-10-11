# Animation clips: importing and playing them

Sub-project 3 of the skinned-mesh work (see
`2026-10-10-skinned-meshes-design.md`), done before sub-project 2 (PoseTool):
imported clips playing on characters is the MVP. The end goal is an
in-engine animation editor, so the clip format is one an editor can write.

## Goal

Import a rigged FBX or glTF (a Mixamo idle and walk, say), get its clips as
assets, and play them on a skinned GameObject from Lua: crossfaded, weighted,
masked per bone, layered under the Bone Offsets that already exist.

## Decisions already made

- **Animation is an asset type**, as Mesh, Texture, and Sound are: a
  `FileAsset` whose Path names a clip file. Others reference it by
  `InstanceRef`, as a Model references its Mesh.
- **The API is Roblox-shaped**, after the user's GrooveAnimator: an `Animator`
  instance under the GameObject, `Animator:LoadAnimation(animation)` returning
  a runtime `AnimationTrack` (not an instance, never saved). No AutoPlay.
- **Clips hold keyframes, as GrooveAnimator's sequences do:** named keyframes,
  each a set of poses per bone, each pose a change from the bone's rest pose
  with its own easing and weight. Pose weight is the blend mask.
- **Stepping:** Play mode steps an Animator once a frame while its saved
  `AutoStep` is true (the default). `Animator:StepAnimations(dt)` steps it in
  edit mode and in Play, whatever AutoStep says. Edit mode never steps by
  itself, so a stray `Play()` never animates a saved place, and the future
  editor owns preview time.
- **Out of the MVP:** priorities, root motion, markers beyond keyframe names,
  IK, state machines, PoseTool, and the editor.

## 1. The Animation asset and its file

### Animation

`Animation : FileAsset`, in a new Assets category, `Assets.Animations`
(`Containment` gets `{"Animations", "Animation", "Animations"}`, and
`GameService` makes the folder with the others).

| Property / method | Type | Meaning |
|---|---|---|
| `Path` | string | the `.aanim` file under the resources folder (FileAsset's) |
| `Length` | number, read-only | seconds: the last keyframe's time; 0 with no file |
| `Looped` | boolean, read-only | the clip's own loop flag, which a track starts from |
| `Loaded` | boolean, read-only | the file reads (as Mesh's) |
| `GetKeyframeNames()` | {string} | the keyframes' names, in time order |

The clip is read and cached as `Mesh::skeleton` does: by Path and root, the
file's time on disk looked at no more than once a second, shared immutable
data (`std::shared_ptr<const Clip>`), so a re-imported clip shows while the
place is open.

### The .aanim file

Custom binary, little-endian, versioned, CRC32 last, read and written by a
new `src/aanim/` library beside `src/amesh/` (no GL, no DataModel):

```
header      magic "AANM", version 1.0, flags, keyframe_count, pose_count,
            name_blob_size
names       UTF-8 blob: clip name, keyframe names, bone names
keyframes   keyframe_count x { time f32, name ref, first_pose u32, pose_count u32 }
poses       pose_count x { bone name ref, position f32x3, rotation f32x4 (unit
            quaternion x,y,z,w), scale f32x3, easing_style u8,
            easing_direction u8, weight f32 }
crc32
```

- Keyframes are sorted by time; times are finite and not negative.
- A pose is a change from its bone's rest local transform:
  `animated_local = rest_local × T(position) × R(rotation) × S(scale)`.
- The clip's loop flag is a header flag bit.
- Easing styles, by value: Linear 0, Constant 1, Sine 2, Quad 3, Cubic 4,
  Quart 5, Quint 6, Exponential 7, Circular 8, Back 9, Elastic 10, Bounce 11.
  Directions: In 0, Out 1, InOut 2. They match `Enum.EasingStyle` and
  `Enum.EasingDirection`, which this spec adds.
- The reader refuses an unknown major version, a bad CRC, an unsorted or
  non-finite time, a name past the blob, a weight outside 0..1, and an easing
  value it does not know, saying where, as AMESH's does.

## 2. Import (`src/ide/ModelImport.cpp`)

- Every `aiAnimation` in a model file becomes an Animation, made with the
  Prefab's assets by `build_model_assets`: `Assets.Animations/<File>/<Clip>`,
  the clip file in `resources/animations/<File>/<Clip>.aanim`. An unnamed clip
  is named after the file; duplicates get the importer's `-2` suffix.
- A file with clips and no triangles (an "animation only" export) imports
  without failing: it makes its Animations alone, no Prefab.
- The "Animations are not imported yet" note goes.
- **Tracks to keyframes.** Each `aiNodeAnim` channel names a bone. Its
  keys (absolute local position, rotation, scale, at their own times) are
  sampled, linear for position and scale and slerp for rotation, at the union
  of every channel's key times in the clip (in seconds: ticks over
  `mTicksPerSecond`, 25 when the file gives 0). Each sample becomes a pose,
  `rest_local⁻¹ × animated_local` decomposed, Linear easing, weight 1.
- **Rest pose.** With a mesh in the same file, a bone's rest local is the
  skeleton's (`2026-10-10-skinned-meshes-design.md` §1). Without one, it is
  the node's own local transform in the file, which for Mixamo exports matches
  their skinned files. A channel whose node is not a bone of the skeleton
  still imports, named as its node, against that node's transform.

## 3. Runtime (engine_core)

### Animator

`Animator : DataModel` (not a PVInstance), suited parents `{"GameObject"}`.

| Property / method | Type | Meaning |
|---|---|---|
| `AutoStep` | boolean, saved, true | Play mode steps it once a frame |
| `LoadAnimation(animation)` | AnimationTrack | a new track on this Animator, stopped |
| `GetPlayingAnimationTracks()` | {AnimationTrack} | tracks whose IsPlaying is true |
| `StepAnimations(dt)` | nil | steps every track by dt now, in edit mode or Play |

Its tracks are runtime state: not saved, not undone, dropped at Stop (which
restores the place) and when the Animator is destroyed.

### AnimationTrack

A userdata, not an instance (GrooveAnimator's "no instance waste").

| Member | Kind | Meaning |
|---|---|---|
| `Animation` | Animation, read-only | the clip |
| `Length` | number, read-only | the clip's |
| `Looped` | boolean | starts as the clip's |
| `Speed` | number | 1; may be negative |
| `TimePosition` | number | seconds; writing it jumps |
| `IsPlaying` | boolean, read-only | |
| `WeightCurrent`, `WeightTarget` | number, read-only | |
| `Play(fade?, speed?, weight?)` | method | fade 0.2, speed 1, weight 1 |
| `Stop(fade?)` | method | fades to 0, then IsPlaying is false and Stopped fires |
| `AdjustWeight(weight, fade?)` | method | |
| `AdjustSpeed(speed, fade?)` | method | |
| `Destroy()` | method | removes it from its Animator |
| `KeyframeReached` | signal (name, index) | |
| `Stopped` | signal | |

Signals on a userdata are new: the script runtime gets a track signal kind,
keyed by the track's id, beside its instance and plugin ones.

### One step, as GrooveAnimator's

For each Animator stepped, with dt:

1. Advance each track's weight and speed transitions (lerp over the fade;
   the last call wins; a finished fade to 0 started by Stop stops the track).
2. For each playing track: `TimePosition += dt × Speed`; past the end, a
   looped track wraps (and fires KeyframeReached for the last keyframe), an
   unlooped one clamps; below 0, a looped track wraps, an unlooped one clamps.
   A track whose weight is 0 and is not fading stops here: its time keeps
   phase, nothing else is done.
3. Find the keyframes either side of the time, walking from the track's last
   bracket (binary search only the first time or after a jump). Fire
   KeyframeReached when the bracket's left keyframe changes.
4. For each pose in the left keyframe (its right pair, or itself when the
   right keyframe has no pose for that bone), ease the fraction by the left
   pose's style and direction, interpolate position, scale, and weight
   linearly and rotation by slerp.
5. Blend: each bone's contributions weighted by
   `track weight / max(1, Σ track weights) × pose weight`, folded as
   GrooveAnimator does (start from the heaviest, lerp/slerp toward each next
   by its share of the running total). A bone with one contributor skips it.
6. Store the result: per bone of the GameObject's skeleton (matched by name,
   resolved once per track and clip, and again when the skeleton changes), a
   change from rest, or identity for bones no track moves; bump the
   Animator's revision.

Events fire through the DataModel's deferred events, as a Dragger's do.

### Feeding the pose

- `GameObject::pose()` takes its Animator child's (the first one, by child
  order) blended changes and revision as one more input; a new revision
  makes the pose again.
- `compute_pose` gains the animation layer: per bone,
  `local = rest_local × animated × Offset`; `Pose::locals` holds
  `rest_local × animated`, the animated local before the Offset, so
  `Bone.set_transform` still solves for the Offset on top of the animation.
- A GameObject with no Animator, or one never stepped, poses as before.

### When it steps

- **Play mode:** once a frame, in `Engine`'s step, after the PreAnimation phase
  and its event drain (so scripts' input handlers and Play/Stop calls of the
  frame are in) and before physics: every Animator under Workspace with
  AutoStep true, by the frame's dt.
- **`StepAnimations(dt)`:** at once, in either mode, on the calling thread
  (SimulationThread, as every script); dt may be 0 (re-evaluate) or negative.

## 4. Lua and the docs

- `Animation`, `Animator` are creatable (`Instance.new`), in the factory,
  `ClassOrder`, Insert Object, and `LuaApi.cpp`'s docs, as Bone was.
- `Enum.EasingStyle` and `Enum.EasingDirection` are new enums, with the values
  above.
- AnimationTrack's members are in the Luau type definitions, so completion
  knows them.

## 5. Testing

- **aanim:** write/read round trip; each refusal (bad magic, version, CRC,
  unsorted time, weight out of range, unknown easing).
- **Sampling and blending (pure, no DataModel):** easing values at known
  points per style; a track between two keyframes; looping and its
  KeyframeReached on wrap; negative speed wrapping; a fade in from 0 and a
  Stop that ends at 0 and stops; normalization by `max(1, Σ)`; a pose weight
  of 0 masking a bone (an upper-body clip over a walk); a zero-weight track
  advancing time only.
- **Animator:** AutoStep on steps in Play and not in edit; AutoStep off steps
  only by StepAnimations; StepAnimations works in both modes; a Bone Offset
  adds on top of the animation; Stop drops the tracks; Lua API refusals
  (bad animation, non-finite numbers).
- **Import:** a glTF with a two-bone skin and one clip, built in the test,
  gives an Animation whose poses are the changes from rest; an animation-only
  glTF imports Animations and no Prefab.
- **Live:** in Release Studio over MCP, an idle and a walk on one character,
  crossfaded from a script, with a screenshot each side of the fade.

## Out of scope

Priorities; root motion; markers beyond keyframe names; IK; state machines;
additive tracks beyond the Offset layer; PoseTool and the animation editor
(next); animations on non-skinned GameObjects.
