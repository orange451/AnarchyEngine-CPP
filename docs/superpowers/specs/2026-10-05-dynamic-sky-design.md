# Dynamic sky

2026-10-05 · Ports S3 (Dynamic sky: atmosphere, sun, clouds) from `docs/research/2026-09-30-legacy-renderer-port.md`. Builds on the skybox and image-based lighting (`2026-10-01-skybox-ibl-design.md`), shadows (`2026-10-01-shadows-design.md`), screen-space reflections, and ambient occlusion (`2026-10-05-*-design.md`). The look follows two Shadertoys: the sun of https://www.shadertoy.com/view/3djSzz and the clouds of https://www.shadertoy.com/view/wslyWs.

## Goal

A `DynamicSky` under `Lighting` draws the sky with a shader instead of an image: an atmosphere that is blue at noon, warm at sunset, and dark at night, with drifting clouds, a sun by day, and a moon and stars by night. It drives its own directional light from the sun or moon, so one TimeOfDay property moves the light, its colour, and its shadows, and the sky's ambient light and reflections follow.

Unlike the research doc's recommendation (`Lighting.ClockTime`), the clock is the instance's own `TimeOfDay`. `Lighting.Brightness` stays as it is: unread.

Efficiency is a design rule:

- A place with no DynamicSky looks and costs exactly what it does today.
- The visible sky is drawn once per pixel at full resolution; it is never read back out of a cube.
- The lighting cube is redrawn only when the sky has changed: at once for a property change, otherwise at most four times a second while clouds drift. A still sky with no wind redraws nothing.
- The BRDF lookup table does not depend on the sky and is drawn once, as now.

## The instance

`DynamicSky` is a `DataModel`, allowed only under `Lighting` (through Folders); `placement_error` refuses it anywhere else with "A DynamicSky must be in Lighting". Every property is a saved registry property (`lua_saved_property`, with `lua_slider` where it has a range), so save, undo, Stop-restore, Lua, completion, and the Properties panel come for free. Setters run on the simulation thread and clamp or wrap as below; a value that is not finite is refused.

| Property | Type | Default | Rule |
| --- | --- | --- | --- |
| TimeOfDay | number | 14 | hours, wrapped into [0, 24): 25 is 1, −1 is 23 |
| Latitude | number | 35 | degrees, clamped to −90–90; tilts the sun's arc |
| Brightness | number | 3 | clamped to 0–20: the sun light's intensity; the moon light is Brightness × 0.1 |
| Shadows | boolean | true | whether the sun/moon light casts shadows |
| CloudCover | number | 0.5 | clamped to 0–1: how much of the sky has cloud |
| CloudDensity | number | 0.5 | clamped to 0–1: how thick and opaque the clouds are |
| WindDirection | Vector3 | (1, 0, 0.3) | the clouds' drift; its length is the speed in studs per second; its Y is ignored |
| SunTexture | Texture? | nil | when set, drawn in place of the procedural sun disc |
| MoonTexture | Texture? | nil | when set, drawn in place of the procedural moon disc |
| SunSize | number | 2 | degrees across, clamped to 0.1–20 |
| MoonSize | number | 2 | degrees across, clamped to 0.1–20 |
| ReflectionQuality | Enum.EffectQuality | Medium | the lighting cube's size (below); not the visible sky's |

`SunTexture` and `MoonTexture` are `InstanceRef`s to a Texture, held and resolved as `Skybox`'s Image is; anything else is refused with "SunTexture must be a Texture". A texture's alpha cuts its shape out of the sky; its colour is multiplied by the body's light colour, so a sun texture still reddens at sunset.

### Two skies

With both a Skybox and a DynamicSky under Lighting, the first in tree order is the sky; the other is ignored. The DynamicSky's light is added beside any DirectionalLight instances and is always first among the suns, so when it has Shadows it takes the shadow cascades.

## Sky math

`SkyMath` (new, `src/runner/`, pure functions with no GL) turns the properties into everything the CPU needs. The shader holds the same formulas for the per-pixel sky; the CPU copy is what the tests check.

### Axes

World +Y is up. The compass is fixed: north is −Z, east is +X, so south is +Z and west is −X. Directions here point *toward* the body, as `DirectionalLight.Direction` does.

### Sun and moon

There is no date: the sun's declination is 0 (the equinox), so it rises due east at 6:00 and sets due west at 18:00 at every latitude.

- Hour angle: H = (TimeOfDay − 12) × 15°.
- At the equator the sun goes around `p = (−sin H, cos H, 0)`: (+1, 0, 0) at 6:00, straight up at 12:00, (−1, 0, 0) at 18:00.
- Latitude φ rotates p about the X axis by φ (y' = y cos φ − z sin φ, z' = y sin φ + z cos φ), so the noon sun stands 90° − |φ| above the horizon, toward the south (+Z) for φ > 0 and the north (−Z) for φ < 0. At ±90° the sun circles on the horizon all day.
- The moon is opposite the sun: `moon = −sun`. It has no phases.
- The celestial pole is the same rotation applied to the north pole axis; the stars turn about it with the hour angle.

### The light

One directional light, `id` 0, from the sun while the sun is up and the moon otherwise:

- Each body's strength is a smoothstep of its elevation from 0° to 6°. The moon is opposite the sun, so at the crossing (the sun on the horizon) both are exactly zero and the light, and its shadow, never jump between them.
- Colour: the sun's light is its transmittance through the same atmosphere the shader draws (Rayleigh and Mie extinction along the path at that elevation), so it yellows and reddens toward the horizon. The moon's light is a fixed cool white (0.75, 0.82, 1.0) times its transmittance.
- Intensity: Brightness for the sun, Brightness × 0.1 for the moon, times the elevation fade, times a cloud dimming of `1 − 0.7 × CloudCover × CloudDensity`.
- Shadows: the instance's Shadows, with `ShadowDistance` 100, as `DirectionalLight`'s default.
- Star visibility: a smoothstep of the sun's elevation from −2° down to −12°.

## Data path

- `SnapshotPump::resolve_lighting` walks Lighting in tree order for the first Skybox or DynamicSky (`find_first` taking both types). A Skybox fills `VisualSky` as now; a DynamicSky fills a new `VisualDynamicSky` (`present` and every property, with the two texture paths resolved). A sandbox test checks `VisualDynamicSky{}`'s defaults against `DynamicSky::kDefault*` (it holds strings, so it cannot be a `static_assert` as the effects' are). `blit` and `SceneFeed` copy it.
- `GameView` calls `SkyMath` and:
  - fills `SceneSky`'s new procedural part: the sun and moon directions, the light colour, star visibility and pole, CloudCover, CloudDensity, the wind, the sizes, the two textures (from `textures_`, as the Skybox image is), ReflectionQuality, and `seconds`;
  - puts the sky's light at the front of `lightDraws_` with `id` 0, skipped when its intensity is 0.
- `seconds` is wall-clock time from a steady clock held by GameView, so clouds drift in the studio's edit mode too, and in a paused game. Only clouds use it; TimeOfDay never moves on its own. GameView turns it into the clouds' offset, `wind × seconds` in studs, worked out in double and wrapped every 100 000 studs (a jump in the clouds once in many hours), so the shader's floats stay precise.
- The studio's Scene View and the player both draw through `GameView`, so both get the dynamic sky. The Assets pane's material balls keep their own fixed lighting.

## Rendering

### Shaders

- `procedural_sky.glsl` (library): `proceduralSky(dir, detail)` returns the sky's linear radiance in a world direction; `detail` is false for the lighting cube. It holds:
  - single-scattering Rayleigh and Mie atmosphere with the sun's glow (after 3djSzz), lit by the moon at night at a much lower level (`kMoonSkyRadiance`, 3% of the sun's);
  - at and below the horizon, the atmosphere and the moon's glow use the horizon in the same azimuth, not `dir` itself, so a column straight down does not fade toward the ground's own colour; straight down, with no azimuth, uses due south (0, 0, −1). A separate ground darkening (unchanged) dims the result below the horizon;
  - the stars, the sun and moon discs, and the moon's glow are only added above the horizon, through a `smoothstep(-0.002, 0.002, dir.y)` fade: without it, every ray below the horizon shares one direction's worth of sky and would smear the sun, moon, and stars straight down the ground;
  - fbm clouds on a plane above the camera (`kCloudHeight` 200 studs up, `kCloudFeature` 120 studs across a cell), offset by `wind × seconds`, thresholded by CloudCover, opacity and self-shadowing by CloudDensity, lit by the current light colour (after wslyWs);
  - stars: a hashed cell field in sky coordinates turned about the pole, faded in by star visibility and hidden by cloud;
  - the sun and moon: a disc of SunSize / MoonSize with limb darkening and a halo, or the texture on a quad facing the viewer at that angular size; drawn behind the clouds.
- `dynamic_sky.frag` draws the visible sky: full screen into the accumulation buffer where depth is 1, as `sky.frag` does, clamped to `kMaxHalf`. Profiled as the existing `"Sky"` pass.
- `dynamic_sky_cube.frag` draws one face of the lighting cube with the same `proceduralSky`, without the sun and moon discs (their glow stays) or the stars: the built-in light already gives the sun's highlight, a disc in the cube would add it twice, and a cube face holds no useful star field at this size anyway. So the textures never reach the cube.

The sun and moon textures are bound to the Material units 0 and 1 (`kUnitDiffuse`, `kUnitNormalMap`): the sky pass reads no Material.

### Shading stays the same

Everything after the cubes reads only `bindSky`'s uniforms and units, and keeps doing so: `uSkyEnabled` is 1, `uSkyColor` and `uSkyLightScale` are 1, `uViewToSky` is the view-to-world matrix with no rotation. This is a hard rule: `merge.frag` recomputes the sky's reflection from `uPrefiltered`, `uSkyColor`, and `uSkyLightScale` to swap in screen-space reflections, and ambient occlusion multiplies the same terms. With a DynamicSky, `image_lighting.glsl`, `ibl.frag`, `forward.frag`, `ssr.frag`, and `merge.frag` are unchanged. `kUnitSky` gets the white stand-in, as with no sky.

### The lighting cube

`EnvironmentMap` gains a second way in beside `update(image, revision)`: `updateProcedural(drawFace, size, revision)`, where the renderer draws the environment cube's faces with `dynamic_sky_cube.frag` and the map then mipmaps it and runs its existing irradiance and prefilter passes on it.

Sizes by ReflectionQuality:

| ReflectionQuality | Environment cube | Prefiltered (top level) | Irradiance |
| --- | --- | --- | --- |
| Low | 128 | 128 | 32 |
| Medium | 256 | 256 | 32 |
| High | 512 | 512 | 32 |

The prefiltered cube keeps 6 levels at every size. A Skybox keeps today's 512 / 256 / 32. Sizes are fixed when the cubes are made; a change of ReflectionQuality, or a change between Skybox and DynamicSky, makes them again.

When it is redrawn:

The renderer decides, with `SkyMath`'s `LightingDue`, from what the cube was last drawn with (TimeOfDay, Latitude, CloudCover, CloudDensity, ReflectionQuality) and when:

- the first time, and at once when ReflectionQuality changes (the cubes are made again);
- when another of those changed, at least 0.05 s after the last redraw, so a script moving TimeOfDay every frame costs at most 20 redraws a second;
- otherwise, while the wind's length is not 0, when 0.25 s of `seconds` have passed since the last redraw;
- never otherwise.

The redraw is profiled as a `"Sky lighting"` pass. The visible sky is not affected by the throttle: between redraws only the clouds' reflections and ambient lag, by at most a quarter second.

### Failure

If the procedural programs fail to build, that is reported once and the frame draws as with no sky at all: no visible sky, and the stand-in ambient and black reflections in its place. The DynamicSky's light (the sun or moon) is driven from TimeOfDay directly, not the shader, so it keeps lighting the scene even then — only the sky itself, its ambient, and its reflections are lost. A missing or unloadable SunTexture or MoonTexture draws the procedural disc instead of failing to build.

If the programs build but the lighting cube cannot be made — the first frame, before it ever has been, or any frame a redraw fails partway — the frame does not fail, unlike a Skybox's unready cubes today: the visible sky still draws every such frame, straight from the shader, same as when the cube is ready. Only what a surface takes from the cube (its ambient and reflections) falls back to the stand-in, the same as with no sky, until a later redraw succeeds.

`EnvironmentMap::updateProcedural` clears its "holds a procedural sky" flag before it draws a single face, and only sets it again once every face, the mipmap, and the filter all succeed. So a redraw that fails partway — one face's program fails to link, say — drops the lighting cube's sky for that frame, the same as it never having been made: the renderer reads `holdsProcedural()` as false and gives surfaces the stand-in ambient and black reflections until a later redraw succeeds. The visible sky (drawn straight from the shader, not the cube) and the DynamicSky's light are unaffected either way.

## Registration

As the effects under Lighting were registered:

- `CMakeLists.txt`: `DynamicSky.cpp` in `engine_instances`, `SkyMath.cpp` in the runner, the shaders copied with the others, and the new tests.
- `Project.cpp` `class_registry`; `ScriptBindings.cpp` `create_dynamic_sky` and `register_lua_creatable`.
- `Containment.cpp`: DynamicSky joins the classes that must be in Lighting.
- `LuaApi.cpp` `build_docs()`: one entry per property.
- `src/ide/ClassOrder.hpp`: rank 4, with Skybox and the effects.
- `src/ide/IdeIcons.cpp`: an override from DynamicSky to `icon-sky.png`.
- `README.md` and `src/engine_instances/README.md`.

## Out of scope

- Seasons and dates (declination is always 0), and Longitude.
- Moon phases.
- Fog and aerial perspective on the scene's objects.
- Cloud shadows moving over the ground; clouds only dim the light as a whole.
- A day–night clock that runs on its own; scripts animate TimeOfDay.
- Reading or removing `Lighting.Brightness`.

## Tests

- `sandbox/dynamic_sky_tests.cpp` (Catch2, `[dynamic_sky]`):
  - DS1 each property's default, clamp, wrap (TimeOfDay), and refusal of non-finite values; undo, save and load, and restore at Stop;
  - DS2 placement: allowed in Lighting and Folders under it, refused elsewhere;
  - DS3 the snapshot carries the first DynamicSky under Lighting, with every value;
  - DS4 a script makes a DynamicSky and sets it, and the snapshot follows;
  - DS5 the first Skybox or DynamicSky in the tree is the sky, in both orders.
- `sandbox/sky_math_tests.cpp` (Catch2, `[sky_math]`):
  - SM1 at the equator the sun is overhead at noon and on the horizon at 6:00 and 18:00, at +X and −X;
  - SM2 Latitude sets the noon sun's height to 90° − |Latitude|, south for positive Latitude and north for negative;
  - SM3 the stars' frame keeps the sun straight up and the pole at −Z;
  - SM4 transmittance is white overhead and redder toward the horizon;
  - SM5 fades, star visibility, and cloud dimming at their ends;
  - SM6 the light comes from the sun by day and the moon by night, and is continuous across sunrise and sunset (the moon is opposite the sun);
  - SM7 the light reddens at a low sun, and the discs hide below the horizon;
  - SM8 the lighting cube's sizes and when it is drawn again (`LightingDue`'s 0.05 s and 0.25 s).
- `tests/SceneFeedTest.cpp`: `VisualDynamicSky` is copied.
- `tests/LuauCompleteTest.cpp`: DynamicSky in the rank-4 row of the Insert list, between BloomEffect and ScreenSpaceReflections.
- `scene-render-check`: a DynamicSky, drawn with a test clock that advances a second per draw (so clouds drift deterministically and `LightingDue`'s throttle is exercised) rather than wall time:
  - the sky pixels are bright and blue at noon and dark at midnight;
  - straight down reads as dark ground, not NaN;
  - at sunset, with the sun on the horizon due west, the ground below the sun matches the ground beside it, within tolerance — the horizon-azimuth fix above keeps the disc from smearing down the column;
  - a cube lit by the sky is lit far more at noon than at midnight, and redraws at once on a ReflectionQuality change;
  - a polar sky (Latitude 90, sun on the horizon all day) still draws;
  - after a DynamicSky, a plain Skybox draws again from its own (non-procedural) cubes.
