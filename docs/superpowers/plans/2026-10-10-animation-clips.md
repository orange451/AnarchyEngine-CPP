# Animation Clips Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Imported FBX/glTF clips become `Animation` assets that an `Animator` plays on a skinned GameObject through Groove-style `AnimationTrack`s, under the existing Bone Offsets.

**Architecture:**
- **File format.** A new `src/aanim/` library reads and writes `.aanim` keyframe files (no GL, no DataModel), as `src/amesh/` does for meshes.
- **Animation core.** A pure module, `src/engine_core/Animation.{hpp,cpp}`, holds easing, quaternions, the runtime `Clip` and `TrackState`, and `step_animations`, GrooveAnimator's step loop. It turns tracks into one change from rest per skeleton bone.
- **Animator.** The `Animator` instance owns its tracks. Each step it stores those changes as a shared, immutable `AnimatedPose`.
- **Pose.** `GameObject::pose()` folds the AnimatedPose in: local = rest × animation × Offset.
- **Stepping.** The engine steps Animators in Play right after PreAnimation and its event drain. `StepAnimations` steps one at once, in either mode.
- **Lua.** Tracks are a userdata whose signals use a new keyed signal kind.

**Tech Stack:** C++17, Luau, Assimp 5.3.1 (studio only), flecs tags for the class query, Catch2 (sandbox), the studio-tests harness.

**Spec:** `docs/superpowers/specs/2026-10-10-animation-clips-design.md`

## Global Constraints

- Distances are "units", never "studs".
- Tracks are runtime state. They are never saved, never undone, and dropped at Stop.
- Edit mode never steps an Animator by itself. Play steps those whose `AutoStep` is true. `StepAnimations` works in both.
- A pose is a change from its bone's rest local transform: `animated_local = rest_local × T(p) × R(q) × S(s)`.
- Blending normalises by `max(1, Σ track weights)`; per-pose weight masks a bone.
- Easing values follow Roblox's: EasingStyle Linear 0, Constant 1, Sine 2, Quad 3, Cubic 4, Quart 5, Quint 6, Exponential 7, Circular 8, Back 9, Elastic 10, Bounce 11; EasingDirection In 0, Out 1, InOut 2.
- MSVC `/W4` clean on touched code. Build Release: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target <t>`. Sandbox runs from the repo root: `build/Release/sandbox.exe "[tag]"`.
- Build only the targets a step needs. A full rebuild is slow.

## Review Focus

- **A clip whose bones the mesh lacks, or a mesh whose skeleton changes while tracks play.** Missing bones are ignored, and the tracks re-resolve their bones on the next step. Pinned in Task 5.
- **Stop during a fade, or Destroy inside a KeyframeReached handler.** No crash, no event left dangling, and the track is gone. Pinned in Tasks 5 and 6.
- **Negative speed and `TimePosition` written past Length** on looped and unlooped tracks. Time wraps or clamps, and no pose freezes. Pinned in Task 2.
- **A Mixamo-style file:** an `Armature` node scaled 0.01 above the root bone, animated root translation. The root lands where the file puts it, not 100× off. Pinned in Task 7.
- **Undo after a play session, or Stop with a track playing.** The pose returns to rest plus Offsets, and no track is left behind. Pinned in Task 5.

---

### Task 1: The .aanim format

**Files:**
- Create: `src/aanim/aanim.hpp`, `src/aanim/aanim.cpp`, `sandbox/aanim_tests.cpp`
- Modify: `CMakeLists.txt`. Add `add_library(aanim STATIC src/aanim/aanim.cpp)` with `target_include_directories(aanim PUBLIC src/aanim)`, link `aanim` PUBLIC into `engine_instances` as `amesh` is, add the test file to `sandbox`, and add `aanim` to the warnings list.

**Interfaces:**
- Produces:
```cpp
namespace anarchy::aanim {
enum class EasingStyle : std::uint8_t { Linear, Constant, Sine, Quad, Cubic, Quart, Quint, Exponential, Circular, Back, Elastic, Bounce };
enum class EasingDirection : std::uint8_t { In, Out, InOut };
struct Pose { std::string bone; float position[3]{0,0,0}; float rotation[4]{0,0,0,1}; float scale[3]{1,1,1};
              EasingStyle style = EasingStyle::Linear; EasingDirection direction = EasingDirection::In; float weight = 1.f; };
struct Keyframe { float time = 0.f; std::string name; std::vector<Pose> poses; };
struct Data { std::string name; bool looped = false; std::vector<Keyframe> keyframes; };
class AnimError : public std::runtime_error { public: AnimError(std::uint64_t byte_offset, std::string reason); std::uint64_t byte_offset; std::string reason; };
using ByteSpan = std::span-like {const std::byte*, size} (same shape as amesh::ByteSpan);
Data read(ByteSpan bytes);                        // throws AnimError
std::vector<std::byte> write(const Data& data);   // throws AnimError on data a reader would refuse
inline constexpr std::size_t kMaxFileSize = 256u << 20;
}
```
- File layout as the spec's §1. Header (32 bytes): `"AANM"`, `u16 major=1, u16 minor=0`, `u16 flags` (bit 0 looped), `u16 reserved`, `u32 keyframe_count`, `u32 pose_count`, `u32 name_blob_size`, `u32 reserved2`, `u32 reserved3`. Then the names blob, keyframe records of 16 bytes (`f32 time, u32 name_offset, u32 first_pose, u32 pose_count`) and pose records of 52 bytes (`u32 name_offset` (name_length is in the blob: u16 prefix), `f32 p[3], f32 q[4], f32 s[3], u8 style, u8 direction, u16 pad, f32 weight`), then the CRC32. Names in the blob are `u16 length` + UTF-8 bytes, each written once.

- [ ] **Step 1: Write the failing tests** in `sandbox/aanim_tests.cpp`, tag `[aanim]`:
  - **Round trip:** two keyframes, three poses, names, easing values, the looped flag; read(write(d)) equals d field for field.
  - **Refusals:** each throws `AnimError`, and its reason names the problem:
    - a wrong magic;
    - major version 2;
    - a flipped CRC byte;
    - keyframes out of time order (from `write` and, hand-edited, from `read`);
    - a NaN time;
    - weight 1.5;
    - easing style 12;
    - a truncated file.
  - An empty clip (no keyframes) round-trips.
- [ ] **Step 2: Run** the sandbox build. Expected: it fails, because `aanim.hpp` doesn't exist.
- [ ] **Step 3: Implement.** Reuse amesh's CRC through `anarchy::amesh::crc32`, linking `amesh` into `aanim`. The reader validates sizes before every load.
- [ ] **Step 4: Run** `build/Release/sandbox.exe "[aanim]"`. Expected: pass.
- [ ] **Step 5: Commit** "aanim: the keyframe clip format".

### Task 2: The animation core (pure)

**Files:**
- Create: `src/engine_core/Animation.hpp`, `src/engine_core/Animation.cpp`, `sandbox/animation_tests.cpp`
- Modify: `CMakeLists.txt` (engine_core sources, sandbox test)

**Interfaces:**
- Consumes: `aanim::Data`.
- Produces:
```cpp
namespace engine_core {
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
Quat quat_slerp(Quat a, Quat b, float t);                 // shortest arc, normalized
Matrix4 bone_delta_matrix(Vec3 p, Quat q, Vec3 s);       // T * R * S
float ease(aanim::EasingStyle style, aanim::EasingDirection direction, float t);  // Roblox's curves, t in 0..1

struct BonePose { Vec3 position{}; Quat rotation{}; Vec3 scale{1, 1, 1}; };
// A clip ready to sample: bone names deduplicated, each keyframe's poses by
// clip bone index, and for each keyframe the pairs (left pose, right pose or
// itself) with the next keyframe and with itself, built once.
struct Clip {
    std::string name; bool looped = false; float length = 0.f;
    std::vector<std::string> bones;
    struct KeyPose { std::uint16_t bone; BonePose pose; aanim::EasingStyle style; aanim::EasingDirection direction; float weight; };
    struct Keyframe { float time; std::string name; std::vector<KeyPose> poses; std::vector<std::pair<int,int>> next, self; };
    std::vector<Keyframe> keyframes;
};
std::shared_ptr<const Clip> make_clip(const aanim::Data& data);

struct TrackEvent { enum class Kind { KeyframeReached, Stopped } kind; std::uint32_t track; std::string name; int index = 0; };
struct TrackState {
    std::uint32_t id = 0;
    std::shared_ptr<const Clip> clip;
    std::vector<int> bone_map;        // clip bone -> skeleton bone, or -1
    bool looped = false, playing = false;
    float speed = 1.f, time = 0.f, weight_current = 0.f, weight_target = 1.f;
    // Transitions: from, to, elapsed, duration (duration < 0: none); stop_on_fade ends the track at 0.
    float w_from = 0, w_to = 0, w_t = 0, w_dur = -1; bool stop_on_fade = false;
    float s_from = 0, s_to = 0, s_t = 0, s_dur = -1;
    int key_lo = -1; int keyframe_index = -1;
};
void track_play(TrackState&, float fade, float speed, float weight);   // fades in from 0 when not playing
void track_stop(TrackState&, float fade);
void track_adjust_weight(TrackState&, float weight, float fade);
void track_adjust_speed(TrackState&, float speed, float fade);
// Steps tracks by dt (GrooveAnimator's step, spec §3 "One step") and writes
// one change from rest per skeleton bone into out (size bone_count; identity
// for bones no track moves); touched[b] says which bones a track moved.
void step_animations(std::vector<TrackState>& tracks, float dt, std::size_t bone_count,
                     std::vector<BonePose>& out, std::vector<bool>& touched, std::vector<TrackEvent>& events);
}
```

- [ ] **Step 1: Failing tests** (`[animation]`). Helper: a clip with keyframes at 0, 1, 2 for bones "A" and "B" (A: translations 0, 2, 4 along X; B: rotation 0°, 90°, 180° about Z), with bone_map {0, 1}.
  - **Easing:** for every style × direction, `ease(…, 0) = 0` and `ease(…, 1) = 1`. Quad In at 0.5 is 0.25, Out at 0.5 is 0.75, InOut at 0.25 is 0.125. Constant is 0 below 1. Bounce Out at 0.5 is 0.765625.
  - **Sampling:** play at weight 1 with fade 0, then step 0.5: A.x is 1 and B turns 45°.
  - **Looping:**
    - A looped track stepped to 2.5 wraps to 0.5 and fires KeyframeReached for the last keyframe on the wrap.
    - An unlooped one clamps at 2.
    - At speed −1 from 0.25, a looped track wraps to 1.75 and an unlooped one clamps at 0.
    - A time written past Length wraps or clamps on the next step.
  - **Fades:** Play with fade 0.2 starts the weight at 0; it reaches 0.5 at 0.1 and 1 at 0.2. Stop with fade 0.2 reaches 0 at 0.2, then `playing` is false and Stopped fires once.
  - **Blending:** two tracks of weight 1 each on bone A, one at x 0 and one at x 2, give x 1 (normalised by `max(1, 2)`). One track of weight 0.5 alone gives half its x; its share is `0.5 / max(1, 0.5)` against rest.
  - **Masking:** a second clip with a pose of weight 0 on A and weight 1 on B, played over the first, leaves A as the first has it and blends B.
  - **Zero weight:** a weight-0 track's time advances, but it contributes nothing and fires no KeyframeReached.
  - **Bone map:** a clip bone mapped to −1 is ignored, and the bones nothing touches are identity.
- [ ] **Step 2: Run** and confirm the tests fail to compile.
- [ ] **Step 3: Implement**, following GrooveAnimator 1.0.9's step:
  - the transitions;
  - time and the loop wrap or clamp, including negative speed;
  - the zero-weight fast path;
  - the keyframe bracket walk from `key_lo`;
  - KeyframeReached on a bracket change;
  - easing per left pose;
  - lerp for position, scale and weight, slerp for rotation;
  - per-bone accumulation of (BonePose, weight), folded from the heaviest by `w / running_total`, with one contributor passed straight through.
- [ ] **Step 4: Run** `[animation]`. Expected: pass.
- [ ] **Step 5: Commit** "Animation core: clips, tracks, and GrooveAnimator's step".

### Task 3: The pose's animation layer

**Files:**
- Modify: `src/engine_core/Skeleton.hpp`/`.cpp` (`compute_pose` takes `const std::vector<Matrix4>* animated = nullptr`)
- Test: `sandbox/skeleton_tests.cpp`

- [ ] **Step 1: Failing test** SKEL10. Using the three-bone chain, an animated delta of a 90° turn about Z on `mid` plus an Offset translation (1,0,0) on `mid`:
  - `locals[mid] == rest_local × delta`;
  - `globals[mid] == rest_local × delta × Offset` (it is a root child);
  - `tip` follows;
  - a null or wrong-sized `animated` is ignored.
- [ ] **Step 2: Run** and confirm failure.
- [ ] **Step 3: Implement** it: `local = rest_local × animated[b] × offset`, and `locals[b] = rest_local × animated[b]`.
- [ ] **Step 4: Run** `[skeleton],[bone]`. Expected: pass, so the Bone tests still hold.
- [ ] **Step 5: Commit** "The pose takes an animation layer under the Offsets".

### Task 4: The Animation asset and the Animations category

**Files:**
- Modify: `src/engine_instances/AssetInstances.hpp`/`.cpp`. Add `class Animation : public FileAsset` with `std::shared_ptr<const Clip> clip() const`, cached as `Mesh::skeleton` is; `Length`, `Looped` and `Loaded` read-only properties; a `GetKeyframeNames` method; `on_reuse`.
- Modify: `Containment.cpp:12-22` (`{"Animations", "Animation", "Animations"}`), `Containment.hpp:22-26` kServices, `Project.cpp:122-137` (the count goes to 12, plus `existing_service<11>`), `GameService.hpp`/`.cpp` (`class Animations`), `Game.cpp:35-45`, `Project.cpp` factory, `ScriptBindings.cpp` creatable, `LuaApi.cpp` docs, `ClassOrder.hpp`, `src/ide/IdeIcons.cpp` (folder icon), `src/ide/InsertPopup.cpp` if it lists asset classes.
- Modify the tests that pin five categories: `sandbox/game_services_tests.cpp:124,668,735,820-823`, `tests/AssetBrowserTest.cpp:46-48`, `tests/McpTest.cpp:239-240`, `sandbox/project_tests.cpp:140-142`. Each gains Animations.
- Test: `sandbox/animation_tests.cpp`

**Interfaces:**
- Produces: `std::shared_ptr<const Clip> Animation::clip() const`. Null with no Path, no file, or a file that does not read. Read again when the Path or root changes, or the file's time changes (looked at once a second).

- [ ] **Step 1: Failing tests** (`[animation]`). Write an `.aanim` with `aanim::write` into a TempDir's `animations/walk.aanim`. An Animation with that Path then has:
  - `clip()->length == 2`;
  - Lua `Length == 2`;
  - `Looped` as the file's flag;
  - `Loaded` true;
  - `GetKeyframeNames()` returning the names.

  Rewriting the file is seen after 1.1 s. An Animation can only live under `Assets.Animations`, as Containment says.
- [ ] **Step 2: Run** and confirm failure.
- [ ] **Step 3: Implement** it, and update the pinned category tests.
- [ ] **Step 4: Run** `[animation]`, the full sandbox, engine-tests and mcp-tests. Expected: pass.
- [ ] **Step 5: Commit** "Animation: a clip asset, in Assets.Animations".

### Task 5: The Animator

**Files:**
- Create: `src/engine_instances/Animator.hpp`/`.cpp`, `src/engine_core/AnimatorStep.hpp`/`.cpp` (`step_animators(DataModel&, double dt)`), `sandbox/animator_tests.cpp`
- Modify:
  - `DataModel.hpp`/`.cpp`: an `animator()` flag becomes an ECS tag, plus `void animators(std::vector<InstanceId>&) const`, as `draggers` does.
  - `Ecs.hpp`.
  - `src/engine_instances/GameObject.cpp`: `pose()` adds the first Animator child's AnimatedPose, with its revision as an input.
  - `src/engine_core/Engine.cpp:426`: call `step_animators(game_, step_dt)` after the drain.
  - The registration sites: factory, creatable, docs, ClassOrder, suited parents `{"GameObject"}`.

**Interfaces:**
- Consumes: `make_clip`, `step_animations`, `prefab_skeleton`, `compute_pose(..., animated)`.
- Produces:
```cpp
struct AnimatedPose { std::uint64_t revision = 0; std::uint64_t skeleton = 0; std::vector<Matrix4> deltas; };
class Animator : public DataModel {
public:
    bool animator() const override { return true; }
    bool auto_step() const; std::optional<std::string> set_auto_step(bool);   // saved, default true
    std::uint32_t load(InstanceId animation);          // a new stopped track's id; 0 when animation is not a live Animation
    TrackState* track(std::uint32_t id);               // null when gone (destroyed, or dropped at Stop)
    void destroy_track(std::uint32_t id);
    std::vector<std::uint32_t> playing() const;
    void step(double dt);                              // SimulationThread: steps tracks, stores the pose, fires events
    std::shared_ptr<const AnimatedPose> animated() const;   // any thread under the DataModel lock; null before a step
    Signal& track_signal(std::uint32_t id, TrackEvent::Kind kind);   // made on first use (EventQueue::host_signal)
};
```
- Tracks are tagged with `world_generation()` when made. A change in the generation (Stop) drops every track, releases their signals, and makes `animated()` null. So do `on_reuse` and destruction.
- `step`:
  - resolves the skeleton through the parent GameObject's `prefab_skeleton`, and rebuilds each track's `bone_map` (by name) when the skeleton's signature changes;
  - turns the BonePoses into deltas with `bone_delta_matrix`;
  - stores a new AnimatedPose under a mutex;
  - emits each TrackEvent through `events().emit_args(track_signal(...).id(), 0, args)`.

  With no skinned parent it steps time only.
- `step_animators`, for every Animator in Workspace with AutoStep on: in Play only (`simulation_running()`), `step(dt)`.

- [ ] **Step 1: Failing tests** (`[animator]`) through the C++ API. Use the skinning Rig with a clip that moves "Hand", and an Animator under the arm GameObject.
  - `load` and play a track, `step(0.5)`, and the Hand Bone's `Transform` moves as the clip says.
  - A Hand Bone with an Offset adds on top.
  - `step_animators` steps in Play when AutoStep is true, and not in edit mode or when AutoStep is false. `step` itself works in both.
  - Stop (`stop_simulation`) drops the tracks: `track(id) == nullptr`, and the pose is back to rest + Offset.
  - Undo of an AutoStep write restores it.
  - A clip naming a bone the mesh lacks is ignored.
  - Swapping the Prefab to a mesh with a different skeleton re-resolves the bones on the next step.
- [ ] **Step 2: Run** and confirm failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `[animator],[bone],[skeleton],[render]`. Expected: pass.
- [ ] **Step 5: Commit** "Animator: tracks on a GameObject, stepped in Play or on demand".

### Task 6: Lua — tracks, signals, enums

**Files:**
- Create: `src/engine_core/AnimationBindings.cpp`. It holds the `AE.AnimationTrack` metatable (`__index`, `__newindex`, `__tostring`, `__eq`, `__type`) and the Animator methods `LoadAnimation`, `GetPlayingAnimationTracks` and `StepAnimations`, registered with `register_lua_class("Animator", nullptr, methods)`. Docs-only `register_lua_class("AnimationTrack", nullptr, fields)` lists its properties, methods and `lua_event("KeyframeReached", {name: string, index: number})` / `lua_event("Stopped")`. `open_animation_track(lua_State*)` is called from `open_host_libraries`.
- Modify:
  - `ScriptBindings.hpp`: `kSignalTrack = 6`; SignalUd gains `std::uint32_t track`; the static method declarations; `link_animation_methods`.
  - `ScriptBindings.cpp`: `signal_of` maps kSignalTrack to `Animator::track_signal`; `signal_cause`; and the `signal_connect`/`signal_wait` arg lists.
  - `Enum.cpp`/`.hpp`: `Enum.EasingStyle`, `Enum.EasingDirection`.
  - `AnalysisDefinitions.cpp` if userdata signals need their own type.
  - `LuaApi.cpp` docs.

**Interfaces:**
- Produces Lua: `animator:LoadAnimation(animation) -> AnimationTrack`, `animator:GetPlayingAnimationTracks() -> {AnimationTrack}`, `animator:StepAnimations(dt)`. The track's members are as the spec's table lists. A track whose Animator or slot is gone raises "AnimationTrack is gone". Non-finite numbers are refused.

- [ ] **Step 1: Failing tests** (`[animator]`) with a ScriptRig whose arm has an Animator. The script:
  - loads the clip and checks `track.Length == 2`;
  - calls `track:Play(0)` and `animator:StepAnimations(0.5)`, then checks the Hand moved;
  - connects KeyframeReached and sees it after stepping across a keyframe;
  - calls `Stop(0)`, `StepAnimations(0)`, and sees Stopped fire and `IsPlaying` false;
  - finds `GetPlayingAnimationTracks()` empty;
  - finds `pcall(LoadAnimation, workspace)` fails, and `track:Destroy()` then any member raises "is gone";
  - checks `Enum.EasingStyle.Bounce.Value == 11`;
  - checks that a Destroy inside a KeyframeReached handler is safe.
- [ ] **Step 2: Run** and confirm failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `[animator]`, the full sandbox, and engine-tests (completion and definitions). Expected: pass.
- [ ] **Step 5: Commit** "AnimationTrack in Lua: LoadAnimation, Play, Stop, fades, and its signals".

### Task 7: Import clips

**Files:**
- Modify: `src/ide/ModelImport.hpp` (`struct ImportedAnimation { std::string name, path; }`; `ImportedModel::animations`), `src/ide/ModelImport.cpp`, `src/ide/McpToolSpecs.cpp` (import_assets text)
- Test: `tests/ModelImportTest.cpp`

**How:**
- Each `aiAnimation` is sampled per channel at the union of the clip's key times (in seconds). Positions and scales are interpolated linearly between keys, rotations by slerp.
- A channel's node local is made relative to its skeleton parent. When the node's parent isn't a bone, it is `globals[parent] × local`, which keeps an Armature's scale.
- Each sample is turned into `rest_local⁻¹ × local`:
  - **with a skeleton:** `rest_local` is the bone's rest local (from the SkeletonTable's rest matrices);
  - **without one:** it is the node's own `mTransformation`, made relative the same way.
- The result is decomposed (`aiMatrix4x4::Decompose`) into one pose, Linear, weight 1, and written with `aanim::write` to `animations/<File>/<Clip>.aanim`.
- A file with clips and no triangles makes no meshes and no error.
- `build_model_assets` makes `Assets.Animations/<File>` and an Animation for each clip, and makes the Prefab only when there are meshes.
- The "Animations are not imported yet" note is removed.

- [ ] **Step 1: Failing tests:**
  - **Skinned arm:** the skinned arm glTF from the skinning tests gains one animation, rotating `Hand` 90° about Z at t = 1 and translating `Root` 1 unit on X at t = 1. It imports one Animation whose `.aanim` has keyframes at 0 and 1. Hand's pose at 1 is the 90° turn (a change from rest), and Root's at 0 is identity.
  - **Animation-only:** the same rig, the triangles removed, imports Animations and no Prefab, with no error.
  - **Mixamo-style:** an `Armature` node scaled 0.01 above Root, with Root translated by 100 in the file, gives Root's rest-to-pose change a translation of 1 unit in model space.
- [ ] **Step 2: Run** `studio-tests` and see the new checks fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `studio-tests`, then the full sandbox. Expected: pass.
- [ ] **Step 5: Commit** "Model import brings clips in as Animations".

### Task 8: Live check

- [ ] Generate a rigged bar with two clips ("Bend": Hand 0°→60°→0° over 2 s, looped; "Twist": Hand twisting about Y) as glTF in the scratchpad, using `py -I`.
- [ ] In Release Studio over MCP: import it, place it, add an Animator, play Bend in Play mode, take a screenshot mid-bend, crossfade to Twist over 0.5 s, and take a screenshot after the fade.
- [ ] Run the full sandbox, studio-tests, engine-tests and mcp-tests, then commit any fixes.
