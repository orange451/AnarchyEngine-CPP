# Ambient occlusion

2026-10-05 · Ports P1 (ambient occlusion) from `docs/research/2026-09-30-legacy-renderer-port.md`, with I13 (GTAO instead of the legacy ray-marched HBAO). Supersedes the design in `docs/superpowers/plans/2026-10-02-ambient-occlusion.md`. That plan's algorithm, its table of legacy bugs and how each is fixed, and its references stand. Its Lighting properties, its full-resolution-only cost, and its failure rule do not: this spec replaces them. Builds on bloom, FXAA, and screen-space reflections (`2026-10-05-*-design.md`).

## Goal

An `AmbientOcclusionEffect` under `Lighting` shades surfaces where nearby geometry hides the sky and ambient light from them: in creases, under objects, where a wall meets a floor. Objects look grounded instead of floating. Only indirect light is occluded; direct lights already have shadows.

Efficiency is a design rule:

- A place with no AmbientOcclusionEffect pays nothing.
- `Quality` trades cost for sharpness, and the default runs at half resolution.
- Every program is validated once, then trusted until it or its buffers are made again.

## The instance

`AmbientOcclusionEffect` is an `Instance`, allowed only under `Lighting` (through Folders). `placement_error` refuses it anywhere else with "An AmbientOcclusionEffect must be in Lighting". With two under Lighting, the first in tree order is used. Every property is a saved registry property.

| Property | Type | Default | Rule |
| --- | --- | --- | --- |
| Enabled | boolean | true | false runs nothing |
| Intensity | number | 1 | clamped to 0–4: an exponent on visibility; 1 is physical, above 1 darker, 0 none |
| Radius | number | 1 | clamped to 0–10 studs: how far an occluder still counts; it fades over the last 60% |
| Quality | `Enum.EffectQuality` | `Medium` | see below |

A number that is not finite is refused with "`<Property>` must be a finite number". A Quality that is not an `Enum.EffectQuality` is refused with "Quality must be an Enum.EffectQuality". As every enum property does, Quality also takes an item's value or name.

`Enum.EffectQuality` is a new enum shared by effects: `Low` 0, `Medium` 1, `High` 2. Later effects, and screen-space reflections, can take the same scale.

| Quality | Resolution | Slices × steps per side | Depth taps per pixel | Blur radius |
| --- | --- | --- | --- | --- |
| Low | half | 2 × 6 | 24 | 4 |
| Medium | half | 3 × 6 | 36 | 4 |
| High | full | 3 × 6 | 36 | 4 |

## Data path

- `SnapshotPump::resolve_lighting` fills `VisualSnapshot::occlusion` (`VisualAmbientOcclusion`: `present`, `enabled`, `intensity`, `radius`, `quality` as an int) from `find_first<AmbientOcclusionEffect>`, beside `VisualReflections`. `SceneFeed` copies it.
- `GameView` copies it into `SceneLighting::occlusion` (`SceneOcclusion`, with `quality` a `runner::SceneQuality`).
- No instance, Enabled false, Intensity 0, or Radius 0 means:
  - the renderer makes no AO buffers and runs no AO pass;
  - the IBL pass and the merge read a white texture;
  - the frame is exactly what it is without this feature.

## Rendering

### Where it runs

`… geometry → AO → blur → light (IBL reads AO) → sky → reflections → transparency → merge → …`

It is profiled as the `"Ambient occlusion"` pass.

### 1. The trace (`gtao.frag`)

GTAO (Jimenez et al. 2016, following Intel's XeGTAO) runs at the Quality's resolution into an R8 target, where 1 is open and 0 is shut in. For each pixel with depth below 1:

1. Rebuild the view-space position from depth, with V toward the camera and N from the G-buffer.
2. The pixel radius is Radius projected at that depth, capped at 25% of the buffer's height. Under one pixel, the output is 1.
3. For each slice, at an angle offset by interleaved-gradient noise:
   1. Project N into the slice plane.
   2. Start both horizons at the tangent plane, and march the steps each way. Steps grow quadratically, with an R2 offset, at least one pixel apart.
   3. Keep the highest horizon on each side. Each sample fades toward the tangent plane with distance over the last 60% of Radius. A sample off screen or on the sky occludes nothing.
   4. Integrate the visible arc in closed form.
4. Average the slices.

These are the 2026-10-02 plan's steps, at the Quality's slice count and resolution. A full-resolution sample position is the texel center the trace reads, at either resolution.

### 2. The blur (`ao_blur.frag`)

The blur is separable (across, then down) and runs at the trace's resolution, into a second R8 target and back. Taps are weighted by:

- distance from the center pixel's tangent plane, relative to its depth (tolerance 0.02);
- normal agreement (power 8).

So creases do not smear across silhouettes. Without TAA this blur is the only denoiser. Low keeps the same radius: a wider one costs more than the slice it saves.

### 3. Applying it

- **Reading it.** `ibl.frag` and `merge.frag` read AO through one shared function, `occlusionAt(uv, depth)`, in `occlusion.glsl`:
  - at half resolution, a depth-aware 2×2 upsample, as SSR's merge uses;
  - at full resolution, the texel itself.
- **Diffuse.** The sky's and the ambient's diffuse light is multiplied by multi-bounce AO (the Jimenez 2016 fit), raised to Intensity. A bright surface loses less in a crease than a dark one.
- **Specular.** The sky's reflection is multiplied by specular occlusion, derived from AO and roughness (Lagarde 2014), so a metal floor's reflection dims where something sits on it.
- **One source of truth with SSR.** `skyReflection()` takes the specular occlusion as an argument. `ibl.frag` and the merge both compute it from `occlusionAt`. So the merge removes exactly the occluded sky reflection the IBL pass added, and a floor where SSR misses looks the same with or without SSR.

### Buffers and failure

- Two R8 targets at the Quality's resolution: 1 byte a pixel each, about 1 MB at half and 4 MB at full 1080p.
- They are made on the first frame with AO, and made again when the pane's size or the Quality changes.
- If the driver refuses them, that is reported once, and frames draw without AO.
- If a program cannot draw yet (macOS), that frame draws without AO (white). It is never a failed frame.
- Nothing goes beyond GLES 3.0: R8 render targets, and GLSL that GLSL ES 3.00 has.

## OcclusionMath

`src/runner/OcclusionMath.hpp/.cpp` holds pure functions, with no GL, that the shaders copy line for line:

- `QualitySettings(quality)`: half resolution or not, slices, and blur radius.
- `PixelRadius(radius, viewDepth, projectionScale, bufferHeight)`: capped at 25% of the height.
- `Falloff(distance, radius)`: 1 within 40% of Radius, 0 at Radius, smooth between.
- `ArcVisibility(n, h0, h1, projectedLength)`: the closed-form integral.
- `MultiBounce(ao, albedo)`: never below `ao`, and closer to 1 for brighter albedo.
- `SpecularOcclusion(ao, NdotV, roughness)`: 1 when `ao` is 1; smooth surfaces fall faster.

## Out of scope

- AO on see-through surfaces: the forward pass does not read it.
- Temporal accumulation, which waits for TAA.
- Visibility bitmasks.
- Bent normals.
- A depth mip chain.
- Depth-derived normals.
- An AO debug view.

The 2026-10-02 plan's "Deferred" section describes each.

## Tests

- `sandbox/ambient_occlusion_tests.cpp`:
  - defaults, clamping, refusal of values that are not finite, undo, save and load, Stop;
  - placement only under Lighting;
  - the first-in-tree rule, and falling through when it is removed;
  - the snapshot carries it;
  - scripts set every property, Quality by item, value, and name;
  - `Enum.EffectQuality` has Low, Medium, and High.
- `sandbox/occlusion_math_tests.cpp`:
  - the per-Quality settings;
  - the pixel radius cap;
  - the falloff ends and smoothness;
  - an open flat surface integrates to exactly 1, and a 90° corner to its known value;
  - multi-bounce never below the raw AO, with white losing less than black;
  - specular occlusion is 1 at AO 1, and falls faster when smooth.
- `tests/SceneRenderCheck.cpp`, a cube resting on a floor slab:
  - **Contact:** with AO, the floor next to the cube is darker than without, and the open floor far from it stays within ±2 of the frame without AO.
  - **No halos:** the floor seen just past the cube's top edge, far behind it, is not darkened.
  - **Camera close:** with the camera almost at a surface, no pixel is black from NaN.
  - **Off:** no instance, Enabled false, Intensity 0, and Radius 0 each give exactly the frame without AO.
  - **Quality:** Low, Medium, and High each darken the contact, and High and Medium agree on it within a tolerance.
  - **Buffers:** resizing and changing Quality make them again, the first frame draws, and there are no GL errors.
  - **SSR together:** with AO and SSR on, a mirror floor with nothing on screen to reflect matches AO alone exactly.
- `tests/SceneFeedTest.cpp`: `VisualAmbientOcclusion` rides through the feed.
- In the studio, screenshots without AO and at Medium and High, and the GPU time per Quality.
