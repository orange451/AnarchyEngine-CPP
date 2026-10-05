# Screen-space reflections

2026-10-05 · Ports P5 (screen-space reflections) from `docs/research/2026-09-30-legacy-renderer-port.md`. Builds on bloom (`2026-10-05-bloom-design.md`) and FXAA (`2026-10-05-fxaa-design.md`).

## Goal

A `ScreenSpaceReflections` under `Lighting` makes smooth surfaces reflect what is on screen: a polished floor shows the objects standing on it, not only the sky. Where a reflected ray finds nothing on screen, the surface keeps the sky reflection it has today, so there are no holes.

Efficiency is a design rule. A place with no ScreenSpaceReflections pays nothing. With one:

- the trace runs at half resolution, on smooth and reflective pixels only, with a bounded number of steps;
- there is no full-resolution pass of its own;
- mips stand in for blur taps.

## The instance

`ScreenSpaceReflections` is an `Instance`, allowed only under `Lighting` (through Folders); `placement_error` refuses it anywhere else with "A ScreenSpaceReflections must be in Lighting". With two under Lighting, the first in tree order is used. Every property is a saved registry property.

| Property | Type | Default | Rule |
| --- | --- | --- | --- |
| Enabled | boolean | true | false traces nothing |
| Intensity | number | 1 | clamped to 0–1: how much of the sky reflection the traced one replaces |
| MaxDistance | number | 50 | clamped to 0–1000 studs: how far a ray may travel; it fades out over the last quarter |
| MaxRoughness | number | 0.5 | clamped to 0–1: rougher surfaces keep the sky reflection; it fades in over the last fifth below |

A value that is not finite is refused with "`<Property>` must be a finite number".

## Data path

- `SnapshotPump::resolve_lighting` fills `VisualSnapshot::reflections` (`VisualReflections`: `present`, `enabled`, `intensity`, `max_distance`, `max_roughness`) from `find_first<ScreenSpaceReflections>`, beside `VisualBloom`. `SceneFeed` copies it.
- `GameView` copies it into `SceneLighting::reflections` (`SceneReflections`). No instance, `Enabled` false, Intensity 0, or MaxDistance 0 means the renderer allocates and runs nothing for it.

## Rendering

### Where it runs

`… light → sky → Reflections → transparency → merge (resolve) → bloom → tone map → FXAA`

At that point the accumulation buffer holds the lit opaque scene and the sky, which is what reflections show. It is profiled as the `"Reflections"` pass.

### 1. The lit-image chain

- One draw copies the accumulation buffer into a half-size RGBA16F texture (`ssrSceneTexture_`), and `glGenerateMipmap` builds its mips.
- The accumulation buffer itself is not changed: its filter and levels stay as they are.
- Hits read this chain at a level chosen by `ReflectionMath::ConeLevel` from roughness and hit distance (a cone approximation). Smooth, near hits read level 0, and rough or far ones read a blurrier level. No extra taps are taken.

### 2. The trace (`ssr.frag`, half resolution)

Per pixel:

1. Read depth; exit for the sky (depth 1).
2. Read the normal and material. Exit when roughness ≥ MaxRoughness, or when the reflection weight (`skyReflection`'s weight, below) is under 0.02 in every channel.
3. Rebuild the view position from depth, and reflect the view ray about the normal. Rays pointing back toward the camera fade out (`ReflectionMath::FacingFade`).
4. March the ray as a screen-space DDA (McGuire and Mara, 2014):
   - The ray is clipped to the near plane and to MaxDistance, and projected to both ends in full-resolution pixels.
   - It is stepped with a pixel stride, at most 32 steps, offset by interleaved-gradient noise.
   - Reciprocal depth is interpolated, so depth along the ray is perspective-correct.
   - A step hits when the ray is behind the depth buffer by less than a thickness proportional to view depth. That way rays do not hit the backs of thin walls.
   - A hit is refined with 4 binary-search steps.
5. Write rgb as the hit color from the lit chain, and alpha as confidence: the product of the screen-edge fade, the distance fade, the roughness fade, and the facing fade (all `ReflectionMath`). A miss writes 0.

### 3. The resolve (in `merge.frag`)

- The merge upsamples the trace with a depth-aware 2×2 filter: the four half-size texels around the pixel, weighted by how close their depth is to the pixel's.
- Where the resulting confidence is above 0, and only there, it adds:

  ```
  color += Intensity × confidence × weight × traced − Intensity × confidence × skyReflected
  ```

- `skyReflection(...)` is factored out of `image_lighting.glsl`'s `skyLight`, and both `ibl.frag` and the merge call it. It returns the reflected light, `skyReflected`, which `skyLight` adds, and its `weight`, so the merge removes exactly what the IBL pass added:
  - with a Skybox: the prefiltered cube's light times the split-sum weight `F0 · brdf.x + brdf.y` and the Skybox's color and LightScale;
  - with none: the legacy stand-in's reflection term (`skyRadiance × Fresnel × Ambient` in `ambientLight`), and that Fresnel as the weight.
- `skyLight`'s output is unchanged by the factoring. The existing render checks pin that.

### Buffers and failure

- The trace target (half-size RGBA16F) and the lit chain (half-size RGBA16F with mips) are made on the first frame that reflects, and remade only when the pane's size changes. That is about 10 MB at 1080p.
- If the driver refuses them, that is reported once, and frames draw without SSR.
- When a program cannot draw yet (macOS), that frame draws without SSR: the merge gets `uReflectionsEnabled` 0.
- Nothing here goes beyond GLES 3.0. `glGenerateMipmap` of RGBA16F needs it color-renderable, which the other targets already require.

## ReflectionMath

`src/runner/ReflectionMath.hpp/.cpp` holds pure functions, with no GL, that `ssr.glsl` copies line for line (as `bloom.glsl` copies `BloomMath`):

- `EdgeFade(uv)`: 1 inside the middle 80% of the screen, falling smoothly to 0 at the edge.
- `DistanceFade(distance, maxDistance)`: 1 up to three quarters of MaxDistance, then 0 at MaxDistance.
- `RoughnessFade(roughness, maxRoughness)`: 1 up to four fifths of MaxRoughness, then 0 at it.
- `FacingFade(reflectedZ)`: 1 for rays leaving the camera, falling to 0 as they turn back toward it.
- `ConeLevel(roughness, hitDistance, viewDepth, levels)`: the lit chain's level a cone of that roughness covers at the hit.

## Out of scope

- Reflections on see-through surfaces: they are drawn in the forward pass, after the trace, and keep the sky reflection.
- See-through objects appearing in reflections: the trace reads the lit opaque image.
- Off-screen and behind-the-camera content: reflection probes are a separate feature.
- Temporal accumulation, which waits for TAA (P6).
- A Hi-Z depth pyramid: a later speed-up if profiling asks for one.
- A quality setting.

## Tests

- `sandbox/reflections_tests.cpp`:
  - defaults, clamping, refusal of values that are not finite, undo, save and load, Stop;
  - placement only under Lighting;
  - the first-in-tree rule, and falling through when it is removed;
  - the snapshot carries it;
  - scripts set it.
- `sandbox/reflection_math_tests.cpp`: each fade is 1 well inside its limit, 0 past it, and continuous and monotonic between; `ConeLevel` is 0 for a mirror and grows with roughness and distance, capped at the last level.
- `tests/SceneRenderCheck.cpp`, with a mirror floor (metal, roughness 0) under a bright emissive cube:
  - with SSR, the floor just below the cube takes the cube's color; without it, the floor shows only the sky reflection;
  - floor roughness above MaxRoughness gives exactly the frame without SSR;
  - no instance, Enabled false, Intensity 0, or MaxDistance 0 each give exactly the frame without SSR;
  - a cube moved off-screen leaves no reflection and no hole: the frame matches the one without SSR;
  - sky pixels and the far clear color are untouched;
  - resizing makes the buffers again, the first frame still draws, and there are no GL errors.
- `tests/SceneFeedTest.cpp`: `VisualReflections` rides through the feed.
- In the studio, the GPU time of the Bloom, FXAA, and Reflections passes is recorded at the Scene View's size and reported with screenshots.
