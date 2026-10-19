# Atmosphere — design

Revised 2026-10-19: Godot-style analytic fog instead of Hillaire LUTs and froxels. The physical version is parked under "Later".

References:
- Godot 4 `Environment` fog: exponential height fog, Sun Scatter, Aerial Perspective, Sky Affect.
- Unreal's ExponentialHeightFog: the closed-form integral of height fog along a ray.

## Goal

A new `Atmosphere` instance makes distant geometry fade into air whose color follows the sky. It also adds a glow toward the sun and a hazy horizon on the sky itself. The renderer has no fog today.

It works with a DynamicSky, with a Skybox, or with no sky at all. It adds no render pass and no texture. With no Atmosphere, or with Enabled false or Density 0, every frame is pixel-identical to today's.

## Instance

`Atmosphere : DataModel` lives under Lighting, at any depth through Folders. It is added to the allowed list in `Containment.cpp`, and the first one in the tree is used. Every property is a saved registry property (`lua_saved_property`). Setters refuse values that are not finite and clamp the rest.

| Property | Type | Meaning | Range | Default |
|---|---|---|---|---|
| Enabled | boolean | false draws no atmosphere | | true |
| Density | number | How thick the air is at BaseHeight. 0 means no effect | 0 to 1 | 0.25 |
| HeightFalloff | number | How fast the air thins going up, per unit. 0 means the same everywhere | 0 to 0.2 | 0.01 |
| BaseHeight | number | World Y where the air is Density thick | any | 0 |
| Color | Color3 | The air's own color, sRGB | | (0.75, 0.8, 0.88) |
| AerialPerspective | number | How much the air takes the sky's color in that direction instead of Color | 0 to 1 | 0.5 |
| SunScatter | number | How strong the glow toward the sun (or moon) is, in that light's color | 0 to 1 | 0.25 |
| SkyHaze | number | How much the air hazes the sky drawn behind everything | 0 to 1 | 1 |

Distances are "units". Density maps to an extinction per unit, `Density * kDensityScale`. `kDensityScale` (0.016 per unit) lives in `AtmosphereMath.hpp`; it was tuned on Bald Eagle Valley so that the default looks like a clear day.

## The model

For a ray from the camera at height `h0 = cameraY - BaseHeight`, with world-space direction `v` (`dy = v.y`) and length `d`:

- density along the ray: `σ(y) = σ0 * exp(-f * y)`, with `σ0 = Density * kDensityScale` and `f = HeightFalloff`;
- optical depth: `τ = σ0 * exp(-f*h0) * d * g(f*dy*d)`, where `g(x) = (1 - exp(-x)) / x` and `g(x) → 1 - x/2` for `|x| < 1e-4`;
- transmittance: `T = exp(-τ)`. The exponent `-f*h0` is clamped to at most 80, so a camera far below BaseHeight does not overflow.

The air's color `C` is built in three steps:

1. Start from `Color`, made linear.
2. Mix it toward the sky's prefiltered cube in direction `v`, by `AerialPerspective`. The cube's mip is `maxLod * T`, so nearby air is the sky's blurry average and far air is the sharp horizon. This step happens only when a sky's cube is ready; otherwise C stays `Color`.
3. Add `lightColor * SunScatter * pow(max(dot(v, L), 0), 8)`, where L points toward the light.

A surface's light becomes `color * T + C * (1 - T)`.

**Which light L is:**
- with a DynamicSky, its `lightDirection` and `lightColor` (the sun, or the moon at night);
- otherwise, the first DirectionalLight (color times intensity);
- otherwise, no glow.

## Where it applies

| What | Where | How |
|---|---|---|
| Opaque surfaces and terrain | `merge.frag`, before the see-through layer goes over | depth → view position → the formula |
| Sky pixels (Skybox or DynamicSky, clouds included) | `merge.frag`, in the `depth >= 1` branch | the same formula with `d = kSkyDistance`, mixed in by SkyHaze. Height falloff keeps the zenith clear and the horizon hazy |
| See-through surfaces | `forward.frag` | the same, before alpha |
| Particles | `particle.frag` | **Normal:** fog the color. **Additive:** multiply by T, adding no air light. **Multiply:** multiply alpha by T |
| Grid, outlines, handles, GUI | not fogged | |

All of these share one include, `atmosphere.glsl`. It declares its own uniforms and its own cube sampler (`uAtmosphereSky`), so it does not depend on `image_lighting.glsl`, and `ParticleRenderer` can build with it as its only include.

## Plumbing

- `VisualAtmosphere` row in the render snapshot (`SnapshotPump`), filled at every Prepare from the first Atmosphere under Lighting. `SceneAtmosphere` lives in `SceneLighting`, and `GameView` maps one onto the other. Unlike bloom, the light it uses (DynamicSky or DirectionalLight) is resolved in the Renderer.
- `Renderer::atmosphereUniforms()` works out the per-frame values once:
  - σ0 and f;
  - the camera's height above BaseHeight;
  - world up in view space;
  - L in view space, and the light's color;
  - the cube's sky rotation and max LOD;
  - whether the cube is ready.

  `bindAtmosphere(program)` uploads them for merge, forward and particles.
- The CPU mirror of the formula is `src/runner/AtmosphereMath.{hpp,cpp}`, for tests and tuning.

## Budget

At most 0.1 ms at 1080p on the UHD 770, measured warmed-up and Release against Release. That is two `exp` calls and one cube fetch per pixel in a pass that already runs. Measured 2026-10-19 on Bald Eagle Valley in Studio (the GPU Studio renders on): the merge pass went from 0.062 to 0.066 ms with the air on; the see-through pass did not change measurably.

## Later

- A froxel volume (Hillaire aerial perspective, or a lit volumetric fog grid) as a higher-quality setting, for light shafts and local fog volumes.
- Feeding Rayleigh, Mie and ozone values into `procedural_sky.glsl`, so the DynamicSky's air is tunable.
