# Roblox ParticleEmitter: feature set, limits, creator pain points

Researched 2026-10-12. Roblox distance values are written here as "units" (Roblox's own distance unit). Roblox API names are given only to identify capabilities; AnarchyEngine names its own APIs.

## What properties and hard limits does ParticleEmitter have today?

### Takeaway
ParticleEmitter is a camera-facing (or velocity-aligned) billboard sprite system. It has per-lifetime Color, Size, Transparency and Squash curves, four procedural emission shapes, flipbooks up to 8x8 or custom grids, simple physics (Acceleration, exponential Drag, wind, velocity inheritance), two blend modes (normal to additive) and burst `Emit(n)`. The documented hard caps are a 20 s lifetime, 400 particles/s per emitter (100/s on mobile), a 30 fps flipbook rate, and flipbook textures of 8x8 to 1024x1024 power-of-two squares. It has no collisions, no events, no mesh particles, no sub-emitters and no per-particle scripting.

### Cited Findings

**Full property list (current reference page)**
- Properties: Acceleration (Vector3), Brightness, Color (ColorSequence), Drag, EmissionDirection (NormalId), Enabled, FlipbookBlendFrames, FlipbookFramerate (NumberRange), FlipbookIncompatible (string), FlipbookLayout, FlipbookMode, FlipbookSizeX, FlipbookSizeY, FlipbookStartRandom, Lifetime (NumberRange), LightEmission, LightInfluence, LocalTransparencyModifier (hidden, not replicated), LockedToPart, Orientation, Rate, Rotation (NumberRange), RotSpeed (NumberRange), Shape, ShapeInOut, ShapePartial, ShapeStyle, Size (NumberSequence), Speed (NumberRange), SpreadAngle (Vector2), Squash (NumberSequence), Texture / TextureContent, TimeScale, Transparency (NumberSequence), VelocityInheritance, VelocitySpread (deprecated), WindAffectsDrag, ZOffset. Methods: `Clear()` and `Emit(particleCount = 16)`. No events are listed. Every property is tagged "Read Parallel". — [ParticleEmitter reference](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter)
- One doc oddity: the summary lists WindAffectsDrag under a "Deprecated" heading, but its detail entry has no deprecation tag. Its status is ambiguous. — [ParticleEmitter reference](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter)

**Property semantics (from the markdown reference)**
- **Drag**: the time in seconds for a particle to lose half its speed (exponential decay). Negative values are allowed and make speed grow exponentially. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- **Brightness** scales emitted light, but only when LightInfluence = 0. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- **LightEmission** runs from 0 (normal alpha blend) to 1 (additive). It does not light the environment; you need a PointLight for that. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md); [Particle Emitters guide](https://create.roblox.com/docs/effects/particle-emitters)
- **LightInfluence** runs from 0 to 1, and values outside that range are undefined. The default is 1 when the emitter is made with Studio tools and 0 when made with `Instance.new()`. At 1 a particle turns black in total darkness. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- **Orientation** options: FacingCamera (default), FacingCameraWorldUp, VelocityParallel, VelocityPerpendicular. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- **Squash** is a NumberSequence for non-uniform scale over lifetime. Above 0 the particle gets narrower and taller; below 0 it gets wider and shorter. The docs give no range. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- **TimeScale** runs from 0 to 1 (1 = normal speed, 0 = frozen), so it can slow particles but not speed them up. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- **LockedToPart**: when true, live particles move with the parent. VelocityInheritance (0 to 1) instead passes on a fraction of the parent BasePart's velocity at the moment of emission. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- **WindAffectsDrag**: particles follow Workspace.GlobalWind, but only when Drag > 0. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md); [guide](https://create.roblox.com/docs/effects/particle-emitters)
- **ZOffset** moves the render position toward the camera (positive) or away from it (negative) without changing on-screen size. Large negative values can hide particles inside or behind the parent part. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- **Lifetime** of 0 stops emission. Changes to Lifetime, Rate or Speed do not affect particles already alive; use Acceleration, Drag or VelocityInheritance for that. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md); [guide](https://create.roblox.com/docs/effects/particle-emitters)
- **Shapes**: Box, Sphere, Cylinder, Disc. ShapeStyle is Volume or Surface. ShapeInOut is Outward, Inward or InAndOut (InAndOut picks a direction at random per particle). ShapePartial means a different thing per shape: the Cylinder's top radius (0 makes a cone), the Disc's inner radius (1 gives the rim only), or the Sphere's emission angle (0.5 gives a hemisphere). — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md); [Creator Hub enum](https://developer.roblox.com/api-reference/enum/ParticleEmitterShapeInOut)
- Shapes take their size from the parent part's bounding box, so a non-uniform parent gives an ellipse. Sphere and Cylinder do not display correctly when the emitter is parented to an Attachment. — [New Procedural Particle Emitter Shapes (Oct 20, 2021)](https://devforum.roblox.com/t/new-procedural-particle-emitter-shapes/1517642); [guide](https://create.roblox.com/docs/effects/particle-emitters)
- **Flipbooks**: FlipbookMode is Loop, OneShot, PingPong or Random. OneShot spreads the frames over the lifetime and ignores the framerate. FlipbookBlendFrames switches between a linear crossfade and an instant frame swap. FlipbookStartRandom gives each particle a random start frame, and with a framerate of 0 each particle shows one random static frame. FlipbookIncompatible holds an error string if the texture size is not an exact multiple of the layout. — [ParticleEmitter.md](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)

**Hard caps**
- Lifetime is capped at 20 s, and higher values are clamped. — [Particle Emitters guide](https://create.roblox.com/docs/effects/particle-emitters)
- "A single particle emitter can create up to 400 particles per second (100 per second on mobile)." — [Particle Emitters guide](https://create.roblox.com/docs/effects/particle-emitters)
- Flipbook layouts are Grid2x2 (4 frames), Grid4x4 (16), Grid8x8 (64) and Custom (FlipbookSizeX by FlipbookSizeY). The maximum flipbook framerate is 30 fps. — [Particle Emitters guide](https://create.roblox.com/docs/effects/particle-emitters)
- Flipbook textures must be square power-of-two images from 8x8 to 1024x1024. — [Particles' Flipbook Release (Oct 21, 2022, 357 likes)](https://devforum.roblox.com/t/particles’-flipbook-release/2029388)
- A community member says in-game textures are downscaled to 1024x1024 and that Roblox "will most likely raise" this. That claim is second-hand and unconfirmed. — [Improve the performance of ParticleEmitters](https://devforum.roblox.com/t/improve-the-performance-of-particleemitters/3547121)
- Quality throttling: clients turn off flipbooks automatically when memory is low, and quality depends on the player's graphics level. — [guide](https://create.roblox.com/docs/effects/particle-emitters)
- Users report that Roblox lowers the Rate of `Enabled` emitters at low graphics levels, that nothing bypasses this, and that `Emit()` is not throttled. — [How could I bypass this? (Sep 24, 2022)](https://devforum.roblox.com/t/how-could-i-bypass-this/1994624); [Performance considerations guide](https://devforum.roblox.com/t/performance-considerations-improvements-and-optimizations-when-making-a-game/817499)
- Roblox's own guidance: keep Rate low, remember that big particles cost GPU fill-rate and overlapping transparent particles cause overdraw, reuse textures, test at the lowest and highest Editor Quality Level, and fade Transparency in and out to avoid popping. — [guide](https://create.roblox.com/docs/effects/particle-emitters)

**Related effects**
- **Beam**: a textured strip between two Attachments along a cubic Bézier. CurveSize0 and CurveSize1 place the inner control points along each attachment's X axis. Segments (default 10) sets the tessellation, and a ColorSequence with n keypoints needs at least n-1 segments to display correctly. — [Beam reference](https://create.roblox.com/docs/reference/engine/classes/Beam#CurveSize1)
- **Trail**: a ribbon between two Attachments, with MaxLength and MinLength properties. The search snippets do not describe their behavior. — [Trail reference](https://create.roblox.com/docs/reference/engine/classes/Trail)
- **Fire, Smoke, Sparkles**: legacy preset emitters. Fire emits along +Y (a negative Heat value emits downward). Smoke lacks Lifetime and `Emit()`. Roblox recommends ParticleEmitter for detailed work. Disabling one lets live particles finish; destroying it removes them instantly. — [Fire reference](https://create.roblox.com/docs/en-us/reference/engine/classes/Fire.md); [Smoke reference](https://create.roblox.com/docs/en-us/reference/engine/classes/Smoke.md)

### Inferences
- Speed, Rotation and RotSpeed are only randomized when a particle spawns. The only things that change over a particle's life are the Color, Size, Transparency and Squash curves, Acceleration, Drag and wind. There is no velocity-over-lifetime, orbital velocity, noise or attractor control.
- With Rate capped at 400/s and Lifetime at 20 s, one emitter can hold at most about 8,000 live particles from Rate alone (about 2,000 on mobile). `Emit()` bypasses both the Rate cap and the quality throttle, which explains why VFX artists burst everything from scripts.
- Blending is a single 0 to 1 slider between alpha and additive. There is no multiply, subtractive or premultiplied mode.

### Gaps
- I found no official keypoint limit for NumberSequence or ColorSequence (often said to be 20) in the pages I fetched. Treat it as unverified.
- The global particle budget (a "16,000 desktop / 3,600 mobile" figure turned up in a search summary) could not be traced to any thread or doc. Treat it as unverified.
- The docs give no range for Squash, Brightness, VelocityInheritance or ZOffset, and no release dates for Squash, Brightness, TimeScale or FlipbookStartRandom. I could not find their announcements.
- I did not verify the exact behavior of Trail MaxLength or the Sparkles properties.
- The docs do not say how throttling is spread across graphics levels.

## What do creators complain about and request (ranked by frequency)?

### Takeaway
The requests that keep coming back, from 2016 to 2025, are: (1) collisions and collision events, (2) 3D/mesh particles, (3) sub-emitters and triggers, (4) more blend modes and real lighting/shadows, (5) turbulence/noise and advanced motion, and (6) performance and view distance. Flipbooks (2022), procedural shapes (2021) and Squash (non-uniform X/Y size) shipped. As of my searches, collisions, mesh particles, sub-emitters and blend modes are still open.

### Cited Findings

**1. Collisions (most requested)**
- "Particle emitter physics?" is an early request for particles to collide, or at least to collide with specified planes, for sparks and debris. Replies point out that point-based billboard particles clip halfway into geometry and that moving geometry breaks simple raycasts. — [devforum t/23039](https://devforum.roblox.com/t/particle-emitter-physics/23039)
- On Roblox's 2021 shapes announcement (405 likes), the most-liked reply (40 likes) asked for particle collisions. A Roblox engineer's reply "Stay tuned for a post about this very soon" (26 likes) was about sprite sheets; flipbooks followed in 2022, but collisions never shipped. — [New Procedural Particle Emitter Shapes](https://devforum.roblox.com/t/new-procedural-particle-emitter-shapes/1517642)
- Collisions also appear in "VFX system update features" (Jun 16, 2021, 56 likes) and "Particle System Revamp/Overhaul" (Oct 14, 2025, 16 likes). — [t/1293316](https://devforum.roblox.com/t/vfx-system-update-features/1293316); [t/4005723](https://devforum.roblox.com/t/particle-system-revampoverhaul/4005723)
- Community stand-ins: Collideable Particles Module, 3D Particle System (collision events), Particle Emitter + (collisions and trails), and Fresh, whose thread notes "Roblox's Particle system doesn't support Particle Collisions". — [t/2279402](https://devforum.roblox.com/t/collideable-particles-module/2279402); [t/2041301](https://devforum.roblox.com/t/3d-particle-system/2041301); [t/1900792](https://devforum.roblox.com/t/particle-emitter/1900792); [t/1003010](https://devforum.roblox.com/t/fresh-new-particle-system/1003010)

**2. 3D / mesh particles and non-billboard orientation**
- "Advanced Particles" asks for 3D and stationary particles (grass, debris, rocks) and for a smoke redesign. — [t/52879](https://devforum.roblox.com/t/advanced-particles/52879)
- A "3D mesh" render mode is in the 2021 VFX list (to avoid scripted spell effects) and in the 2025 overhaul request ("Mesh particles require creating and destroying instances manually"). A Dec 2025 bump asks for particles that do not always face the camera. — [t/1293316](https://devforum.roblox.com/t/vfx-system-update-features/1293316); [t/4005723](https://devforum.roblox.com/t/particle-system-revampoverhaul/4005723)
- Workarounds: the Sine VFX plugin ("3d particles - basically mesh vfx"), and custom Parallel Luau systems that render about 500 particles at 40 fps. — [t/4803771](https://devforum.roblox.com/t/plugin-sine-vfx-a-vfx-plugin-for-roblox-studio/4803771); [t/2172310](https://devforum.roblox.com/t/i-made-a-new-particle-system/2172310)
- Mesh-shaped *emission volumes*: Roblox said in 2021 that they had "experimented locally with mesh-shaped emitters", on the roadmap with no ETA. I found no sign this has shipped. — [Shapes announcement](https://devforum.roblox.com/t/new-procedural-particle-emitter-shapes/1517642)
- Emission from part shapes is limited: one user could not get rim-only Disc emission on a union and placed a ring of Attachments instead. — [t/3985524](https://devforum.roblox.com/t/particleemitter-from-certain-part-of-part/3985524)

**3. Sub-emitters, triggers and events**
- The 2021 VFX list asks for sub-emitters (rain splashes, fireworks) and triggers that fire on enter, exit or inside a collider, with kill, dampen and bounce responses. The 2025 overhaul asks for sub-emitters again for explosions. ParticleEmitter exposes no events. — [t/1293316](https://devforum.roblox.com/t/vfx-system-update-features/1293316); [t/4005723](https://devforum.roblox.com/t/particle-system-revampoverhaul/4005723); [reference](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter)

**4. Blend modes, lighting, shadows, sorting**
- Requests for multiply blending (to show a sprite's dark parts), particles that cast shadows (leaves), and particles that emit actual light. — [t/1293316](https://devforum.roblox.com/t/vfx-system-update-features/1293316)
- Requests for additive, subtractive and multiplicative modes "similar to Unity". — [t/4005723](https://devforum.roblox.com/t/particle-system-revampoverhaul/4005723)
- Sorting: background Beams draw over ParticleEmitters depending on view angle, and no property fixes it. Roblox staff called this by design but "added to the feature backlog". — [Background Beams overriding ParticleEmitters](https://devforum.roblox.com/t/background-beams-overriding-particleemitters/3664511)

**5. Advanced motion and over-lifetime controls**
- Requests for noise/turbulence (strength, frequency, scroll, octaves), size, color or texture driven by speed, rate over distance (wheel smoke, rocket trails), burst emission without scripts, trails from particles, dissolve masks instead of transparency fades, and split X/Y size. Split X/Y size was later covered in part by Squash. — [t/1293316](https://devforum.roblox.com/t/vfx-system-update-features/1293316)
- Turbulence again in 2025 for realistic fire and smoke, plus "larger maximum sizes" and "greater particle view distance". — [t/4005723](https://devforum.roblox.com/t/particle-system-revampoverhaul/4005723)

**6. Performance, throttling, inconsistency**
- "Improve the performance of ParticleEmitters" (Mar 12, 2025, 28 likes). The original poster claims a few hundred static particles fell below 1 fps while 30k BillboardGuis held 30+ fps on a Ryzen 3200G with an RX 570. Another user disputes this: about 20k particles across 90 emitters ran at 120-144 fps on a GTX 1080 Ti, falling to 60-70 fps with large overlapping particles. A close-up aura emitter took one user from 240 to 60 fps. — [t/3547121](https://devforum.roblox.com/t/improve-the-performance-of-particleemitters/3547121)
- Automatic throttling at low graphics levels annoys creators, and the usual workaround is `Emit()` inside a loop. — [t/1994624](https://devforum.roblox.com/t/how-could-i-bypass-this/1994624)
- Emitters can also be culled at distance (one user's rocket smoke vanished), and Studio and playtest quality levels differ. — [search summary of "Particles disappearing" threads](https://devforum.roblox.com/t/particles-disappearing/358890)

**7. Flipbook-specific**
- On the flipbook release, the top replies asked to turn off frame crossfading (19 and 12 likes), which shipped as FlipbookBlendFrames, and for animated ImageLabels (11 likes). — [Flipbook release](https://devforum.roblox.com/t/particles’-flipbook-release/2029388); [reference](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)
- Shapes ignore Attachment parents because size comes from the parent part. — [Shapes announcement](https://devforum.roblox.com/t/new-procedural-particle-emitter-shapes/1517642)

**Shipped (with dates where known)**
- Procedural shapes (Shape, ShapeStyle, ShapeInOut, ShapePartial): Oct 20, 2021. — [announcement](https://devforum.roblox.com/t/new-procedural-particle-emitter-shapes/1517642)
- Flipbooks out of beta: Oct 21, 2022 (enum renames, any power-of-two texture from 8 to 1024, border-clipping fix). — [announcement](https://devforum.roblox.com/t/particles’-flipbook-release/2029388)
- Squash, FlipbookBlendFrames, FlipbookStartRandom, Brightness, TimeScale and WindAffectsDrag are in the current API. I could not find their release dates. — [reference](https://create.roblox.com/docs/reference/engine/classes/ParticleEmitter.md)

### Inferences
- Ranking by how often each comes up and how much engagement it gets: collisions > mesh/3D particles > sub-emitters/events > blend modes/lighting/shadows > turbulence/advanced motion > performance/throttling/view distance > sorting > flipbook polish. Collisions alone has the single most-liked reply in the threads I sampled (40 likes).
- Roblox's pattern is to ship look features (shapes, flipbooks, squash, blend-frame toggle) but not simulation features (collision, events, sub-emitters, noise). Those are exactly what VFX artists rebuild in Luau with parts and meshes.
- For AnarchyEngine, the cheapest gaps to close are features a plain CPU or GPU sim can do cheaply: depth-buffer collision or plane collision, death/collision sub-emitters, curl noise, velocity-over-lifetime, more blend modes and soft particles. Doing them would answer the top Roblox complaints without per-particle scripting.

### Gaps
- Like counts for most of the feature-request threads were not visible beyond the original post, and I did not get a full reply ranking.
- I found no official staff roadmap statement from 2023-2026 on collisions, mesh particles or sub-emitters.
- Reddit, Twitter/X and YouTube sources were not covered within the tool budget.

## What workarounds do Roblox VFX artists use?

### Takeaway
They burst particles from scripts with `Emit()`, stack many emitters, move Attachments around from scripts, tween Parts and MeshParts for 3D effects, use Beams and Trails for shockwaves and ribbons, write custom Luau particle systems for collisions and meshes, and use flipbooks to cut particle counts.

### Cited Findings
- `Emit()` bursts, including `Emit(1)` on every RunService step, to get around quality throttling and distance culling. — [t/1994624](https://devforum.roblox.com/t/how-could-i-bypass-this/1994624); [Performance guide](https://devforum.roblox.com/t/performance-considerations-improvements-and-optimizations-when-making-a-game/817499)
- Many rate-1 emitters stacked for explosions, which is why people ask for a native burst type. Attachments moved by scripts stand in for particle light. — [t/1293316](https://devforum.roblox.com/t/vfx-system-update-features/1293316)
- Mesh VFX done by creating, tweening and destroying instances, through plugins like Sine VFX or custom modules. — [t/4005723](https://devforum.roblox.com/t/particle-system-revampoverhaul/4005723); [Sine VFX](https://devforum.roblox.com/t/plugin-sine-vfx-a-vfx-plugin-for-roblox-studio/4803771)
- Custom collision particle modules (Collideable Particles, 3D Particle System, Particle Emitter +). — [t/2279402](https://devforum.roblox.com/t/collideable-particles-module/2279402); [t/2041301](https://devforum.roblox.com/t/3d-particle-system/2041301); [t/1900792](https://devforum.roblox.com/t/particle-emitter/1900792)
- Rings of Attachments where shape emission falls short. — [t/3985524](https://devforum.roblox.com/t/particleemitter-from-certain-part-of-part/3985524)
- Flipbook textures instead of spamming particles. — [t/3547121](https://devforum.roblox.com/t/improve-the-performance-of-particleemitters/3547121)
- Roblox recommends LightEmission = 1 for grayscale textures without alpha, and ZOffset to push particles in front of or behind geometry. — [guide](https://create.roblox.com/docs/effects/particle-emitters)

### Inferences
- Each workaround points to a missing native feature: script bursts point to burst and timeline scheduling, emitter stacking to multi-module emitters, tweened meshes to mesh particles, Attachment rings to arbitrary or mesh emission shapes, and script Attachments to particle lights.

### Gaps
- No source I fetched for this section covered Beam-based shockwaves, so that workaround rests on the brief, not on a citation.
