# ParticleEmitter Design

2026-10-12 · A property-driven particle emitter that keeps Roblox's simplicity but closes the gaps its creators complain about: curves and ranges on every value, bursts without scripts, collision, orbit motion, soft edges, glow into bloom, any flipbook grid, and one visible particle budget instead of silent throttling. CPU-simulated, drawn as instanced quads on GL 3.3.

Research behind it: `docs/research/reports/Particle emitter feature landscape.md` (notes in `docs/research/research_notes/Particle emitter feature landscape/`).

## Goal

A creator drops a `ParticleEmitter` into a GameObject, PhysicsObject, Brush, or Attachment and sees particles at once, in edit mode and in play. Every number they would want to vary is a constant, a random range, a curve, or a random blend of two curves, edited in Properties. Particles can hit terrain or the world, glow, fade softly into surfaces, swirl around the emitter, and animate a sprite sheet. Nothing about it is capped beyond what the creator can see: the emitter's own `MaxParticles` and the Workspace's `ParticleBudget`.

The look must be modern on a low-end GPU: premultiplied blending, HDR emission into the existing bloom, soft edges from the existing depth buffer. The simulation must be cheap on a low-end CPU: tight per-emitter arrays, curves read from lookup tables, collision opt-in per emitter.

## Decisions

| Question | Decision |
| --- | --- |
| Shape of the API | One fat `ParticleEmitter` instance with grouped properties (approach A). Not a stack of behaviour child instances, not an effect asset. Unreal's Lightweight emitters and Godot's ParticleProcessMaterial converged on the same fixed property sheet. |
| Value model | Two new immutable value types, `NumberCurve` and `ColorCurve`, after Unity's MinMaxCurve. No keyframe cap; each is baked to a 64-sample lookup table on change. |
| Where it lives | A plain `Instance` (no Transform), under a PVInstance or Workspace, as `SoundEmitter`. It follows its parent's transform. Offsets come from putting it in an Attachment. |
| Shape size | Inherited from the parent, never a property. PhysicsObject and Brush: their `Size`. GameObject: its Prefab's bounds times its `Scale`. Attachment, Bone, Workspace, or any other parent: a point. |
| Simulation | CPU, in a new `ParticleWorld` stepped once per frame on the engine thread after physics, at the frame's dt. Never at the 240 Hz physics rate. |
| Edit mode | Emitters simulate and draw always, stopped or playing. |
| Off screen | Simulated, not drawn. |
| Budget | Each emitter's `MaxParticles`, plus `Workspace.ParticleBudget` (20000). Over budget, the emitters farthest from the camera spawn proportionally fewer; live particles are never killed early. `Emit` obeys both. |
| Rendering | One instanced quad draw per drawn emitter in the forward transparency pass, premultiplied. Brightness above 1 feeds bloom. |
| ViewportPane | Emitters inside one are neither simulated nor drawn in this version. |
| Not in this version | Sub-emitters, mesh particles, trails/ribbons, turbulence, attractors, rate over distance, by-speed curves, quad trimming, mesh-surface emission, ring-buffer persistence, `Emit` overrides, presets, flipbook playback modes and frame blending, start delay, speed limit, outward/radial speed, particle shadows and particle lights. Nothing here blocks adding them. |

## 1. Value types (`engine_datatypes/`)

### NumberCurve

Immutable, like `Vector3`. One of four modes:

| Mode | Holds | A particle's value |
| --- | --- | --- |
| `Constant` | one number | that number |
| `Range` | min, max | one uniform pick between them, fixed for the particle |
| `Curve` | keyframes | the curve at the property's time |
| `CurveRange` | two keyframe lists | lerp of the two curves at the property's time, by a per-particle random fixed at birth |

A keyframe is `{time, value, interpolation}`: time in 0..1, interpolation `Linear`, `Smooth` (cubic Hermite with automatic tangents), or `Step`. A curve has at least one key; keys are sorted by time on construction; the curve holds its end values outside its first and last key. Any number of keys is allowed.

Lua:

```lua
NumberCurve.new(5)                                   -- Constant
NumberCurve.range(1, 3)                              -- Range
NumberCurve.keys({ {0, 0}, {0.2, 3, "Smooth"}, {1, 0} })  -- Curve
NumberCurve.keysRange(keysA, keysB)                  -- CurveRange
curve.Mode, curve.Min, curve.Max, curve.Keys         -- read-only
curve:Evaluate(t, blend)                             -- blend picks within Range/CurveRange, default 0
```

### ColorCurve

The colour counterpart. A key is `{time, Color3, transparency}`, so colour and transparency live in one gradient. Modes: `Constant`, `RandomBetween` (two colour+transparency values, one pick per particle), `Gradient`, `GradientRange`. Interpolation is linear in linear colour space. Lua mirrors NumberCurve: `ColorCurve.new(color, transparency?)`, `ColorCurve.between(a, ta, b, tb)`, `ColorCurve.gradient(keys)`, `ColorCurve.gradientRange(keysA, keysB)`.

### Time axis

What a curve's 0..1 means is the property's, not the type's, and each property's doc says which:

- **Particle life**: `Color`, `Size`, `Stretch`, `Drag`, `OrbitSpeed`. Evaluated every step at the particle's normalised age.
- **Emitter cycle**: `Rate`, `Lifetime`, `EmitSpeed`, `EmitRotation`, `RotationSpeed`, a burst's `Count`. Evaluated once, at spawn (or for `Rate` every step), at the cycle's normalised time.

### Baking

On each write the engine thread bakes a curve property into a 64-sample table per curve (two for the Range modes). The simulation only samples tables: a 50-key curve costs what a constant does. Constant and Range skip the table.

### Save, load, registry

Saved as a JSON object, e.g. `{"mode":"Curve","keys":[[0,0,"Linear"],[1,1,"Smooth"]]}`; a property's `default_json` uses the same form. The registry's saved-property path (`lua_saved_property`) learns the two type names, so DataModel saves, loads, undoes, and restores them at Stop as it does Color3. A malformed value is refused with a message saying why.

### Properties editor

A curve row shows a compact preview: the constant, "1 – 3" for a range, a sparkline for a curve. A ColorCurve row shows a gradient strip with a checkerboard under transparency. Clicking opens a popover:

- a mode dropdown;
- for curves: a graph with draggable keys, double-click to add, Delete to remove, numeric time/value fields and an interpolation picker for the selected key, auto-fitting vertical range;
- for gradients: stops on a bar, each with a colour picker and transparency field.

One drag is one undo step and one Changed, on release.

## 2. Properties panel additions

- **Grey-out rule.** A new `lua_enabled_when(field, predicate)` shows a row disabled (visible, not editable) while a predicate over the instance is false. Unlike `lua_shown_when` it takes a function, since some rules depend on the parent (whether the emitter emits from a point). Hide-versus-grey is chosen per property.
- **Burst list row.** A small table editor: rows of `Time` and `Count` (min–max), add and remove buttons.

## 3. The `ParticleEmitter` class (`engine_instances/ParticleEmitter.{hpp,cpp}`)

Suited parents: `Workspace`, `PVInstance`. Properties groups in order: Emission, Shape, Particle, Motion, Collision, Appearance, Rendering. All properties are saved registry properties unless marked.

### Emission

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `Playing` | bool | true | Writing true starts a cycle from its beginning. False stops spawning; live particles finish. A non-looping emitter sets it false itself when its cycle ends (a quiet engine write that still fires `Changed` with "Playing"). Off-then-on mid-cycle restarts. |
| `Rate` | NumberCurve (cycle) | 20 | Particles per second. Range: re-picked each cycle. No cap. |
| `Bursts` | list of `{Time, Count}` | empty | Added on top of `Rate`. `Time` in seconds into the cycle; `Count` a min–max (a NumberCurve restricted to Constant/Range). Saved as a JSON array; Lua reads and writes an array of `{Time=, Count=}` tables. |
| `Duration` | number | 5 | Seconds per cycle; must be > 0. |
| `Looping` | bool | true | Off: one cycle, then `Playing` becomes false. |
| `Prewarm` | bool | false | A looping emitter starts as though one full cycle had already run. |
| `MaxParticles` | int | 1000 | Live limit for this emitter. At the limit new spawns are skipped. |
| `Seed` | int | 0 | 0: a fresh random seed per cycle start. Non-zero: identical replay. |

### Shape

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `Shape` | `Enum.ParticleShape` | `Box` | `Point`, `Box`, `Sphere`, `Hemisphere`, `Cylinder`, `Cone`, `Disc`. Sized by the parent (see Decisions); every shape reads that size as the box it fits in: sphere/hemisphere an ellipsoid, cylinder X/Z width and Y height, cone Y height and X/Z base opening along +Y, disc X/Z. |
| `EmitFrom` | `Enum.ParticleEmitFrom` | `Volume` | `Volume`, `Surface`, `Edge`. Greyed while emitting from a point (Shape `Point` or a parent with no size). |
| `Arc` | number (deg) | 360 | Fraction of the shape around its Y axis. Greyed for `Box` and while emitting from a point. |
| `EmitDirection` | Vector3 | (0, 1, 0) | Normalised on write; a zero vector is refused. |
| `EmitDirectionSpace` | `Enum.TransformSpace` | `Local` | Local turns with the parent; World stays fixed. |
| `EmitSpread` | Vector2 (deg) | (0, 0) | Random spread around `EmitDirection` on two axes; (180, 180) is every direction, (90, 0) a flat fan. |

Sampling is analytic per shape: a box surface picks a face weighted by area, an edge picks one of the 12 edges weighted by length, a sphere volume scales a random direction by radius·∛u, and so on. The local point is then moved by the parent's world transform. When the emitter is selected the studio draws the effective shape as a wireframe at the parent.

### Particle (values at birth)

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `Lifetime` | NumberCurve (cycle) | Range 1–2 | Seconds; must be > 0. No upper cap. |
| `EmitSpeed` | NumberCurve (cycle) | 5 | Units/s along the particle's direction. Negative flies backward. |
| `EmitRotation` | NumberCurve (cycle) | 0 | Degrees. |
| `RotationSpeed` | NumberCurve (cycle) | 0 | Degrees/s, picked once per particle. |

### Motion

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `SimulationSpace` | `Enum.TransformSpace` | `World` | World: particles stay where born. Local: they move with the parent. |
| `InheritVelocity` | number 0–1 | 0 | Fraction of the velocity at the emitter's point of the parent's assembly body, spin included (v + ω × r). A parent in no assembly gives none. Greyed in Local space. |
| `Acceleration` | Vector3 | (0, 0, 0) | Units/s². Gravity is just a value here. |
| `AccelerationSpace` | `Enum.TransformSpace` | `World` | |
| `Drag` | NumberCurve (life) | 0 | Unity-style coefficient: `v *= exp(-Drag·dt)`, frame-rate independent. |
| `OrbitSpeed` | NumberCurve (life) | 0 | Degrees/s about `OrbitAxis` through the emitter's position. |
| `OrbitAxis` | Vector3 (local) | (0, 1, 0) | Normalised; zero refused. |

### Collision

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `Collision` | `Enum.ParticleCollision` | `None` | `None`; `Terrain` (voxel lookups); `World` (terrain plus physics bodies and Brushes via budgeted raycasts). |
| `CollisionResponse` | `Enum.ParticleCollisionResponse` | `Bounce` | `Bounce`, `Stick` (stops and stays for the rest of its life), `Die`. Greyed while `Collision` is `None`. |
| `Bounciness` | number 0–1 | 0.5 | Normal speed kept. Hidden unless `Bounce`. |
| `Friction` | number 0–1 | 0.2 | Tangential speed lost per hit. Hidden unless `Bounce`. |
| `CollisionGroup` | string | "Default" | The engine's collision groups decide which bodies particles hit. Greyed unless `World`. |

### Appearance

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `Texture` | `Texture?` | nil | Streams like other textures. Nil draws a built-in soft round dot, also shown until the texture loads. |
| `TextureMode` | `Enum.ParticleTextureMode` | `Static` | `Static` or `Flipbook`. |
| `FlipbookGrid` | Vector2 | (1, 1) | Columns × rows, any integers ≥ 1; non-square textures fine. Shown only for `Flipbook`. The sheet plays once over each particle's life, frame by frame, no blending. |
| `Color` | ColorCurve (life) | white, opaque | Colour and transparency. RandomBetween / GradientRange give per-particle variety. |
| `Size` | NumberCurve (life) | 1 | Units. Range / CurveRange give per-particle variety. |
| `Brightness` | number ≥ 0 | 1 | Multiplies colour; above 1 glows into bloom. |
| `Stretch` | NumberCurve (life) | 0 | Aspect change at constant area: positive taller, negative wider. |

### Rendering

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| `Facing` | `Enum.ParticleFacing` | `Camera` | `Camera`, `CameraUpright` (yaw only), `Velocity` (long axis along motion), `World` (flat, normal along `FacingAxis`). |
| `FacingAxis` | Vector3 | (0, 1, 0) | Shown only for `World`. Normalised; zero refused. |
| `FacingAxisSpace` | `Enum.TransformSpace` | `Local` | Shown only for `World`. |
| `VelocityStretch` | number ≥ 0 | 0.1 | Extra length per unit of speed. Shown only for `Velocity`. |
| `BlendMode` | `Enum.ParticleBlendMode` | `Normal` | `Normal`, `Additive`, `Multiply`. |
| `SoftEdge` | number ≥ 0 | 0.5 | Units over which a particle fades where it meets scene depth. 0 is off. |
| `LightInfluence` | number 0–1 | 1 | How much scene lighting tints the particle. Same default from Studio and from `Instance.new`. |
| `DrawOrder` | int | 0 | Among overlapping transparent draws, a higher value draws on top; ties sort by distance. |

### Scripting

| Member | Kind | Notes |
| --- | --- | --- |
| `Emit(count)` | method | Spawns now, ignoring `Rate` and `Playing`; obeys `MaxParticles` and the budget. |
| `Clear()` | method | Removes every live particle at once. |
| `Finished` | event | A non-looping emitter's cycle has ended and its last particle has died. |
| `ParticleCount` | read-only int | Live particles. Shown in Properties, not saved. |

### Workspace

`Workspace.ParticleBudget` (int, default 20000, ≥ 0): the live-particle limit across all emitters.

## 4. Simulation (`engine_core/ParticleWorld.{hpp,cpp}`)

A world in the `AudioWorld` mould: the class says `particle_emitter()`, which tags its entity; `ParticleWorld` finds tagged emitters in Workspace (not in a ViewportPane) and keeps one `EmitterState` per emitter. `Engine::run_step` steps it once per frame after physics and scripts, beside `audio_.step`, stopped or playing. `read_place` clears live particles so Stop never carries them into the next Test.

**Per emitter state.** Structure-of-arrays: position, velocity, age, lifetime, per-particle random (one float used for Range/CurveRange blends), rotation, rotation speed, flags (stuck). Swap-remove on death. Cycle time, burst cursor, RNG (seeded from `Seed` or fresh), last-frame parent transform.

**A step**, for each emitter:

1. Read the parent's world transform and shape size; if `Playing`, advance cycle time and work out spawn count from `Rate` (with a fractional carry) plus bursts crossed.
2. Clamp spawns by `MaxParticles` and the emitter's budget share.
3. Spawn: each new particle gets a sub-frame birth time spread across the frame; its position is sampled on the shape at the parent transform interpolated to that time, then it is pre-aged by the rest of the frame. A moving emitter leaves a smooth stream.
4. Integrate: acceleration, drag, orbit about the emitter, inherited velocity applied once at birth; age; kill expired.
5. Collide (if on): `Terrain` samples the voxel volume at the new position, and on entering solid estimates the normal from the density gradient. `World` casts a ray along the step against Box3D, filtered by `CollisionGroup`, under a per-frame ray budget; a hit plane is cached per particle and reused while it stays valid, so most frames test a plane, not a ray. Response: reflect with `Bounciness`/`Friction`, stop and flag stuck, or kill.
6. Sort `Normal`-blend particles back to front from the last frame's camera; skip for Additive and Multiply.
7. Publish an immutable, shared particle block (positions, size, rotation, colour, frame, velocity for stretch) for the snapshot.

**Budget.** Before spawning, if the live total plus the frame's requested spawns exceeds `ParticleBudget`, emitters are ranked by distance from the camera and the farthest have their spawn counts scaled down first until it fits. Live particles are never removed.

**Prewarm** runs the emitter's simulation forward one cycle at a coarse step on first activation. Collision is skipped during prewarm.

**Writes.** Setters run on the engine thread and mark a dirty mask that ParticleWorld takes each step (curves rebake, `Seed`, `Playing`). `Playing` set false at a one-shot's end and `ParticleCount` go through a quiet store, as SoundEmitter's `store_playback` does. `Finished` fires through `fire_event`.

## 5. Snapshot and rendering (`runner/`)

`VisualSnapshot` gains a per-frame list of drawn emitters: the shared particle block plus the emitter's render settings (texture path, flipbook grid, blend mode, facing, soft edge, light influence, brightness, draw order, bounds). Like `terrains`, the block is a shared immutable pointer, so `take_changes` copies no particle data and the UI thread reads it without a lock. Emitters whose bounds miss the frustum are left out.

The renderer adds a particle pass inside the forward transparency pass: each emitter is one `glDrawElementsInstanced` of a unit quad, per-instance data streamed through `InstanceBuffer`. Emitters interleave with other transparent draws by `DrawOrder` then distance. The vertex shader builds the quad for its `Facing` mode, applies `Stretch` and `VelocityStretch`, and picks the flipbook cell. The fragment shader samples the texture (or the built-in dot), applies colour × brightness, lighting at the particle centre (ambient, sun, forward lights as the transparency pass does) mixed by `LightInfluence`, and the soft-edge fade from scene depth, then writes premultiplied output: Normal and Additive share the premultiplied blend (`ONE, ONE_MINUS_SRC_ALPHA`, additive writing zero alpha); Multiply uses `DST_COLOR, ZERO` and is drawn as its own batch. HDR output above 1 reaches bloom unchanged. GLSL stays within 330 / ES 3.00.

## 6. Studio

- Insert menu, class icon, `ClassOrder`, API docs and autocomplete, `Instance.new` (the files listed in `src/engine_instances/README.md`).
- The selected emitter's effective shape is drawn as a wireframe at its parent.
- The NumberCurve/ColorCurve popover editors, grey-out rule, and burst list row (section 1 and 2).

## 7. Testing

- **Value types:** evaluation in every mode and interpolation, baking matches direct evaluation within tolerance, JSON round trip, refusal of malformed input, Lua constructors and readers.
- **Shapes:** every shape × `EmitFrom` lands inside / on / along the shape at the parent transform; `Arc` limits the angle; distributions are even (bucket counts within tolerance).
- **Simulation:** same `Seed` gives identical particles; `Drag`, `OrbitSpeed`, `Acceleration` in both spaces, `SimulationSpace`, `InheritVelocity` from an assembly body and none without one; sub-frame spawning gives evenly spaced particles from a fast emitter.
- **Collision:** bounce, stick and die against terrain and against a body; `CollisionGroup` filtering; ray budget is respected.
- **Lifecycle:** `Playing` restart and one-shot end, `Finished` timing, `Emit`, `Clear`, `MaxParticles`, `ParticleBudget` far-first scaling, Stop clears particles.
- **Properties:** defaults, hide and grey-out rules, undo of a curve drag as one step (`tests/PropertiesTest.cpp`).
- **In the app:** drive the studio over MCP; screenshots of soft edges, the three blend modes, a flipbook, and glow into bloom. Profile a Release build with a scene at the 20000 budget, warmed up, to confirm the CPU cost.

## As built (2026-10-13)

Where the implementation differs from the sections above, and why:

- **Draw order.** Particles draw after every see-through mesh, sorted among themselves by `DrawOrder` then distance, rather than interleaved with see-through meshes (which `DrawBatches` sorts on its own). A particle behind a glass pane draws over it.
- **Multiply.** The see-through buffer composites over the scene with one coverage value, so a per-channel multiply would need dual-source blending, which GLES 3 lacks. Multiply darkens by the color's lightness (a black over with alpha `a·(1 − luminance)`); a colored tint is lost.
- **Lighting.** Ambient plus each forward light's falloff and cone, with no surface term and no shadows.
- **Colors** interpolate per channel as stored.
- **Properties editors** open inline under the row (a fold arrow, like Transform), not as popovers: a popover would nest the color picker's popup. A `NumberCurve` row's field takes a typed number or `1 - 3`; its editor has mode tabs, a graph (double-click adds a key, drag moves, right-click removes), interpolation tabs, and typed key time and value. A `ColorCurve` row's editor has mode tabs and gradient bars; the selected stop takes the row's color picker and a transparency slider.
- **Bursts** is a typed field, `time: count` pairs such as `0: 50, 1.5: 10 - 20`, not a table of rows.
- **Soft edges** read a copy of scene depth (`glBlitFramebuffer`, newly loaded), since the see-through target has the depth attached.
- **World collision** gives each emitter's particles rays in a rotating window, from 512 rays a frame shared by all emitters; a particle whose turn has not come tests the plane its last ray hit.
- **Shape size** of a PhysicsObject or Brush ignores a driving GameObject's `Scale`.
