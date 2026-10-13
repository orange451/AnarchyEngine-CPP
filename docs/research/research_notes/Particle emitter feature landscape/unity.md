# Unity particle systems: Built-in Particle System (Shuriken) and VFX Graph

Context for readers: these notes feed a CPU-simulated, property-driven emitter drawn as instanced quads on OpenGL 3.3 (no compute). Each section flags how CPU-feasible and cheap each feature is. Facts marked with a source came from Unity docs, blogs or forums fetched on 2026-10-12, mostly the Unity 6.0 manual. Lines marked "(unverified, prior knowledge)" were not checked against a fetched source this session. Treat them as leads, not facts.

## Q1. Shuriken module inventory: what each module contains

### Takeaway
Shuriken is a fixed stack of about 23 optional modules on top of an always-on Main module. Each module is a small, predefined behaviour, and almost every numeric field is a MinMaxCurve or MinMaxGradient (see Q2). Nearly everything in it is CPU-feasible for an instanced-quad renderer. The costly modules are World Collision, Lights, Trails/Ribbons (they build geometry), and Noise with high octave or quality settings.

### Cited Findings

**Main module (always present)**
- Timing: Duration, Looping, Prewarm (requires Looping; starts as if one full cycle had played), Start Delay, Play on Awake, Simulation Speed, Delta Time (Scaled / Unscaled; Unscaled suits pause menus) — [Unity Manual: Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Initial values: Start Lifetime, Start Speed, Start Size with an optional 3D per-axis toggle, Start Rotation with an optional 3D toggle, Flip Rotation (a fraction of particles spin the other way), Start Color — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Gravity Modifier scales the physics gravity (0 turns it off). Simulation Space is Local, World or Custom (relative to a chosen Transform) — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Scaling Mode: **Hierarchy** applies scale from the whole parent chain; **Local** applies only this transform's scale; **Shape** scales start positions without changing particle size — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Emitter Velocity mode sets how emitter velocity is measured for Inherit Velocity and Emission (rate over distance): from a Rigidbody if one exists, otherwise from the Transform — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Max Particles caps live particles; emission past the cap is dropped. Auto Random Seed on gives a different look each play; off plus Random Seed gives an exactly repeatable effect — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Stop Action: Disable, Destroy, or Callback (sends OnParticleSystemStopped) — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Culling Mode: **Automatic** (looping systems Pause, others Always Simulate), **Pause And Catch-up** (stops offscreen, then takes one large step on return; can spike), **Pause**, **Always Simulate** — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Ring Buffer Mode: **Disabled** (particles die at end of life), **Pause Until Replaced** (old particles freeze at end of life until the Max Particles cap recycles them), **Loop Until Replaced** (old particles rewind to a set fraction of their lifetime until recycled). Typical uses are persistent decals or footprints — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)

**Emission**
- Rate over Time, Rate over Distance (particles per unit moved), and Bursts. Each burst has Time, Count, Cycles, Interval and Probability (0–1) — [Unity Manual: Emission module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysEmissionModule.html)

**Shape**
- Types: Sphere, Hemisphere, Cone, Donut, Box, Mesh, Mesh Renderer, Skinned Mesh Renderer, Sprite, Sprite Renderer, Circle, Edge, Rectangle — [Unity Manual: Shape module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysShapeModule.html)
- Common settings: Radius Thickness (0 = surface only, 1 = full volume); Arc with Mode Random / Loop / Ping-Pong / Burst Spread (Burst Spread only for Sphere, Hemisphere, Cone, Donut, Circle and Edge); Spread (snaps spawns to discrete arc intervals); Emit From (Box: Volume/Edge/Shell; Cone: Base/Volume); Texture (tint and discard by texture); Position/Rotation/Scale offsets; Align to Direction; Randomize Direction (0–1); Spherize Direction (0–1); Randomize Position — [Shape module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysShapeModule.html)

**Velocity over Lifetime**
- Linear x/y/z curves; orbitalX/Y/Z (speed around each axis); orbitalOffsetX/Y/Z (a custom centre for orbital and radial motion); radial (speed away from the centre); speedModifier (scales speed without changing direction); space (Local / World). Each field has a scalar multiplier — [ScriptReference: VelocityOverLifetimeModule](https://docs.unity3d.com/ScriptReference/ParticleSystem.VelocityOverLifetimeModule.html)

**Limit Velocity over Lifetime (drag)**
- limit, or limitX/Y/Z when separateAxes is on; dampen (how much speed above the limit is removed); drag; multiplyDragByParticleSize; multiplyDragByParticleVelocity; space — [ScriptReference: LimitVelocityOverLifetimeModule](https://docs.unity3d.com/ScriptReference/ParticleSystem.LimitVelocityOverLifetimeModule.html)
- A tutorial notes it "can be used to simulate drag" — [Kodeco / raywenderlich tutorial](https://www.kodeco.com/138-introduction-to-unity-particle-systems)

**Noise**
- Separate Axes, Strength (curve over lifetime), Frequency, Scroll Speed, Damping (strength scales with frequency), Octaves, Octave Multiplier, Octave Scale, Quality, Remap plus Remap Curve, and Position / Rotation / Size Amount — [Unity Manual: Noise module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysNoiseModule.html)
- Extra octaves "significantly add" to cost. Lower Quality "substantially" reduces cost, so the manual advises the lowest setting that works — [Noise module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysNoiseModule.html)

**Collision**
- Type Planes (a list of Transforms) or World (Collision Mode 2D/3D). Shared settings: Dampen, Bounce, Lifetime Loss, Min/Max Kill Speed, Radius Scale, Send Collision Messages (OnParticleCollision), Visualize Bounds — [Unity Manual: Collision module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysCollisionModule.html)
- World-only settings: Collision Quality (High / Medium (Static Colliders) / Low (Static Colliders)), Collides With (layer mask), Max Collision Shapes (terrains take priority), Enable Dynamic Colliders, Voxel Size (Medium/Low only), Collider Force, and Multiply by Collision Angle / Particle Speed / Particle Size — [Collision module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysCollisionModule.html)
- Cost: High always queries physics and is the most expensive. Medium and Low cache results in a voxel grid reused across frames, and should only be used with colliders that never move. Low queries least often, so particles can occasionally pass through colliders — [Collision module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysCollisionModule.html)

**Sub Emitters**
- Spawn conditions: Birth (for the whole parent lifetime; full Emission settings and multiple bursts), Collision, Death, Trigger, Manual (fires only from `ParticleSystem.TriggerSubEmitter`, for all particles or a subset). Collision, Death, Trigger and Manual support only a single burst — [Unity Manual: Sub Emitters module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysSubEmitModule.html)
- Each entry has an Inherit selector (which parent properties to inherit) and an Emit Probability (0–1) — [Sub Emitters module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysSubEmitModule.html)

**Trails**
- Mode **Particle** (each particle leaves a stationary trail) or **Ribbon** (connects particles in order of age) — [Unity Manual: Trails module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysTrailsModule.html)
- Settings: Ratio, Lifetime (as a multiple of the particle's lifetime), Minimum Vertex Distance, World Space, Die With Particles, Ribbon Count (connect every Nth particle), Split Sub Emitter Ribbons, Texture Mode (Stretch / Tile / Repeat per Segment / Distribute per Segment), Size affects Width, Size affects Lifetime, Inherit Particle Color, Color over Lifetime, Width over Trail, Color over Trail, Generate Lighting Data, Shadow Bias — [Trails module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysTrailsModule.html)

**Lights**
- Settings: Light (a prefab), Ratio, Random Distribution (random chance vs every Nth particle), Use Particle Color, Size Affects Range, Alpha Affects Intensity, Range Multiplier and Intensity Multiplier curves, Maximum Lights. The cap exists because too many lights can make the Editor or game "run very slowly" — [Unity Manual: Lights module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysLightsModule.html)

**Renderer**
- Render Mode: Billboard, Stretched Billboard (Camera Scale, Velocity Scale, Length Scale, Freeform Stretching, Rotate With Stretch), Horizontal Billboard, Vertical Billboard, Mesh (Mesh Distribution Uniform / Non-uniform Random with weightings), None — [Unity Manual: Renderer module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysRendererModule.html)
- Normal Direction runs from 0 (sphere-like normals) to 1 (flat quad normals) for billboard lighting — [Renderer module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysRendererModule.html)
- Sort Mode: None, By Distance, Oldest in Front, Youngest in Front, By Depth. Sorting Fudge biases the system's order against other transparent objects — [Renderer module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysRendererModule.html)
- Render Alignment: View, World, Local, Facing, Velocity. Also Flip (mirror a fraction of particles per axis), Allow Roll (useful in VR), Pivot (as a multiple of particle size), Visualize Pivot, and Min/Max Particle Size (a fraction of the viewport; billboard modes only) — [Renderer module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysRendererModule.html)
- Also: Masking (for Sprite Masks), Apply Active Color Space, Custom Vertex Streams, Cast Shadows (On / Off / Two-Sided / Shadows Only), Shadow Bias, Motion Vectors, Receive Shadows, sorting layer and order, light and reflection probes, and Enable Mesh GPU Instancing for Mesh mode (needs a compatible shader) — [Renderer module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysRendererModule.html)
- The fetched Renderer page does not mention soft particles — [Renderer module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysRendererModule.html)

### Inferences
- **Remaining modules (unverified, prior knowledge; fetch these pages before relying on details):**
  - Inherit Velocity: Mode Initial (sampled once at birth) or Current (applied every frame), plus a Multiplier curve.
  - Force over Lifetime: x/y/z, Space, and Randomize (re-pick a random force every frame).
  - Color over Lifetime (a gradient multiplied by Start Color); Color by Speed (gradient plus speed Range).
  - Size over Lifetime and Size by Speed, each with a Separate Axes option (x/y/z curves).
  - Rotation over Lifetime and Rotation by Speed, with Separate Axes.
  - External Forces: a multiplier plus an influence filter (layer mask or list) for Particle System Force Field components and wind zones.
  - Triggers: a collider list with Inside / Outside / Enter / Exit actions (Ignore / Kill / Callback) and OnParticleTrigger.
  - Texture Sheet Animation: Grid or Sprites mode; tiles, Whole Sheet or Single Row, Frame over Time, Start Frame, Cycles, Time Mode (Lifetime / Speed / FPS), Affected UV channels.
  - Custom Data: two slots (Custom1 and Custom2), each Vector (up to 4 MinMaxCurves) or Color (MinMaxGradient), fed to the shader through vertex streams.
  - Renderer soft particles: in Unity these are a shader or material feature (Soft Particles Factor, using camera depth), not a Renderer module checkbox. This matches the gap above.
- **CPU feasibility for the target engine:**
  - Cheap per-particle maths: Main, Emission, bursts, every Shape primitive, Velocity (linear, orbital, radial, speedModifier), Limit Velocity/drag, Inherit Velocity, Force over Lifetime, all over-Lifetime and by-Speed modules, Texture Sheet, and Custom Data. Each is an O(1) curve sample plus integration per particle.
  - Noise is cheap at 1 octave and low quality, and grows roughly linearly with octaves.
  - Planes collision is cheap (a dot product per plane). World collision needs raycasts against the engine's physics and is the most expensive CPU path. A voxel cache of static geometry, like Unity's Medium/Low, is a proven trick.
  - Trails and Ribbons need dynamic strip geometry, not instanced quads. That is a separate render path, and per-particle position history costs memory.
  - Lights are a renderer cost: forward renderers pay for every light, so a hard cap is mandatory.
  - Billboard, Horizontal, Vertical, Stretched and Velocity alignment all fit instanced quads well; the instance data needs centre, size, rotation, velocity and colour.

### Gaps
- Exact Unity 6 manual pages for Inherit Velocity, Force over Lifetime, Color/Size/Rotation by Speed, External Forces, Triggers, Texture Sheet Animation and Custom Data were not fetched within the tool budget. The fetched Velocity over Lifetime manual URL returned 404, so the ScriptReference page was used instead.
- The sub-emitter Inherit flags (Color, Size, Rotation, Lifetime, Duration) were not confirmed on the fetched page.

## Q2. The MinMaxCurve and MinMaxGradient value model

### Takeaway
Nearly every scalar property in Shuriken accepts one of four modes: Constant, Curve, Random Between Two Constants, or Random Between Two Curves. A single type therefore covers fixed values, time-varying values and per-particle randomness. This uniformity is the system's most transferable idea, and it is very cheap on the CPU.

### Cited Findings
- `ParticleSystemCurveMode` values: **Constant** (one value), **Curve** (one curve), **TwoCurves** (random value between two curves), **TwoConstants** (random value between two constants). MinMaxCurve uses this enum to pick its evaluation mode — [ScriptReference: ParticleSystemCurveMode](https://docs.unity3d.com/ScriptReference/ParticleSystemCurveMode.html)
- Tutorial advice: random ranges on lifetime, size and speed make effects "feel natural"; ending Color over Lifetime at alpha 0 is called the biggest single factor in how good an effect looks — [uhiyama-lab guide](https://uhiyama-lab.com/en/notes/unity/unity-particle-system-vfx-guide/)
- Curves with more than 8 segments cannot run in procedural mode (count the keys, plus 1 if the curve does not start at 0.0, plus 1 if it does not end at 1.0). So Unity's own fast path assumes short, simple curves — [Unity blog: ParticleSystem Performance – Culling](https://unity.com/blog/engine-platform/particlesystem-performance-culling-tips)

### Inferences
- **Details (unverified, prior knowledge):**
  - Curves are normalised to the particle's lifetime (0..1, or the system's duration for system-level values like emission rate) and scaled by a scalar "curve multiplier" (`xxxMultiplier` in script). The `*Multiplier` fields on every module, seen on the VelocityOverLifetime and LimitVelocity ScriptReference pages above, are consistent with this.
  - For TwoCurves and TwoConstants, each particle gets a stable random number at birth that lerps between min and max for its whole life, so a particle follows one consistent curve rather than jittering.
  - `ParticleSystemGradientMode` has Color, Gradient, TwoColors, TwoGradients and RandomColor (a random sample from one gradient).
  - Why artists praise it: one consistent widget everywhere, randomness with no scripting, and curves that are easy to read.
- **For the engine:** implement one templated `NumberRange` / `NumberSequence`-style type with four modes and a per-particle random seed. Evaluating it is a branch, a curve lookup and a lerp. Pre-baking curves to small lookup tables (for example 32–64 samples) makes it trivially cheap. Roblox already uses NumberRange and NumberSequence, so this maps naturally onto the property-driven model.

### Gaps
- The Unity 6 manual page for MinMaxCurve and MinMaxGradient returned 404 at the guessed URL. The gradient mode enum and the Inspector wording were not verified this session.
- No artist-written source explicitly praising MinMaxCurve was found. The praise is inferred from tutorial emphasis on randomness and curves.

## Q3. Which modules artists treat as essential vs rarely used

### Takeaway
No survey or usage ranking exists. Tutorials consistently centre on a core set: Main, Emission (rate and bursts), Shape, Color over Lifetime, Size over Lifetime, Renderer, plus Texture Sheet, Noise, Sub Emitters and Velocity/Limit Velocity for polish. Lights, Triggers, External Forces, Custom Data and World Collision are niche or treated as costly.

### Cited Findings
- One guide frames modules along a particle's life (born, move, change, fade out): Rate over Time for continuous effects, Bursts for pops, Shape for silhouette, Color over Lifetime fade as the key quality factor, Sub Emitters for multi-stage effects such as fireworks — [uhiyama-lab guide](https://uhiyama-lab.com/en/notes/unity/unity-particle-system-vfx-guide/)
- The same guide says to keep Collision counts modest because it costs CPU time — [uhiyama-lab guide](https://uhiyama-lab.com/en/notes/unity/unity-particle-system-vfx-guide/)
- The Kodeco tutorial teaches "the most commonly used" modules through a torch and a bomb explosion, without ranking them — [Kodeco tutorial](https://www.kodeco.com/138-introduction-to-unity-particle-systems)

### Inferences
- A minimum viable Roblox-like emitter should cover: Main (lifetime, speed, size, rotation, colour as ranges or sequences; Duration, Looping, Prewarm, Start Delay; Max Particles; simulation space), Emission rate plus bursts, Shape primitives (sphere, hemisphere, cone, box, circle/disc, edge), colour, alpha and size over lifetime, gravity/acceleration, drag, speed modifier or orbital/radial velocity, cheap noise, flipbook animation, billboard/stretched/velocity-aligned rendering with additive and alpha blending, and death/birth sub-emitters. These are all CPU-cheap.

### Gaps
- No realtimevfx.com thread or artist talk with an explicit "most used modules" list was retrieved.

## Q4. Simulation, performance characteristics and pain points

### Takeaway
Shuriken simulates on the CPU with native code. Unity's docs place it at "thousands" of particles versus VFX Graph's "millions". Its best optimisation is **procedural mode**: a system with a predictable state can be culled offscreen and fast-forwarded on return. World space, noise, collision, sub-emitters, trails, scripted writes and similar features break procedural mode and force updates every frame. Known pain points are overdraw (a GPU cost), collision cost, GC from Instantiate/Destroy or unpooled arrays, and main-thread scripting.

### Cited Findings
- Unity's comparison: Built-in Particle System handles "thousands" of particles, VFX Graph "millions" with GPU simulation; Built-in works with Unity physics and offers full per-particle C# read/write access, collision events, and playback control including custom step sizes — [Unity Manual: Choosing your particle system](https://docs.unity3d.com/6000.0/Documentation/Manual/ChoosingYourParticleSystem.html)
- Procedural vs non-procedural: a procedural system's state can be computed for any time, so it can be culled and caught up later. A non-procedural one must keep updating offscreen — [Unity blog: Culling tips](https://unity.com/blog/engine-platform/particlesystem-performance-culling-tips)
- **Settings that break procedural mode:** World simulation space; a gravity modifier that uses curves; non-zero Rate over Distance; External Forces; Limit (Clamp) Velocity; Rotation by Speed; Collision; Triggers; Sub Emitters; Noise; Trails; curves too complex for procedural evaluation in Rotation/Velocity/Force over Lifetime; Force over Lifetime Randomize — [Unity blog](https://unity.com/blog/engine-platform/particlesystem-performance-culling-tips)
- Changing properties from script or the Editor during play, or emitting from script, invalidates procedural mode. Calling Play on a stopped system revalidates it. The Inspector shows a warning icon whose tooltip lists the reasons — [Unity blog](https://unity.com/blog/engine-platform/particlesystem-performance-culling-tips)
- Bounds are a diagnostic: procedural systems have fixed bounds covering the whole lifetime volume, while non-procedural bounds change constantly to fit live particles — [Unity blog](https://unity.com/blog/engine-platform/particlesystem-performance-culling-tips)
- Test: 120 systems of 1,000 particles each; culled local-space (procedural) systems performed "significantly better" than world-space ones, especially offscreen. Custom culling with CullingGroup is suggested, with `Simulate` used to catch up on resume — [Unity blog](https://unity.com/blog/engine-platform/particlesystem-performance-culling-tips)
- Pause And Catch-up culling can cause spikes in complex systems — [Main module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysMainModule.html)
- Job System integration: `IJobParticleSystem` (one job sees all particles), `IJobParticleSystemParallelFor` (one particle per Execute), `IJobParticleSystemParallelForBatch` (a range per Execute). Jobs are Burst-compilable. `GetParticles`/`SetParticles` offer similar functionality "but run on the main thread and cannot make use of" Burst — [Unity Manual: Particle System C# Job System integration](https://docs.unity3d.com/Manual/particle-system-job-system-integration.html)
- Pitfalls: pool effects instead of using Instantiate/Destroy (GC spikes), for example with Stop Action Callback to return effects to a pool. Transparent overlap causes overdraw and GPU spikes; on mobile, use "fewer, larger particles". Use Shuriken for a few thousand particles and VFX Graph beyond that — [uhiyama-lab guide](https://uhiyama-lab.com/en/notes/unity/unity-particle-system-vfx-guide/)
- Collision quality trade-offs and the static-only voxel cache — [Collision module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysCollisionModule.html)
- Lights need a cap to avoid severe slowdowns — [Lights module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysLightsModule.html)
- A forum developer reported that Shuriken "can definitely outperform" VFX Graph on weak GPUs and shipped a runtime toggle between the two — [Unity forum: fallback to Shuriken](https://forum.unity.com/threads/should-i-allow-fallback-to-shuriken-particles-for-lower-end-gpu-bound-systems.892072/)

### Inferences
- Unity's internal Shuriken update is widely understood to be multithreaded native code. Systems are updated in parallel on worker threads, with sorting and mesh generation also off the main thread (unverified, prior knowledge; no fetched source states this).
- **Lessons for the engine:**
  1. A procedural, analytic fast path is valuable. Particles under only constant acceleration plus over-lifetime curves have closed-form positions, so offscreen emitters can skip simulation entirely and catch up with no per-frame cost. Treat that as an optimisation tier, and fall back to step integration when noise, drag, collision or world space are on.
  2. Sorting is per emitter. Large transparent effects that overlap other emitters will sort incorrectly; Unity offers Sorting Fudge only as a coarse bias.
  3. Overdraw, not simulation, is often the real limit on low-end GPUs. Supporting quad-shape trimming, low-res particle passes or fewer/larger defaults matches the low-end-hardware principle.
  4. Fixed Max Particles plus a ring buffer gives zero-allocation storage, a natural structure-of-arrays layout for the CPU, and an instance buffer upload per frame.

### Gaps
- No primary Unity source quantifying Shuriken's per-particle CPU cost or its internal threading model was retrieved.
- No source on sorting cost (By Distance sorts per frame) was retrieved.

## Q5. Scripting API: GetParticles/SetParticles, Emit and EmitParams, custom vertex streams, sub-emitter triggering

### Takeaway
Shuriken exposes a full CPU-side particle array. You can read, modify and write back particles, emit with per-call overrides, push extra per-particle data to shaders, and fire Manual sub-emitters from code. That only works because the simulation is CPU-resident, which is exactly the target engine's position.

### Cited Findings
- `GetParticles` has 6 overloads: a managed `Particle[]` or a `NativeArray<Particle>`, each with optional size and offset. It returns the live count, and is allocation-free if you preallocate the array once (for example to `main.maxParticles`) and reuse it. The documented pattern is GetParticles, modify velocity, then `SetParticles(particles, numAlive)` — [ScriptReference: GetParticles](https://docs.unity3d.com/ScriptReference/ParticleSystem.GetParticles.html)
- `EmitParams` overrides: position (plus applyShapeToPosition to keep the Shape module's influence), velocity, startLifetime, startSize/startSize3D, startColor, rotation/rotation3D, angularVelocity/angularVelocity3D, axisOfRotation, meshIndex, randomSeed, particle (override everything). Each has a matching Reset method that returns it to the Inspector value — [ScriptReference: EmitParams](https://docs.unity3d.com/ScriptReference/ParticleSystem.EmitParams.html)
- Custom Vertex Streams map particle attributes to shader inputs: Position, Normal, Color, UV (TEXCOORD0.xy), UV2 (TEXCOORD0.zw), AnimBlend (TEXCOORD1.x, for flipbook frame blending), Tangent, Custom1/Custom2 (set with SetCustomParticleData), plus built-ins such as velocity, size and centre. Only include the streams the shader uses — [Unity Manual: Particle vertex streams](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysVertexStreams.html)
- Manual sub-emitters fire through `ParticleSystem.TriggerSubEmitter` — [Sub Emitters module](https://docs.unity3d.com/6000.0/Documentation/Manual/PartSysSubEmitModule.html)
- Scripted emission and property writes during play break procedural mode, and therefore offscreen culling — [Unity blog](https://unity.com/blog/engine-platform/particlesystem-performance-culling-tips)

### Inferences
- Custom vertex streams map directly onto per-instance attributes in an instanced-quad GL 3.3 renderer. A compact instance layout (centre xyz, size xy, rotation, colour RGBA8, flipbook frame plus blend, optionally velocity for stretched particles) mirrors Unity's default streams.
- An `Emit(count, overrides)` Lua API analogous to EmitParams fits the Roblox-like model (Roblox has `ParticleEmitter:Emit(count)`). Emit with overrides is cheap.
- Full per-particle read/write from Lua would be costly across the scripting boundary. Unity's own move to jobs and Burst for this shows the main-thread array API is a known bottleneck.

### Gaps
- The exact signatures of `Emit` and `SetParticles` and the full `ParticleSystemVertexStream` list (Center, Speed, AgeNormalized, Random, StableRandom and others) were not fetched.

## Q6. VFX Graph, mainly as contrast

### Takeaway
VFX Graph is a GPU (compute-shader) node-graph system. Event and Spawn contexts drive particle systems made of Initialize, Update and Output contexts. It scales to millions of particles and adds GPU events, strips, SDF collision and point caches. In exchange it gives up CPU readback, Unity physics collisions, OpenGL ES, and dependable mobile support, and it carries a steeper authoring curve. None of its core is feasible without compute shaders, so it serves only as a source of feature ideas.

### Cited Findings
- Contexts: Spawn (how many particles), Initialize (starting state), Update (forces, collisions and other simulation), Output (shape and pre-render transforms). Blocks stack inside a context and run top to bottom; Operators build low-level maths that feeds block ports — [VFX Graph manual: Graph logic and philosophy](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.0/manual/GraphLogicAndPhilosophy.html)
- Requirements: `SystemInfo.supportsComputeShaders` must be true and `SystemInfo.maxComputeBufferInputsVertex` > 0 (SSBOs in the vertex stage). HDRP is fully supported. Mobile is described as "not out of preview", and the docs state: "does not support Open GL ES" — [VFX Graph manual: System requirements](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.0/manual/System-Requirements.html)
- Unity's comparison: VFX Graph is URP/HDRP only; millions of particles; physics only through elements defined in the graph (for example the depth buffer); scripting through exposed properties and custom events with attached data, not per-particle access; colour and depth buffer access in HDRP — [Choosing your particle system](https://docs.unity3d.com/6000.0/Documentation/Manual/ChoosingYourParticleSystem.html)
- Feature blocks: Collide with Signed Distance Field, Conform to SDF, Position (SDF), an SDF Bake Tool, Point Cache assets plus a Point Cache Bake Tool, and GPU Event blocks (Trigger Event Always / On Die / Rate) for particle-spawns-particle effects such as fireworks — [VFX Graph manual TOC 17.4](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.4/manual/toc.html); [VFX Graph examples repo](https://github.com/UlasTosun/Unity-VFX-Graph-Examples)
- Forum complaints and trade-offs: mobile support is limited to devices with compute and vertex-stage SSBOs (many Mali GPUs lack them). Shuriken is preferred for PhysX collisions and reading results back. Shuriken can beat VFX Graph on weak GPUs. A Unity rep said CPU simulation for non-compute devices was being worked on (2020–21; no later confirmation found) — [Unity forum: VFX on Android](https://discussions.unity.com/t/effects-created-using-vfx-graph-not-showing-on-android/779552); [Is VFX graph always better](https://discussions.unity.com/t/is-vfx-graph-always-better-than-the-particle-system/816576); [Fallback to Shuriken thread](https://forum.unity.com/threads/should-i-allow-fallback-to-shuriken-particles-for-lower-end-gpu-bound-systems.892072/)
- Output Particle Strip may not be supported on mobile, and older devices may lack compute shaders — [uhiyama-lab VFX Graph guide](https://uhiyama-lab.com/en/notes/unity/unity-vfx-graph-guide/)

### Inferences
- **What VFX Graph enables that Shuriken does not:** orders of magnitude more particles; arbitrary per-particle attributes and logic; GPU events (particle-to-particle spawning without a CPU round trip); depth-buffer and SDF collision; spawning from baked point caches; strips with custom logic; and sampling scene colour and depth.
- **What can be borrowed cheaply on the CPU:**
  - Spawning from a baked point list (a point-cache analogue; "emit from mesh surface" is just a precomputed weighted triangle list).
  - Simple analytic shape colliders (plane, sphere, box) instead of SDFs.
  - Death and birth events, which Shuriken sub-emitters already cover.
  - Depth-buffer collision and SDFs are GPU-centric and out of scope for GL 3.3 without compute. Depth-based soft particles, though, are a cheap fragment-shader feature that works on GL 3.3.
- **Complexity complaint:** the node graph has a steep authoring curve compared with Shuriken's Inspector modules (inferred from Unity's "modular, Inspector-based" vs "graph-view, highly customizable" framing; no direct user complaint quote was retrieved). A property-driven Roblox-like emitter sits firmly on the Shuriken side of this trade-off.

### Gaps
- Full-text pages for VFX Graph strips, GPU events, SDF collision and point caches were not fetched, only their existence from the TOC.
- The CPU readback limitation is implied by the docs, which list exposed properties and events but no per-particle access. An explicit statement or a forum thread from 2024–2026 was not retrieved.
- The Unity 6-era status of URP mobile and VFX Graph (whether it is still "preview" on mobile in the 17.x docs) is based on the 17.0 requirements page. Newer versions (17.3/17.4) may differ.
