# Unreal Niagara (and Cascade) vs Godot 4 particle systems: feature inventory for a CPU, property-driven emitter

Context for the reader: the target is a CPU-simulated emitter configured through a property panel and drawn as instanced quads on OpenGL 3.3 with no compute shaders, in an engine with a 240 Hz physics step and a variable render rate. Each section gives CPU-feasibility and cost notes under Inferences. Where an inference comes from general engine knowledge rather than a fetched source, it says so.

## Q1. Niagara structure: which concepts are essential and which are overkill for a property-panel emitter? What do users praise and complain about?

### Takeaway
Niagara's useful core is its execution split: emitter-level logic (spawn rate, bursts) runs once per emitter, and particle logic runs once at spawn and then per frame. A fixed, ordered list of modules sets values in each phase. Godot's ParticleProcessMaterial is close to a "pre-baked Niagara stack": one property sheet that covers Particle Spawn and Particle Update. Niagara's programmable layers are overkill for a property panel: scratch pads, custom modules, dynamic inputs, events, data interfaces and simulation stages. Epic's own UE 5.4+ "Lightweight/stateless emitter" is a fixed-function subset of about 17 modules, and it is the best model for what a property-driven emitter should expose.

### Cited Findings
**Hierarchy and stack**
- System = "a container for everything you will need to build that effect". Emitter = "where particles are generated". Modules "are processed sequentially from top to bottom", each reading data, doing math and writing it back. — [Epic: Overview of Niagara Effects](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-niagara-effects-for-unreal-engine)
- Emitter stack groups:
  - Emitter Spawn: runs once on the CPU when the emitter is created.
  - Emitter Update: runs every frame on the CPU, e.g. continuous spawning.
  - Particle Spawn: runs once per particle at birth (location, color, size).
  - Particle Update: "called per particle on each frame" (forces, size over life).
  - Event Handler: Generate/Listen events between emitters.
  - Render: one or more renderers.
  - Source: [Epic overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-niagara-effects-for-unreal-engine)
- Parameter types are Primitive, Enum, Struct and Data Interface. Module outputs accumulate in a temporary namespace, so modules writing the same attribute stack together. Data interfaces expose outside data such as meshes, skeletal meshes and audio, and can be written as plugins. — [Epic overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-niagara-effects-for-unreal-engine)
- Emitter inheritance: "Anything inherited can be overridden for a child emitter in a system". Inheritance covers spawning, lifetime, looping and bursts. — [Epic overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-niagara-effects-for-unreal-engine)
- Renderers: the docs name Sprite and Mesh and say "There are many different renderers to choose from". Ribbon is covered under Q6. The fetched pages did not document the Light and Component renderers (see Gaps).

**Sprite renderer properties worth copying (UE 5.x Python API)**
- Alignment: Unaligned or CustomAlignment, chosen automatically from whether a SpriteAlignment attribute is bound.
- Facing mode: FaceCamera, CustomFacingVector, or FacingCameraDistanceBlend with min and max blend distances.
- Sort settings: `sort_mode`, `sort_only_when_translucent`, `sort_order_hint` (draw order between emitters, lower draws first), and `custom_sorting_binding` (default `Particles.NormalizedAge`).
- `motion_vector_setting`.
- Flipbook settings: `sub_image_size` (columns × rows) and `sub_image_blend`, which blends the current and next frame using the fractional part of SubImageIndex.
- `pivot_in_uv_space`.
- Cutout: `cutout_texture`, `alpha_threshold` and `bounding_mode`, which generate a tighter polygon than a quad to reduce overdraw.
- `pixel_coverage_mode`, which compensates for sprites smaller than one pixel.
- `enable_camera_distance_culling` with min and max camera distance.
- `renderer_visibility` tag.
- `material_parameters`, which binds sim variables to a per-instance dynamic material.
- Source: [Epic Python API: NiagaraSpriteRendererProperties](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/NiagaraSpriteRendererProperties)

**Lightweight (stateless) emitters, UE 5.4+**
- Described as "optimized to minimize (and in some cases eliminate) the use of tick during the simulation". A system can mix stateless and stateful emitters, and a fully stateless system performs best. — [Epic: Lightweight Emitters Overview](https://dev.epicgames.com/documentation/unreal-engine/niagara-lightweight-emitters-overview?lang=en-US)
- Fixed-function module list, the complete set:
  - Acceleration Force, Add Velocity, Curl Noise / Noise Vector Field, Drag, Gravity Force
  - Initialize Particle, Initial Mesh Orientation, Rotate Around Point, Scale Color
  - Scale Mesh Size, Scale Mesh Size By Speed, Scale Sprite Size, Scale Sprite Size By Speed
  - Shape Location, Solve Velocities And Forces, Sprite Rotation Rate, Sub UV Animation
  - Source: [Epic](https://dev.epicgames.com/documentation/unreal-engine/niagara-lightweight-emitters-overview?lang=en-US)
- Users "cannot create custom modules, scratchpads, or use dynamic inputs". Features can be extended in C++.
- Benefits: "no scripts or particle information in memory", no per-emitter concurrent tick cost, lower Game Thread and Render Thread cost, and they "don't need to be compiled". — [Epic](https://dev.epicgames.com/documentation/unreal-engine/niagara-lightweight-emitters-overview?lang=en-US)
- Epic gives the motivation as reducing Niagara's memory and CPU cost and speeding up artist iteration. — [iRendering summary, Jan 2025](https://irendering.net/introduction-to-lightweight-emitters-for-niagara-in-unreal-engine/) (secondary source)
- In 5.6, the "Emitter Mode" menu option for converting between Standard and Lightweight appears to have been removed without a changelog entry. Lightweight GPU cost was initially not tracked in the Niagara Debugger. — [UE forum thread](https://forums.unrealengine.com/t/niagara-lightweight-emitters-emitter-mode-gone/2639263); [UE forum: debugger](https://forums.unrealengine.com/t/niagara-debugger-lightweight-stateless-emitters/2733793)

**Praise and complaints**
- An artist on the UE forums found Niagara "insufficient" for simple flipbook and mesh work compared with Cascade. They were "very disappointed" with the converter and had found no simple workflow comparable to Cascade's. A reply calls Niagara the right default despite lacking some Cascade features. — [UE forums: Cascade vs Niagara](https://forums.unrealengine.com/t/cascade-vs-niagara/629389)
- realtimevfx: Niagara is "a lot more open than Cascade", with many ways to optimize, "but ultimately it depends on how you use it". — [realtimevfx thread](https://realtimevfx.com/t/similar-vfx-tutorials-to-compare-cascade-to-niagara/11976)
- Epic's Fortnite team praised emitter inheritance and programmable workflows. They noted that Niagara produces many more assets, which requires naming and folder discipline. — [Epic tech blog: Niagara in Fortnite](https://www.unrealengine.com/tech-blog/how-epic-is-integrating-niagara-into-fortnite?lang=ja) (Japanese page, translated by the search tool)
- The Cascade-to-Niagara converter has gaps: Beam and AnimTrail renderers and ribbon UVs are unsupported or only partly supported. — [Epic: Cascade to Niagara Converter](https://dev.epicgames.com/documentation/unreal-engine/cascade-to-niagara-effects-converter-plugin-for-unreal-engine?lang=en-US)
- Niagara was introduced in beta in UE 4.20 as Cascade's eventual replacement, with the aim that artists could work without a programmer. — [Epic tech blog 2018](https://www.unrealengine.com/en-US/tech-blog/niagara-enhancements-and-sample-overview)

### Inferences
- **Essential for a property panel.** Map these Niagara phases onto fixed property groups:
  - Emitter Update: Rate, bursts, a loop or one-shot flag, and duration.
  - Particle Spawn: shape location, initial velocity and cone, lifetime, size, rotation and color ranges.
  - Particle Update: gravity or acceleration, drag, curl noise, size, color and alpha over life, rotation rate, and flipbook.
  - Render: sprite facing and alignment, sort mode, flipbook grid with blending, and pivot.
  - All of these are O(N) per-particle arithmetic and cheap on the CPU, roughly tens of ns per particle per module if written as structure-of-arrays loops. That figure is engineering judgement, not from a source.
- **Overkill for this engine:**
  - Scratch pads, custom modules and dynamic inputs; Lightweight emitters omit them on purpose.
  - Event handlers. Epic itself says attribute readers are cheaper than events.
  - General data interfaces.
  - Simulation stages, which are iterative grid and GPU passes.
  - Emitter inheritance. Templates or presets give most of its value.
- **A stateless design suits a 240 Hz engine.** If a particle's position comes from a closed-form function of age, it can be evaluated at the exact render time with no fixed-step jitter and no stored velocity. This only works with no collision and no history-dependent forces. Epic's docs do not explain the stateless math, so that part is an inference.
- **Two-tier option.** Offer a "simple/stateless" path for most effects and a "stateful" path that enables collision, sub-emitters and trails.
- A Mesh renderer is feasible on GL 3.3 with instancing. A Light renderer (dynamic point lights per particle) is feasible only for a few particles, and capped by the engine's light budget. A Component renderer (spawning engine objects per particle) is expensive and best skipped.

### Gaps
- No primary Epic page on the System Spawn/Update groups, simulation stages, scratch pad or the Light and Component renderers was retrievable. The dev.epicgames.com reference pages render client-side, and WebFetch returned empty content.
- No quantitative user survey on Niagara's learning curve; the evidence is anecdotal forum posts.

## Q2. Scalability for low-end hardware: Niagara (Effect Types, scalability, significance, pooling) and Godot (fixed_fps, amount_ratio, visibility AABB)

### Takeaway
Niagara scales in layers.
- **Effect Type.** A shared asset assigned to each system, holding per-quality-level and per-platform rules:
  - cull by distance
  - cap instances per Effect Type or per System
  - cull by visibility or global budget
  - scale down spawn counts
  - disable renderers or emitters
- **Significance handler.** Decides which instances survive a cap.
- **Component pooling.** Reuses system components instead of re-allocating them.
- **Fixed bounds.** Avoids the per-frame cost of recomputing dynamic bounds.

Godot's equivalents are far thinner: `fixed_fps` throttles simulation, `amount_ratio` scales emission at runtime without a restart, and `visibility_aabb` gates both simulation and rendering. Godot has no built-in global budget or instance cap.

### Cited Findings
**Niagara**
- Effect Types are reusable scalability assets: "it's usually best to start with setting up some core Effect Types for your project". Settings exist per quality level and per platform, with overrides at System, Emitter and Renderer level. They can cull instances, reduce spawn counts, disable expensive renderers, or disable emitters and systems. — [Epic: Scalability and Best Practices](https://dev.epicgames.com/documentation/en-us/unreal-engine/scalability-and-best-practices-for-niagara)
- NiagaraSystemScalabilitySettings fields — [Epic Python API](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/NiagaraSystemScalabilitySettings):
  - `cull_by_distance` with `max_distance`.
  - `cull_max_instance_count` with `max_instances`, counted per Effect Type and kept "using significance sorting if a handler exists".
  - `cull_per_system_max_instance_count` with `max_system_instances`.
  - `cull_proxy_mode` and `max_system_proxies`.
  - `budget_scaling`, which scales effects down based on global budget usage.
  - `visibility_culling`.
  - `platforms`.
- Without a significance handler, "instance count culling is applied at spawn time only": new effects over the cap are simply not spawned. With a handler, instances are sorted by significance and the N most significant are kept. A global budget culls when FX workload exceeds a set fraction, and budgets are configured through `FX.Budget` CVars. — [UE API docs (4.27–5.x)](https://docs.unrealengine.com/4.27/en-US/PythonAPI/class/NiagaraSystemScalabilitySettings.html)
- Pooling reuses components instead of allocating and garbage-collecting them, which helps frequently spawned effects. "Priming the pool can reduce runtime allocations, but can cause hitches if the asset is loaded dynamically." — [Epic best practices](https://dev.epicgames.com/documentation/en-us/unreal-engine/scalability-and-best-practices-for-niagara)
- Bounds:
  - "fixed bounds will be more performant than dynamic bounds since it requires less work".
  - Caveat: a small effect that travels far, such as a bullet trace, needs huge fixed bounds and so stays culling-relevant too often.
  - Dynamic bounds can be snapped or expanded through CVars to update less often.
  - Source: [Epic best practices](https://dev.epicgames.com/documentation/en-us/unreal-engine/scalability-and-best-practices-for-niagara)
- More best practices from the same page — [Epic best practices](https://dev.epicgames.com/documentation/en-us/unreal-engine/scalability-and-best-practices-for-niagara):
  - Each emitter has a fixed CPU overhead, so consolidate emitters.
  - Avoid warmup, because frames evaluate sequentially and cause hitches.
  - Spread large bursts over several frames.
  - Move shared work up the stack into Emitter or System scripts.
  - Prefer attribute readers over events.
  - Sorting is off by default; GPU sorting costs GPU time and index buffer memory.
- CPU vs GPU sim — [Epic best practices](https://dev.epicgames.com/documentation/en-us/unreal-engine/scalability-and-best-practices-for-niagara):
  - All sims have CPU overhead. Sim Target only moves the Particle Spawn, Particle Update and Simulation Stage scripts.
  - "In most cases GPU sims are more performant, and allow for a greater number of particles".
  - CPU suits small emitters: "A simulation of 1 particle can potentially take up the same resources as one with 64 particles" on the GPU.
  - GPU-limited platforms such as mobile may favor CPU sims.

**Godot**
- `amount_ratio` sets "the ratio of particles that should actually be emitted". Changing it does not restart the system, so it can vary emission over time. In contrast, changing `amount` restarts the system. — [Godot GPUParticles3D class ref](https://docs.godotengine.org/en/stable/classes/class_gpuparticles3d.html)
- Particles emitted per lifetime = `Amount` × `Amount Ratio`. Existing particles are unaffected when it changes. — [Godot 3D particle properties](https://docs.godotengine.org/en/stable/tutorials/3d/particles/properties.html)
- `amount_ratio` was added in 4.2, together with `interp_to_end`, which interpolates all particles toward the end of their life at once for graceful kill. — [Godot blog: state of particles, Oct 2023](https://godotengine.org/article/progress-report-state-of-particles/)
- Fixed FPS "limits how often the particle system is processed" and "can improve performance a lot, especially in scenes that make heavy use of particle collision". — [Godot properties tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/properties.html)
- Visibility AABB: the system renders while any part of the box is in view. Particles outside it "disappear instantly". Collision does not happen outside it, and a small box helps performance. — [Godot properties tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/properties.html). The editor's "Generate Visibility AABB…" tool builds it. — [Godot collision tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/collision.html)
- `preprocess` can be "very expensive" at high values. — [Godot GPUParticles3D class ref](https://docs.godotengine.org/en/stable/classes/class_gpuparticles3d.html)

### Inferences
- **Recommended minimum scalability kit for a Roblox-like engine:**
  - A fixed particle cap (`MaxParticles`).
  - A runtime `RateScale` (like `amount_ratio`) driven by a global quality setting.
  - Distance culling, and frustum culling against fixed or auto-computed bounds that pause simulation when off-screen.
  - A global particle budget that scales spawn rates when the total live count exceeds the budget. This gives "it just works" behaviour without per-asset tuning.
- **Significance sorting.** Distance to camera is the simplest significance metric and is cheap on the CPU: sort the emitters, not the particles.
- **Avoid warmup and preprocess hitches.** Run it in the background or amortize it, or use the stateless closed-form idea to jump to time T in O(N).
- **Pooling.** Reuse particle SoA buffers and emitter objects. This is trivial on the CPU and avoids allocation churn for bursty effects such as hits and explosions.
- **Off-screen emitters.** "Pause when off-screen" has a visible artifact when the camera turns back. Godot simply hides particles. A cheap middle ground is to keep aging particles and spawning counts but skip force integration; that is engineering judgement.

### Gaps
- Sub-properties of `budget_scaling` and `visibility_culling` (e.g. "max time without render") and the values of the cull-proxy enum were not documented on the fetched pages.
- Significance handler classes (e.g. distance and age variants) and their exact behaviour could not be confirmed from a primary source.
- Niagara per-quality "spawn count scale" field names were not retrieved.

## Q3. Godot fixed_fps + interpolate + fract_delta, relevant to a 240 Hz physics step with variable-rate rendering

### Takeaway
Godot simulates particles on its own clock (`fixed_fps`, default 30 in 4.3), independent of physics and render rates. `interpolate` blends between the last two sim states at render time, so a low-rate sim looks smooth. `fract_delta` (default on) gives each newly emitted particle a fractional time offset within the step, so particles emitted mid-step start at sub-step positions instead of bunching. Users have reported jitter from the interpolate/fixed-fps pairing, especially with time scaling and orbit velocity.

### Cited Findings
- `fixed_fps`: the system's frame rate is fixed; e.g. a value of 2 renders at 2 fps "without slowing the simulation itself". `interpolate` "makes the particle movement smoother" when `fixed_fps` is below the display refresh rate. `fract_delta`: "If true, results in fractional delta calculation which has a smoother particles display effect." — [Godot GPUParticles3D class ref](https://docs.godotengine.org/en/stable/classes/class_gpuparticles3d.html)
- Fixed FPS "does not change the speed at which particles move or rotate" (Speed Scale does that). Interpolate lets a 10 FPS system look close to 60 FPS. Fixed FPS also limits how often property changes, collision and attractors are processed. — [Godot properties tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/properties.html)
- The 2D tutorial says fract_delta's smoothness comes from higher accuracy and is most noticeable with high randomness or fast particles. It keeps each particle's motion consistent with its actual lifespan. — [Godot 2D particle tutorial (4.4)](https://docs.godotengine.org/de/4.4/tutorials/2d/particle_systems_2d.html); [GPUParticles2D 4.2 ref](https://docs.godotengine.org/en/4.2/classes/class_gpuparticles2d.html)
- The 4.3 class reference lists `fixed_fps` default 30 and `interpolate` default true. Issue #50824 reports particle jitter tied to interpolation: a maintainer suspected interpolation "doesn't take the time scale into account", and it is "extremely visible when using the orbit parameter". The 2021 workaround was to disable interpolation and set fixed_fps to 0 or 1000. — [Godot issue #50824](https://github.com/godotengine/godot/issues/50824); [GPUParticles3D 4.3 ref](https://docs.godotengine.org/es/4.3/classes/class_gpuparticles3d.html)
- Collision tunnelling: fast particles and thin colliders can tunnel. Raising Fixed FPS helps at a performance cost. — [Godot properties tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/properties.html)

### Inferences
- **Fit for this engine:**
  - Do not simulate particles at 240 Hz; that would be wasteful for visual-only particles.
  - Run the sim either at render rate with variable dt, or at a decoupled lower fixed rate (30–60 Hz) and interpolate previous/current positions at render time.
  - Interpolation doubles the per-particle position storage, which is cheap.
  - For rotation, interpolate angles carefully, or store angular velocity and evaluate it.
- **Implement the fract_delta concept regardless of tick strategy.** When k particles spawn within a step of length dt, give particle i a birth time of t0 + (i + u)/k · dt. Advance it by only its remaining fraction of the step, and interpolate its spawn position along the emitter's path during the step. This removes "clumped bands" in high-rate emitters on moving parents and costs almost nothing. Godot's docs describe the effect but not the formula, so the formula is my reconstruction.
- **Handle time scaling explicitly.** Godot's jitter bug (#50824) suggests the interpolation factor must use the sim's own time accumulator with time scale applied, not wall-clock dt.

### Gaps
- No primary-source description of Godot's exact fract_delta formula was found; the engine source was not retrieved.
- Whether #50824 is fully fixed in Godot 4.4–4.6 could not be confirmed.

## Q4. Godot turbulence, collision, sub-emitters and trails: costs and user complaints. Also, the ParticleProcessMaterial inventory and CPUParticles3D parity

### Takeaway
ParticleProcessMaterial is a large but flat property sheet, and a good template for a property panel. Godot's docs warn that its turbulence (3D noise per particle per frame) is expensive on the GPU. Collision runs against special GPU collider nodes (box, sphere, baked static SDF, runtime heightfield), never the physics world. Sub-emitters (constant, at start, at end, at collision) feed a separate fixed-capacity system. Trails use mesh skinning and are not supported in the Compatibility (GL) renderer. CPUParticles3D is frozen as a "lower-end, simpler alternative" with no turbulence, sub-emitters, trails, attractors, collision or `amount_ratio`.

### Cited Findings
**ParticleProcessMaterial inventory** (all items: [Godot ParticleProcessMaterial class ref](https://docs.godotengine.org/en/stable/classes/class_particleprocessmaterial.html))
- **Emission shapes**
  - Point, Sphere, Sphere Surface, Box, Points, Directed Points, Ring.
  - `emission_shape_offset` and `emission_shape_scale`.
  - Points shapes sample `emission_point_texture`, `emission_normal_texture` and `emission_color_texture`, with `emission_point_count`.
  - Ring shape uses `emission_ring_*`, i.e. axis, height, radius and inner radius.
- **Initial motion:** `direction`, `spread`, `flatness` (squashes the spread cone), `initial_velocity_min/max`, `inherit_velocity_ratio`, `lifetime_randomness`.
- **Velocity modifiers.** Each takes min/max plus an optional curve:
  - angular velocity (deg/s)
  - orbit velocity
  - radial velocity (relative to `velocity_pivot`)
  - directional velocity (per-axis)
  - `scale_over_velocity`
- **Accelerations:** gravity, plus linear, radial and tangential acceleration (min/max plus curve).
- **Damping:** `damping_min/max` and `damping_curve`. `particle_flag_damping_as_friction` switches to a speed-proportional model. `velocity_limit_curve` exists because animated velocities ignore damping.
- **Scale and rotation:**
  - `scale_min/max` and `scale_curve`; a CurveXYZ allows per-axis scale.
  - 3D scale and 3D rotation options.
  - `angle_min/max` and `angle_curve`.
  - Per-axis rotation velocity.
- **Color:**
  - `color`
  - `color_initial_ramp` (random pick at spawn)
  - `color_ramp` (over life)
  - `alpha_curve`, `emission_curve`
  - `hue_variation_min/max` and its curve
  - These need vertex color enabled in the draw material.
- **Animation (flipbook):** `anim_speed_min/max` with curve, and `anim_offset_min/max` with curve.
- **Turbulence:** enabled flag, noise strength, scale, speed (vector) and speed random, `turbulence_influence_min/max` with influence-over-life, `turbulence_initial_displacement_min/max`.
- **Collision:**
  - Modes: disabled, rigid, hide on contact.
  - `collision_bounce` and `collision_friction` (rigid only).
  - `collision_use_scale`.
  - 3D particles collide only with GPUParticlesCollision3D nodes.
- **Attractors:** `attractor_interaction_enabled`, effective only inside the visibility AABB.
- **Sub-emitters:**
  - `sub_emitter_mode`: disabled, constant, at end, at collision, or at start.
  - `sub_emitter_frequency`.
  - `sub_emitter_amount_at_start`, `_at_collision` and `_at_end`.
  - `sub_emitter_keep_velocity`.
- **Particle flags:** `align_y` (to velocity), `rotate_y`, `disable_z`, `damping_as_friction`, `inherit_emitter_scale`.
- 4.2 added or reworked: `velocity_pivot`, `directional_velocity`, `orbit_velocity` (now 3D), `radial_velocity`, `velocity_limit`, `scale_over_velocity`, separate alpha and emission over life. — [Godot blog, Oct 2023](https://godotengine.org/article/progress-report-state-of-particles/)

**GPUParticles3D node-level properties** (all items: [class ref](https://docs.godotengine.org/en/stable/classes/class_gpuparticles3d.html))
- Emission and timing: `amount`, `lifetime`, `one_shot`, `explosiveness` (0 = continuous, 1 = all at once), `randomness`, `preprocess`, `speed_scale`.
- Simulation rate: `fixed_fps`, `interpolate`, `fract_delta`.
- Space and culling: `local_coords`, `visibility_aabb`.
- Rendering: `draw_order`, `transform_align` (billboard, or align to velocity), up to 4 `draw_passes` (meshes).
- Trails: `trail_enabled`, `trail_lifetime`.
- Collision and sub-emitters: `collision_base_size`, and `sub_emitter` (a node path; that node stops emitting on its own).
- Local coords off means "Down will always be -Y in world space, regardless of the particle system's rotation". — [properties tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/properties.html)

**Turbulence**
- Uses a noise pattern to vary motion. It "modifies a particle's movement direction and speed, but it doesn't create any", so another force is needed.
- Doc warnings: "Turbulence makes use of 3D noise, which has a high performance cost on the GPU." "Only enable turbulence on a few particle systems on screen at most." It is not recommended on mobile/web.
- Source: [Godot turbulence tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/turbulence.html)
- Counterpoint: in turbulence-rework PR #64606 (superseded by #77154), a commenter said they and others "tested turbulence with millions of particles and never had performance problems". Another said rendering was the bigger bottleneck. Issue #83744: a GPUParticles2D slowdown first blamed on turbulence was actually the 2D `COLLISION` built-in auto-enabling screen-SDF generation. — [PR #64606](https://github.com/godotengine/godot/pull/64606); [Issue #83744](https://github.com/godotengine/godot/issues/83744)
- A docs commit suggests GPUParticlesAttractorVectorField3D with a NoiseTexture3D as a cheaper world-space alternative in 3D. — [commit mirror](https://git.objectionable.solutions/Sara/behaviour-tree-test/commit/f10759489dbbe852144abc9f7746cb676102adc5) (mirror of the Godot docs commit; secondary)

**Collision**
- GPU particles "can't see the physics world". Collider node types — [Godot collision tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/collision.html):
  - Box (extents) and Sphere (radius).
  - HeightField: rebuilds a heightmap from meshes inside its box. Update Mode is When Moved, or Always (costly). Follow Camera keeps it around the player. It cannot do "room over room".
  - SDF: baked in the editor, static at runtime. Thickness option. "The performance overhead is larger compared to height fields". SDF is not supported in the Compatibility renderer.
  - All colliders share a Cull Mask.
  - Collision needs the particle visibility AABB to overlap the collider.
  - Thin colliders tunnel; fix by thickening them or raising Fixed FPS.
- `collision_base_size` should be raised for small, fast particles to reduce tunnelling. — [properties tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/properties.html)

**Attractors**
- Three types: box, sphere, vector field. They pull or push particles. — [Godot 3D particles index](https://docs.godotengine.org/en/stable/tutorials/3d/particles/index.html)

**Sub-emitters**
- The parent's `Sub Emitter` points at a child system. Constant mode spawns at a frequency; At End and At Collision spawn a set amount. — [Godot sub-emitters tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/subemitters.html)
- The child's total live count is capped by its own `Amount`.
- Explosiveness has no effect on sub-emitters.
- A system used as a sub-emitter cannot emit by itself, and self-reference does not spawn.
- Chains can be any depth.

**Trails**
- `trail_enabled` gives "particle trails using a mesh skinning system". The material's `use_particle_trails` must also be on, and `trail_lifetime` sets how many seconds the trail represents. — [class ref](https://docs.godotengine.org/en/stable/classes/class_gpuparticles3d.html)
- Mesh types — [Godot trails tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/trails.html):
  - RibbonTrailMesh: Flat or Cross shape, plus Size.
  - TubeTrailMesh: Radius and Radial Steps; needs Transform Align = Y to Velocity or it flattens.
  - Shared shape controls: Sections × Section Length (bend smoothness and length), Section Segments or Rings, and a width Curve from head (0) to tail (1).
- Trails are "only supported in the Forward+ and Mobile renderers, not Compatibility".

**CPUParticles3D parity**
- Features lost on conversion include multiple draw passes, turbulence, sub-emitters, trails, attractors, collision, Amount Ratio, Interp to End and Velocity Limit. — [Godot docs (4.5) via search summary](https://docs.godotengine.org/pt-br/4.x/tutorials/3d/particles/index.html)
- "From Godot 4.2 onwards, new features for CPUParticles will not be accepted unless they have a counterpart in GPUParticles." CPUParticles is "a lower-end, simpler alternative". Reason: "high maintenance cost and bug surface". A CPU-to-GPU converter was added. — [Godot blog, Oct 2023](https://godotengine.org/article/progress-report-state-of-particles/); [GameFromScratch summary](https://gamefromscratch.com/the-future-of-godot-particles/)
- CPUParticles3D "works on a wider range of hardware, including older devices and mobile". GPUParticles3D can do "hundreds of thousands" of particles on modern hardware. — [Godot 3D particles index](https://docs.godotengine.org/en/stable/tutorials/3d/particles/index.html)

### Inferences (CPU feasibility and cost)
- **Cheap on the CPU; include all:**
  - every emission shape. Ring, sphere surface, box and point are trivial. Points-from-mesh needs a pre-sampled point list, which is an editor-side bake.
  - direction/spread/flatness
  - all the velocity types (orbit, radial, tangential, directional), each a few vector ops
  - gravity, damping, damping-as-friction, velocity limit
  - curves over life: bake to a 32–64 entry LUT and sample, about one lerp per curve per particle
  - hue variation
  - flipbook with frame blending
  - `inherit_velocity_ratio`
  - `local_coords`
- **Turbulence.** True 3D simplex or curl noise per particle per tick is the most expensive CPU item, roughly 100–300 ns per particle per tick for 3D curl from three noise evaluations. That is an estimate, not sourced. Cheaper options:
  - Sample a small precomputed 3D vector-field texture (e.g. 16³ or 32³) with trilinear filtering on the CPU. This is Godot's own suggested vector-field alternative.
  - Update turbulence at a lower rate than integration.
  - Cap the number of turbulent emitters.
- **Collision.** The engine already has a physics engine (Box3D). Two options:
  - Ground-plane or heightfield collision. Terrain heightfield lookup is O(1) and the cheapest useful case.
  - Raycasts or sphere-casts per particle against physics. Expensive; cap them, or limit to "N rays per frame round-robin".
  - Godot's design shows users accept "approximate" particle collision. "Hide on contact" plus "spawn sub-emitter on contact" covers most gameplay VFX (sparks, rain splashes) without full bounce physics.
- **Sub-emitters.** Easy on the CPU: append a spawn request to a queue that the child emitter drains the next tick. Godot's "child capped by its own Amount" behaviour is a sensible, budget-safe rule.
- **Trails.** Godot skins a mesh on the GPU, and Godot's own GL Compatibility renderer lacks it. On GL 3.3 without compute, a CPU-built ribbon (a dynamic VBO of quads from position history) is the practical route; see Q6.
- **Attractors.** Point, sphere or box attractors are cheap: per particle, per attractor, a distance check and one force. Cap the attractor count per emitter.

### Gaps
- The exact noise algorithm in Godot's turbulence (simplex or value noise, and whether it uses curl) is not stated in the docs. Search found no source naming curl noise.
- No numeric benchmark of Godot turbulence or collision cost was found.

## Q5. Sorting, soft particles / depth fade, lit particles and motion vectors

### Takeaway
Both engines sort per emitter.
- Niagara: none, ViewDepth, ViewDistance, or a custom attribute ascending/descending (default NormalizedAge). Sorting can be limited to translucent materials, and `sort_order_hint` orders whole emitters.
- Godot draw orders: Index (emission order), Lifetime, Reverse Lifetime, View Depth. Only Index order supports motion vectors for TAA.
- Soft particles, depth fade and lighting are material features in both engines, not emitter features.

### Cited Findings
- Niagara sort modes:
  - None.
  - ViewDepth: depth to the camera near plane.
  - ViewDistance: distance to the camera origin.
  - CustomAscending/CustomDescending: by `custom_sorting_binding`, default `Particles.NormalizedAge`.
  - Source: [Epic NiagaraSortMode](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/NiagaraSortMode); [Sprite renderer props](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/NiagaraSpriteRendererProperties)
- Related Niagara settings:
  - `sort_only_when_translucent`, `sort_precision`, `sort_order_hint` (lower draws first).
  - `motion_vector_setting`, a hint for generating velocity vectors.
  - `gpu_translucent_latency`, which chooses current- or previous-frame sim data so translucents match opaque draws.
  - `pixel_coverage_mode`, which compensates for sub-pixel sprites; disabling it can flicker under TSR.
  - Source: [Sprite renderer props](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/NiagaraSpriteRendererProperties)
- Niagara sorting is disabled by default. GPU sorting adds GPU overhead and index memory, and there are CVars to force CPU sorting below a threshold. — [Epic best practices](https://dev.epicgames.com/documentation/en-us/unreal-engine/scalability-and-best-practices-for-niagara)
- Godot `draw_order`: Index, Lifetime, Reverse Lifetime, or View Depth. "Index order is the only option that supports motion vectors for TAA." — [GPUParticles3D class ref](https://docs.godotengine.org/en/stable/classes/class_gpuparticles3d.html); [properties tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/properties.html)
- Godot color and hue settings only show if the draw material uses vertex color (`vertex_color_use_as_albedo`, or `ALBEDO *= COLOR.rgb`). — [ParticleProcessMaterial ref](https://docs.godotengine.org/en/stable/classes/class_particleprocessmaterial.html)

### Inferences
- **Sorting.** For a CPU sim, view-depth sorting is an O(N log N) CPU sort per emitter per frame. Mitigations:
  - Use radix or insertion sort with temporal coherence (nearly sorted from the previous frame).
  - Sort only for alpha-blended materials, as with Niagara's `sort_only_when_translucent`.
  - Skip sorting for additive blending, which is order-independent.
  - Sort emitters as a whole by distance, which also handles emitter-vs-emitter order.
  - Age sorting is free if particles stay in spawn order in a ring buffer, the equivalent of Godot's "Index" mode.
- **Soft particles / depth fade.** On GL 3.3 this needs the scene depth texture readable in the particle fragment shader: render opaque geometry first, then bind a depth copy. It is cheap and standard. The fade distance should be a renderer property. These are general rendering practice, not from a source.
- **Lit particles.** Simplest is unlit plus per-emitter ambient tint. Next is per-vertex lighting from the engine's light list evaluated at the particle center. Full per-pixel normal-mapped lighting is costly with overdraw. For low-end hardware, per-particle or per-vertex lighting is the right default.
- **Motion vectors.** Only needed if the engine has TAA or motion blur. Godot's limit shows it requires a stable particle index between frames; a ring buffer in spawn order gives that. Interpolation history (Q3) can double as the previous-frame position.

### Gaps
- No source confirmed a Niagara soft-particle or depth-fade setting on the renderer. It is believed to live in the material, as a DepthFade node, but this was not verified from fetched sources.
- Enum values of Niagara's `motion_vector_setting` were not retrieved.
- Godot's soft-particle option (BaseMaterial3D "Proximity Fade") was not fetched in this session.

## Q6. Ribbons and trails: how are ribbon renderers modelled?

### Takeaway
Niagara builds ribbons from ordinary particles.
- Particles sharing a **RibbonID** attribute form one ribbon.
- Within a ribbon they are ordered by **RibbonLinkOrder**, usually age or spawn order.
- The renderer builds a strip through them, with these controls:
  - Facing mode.
  - Width and twist as particle attributes.
  - UV distribution: ScaledUniformly, ScaledUsingRibbonSegmentLength, TiledOverRibbonLength, TiledFromStartOverRibbonLength.
  - Tessellation: disabled, custom factor 1–16, or screen-space adaptive.

Godot instead attaches per-particle history trails, skinned onto a RibbonTrailMesh or TubeTrailMesh (sections, segments, width curve).

### Cited Findings
- The ribbon renderer has bindings selecting the attribute for RibbonID and for RibbonLinkOrder. The 5.x Python names are `ribbon_id_binding` and `ribbon_link_order_binding`. — [UE 4.26 API: UNiagaraRibbonRendererProperties](https://docs.unrealengine.com/4.26/API/Plugins/Niagara/UNiagaraRibbonRendererProperties/index.html); [UE 5.2 Python API](https://docs.unrealengine.com/5.2/en-US/PythonAPI/class/NiagaraRibbonRendererProperties.html)
- UV distribution enum: ScaledUniformly, ScaledUsingRibbonSegmentLength, TiledOverRibbonLength, TiledFromStartOverRibbonLength. The tiled modes repeat by a TilingLength; the scaled modes stretch. — [UE 4.27 API: ENiagaraRibbonUVDistributionMode](https://docs.unrealengine.com/4.27/en-US/API/Plugins/Niagara/ENiagaraRibbonUVDistributionMode/index.html)
- Tessellation mode: custom or disabled. Custom factor ranges from 1 to 16. Screen-space mode adapts the factor to the ribbon's screen percentage. — [UE 4.26 API](https://docs.unrealengine.com/4.26/API/Plugins/Niagara/UNiagaraRibbonRendererProperties/index.html)
- Cascade's Beam and AnimTrail renderers and ribbon UVs only partly convert to Niagara. — [Epic converter doc](https://dev.epicgames.com/documentation/unreal-engine/cascade-to-niagara-effects-converter-plugin-for-unreal-engine?lang=en-US)
- Godot trails — [Godot trails tutorial](https://docs.godotengine.org/en/stable/tutorials/3d/particles/trails.html); [class ref](https://docs.godotengine.org/en/stable/classes/class_gpuparticles3d.html):
  - Implemented by mesh skinning with `trail_lifetime` in seconds.
  - RibbonTrailMesh comes in Flat or Cross shape. TubeTrailMesh has radial steps.
  - Sections × Section Length set smoothness and length; a Curve sets width from head to tail.
  - Not supported in Compatibility.

### Inferences
- **Recommended CPU ribbon model, Niagara-style:**
  - Each "trail" emitter (or each particle with a trail) keeps a ring buffer of points: position, width, color and age.
  - Each frame, build a camera-facing strip on the CPU. Each segment's side vector is cross(tangent, toCamera), normalized and scaled by width.
  - Upload to a dynamic VBO with orphaning: `glBufferData(NULL)` then `glBufferSubData`.
  - Cost is O(points) on the CPU plus one draw per material; very feasible on GL 3.3.
- **Two use cases, one data model.** "Ribbon from a stream of particles" (RibbonID and link order) and "trail behind each particle" (Godot) can share the same point-list model:
  - Mode A: the emitter's own particles linked in spawn order, giving one ribbon (sword swipes, beams).
  - Mode B: each particle records a short history (bullet tracers, sparks).
- **Expose few properties:**
  - Width, plus width-over-length as a LUT.
  - Color or transparency over length.
  - Texture mode: Stretch, or Tile with a tile length. These map to Niagara's ScaledUniformly and TiledOverRibbonLength.
  - Facing: camera, or a custom/normal axis.
  - Min segment length and max points, as a budget.
- **Optional smoothing.** Catmull-Rom subdivision gives Niagara-like tessellation. Make it a small integer 1–4 to bound cost; Niagara's 1–16 range is for the GPU.

### Gaps
- No retrieved docstrings for Niagara ribbon facing modes, ribbon shape options (plane, multi-plane, tube, custom vertices), twist, draw direction or GPU-ribbon init. The 5.x Python API page redirected to a 404, and other pages rendered empty.
- Light renderer and Component renderer details (Niagara) were not retrievable; see Q1 gaps.
- Legacy Cascade details beyond converter gaps (e.g. its modules, LOD levels, Required/Spawn/Lifetime module structure) were not researched in depth for lack of tool budget. Cascade is deprecated in favour of Niagara.
