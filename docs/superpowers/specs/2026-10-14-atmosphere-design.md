# Atmosphere — design

Reference: https://www.shadertoy.com/view/WlSSzK (Rayleigh, Mie, ozone, scattering in front of scene geometry).

## Goal

A new `Atmosphere` instance gives the scene physically based atmospheric scattering:

1. **Aerial perspective**: distant geometry fades into haze and picks up the air's color. The renderer has no fog today.
2. **Ozone absorption**: a deeper blue at the zenith and purple twilight.
3. **Tunable air**: the scattering constants in `procedural_sky.glsl` become properties.

It works with a DynamicSky, with a Skybox, or with no sky at all.

## Instance

`Atmosphere : DataModel`, under Lighting at any depth through Folders. Add it to the allowed list in `Containment.cpp`. The first one in the tree is used. Every property is a saved registry property (lua_saved_property), and setters refuse values that are not finite and clamp the rest.

| Property | Type | Meaning | Default |
|---|---|---|---|
| Density | number | Multiplies all scattering. 0 means no effect | 1 |
| Haze | number | Mie (aerosol) amount | 0.3 |
| HazeAnisotropy | number | Mie g, 0 to 0.99 | 0.76 |
| RayleighColor | Color3 | Rayleigh coefficients, normalized | Earth |
| Ozone | number | Ozone absorption strength | 1 |
| Scale | number | Atmosphere meters per unit; exaggerates distance so haze shows at game scale | tune on real places |
| SeaLevel | number | World Y of the atmosphere's ground | 0 |
| Color | Color3? | When set, overrides the haze color (art direction) | nil |
| PlanetRadius, AtmosphereHeight | number | Advanced | 6371e3, 100e3 |

Distances are "units".

## What lights it

| Setup | Haze on geometry | Sky scattering | Light source | Haze color |
|---|---|---|---|---|
| DynamicSky | Yes | Yes, from the Atmosphere's values | DynamicSky's sun or moon | Physical |
| Skybox | Yes | No, the image is drawn as-is | First DirectionalLight | Pulled toward the sky's horizon color, sampled from the IBL cube |
| No sky | Yes | No | First DirectionalLight, else a fixed overhead sun | Physical |

`Color`, when set, wins in every case.

When a DynamicSky is present, `proceduralSky()` and `SkyMath.hpp` read the Atmosphere's values instead of their constants, so the sky, the sunlight and the haze always agree. With no Atmosphere, they keep today's constants unchanged.

## Rendering (Hillaire 2020 LUTs, aimed at low-end hardware)

1. **Transmittance LUT**: 256×64 RG16F. Rebuilt only when Atmosphere values change.
2. **Aerial-perspective froxels**: 32×32×32 RGBA16F over the camera frustum. rgb holds in-scatter, a holds transmittance. Filled once per frame with about 32 steps per column.
3. **Apply**: one fullscreen pass after the merge, before transparency and tonemap: `color = color * T + inscatter`, with one 3D fetch keyed on depth. Skipped for sky pixels when a DynamicSky draws them; with a Skybox, the sky pixels are not hazed.
4. **Forward draws** (transparent surfaces, particles) apply the same fetch in `forward.frag` and `particle.frag`.
5. **Later**: a sky-view LUT for `dynamic_sky.frag`.

Budget: at most about 0.3 ms at 1080p on the UHD 770, measured warmed-up, Release against Release. Density 0 or no Atmosphere costs nothing (no passes run).

## Plumbing

- `VisualAtmosphere` row in the render snapshot (`SnapshotPump`), read at every Prepare.
- Renderer: the transmittance-LUT and froxel programs, the apply program and FBO next to `merge_`.
- Shared `atmosphere_common.glsl`: ray-sphere test, densities, phase functions.

## Steps

1. Instance, properties, Lua bindings, Containment, save.
2. Snapshot row and Renderer plumbing.
3. Turn `procedural_sky.glsl`'s constants into uniforms and add ozone. Defaults must be identical to today (before/after screenshot).
4. `atmosphere_common.glsl`, the transmittance LUT, the froxels, the apply pass.
5. Fetch in the forward and particle shaders.
6. Skybox horizon-color tint, and the `Color` override.
7. `SkyMath.hpp` respects the Atmosphere.
8. Verify in Studio: noon, sunset and night; Skybox and no-sky; large terrain; Density 0 matches having no Atmosphere; profile against main.

## Open

- Should haze also shade the cloud layer and the sun disc? For now: the clouds yes (they are inside the air), the discs no.
