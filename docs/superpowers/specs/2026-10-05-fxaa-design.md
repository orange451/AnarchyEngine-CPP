# FXAA

2026-10-05 · Ports P3 (FXAA) from `docs/research/2026-09-30-legacy-renderer-port.md`. Builds on bloom (`docs/superpowers/specs/2026-10-05-bloom-design.md`), whose tone map it follows.

## Goal

The Scene View and the player draw the 3D scene without jagged edges, at a cost of a fraction of a millisecond. `Lighting.Antialiasing` picks the method. `FXAA` is the default, and `None` draws exactly what the renderer draws today. The editor's grid, outlines, and Dragger handles, and the GUI, stay crisp: they draw after it.

Efficiency is a design rule. FXAA is one full-screen pass at the pane's size, and most pixels leave it after a few texture reads.

## The property

`Enum.AntialiasingMode` is a new enum: `None` (0) and `FXAA` (1). Later methods, such as TAA (P6) or SMAA, are new entries.

| Property | Type | Default | Rule |
| --- | --- | --- | --- |
| `Lighting.Antialiasing` | `Enum.AntialiasingMode` | `FXAA` | a value that is not an `Enum.AntialiasingMode` is refused with "Antialiasing must be an Enum.AntialiasingMode" |

It is a saved registry property (`lua_saved_property`): save, undo, Stop-restore, Lua, completion, and the Properties panel come for free. Its default is not saved, so places that already exist get FXAA.

## Data path

- `VisualLighting` gains `antialiasing`, filled by `SnapshotPump::resolve_lighting` as `Exposure` is. The static_assert that ties `VisualLighting`'s defaults to `Lighting`'s covers it.
- `SceneLighting` gains `antialiasing`, defaulting to FXAA like a new Lighting. `GameView` copies it. The Assets pane's material balls use `SceneLighting`'s defaults, so they get FXAA too.

## Rendering

### With FXAA

1. The tone map draws into a new pane-sized RGBA8 target (`ldrTexture_`, linear filtering), not onto the pane. The target is first cleared to the pane's clear color, opaque, and the tone map blends onto it with the same (ONE, ONE − SRC_ALPHA) blend it uses on the pane today. So the target holds exactly what the pane would have shown.
2. `fxaa.frag` reads the target and draws onto the pane, opaque, with blending off, in the pane's viewport and its parent's scissor clip.
3. The grid, outlines, and handles draw on top, as now.

### The filter

`fxaa.frag` is FXAA 3.11 (Timothy Lottes), PC quality preset 12. Luma is computed in the shader from the sRGB-encoded color (green-weighted), so no extra channel is needed. Pixels whose local contrast is below the edge thresholds (`edgeThreshold` 0.166, `edgeThresholdMin` 0.0833) return their own color unchanged. So flat areas, including the empty pane, are untouched exactly, and the tone map's ±0.5/255 dither is never treated as an edge.

### With None

The tone map draws onto the pane exactly as today: the same pixels and no extra pass.

### Buffers and failure

- `ldrTexture_` is made in `ensureTargets` with the other buffers and resized with them: 4 bytes a pixel.
- If the driver refuses it, the existing all-or-nothing rule of `ensureTargets` applies.
- When the FXAA program or the tone map into `ldrTexture_` cannot draw yet (macOS readies programs a frame late), the frame takes the direct tone-map path. It is not antialiased, but it is never blank.
- The depth probe, `sceneDepth()`, and `read()` are unchanged.
- The pass is profiled as `"FXAA"`.

## Out of scope

TAA, SMAA, and MSAA; antialiasing the grid, outlines, handles, or GUI; a sharpness or quality setting.

## Tests

- `sandbox/antialiasing_tests.cpp`:
  - `Enum.AntialiasingMode` has `None` and `FXAA`;
  - `Lighting.Antialiasing` defaults to `FXAA`, is not saved at its default, saves and loads when changed, undoes, and comes back at Stop;
  - other values are refused with the message above;
  - scripts read and set it;
  - the snapshot carries it.
- `tests/SceneFeedTest.cpp`: `antialiasing` rides through the feed.
- `tests/SceneRenderCheck.cpp`, with a cube turned 30° so its silhouette has slanted edges:
  - with `None`, edge pixels along a row are mostly hard (cube or clear); with `FXAA`, clearly more are in between;
  - the cube's flat interior and the far corner match `None` exactly, and the corner is exactly the clear color;
  - the grid drawn over the scene is identical with and without FXAA;
  - the first frame after the target is made draws a non-blank pane;
  - resizing makes the target again;
  - no GL errors.
- The existing render checks now run with FXAA on, since it is the default. One that fails at an edge pixel is judged per check: a real regression is fixed, and a check that assumed hard edges is changed to set `None`. Each such call is recorded.
