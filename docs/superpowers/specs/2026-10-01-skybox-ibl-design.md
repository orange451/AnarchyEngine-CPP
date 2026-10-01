# Skybox and image-based lighting

2026-10-01 · Ports S1 (Skybox) and S2 (image-based lighting) from `docs/research/2026-09-30-legacy-renderer-port.md`, with I8 (rebuild the environment only when the sky changes).

## Goal

A `Skybox` under `Lighting` puts an HDRI (or an ordinary image) around the place. The Scene View draws it behind everything, and the PBR pipeline is lit by it instead of the flat stand-in sky. Poly Haven's `.hdr` downloads work as they are.

## The instance

`Skybox` is an `Instance`, allowed only under `Lighting` (through Folders); `placement_error` refuses it anywhere else with "A Skybox must be in Lighting". Every property is a saved registry property, so save, undo, and Stop come for free.

| Property | Type | Default | Rule |
| --- | --- | --- | --- |
| Image | Texture? | nil | the sky; nil draws none |
| Exposure | number | 1 | clamped to 0–10 |
| Rotation | number | 0 | degrees about world Y, wraps into 0–360 |
| Tint | Color3 | white | sRGB, made linear by the renderer |
| Reflections | Texture? | nil | reflected in place of Image when set |

A value that is not finite is refused. With two Skyboxes under Lighting, the first in tree order is the sky.

## Data path

- `SnapshotPump::resolve_lighting` fills `VisualSnapshot::sky` (`VisualSky`) from the first Skybox, every Prepare, as it does Lighting. It sits beside `VisualLighting`, not in it, so `VisualLighting` stays a literal type.
- `SceneFeed` copies `lighting` and `sky` to the view. It copied neither before, so Lighting's Ambient, Exposure, Saturation, and Gamma never reached the studio's Scene View; that is fixed too.
- `TextureCache::getEnvironment` decodes with `stbi_loadf`: `.hdr` stays linear, other formats go through gamma 2.2. Values are clamped to 0–65000 (half float), images wider than 4096 px are halved, and the upload is RGBA16F (RGB16F need not be renderable or mipmappable). Each upload gets a never-reused revision. `.exr` reports that OpenEXR is unsupported.

## Rendering

- `EnvironmentMap` (new, `src/runner/`) builds, when an image revision changes: a 512² environment cube from the equirect, a 32² irradiance cube (cosine-weighted, 512 samples), a 256² prefiltered cube over 6 mips (GGX, 256 samples), and once a 256² BRDF table. Sampling is mip-filtered (Colbert and Křivánek) so a bright sun does not sparkle. With Reflections, Image gives the irradiance and Reflections the prefiltered cube. Every pass checks `CanDraw` first and retries next frame on macOS.
- `image_lighting.glsl`'s `skyLight` replaces the legacy `ambientLight` in `ibl.frag` and `forward.frag` when a sky is present: split-sum IBL, F0 = 0.16 × Reflectivity² for dielectrics, plus `Ambient × albedo / π` as a flat fill. With no sky, the legacy term is unchanged.
- `sky.frag` draws the equirect where depth is 1 into the accumulation buffer, so the sky is tone mapped with everything else. Its mip is chosen with Tarini's two-u trick, so the wrap line shows no seam. `merge.frag` treats sky pixels as opaque.
- Exposure, Tint, and Rotation are uniforms (`uSkyColor`, `uViewToSky`), so changing them never rebuilds the cubes.
- With a sky, the pipeline runs even with no meshes. A sky whose cubes can never be made (driver refusal) is drawn as no sky.
- Unused sky samplers get a white 2D texture and a black 1×1 cube, which keeps macOS's driver from logging unloadable textures.

## Out of scope

Six-image cubemap skies, EXR, the Assets pane's material balls using the place's sky, and a dynamic sky (S3).

## Tests

- `sandbox/skybox_tests.cpp`: setters, undo, save, Stop; placement; the snapshot's first-Skybox rule; scripts.
- `tests/SceneRenderCheck.cpp`: HDR decoding and halving, revisions, the `.exr` message, the backdrop's orientation, Rotation, Exposure, Tint, diffuse lighting from above and below, and Reflections, with no GL errors.
- `tests/SceneFeedTest.cpp`: lighting and sky reach the view.
