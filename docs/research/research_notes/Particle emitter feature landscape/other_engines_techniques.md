# Particle systems in other engines, and techniques for modern-looking CPU particles on low-end GPUs

Scope: (1) how other engines and creator platforms shape their particle tools, from approachable to pro; (2) a catalogue of techniques for a CPU-simulated, GL 3.3 / GLES 3 instanced-quad renderer (deferred opaque, forward transparency, bloom).

Note on sourcing: the research budget was about 17 tool calls. Several primary pages (Valve wiki, Roblox reference details, Effekseer docs) were only partly reachable, so some engine details are thin and are listed under Gaps. Items marked "Inference" are engineering judgement, not sourced facts.

---

## Q1. Engine and tool feature summaries: approachable vs pro designs

### Takeaway
There are two families of design. One is a single "fat emitter" component whose properties are fixed and keyable over the particle's life and the emitter's life (Defold, Roblox ParticleEmitter, Effekseer's per-node parameters, Unity Shuriken). It is easy for non-experts. The other is a modular stack or graph of functions (Source 2 emitters/initializers/operators/renderers, s&box, Niagara/UEFN, PopcornFX, Bevy Hanabi expressions). It is powerful but expert-oriented. Creator platforms close the gap with locked presets and templates (UEFN) or curve editors on a fixed property set (Defold).

### Cited Findings

**Defold ParticleFX (approachable fat emitter with curves)**
- Emitter shapes: Circle, 2D Cone, Box, Sphere, Cone. Play mode is Once or Loop. Timing has Duration, Start Delay, and Start Offset (prewarm). — [Defold manual](https://defold.com/manuals/particlefx/)
- Two curve domains. Keys over the emitter's duration: Spawn Rate, Emitter Size, Particle Life Time, Initial Speed/Size/RGBA/Rotation/Stretch/Angular Velocity. Keys over the particle's lifetime: Life Scale, Life RGBA, Life Rotation, Life Stretch, Life Angular Velocity. Every keyable property has a value plus a per-particle random spread. Curves are edited only in the Curve Editor. — [Defold manual](https://defold.com/manuals/particlefx/)
- Modifiers: Acceleration, Drag, Radial (attract/repel), Vortex, each with Magnitude (and Max Distance for Radial/Vortex). A modifier on the effect root affects all emitters; a modifier under one emitter affects only that emitter. — [Defold manual](https://defold.com/manuals/particlefx/)
- Emission space is World or Emitter. Inherit Velocity works only in World space. Orientation modes: Default, Initial Direction, Movement Direction. There is a Stretch With Velocity option. — [Defold manual](https://defold.com/manuals/particlefx/)
- Blend modes: Alpha, Add, Multiply, Screen. Max Particle Count caps each emitter. A `tint` constant can be set at runtime. Effects keep emitting after their game object is deleted. — [Defold manual](https://defold.com/manuals/particlefx/)

**Roblox ParticleEmitter (approachable fat emitter; the reference platform for this engine)**
- FlipbookLayout is an enum: None, Grid2x2, Grid4x4, Grid8x8. FlipbookFramerate sets frames per second. — [Roblox ParticleEmitter reference](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter#FlipbookLayout)
- LightInfluence (number) and LightEmission exist. LightEmission scales emitted light "when LightInfluence is 0", which implies LightInfluence blends between lit and unlit. Squash is listed as a property, but no description was retrieved. — [Roblox ParticleEmitter reference](https://create.roblox.com/docs/en-us/reference/engine/classes/ParticleEmitter)

**Effekseer (approachable standalone editor, many runtimes)**
- Free and open source. Runs on Windows, macOS, Linux, iOS, Android, and Emscripten. Runtimes for DX9/11/12, OpenGL, Metal, Vulkan, Unity, UE4, Cocos2d-x, and WebGL. Latest listed release is 1.80.7. — [Effekseer site](https://effekseer.github.io/en/)
- Version 1.5 added a node-based material editor, dynamic parameters and expressions for runtime control, and a turbulence function. — [GameFromScratch on Effekseer 1.5](https://gamefromscratch.com/effekseer-1-5/)
- Its stated goal is to let "the designer, not the programmer" own effect editing, with real-time preview. Godot support was added later. — [GameFromScratch](https://gamefromscratch.com/effekseer-1-5/); [Godot support](https://gamefromscratch.com/effekseer-adds-godot-engine-support/)
- A three.js forum commenter calls Effekseer a benchmark for node- and curve-driven pipelines with a standalone editor. This is opinion. — [three.js forum](https://discourse.threejs.org/t/experimenting-with-stylized-particle-vfx-in-three-js-using-nixiefx/93712)

**Source 2 particle system (pro, modular function stack)**
- Effects are built from Functions in these categories: Renderer, Operator, Initializer, Emitter, Force Generator, Constraints, Children. Each Function has Properties (scalars, vec3s, booleans) in a property panel. — [Valve Developer Community: HL:Alyx Particles](https://developer.valvesoftware.com/wiki/Half-Life:_Alyx_Workshop_Tools/Particles)
- The renderer is usually a sprite but can be a ribbon or a 3D model. — [VDC: Renderer](https://developer.valvesoftware.com/wiki/Half-Life:_Alyx_Workshop_Tools/Particles/Renderer)
- The Particle Editor was written for Dota 2. Valve says it still applies to Alyx, CS2, and other newer Source 2 games. — [VDC: Particle Editor (Source 2)](https://developer.valvesoftware.com/wiki/Particle_Editor_(Source_2))

**s&box (Source 2-derived, same model)**
- Emitters decide how many particles are created and when (for example Continuous Emitter with Duration Limit, where 0 means forever, plus Start Time and Emit Rate). Initializers set spawn values, either constant or random range (for example Set Float, Position Along Ring). Operators run over the particle's life (Alpha Fade and Decay, Lifespan Decay, Movement Basic with gravity). Renderers draw them (Sprite Renderer: texture, color blend, radius scale, lighting, UVs, animation, orientation). — [s&box wiki: Emitters/Initializers/Operators](https://wiki.facepunch.com/sbox/Understanding_Emmiters_Initializers_&_Operators); [s&box wiki: first particle effect](https://wiki.facepunch.com/sbox/Creating_your_first_particle_effect)
- "Collision via traces" lets particles stick or bounce. The wiki warns it is expensive and should be used sparingly. A particle without a renderer is invisible. These are community-edited pages. — [s&box wiki](https://wiki.facepunch.com/sbox/Understanding_Emmiters_Initializers_&_Operators)

**Bevy Hanabi (GPU compute, expression-based)**
- GPU compute particle system. Spawners: rate, once, burst, with randomized parameters. Init: position or velocity over shapes (cube, circle, sphere, cone, plane, mesh). Update: Euler integration, constant/radial/tangent acceleration, force fields, linear drag. Collision against planes, cubes, spheres, and the depth buffer. Kill boxes. Size and color over lifetime. — [bevy_hanabi README](https://github.com/djeedai/bevy_hanabi)
- An expression `Module` API customizes attributes beyond the fixed modifiers. Render modes: textured quads, meshes, velocity stretch, trails and ribbons. Orientation: camera-facing, fixed direction, along velocity, screen-space size. Supports HDR and bloom. — [bevy_hanabi README](https://github.com/djeedai/bevy_hanabi)
- Self-described as "under heavy development" and lacking polish. Each effect has a fixed maximum particle count. On the web it needs WebGPU; WebGL is not mentioned. — [bevy_hanabi README](https://github.com/djeedai/bevy_hanabi)

**PopcornFX / O3DE**
- PopcornFX has a node-based editor with in-editor CPU and GPU simulation matching the runtimes, plus debugging/profiling tools and a C++ SDK. This comes from an older Lumberyard-era hands-on, so it may be dated. — [GameFromScratch hands-on](https://gamefromscratch.com/popcornfx-particle-creation-software-and-run-time-hands-on/)
- Ships integrations for Unreal, Unity, and O3DE plus a runtime SDK. — [Flathub listing](https://flathub.org/apps/com.popcornfx.PopcornFXEditor)
- O3DE 26.05 added its own "Open Particle System" gem (preview): runtime integrated with the Atom renderer, a preview editor, sprite/ribbon/mesh types, and modules such as color, force, speed. — [Phoronix](https://www.phoronix.com/news/O3DE-26.04-Released)

**UEFN (Niagara for creators)**
- Custom VFX are made with Niagara. Effects are driven through devices: attached to a Prop Mover, fired by Trigger devices, sequenced with Pulse Triggers. Custom VFX can replace the native effects of Fortnite devices. — [Epic: Effects and particle systems in UEFN](https://dev.epicgames.com/documentation/en-us/uefn/effects-and-particle-systems-in-unreal-editor-for-fortnite)
- Built-in effects are locked: you can place them but not open or edit them. — [Epic UEFN docs](https://dev.epicgames.com/documentation/en-us/uefn/effects-and-particle-systems-in-unreal-editor-for-fortnite)
- Devices plus Verse are the building blocks for game logic. — [VFX Apprentice](https://www.vfxapprentice.com/blog/unreal-editor-fortnite-uefn)

**Unity VFX Graph (pro reference point for six-way lighting)**: see Q2.

### Inferences
- The Roblox / Defold / Effekseer model is the right baseline for an approachable engine: one emitter object, properties as constant / random range / curve (NumberSequence-style), a few modifiers, and a flipbook enum. Power users can be served later by additive "modules" on the emitter (Source 2-style operator list) without exposing a graph.
- Defold splits curves into "over emitter life" and "over particle life". That split is cheap to support and covers most authoring needs.
- UEFN's pattern (locked library presets plus full custom authoring) argues for shipping a preset library (fire, smoke, sparks, dust, magic) that users can duplicate and modify.

### Gaps
- Could not retrieve: Core Games, Rec Room, or Horizon Worlds particle docs; Stride particle docs; full Effekseer node list (sprite/ribbon/ring/track/model) or its F-curve/easing parameter modes; Roblox Squash and LightInfluence definitions; Source 2 initializer and operator lists; UEFN per-island particle budgets.

---

## Q2. Techniques catalogue for CPU sim + GL 3.3 instanced quads

### Takeaway
For a fill-rate-bound low-end GPU, these give the most quality for their cost:
- premultiplied alpha, so additive and alpha particles share one sorted pass;
- soft particles (depth fade, one depth texture fetch);
- flipbook frame blending (motion-vector blending where assets allow);
- HDR emissive feeding the existing bloom;
- cheap lighting (ambient plus directional per particle, or six-way lightmaps for smoke).

Overdraw is the dominant cost. Trimmed quads, size caps, and off-screen low-resolution rendering address it.

### Cited Findings

**Soft particles / depth fade**
- NVIDIA's 2007 Soft Particles whitepaper adds "a transition zone where the sprite fades-out against the background scene" instead of a hard depth-test intersection. — [NVIDIA SoftParticles whitepaper](https://developer.download.nvidia.com/whitepapers/2007/SDK10/SoftParticles_hi.pdf)
- The same idea works with a deferred renderer. The scene depth is already available as a texture, so the forward transparency pass samples it. (Inference.)

**Off-screen / low-resolution particle buffer**
- GPU Gems 3 chapter 23 renders particles to an off-screen target whose size is a fraction of the frame buffer, with a matching downsampled depth buffer for occlusion, then composites. — [GPU Gems 3 ch.23, High-Speed Off-Screen Particles](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles)

**Overdraw reduction / quad trimming**
- Persson (Humus): shrinking quads to each texture's alpha bounding box alone saved over 30%. Fitted polygons enclose the opaque "meat" of the texture. Going past about 8 vertices is "usually not really worth it". Just Cause 2 used 4 vertices for clouds and 8 for GPU-generated particle effects. For CPU-generated particles, extra vertices cost more. — [Humus blog](https://www.humus.name/index.php?ID=266); [Persson, Graphics Gems for Games (PDF)](https://www.humus.name/Articles/Persson_GraphicsGemsForGames.pdf); [Particle trimmer tool](https://humus.name/index.php?page=Cool)
- Unreal exposes the same trade-off for SubUV particles: more bounding vertices give less overdraw but more triangle overhead. — [Unreal ESubUVBoundingVertexCount](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/ESubUVBoundingVertexCount?lang=en-US)
- PowerVR: stacked alpha-blended quads are a leading cause of overdraw, even with few but large particles. Recommendations: measure overdraw first, reduce counts, use shaped meshes that fit the texture's opaque edge, and disable anisotropic filtering for distant particles. — [Imagination: Optimal Particle Rendering on PowerVR](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/optimal-particle-rendering-on-powervr.html)
- With instancing, a trimmed polygon is a per-texture vertex buffer of 4–8 vertices shared by all instances. Unlike the CPU-generated case Persson warns about, the per-instance cost does not rise. (Inference.)

**Premultiplied alpha: additive and alpha blend in one pass**
- Blend func (ONE, ONE_MINUS_SRC_ALPHA) with premultiplied color. Alpha = 0 gives pure additive (fire); alpha > 0 with RGB premultiplied gives occluding smoke. Both can share one draw call or batch with no state change. — [gamedev.stackexchange answer](https://backiee.wasmer.app/http_gamedev_stackexchange_com/a/23009); [Anisoptera Games: HDR effects](https://www.anisopteragames.com/hdr-effects/); [Wicked Engine: Which blend state for me?](https://wickedengine.net/2017/10/which-blend-state-for-me)
- Caveats: lowering alpha makes a particle more additive rather than more transparent unless RGB is scaled too. Independent "additiveness" needs a second scalar, for example out = (rgb·a, a·β) where β = 0 is fully additive. Textures must be authored or converted to premultiplied form. — [polycount discussion](https://polycount.com/discussion/125927/shader); [Unity forum](https://discussions.unity.com/t/stop-particle-color-mixing/655234); [realtimevfx small tips](https://realtimevfx.com/t/small-vfx-tips/923)

**Flipbooks: frame blending and motion vectors**
- Plain frame blending cross-fades frame N into N+1, which can ghost silhouettes. Motion-vector ("optical flow") blending warps frame N toward N+1 using a stored flow map. It was developed by Guerrilla for Killzone 2. — [realtimevfx: Motion vectors for animated textures](https://realtimevfx.com/t/motion-vectors-for-animated-textures-how-do-you-generate-them/575?page=2)
- Anecdotal gains: a 16-frame flipbook looks as smooth as a 36-frame one, and flipbook lifetimes can be stretched "over 100x". The motion texture can be half resolution or lower. Strength must be tuned: too low looks like a cross-fade, too high shows frames sliding past each other. Sources for motion vectors: velocity export from simulations, or optical-flow tools (Slate, Twixtor, TFlow). — [realtimevfx thread](https://realtimevfx.com/t/motion-vectors-for-animated-textures-how-do-you-generate-them/575?page=2); [realtimevfx: Increasing flipbook duration](https://realtimevfx.com/t/increasing-flipbook-based-effects-duration/27592)
- Unity VFX Graph's Flipbook Player has a motion-blend UV mode. — [Unity Flipbook Player block](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@10.3/manual/Block-FlipbookPlayer.html)

**Lit particles: six-way lightmaps**
- Six-way lighting pre-bakes lighting from six directions (top, bottom, left, right, front, back) into two RGBA textures. The shader blends them at runtime from scene lights. It supports dynamic lights and indirect light from probes and ambient, but not reflection-probe specular. — [Unity six-way lighting docs](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.0/manual/six-way-lighting.html)
- Unity says it is still a texture sheet, so it suits background and ornamental smoke, steam, and explosions. Colored smoke can be done with per-channel absorption (more realistic) or a simple multiply (cheapest). The 2023 launch was HDRP-only; current docs list URP and HDRP. — [Unity blog: realistic smoke with 6-way lighting](https://unity.com/blog/engine-platform/realistic-smoke-with-6-way-lighting-in-vfx-graph); [Unity forum announcement](https://discussions.unity.com/t/vfx-graph-introducing-six-way-lighting-for-smoke-dust-and-clouds-vfx/906564)
- Roblox exposes a LightInfluence scalar that blends unlit and lit, plus LightEmission. This is a creator-friendly control for the cheap per-particle lighting path. — [Roblox ParticleEmitter reference](https://create.roblox.com/docs/en-us/reference/engine/classes/ParticleEmitter)

**Camera-facing modes**
- Defold: Default, Initial Direction, Movement Direction, and Stretch With Velocity. — [Defold manual](https://defold.com/manuals/particlefx/)
- Bevy Hanabi: camera billboard, constant direction, along velocity, screen-space size, velocity stretch. — [bevy_hanabi README](https://github.com/djeedai/bevy_hanabi)
- Common set to implement, all done in the instanced vertex shader from per-instance position, velocity, and size: billboard (view-aligned), velocity-stretched, axis-locked (cylindrical), world-oriented. (Inference.)

**Collision (CPU)**
- Hanabi supports plane, cube, and sphere colliders and depth-buffer collision. Depth-buffer collision depends on GPU sim. — [bevy_hanabi README](https://github.com/djeedai/bevy_hanabi)
- s&box trace-based collision is flagged as expensive. — [s&box wiki](https://wiki.facepunch.com/sbox/Understanding_Emmiters_Initializers_&_Operators)
- CPU alternatives (Inference): analytic planes, spheres, and boxes; terrain heightfield lookup (one bilinear sample per particle, very cheap); and a per-frame raycast budget, for example N rays round-robin across particles with cached hit planes reused between casts.

**Curl noise**
- Curl noise (Bridson et al. 2007) gives divergence-free, boundary-respecting turbulent velocity fields from Perlin noise. Its value is speed and animator control compared with a fluid solver. — [SIGGRAPH archive: Curl-noise for procedural fluid flow](https://history.siggraph.org/?p=115355); [Cornell CS5643 assignment](https://cs.cornell.edu/courses/cs5643/2023sp/assignments/A1.html); [Notch: Curl Noise Fluid Affector](https://manual.notch.one/2026.1/en/docs/reference/nodes/particles/affectors/curl-noise-fluid-affector/)
- Cost model (Inference, no benchmark found): finite differences need about 6 potential evaluations of 3D noise per particle per octave; analytic-derivative simplex noise reduces this. Cheap CPU options: evaluate at a lower rate (every 2–4 frames, or a cached coarse 3D velocity grid sampled trilinearly), or use one octave.

**Budgets and LOD**
- Blog guidance (not vendor data) for 60 FPS: mobile 50–200 particles per effect and up to about 2,000 on screen; mid-range desktop 500–2,000 per effect and up to about 10,000 total. Recommends a global particle manager that throttles emission, shortens lifetimes, or culls low-priority systems over budget, and distance LOD (halve emission and max count at medium range, disable past a far threshold). Screen coverage matters more than count on fill-rate-bound mobile GPUs. — [Bugnet: particle overdraw on mobile](https://bugnet.io/blog/how-to-fix-particle-overdraw-killing-mobile-performance); [Bugnet: debugging particle performance](https://bugnet.io/blog/debugging-particle-system-performance-issues)
- Forum anecdotes conflict: one Solar2D team keeps 150–200 on screen; a Construct answer says under 50 on mobile. These are 2D-engine contexts and not comparable to an instanced 3D renderer. — [Solar2D forum](https://forums.solar2d.com/t/how-many-particles-is-too-much/352924); [Construct forum](https://www.construct.net/forum/construct-2/how-do-i-18/particles-vs-particle-sprites-92623)
- Per-effect hard caps are universal: Defold Max Particle Count per emitter; Hanabi fixed capacity per effect. — [Defold](https://defold.com/manuals/particlefx/); [bevy_hanabi](https://github.com/djeedai/bevy_hanabi)

### Inferences (techniques with no source retrieved in this pass; standard practice)
- **SoA + SIMD sim**: store position x/y/z, velocity, age, and lifetime as separate float arrays in particle blocks. Integrate 4 or 8 particles per SSE/AVX/NEON op. Kill by swap-with-last to keep arrays dense. Write the instance buffer (pos, size, rotation, color, frame) straight from SoA into a persistently mapped or orphaned VBO (glBufferData(NULL) then glBufferSubData on GL 3.3 / GLES 3).
- **Sorting**: sort emitters back to front by bounds center (cheap), and sort particles within an emitter only when it uses non-additive blending. With premultiplied blending, additive-heavy effects tolerate missorting. A full global per-particle sort across emitters gives correct interleaving but costs O(n log n) per frame. A radix sort on quantized view depth is the usual compromise. Sorting against transparent world geometry is unsolved without OIT; engines sort per object or emitter alongside other transparent draws.
- **Trails / ribbons on CPU**: keep a ring buffer of the last K positions per particle (or per emitter for ribbons). Build a camera-facing strip in the vertex shader from adjacent points, using a per-instance segment and texture buffer or instanced segments.
- **Fixed-timestep sim with interpolation**: simulate at a fixed rate, render the previous/current state lerped by the accumulator alpha (the standard "fix your timestep" pattern). Or, cheaper, simulate particles at variable dt because they are not gameplay-relevant. Time scale and pause are a multiply on dt. Particles should optionally use unscaled time for UI effects.
- **Deterministic seeding**: seed each emitter's RNG from (emitter id, spawn index), or hash(seed, particle index), so replays, prewarm/Start Offset, and networked clients reproduce identical visual bursts without replicating particles. Only emitter state (enabled, seed, start time) needs to be networked.
- **Local vs world simulation and inheritance**: Defold's World/Emitter space and "inherit velocity only in World space" are the canonical pair. Local space makes particles move rigidly with a moving parent (good for auras). World space leaves trails behind (good for smoke and exhaust). Also handle spawn interpolation along the emitter's path between frames, or fast-moving emitters will emit in visible clumps.
- **HDR and bloom**: emissive particles should write color greater than 1 into the HDR transparency target so the existing bloom picks them up. This, together with premultiplied additive cores, is the cheapest way to make fire and sparks look modern. Pitfall: alpha-blended smoke over bright bloom sources should darken before bloom, so composite transparency before the bloom bright-pass.
- **Shadows**: receiving shadows means sampling the sun shadow map once per particle in the vertex shader (cheap). Casting needs a depth-only instanced pass with alpha test or opacity-map dithering, and is usually reserved for large opaque-ish smoke. Both should be per-emitter opt-in.

### Gaps
- No vendor-published particle budgets were found for GL 3.3 / GLES 3 instanced quads with CPU sim. All numeric budgets above are blog or forum guidance.
- No published per-particle cost for curl noise on CPU.
- Did not retrieve GDC talk content (Inside, Naughty Dog, Bungie, Guerrilla) or Simon Trümpler's blog in this pass. The techniques catalogue relies on the sources above plus standard practice marked as inference.

---

## Q3. Highest visual-quality jump per unit cost, and known pitfalls

### Takeaway
Ranked by value for this engine (largely inference, supported by the sources above):
1. Premultiplied blending, which unifies additive and alpha, reduces sort sensitivity, and batches.
2. HDR emissive into existing bloom.
3. Soft particles, using deferred depth that already exists.
4. Flipbook frame blending (a one-line shader lerp; motion vectors optional).
5. Ambient + directional per-particle lighting with a LightInfluence-style blend.
6. Six-way lightmaps for smoke (2 texture fetches, needs authored assets).

The cost to control is overdraw, through trimmed quads, size caps, and a global budget, not particle count.

### Cited Findings
- Premultiplied: additive and alpha particles in one batch without state changes. — [Wicked Engine](https://wickedengine.net/2017/10/which-blend-state-for-me); [Anisoptera Games](https://www.anisopteragames.com/hdr-effects/)
- Soft particles remove hard intersection lines with geometry. — [NVIDIA whitepaper](https://developer.download.nvidia.com/whitepapers/2007/SDK10/SoftParticles_hi.pdf)
- Motion-vector blending lets short flipbooks look smooth (anecdotal: 16 frames look like 36). — [realtimevfx](https://realtimevfx.com/t/motion-vectors-for-animated-textures-how-do-you-generate-them/575?page=2)
- Six-way gives convincing dynamic-lit smoke from texture sheets. — [Unity docs](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.0/manual/six-way-lighting.html)
- Quad trimming saves 30%+ of fill. — [Humus](https://www.humus.name/index.php?ID=266)
- Transparent pixels cost the same as opaque ones; screen coverage is the main mobile cost. — [Bugnet](https://bugnet.io/blog/how-to-fix-particle-overdraw-killing-mobile-performance); [PowerVR guide](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/optimal-particle-rendering-on-powervr.html)
- Pitfall, emitter lifetime: Defold effects keep emitting after their game object is deleted, which is a lifetime-ownership choice that must be explicit. — [Defold](https://defold.com/manuals/particlefx/)
- Pitfall, velocity inheritance: inherit velocity is only meaningful in world space. — [Defold](https://defold.com/manuals/particlefx/)
- Pitfall, collision cost: trace-based collision is expensive. — [s&box wiki](https://wiki.facepunch.com/sbox/Understanding_Emmiters_Initializers_&_Operators)
- Pitfall, unbounded effects: without budgets, artists raise emission until one effect looks good alone, ignoring instances stacking. — [Bugnet](https://bugnet.io/blog/debugging-particle-system-performance-issues)

### Inferences
- Approachable-tool balance: ship constant / random-range / curve on every property (NumberSequence/ColorSequence-style), a flipbook enum (2x2/4x4/8x8 like Roblox) with a "blend frames" checkbox, a LightInfluence slider, a soft-particle distance, and a preset library. Hide sorting, blend equations, and SoA details. Express "additive vs normal" as a single 0–1 slider mapped to premultiplied β.
- Pitfalls to design for: transparent-vs-transparent sorting with world glass (per-object sort only); pause and time scale (sim on scaled dt, flipbook on particle age so frames freeze consistently); prewarm (Start Offset) with deterministic seeds; network determinism (replicate seed and start time only); bloom double-counting if particles are added after the bright-pass.

### Gaps
- No primary source found for published "quality per cost" rankings. The ranking is synthesized.
- No primary data on how Rec Room, Horizon, or Core balance power and simplicity.
