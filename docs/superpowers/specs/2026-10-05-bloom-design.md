# Bloom

2026-10-05 · Ports P2 (Bloom) from `docs/research/2026-09-30-legacy-renderer-port.md`, with I11 (a downsample/upsample mip chain, Jimenez 2014, instead of the legacy unthresholded 17-tap blur).

## Goal

A `BloomEffect` under `Lighting` spreads bright light into its surroundings. With its default Threshold of 0 it is a soft filmic haze: everything spreads a little, in proportion to its brightness. Raising Threshold turns it into glow on bright things only: emissive and neon parts, an HDR sky's sun, hot highlights. One pass serves both. A place with no BloomEffect looks and costs exactly what it does today.

## The instance

`BloomEffect` is an `Instance`, allowed only under `Lighting` (through Folders); `placement_error` refuses it anywhere else with "A BloomEffect must be in Lighting". With two under Lighting, the first in tree order is the bloom. Every property is a saved registry property (`lua_saved_property`), so save, undo, Stop-restore, Lua, completion, and the Properties panel come for free.

| Property | Type | Default | Rule |
| --- | --- | --- | --- |
| Enabled | boolean | true | false draws no bloom |
| Intensity | number | 0.05 | clamped to 0–1: how much of the image moves into the blurred copy |
| Size | number | 24 | clamped to 0–56: the spread, in pixels at a 1080-pixel-tall view |
| Threshold | number | 0 | clamped to 0–10: linear brightness where bloom starts, with a soft knee; 0 blooms everything |

A value that is not finite is refused. The names and Size's range follow Roblox's BloomEffect. Intensity and Threshold do not: our combine conserves energy (see below), so a haze-by-default look needs a small Intensity, and Roblox's Intensity 1 and Threshold 2 would mean something else here.

## Data path

- `SnapshotPump::resolve_lighting` fills `VisualSnapshot::bloom` (`VisualBloom`: `present`, `enabled`, `intensity`, `size`, `threshold`) from the first BloomEffect under Lighting, every Prepare, beside `VisualSky`. `find_skybox`'s walk becomes a template `find_first<T>` that both use.
- `GameView` copies it into `SceneLighting::bloom` (`SceneBloom`, the same fields as floats and a bool). No BloomEffect, `Enabled` false, Intensity 0, or Size 0 all mean the renderer skips bloom.
- The studio's Scene View and the player both draw the place through `GameView`, so both get bloom. The Assets pane's material balls set their own fixed lighting and stay without it.

## Rendering

### Where it runs

A new `Renderer::bloomPass()` runs after `mergePass()` and before the tone map, on the linear HDR merge image, so see-through surfaces and emissive glow bloom too. It is profiled as the `"Bloom"` pass. When bloom is off it does nothing, and the tone map gets Intensity 0 and a black texture.

### The chain

- RGBA16F textures with linear filtering and clamp-to-edge. The largest is half the pane's size (level 1); each after it halves again. The chain stops at 8 levels, or before a level would be under 2 pixels on either side.
- `BloomMath` (new, `src/runner/`, pure functions with no GL) turns Size and the pane's height into the number of levels used and the upsample filter's radius. Size is scaled from 1080 pixels to the pane's height. The chain goes down to the first level whose texel is at least that many pixels wide, and the tent radius is scaled so the spread changes smoothly between one level count and the next.
- Made on the first frame that blooms, and made again only when the pane's size changes. It is kept apart from `ensureTargets`, so a place with no bloom allocates nothing. `destroyTargets` and `shutdown` free it.
- If the driver refuses the chain, that is reported once, and frames draw without bloom; the draw does not fail.

### Down

`bloom_down.frag` takes Jimenez's 13-tap filter from one level into the next smaller one. The first step, from the merge image to level 1, also:

- applies the threshold with a soft knee (knee width = half the threshold), so light just under it fades in instead of popping on;
- weights each of the filter's five tap groups by 1 / (1 + luma) (a Karis average), so one very bright pixel cannot flicker as the camera moves.

### Up

`bloom_up.frag` reads the smaller level through a 3×3 tent at `BloomMath`'s radius and adds it (blend ONE, ONE) into the next larger level, which still holds its own downsample. This repeats from the smallest level used back up to level 1.

### Combine

`tonemap.frag` samples level 1 bilinearly before tone mapping:

```
rgb = scene + Intensity × (bloom − brightPart(scene))
```

where `brightPart` is the same knee as the threshold. This conserves energy in both modes:

- With Threshold 0, `brightPart(scene)` is the scene, and this becomes a plain mix toward the blurred image: the filmic haze.
- With a threshold, only the light above it moves into the halo. Everything below it is unchanged.

No extra full-size pass is needed.

### Edges

- Alpha is untouched. Where nothing was drawn, the merge image is premultiplied with alpha 0, so a halo spilling past an object's edge adds over the pane's clear color under the tone map's existing (ONE, ONE − SRC_ALPHA) blend. Far from any object the clear color is exact.
- Every bloom program asks `CanDraw` first, as the other passes do. Where it cannot draw yet (macOS readies programs a frame late), that frame is drawn without bloom, not failed, and bloom appears on the next.
- Nothing beyond GLES 3.0: RGBA16F render targets (which the other targets already use), separate textures per level rather than rendering into mip levels, and GLSL that GLSL ES 3.00 has.

## Out of scope

The tone-mapping curve (still Hable; replacing it is a decision of its own), lens dirt, lens flares, and the rest of P7, anamorphic streaks, per-object bloom masks, and auto exposure.

## Tests

- `sandbox/bloom_tests.cpp`: setters and clamping, refusal of values that are not finite, undo, save and load, Stop, placement (only under Lighting, through Folders), the first-BloomEffect rule, and scripts setting each property.
- `tests/BloomMathTest.cpp`:
  - Size to level count and tent radius, giving the same spread relative to the pane at 540, 1080, and 2160 pixels tall;
  - the 2-pixel and 8-level limits, and Size 0;
  - the knee: 0 below threshold − knee, continuous across the knee, and the identity well above the threshold.
- `tests/SceneRenderCheck.cpp`, with no GL errors throughout:
  - an emissive cube on an empty pane lights pixels just outside its edge with bloom, and not without;
  - with a high Threshold, a dim cube gets no halo while a bright emissive one does;
  - Threshold 0 keeps the image's total light within a tolerance of the image without bloom;
  - Enabled false and Intensity 0 each match the image without bloom exactly;
  - far from the cube the pane's clear color is exact;
  - resizing the pane makes the chain again.
- `tests/SceneFeedTest.cpp`: a BloomEffect's values reach `SceneLighting`, and removing it turns bloom off.
