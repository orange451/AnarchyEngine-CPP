# Shadows

2026-10-01 · Ports H1 (directional cascades) and H2 (spot and point shadows) from `docs/research/2026-09-30-legacy-renderer-port.md`, with I6 (real cascades: split along the view frustum, snapped to texels, normal-offset bias).

## Goal

Every light can cast shadows in the Scene View, and any number of lights can, not a fixed handful.

- **SpotLight:** one tile in a shared shadow atlas.
- **PointLight:** six tiles in the atlas, one per cube face.
- **DirectionalLight:** cascaded shadow maps that do not shimmer when the camera moves or turns.

Maps are cached and redrawn only when something they depend on changes. Each light's tile size follows how big the light looks on screen. The redraw work per frame is capped. Every rule about size is a ratio (Radius over distance, or a part of the Radius), never a stud count, so a place built at any scale gets the same shadows.

Mobile is not being built now. The design still avoids anything GLES 3.0 lacks, so a later iPhone port (through ANGLE or Metal) does not have to redo it.

## The instances

All of these are saved registry properties (`lua_saved_property`): save, undo, Stop-restore, Lua, completion, and the Properties panel come for free.

| Class | Property | Type | Default | Rule |
| --- | --- | --- | --- | --- |
| PointLight, SpotLight | Shadows | boolean | false | casts shadows when true |
| DirectionalLight | Shadows | boolean | true | casts shadows when true |
| DirectionalLight | ShadowDistance | number | 100 | studs from the camera that get shadows; not below 0; slider 0–1000 |

The defaults follow Roblox. They are not saved, so places that already exist pick up sun shadows.

## Data path

- `VisualLight` gains `shadows` and `shadow_distance`.
- `runner::LightDraw` gains `id`, `shadows`, and `shadowDistance`.
- `runner::MeshDraw` gains:
  - `owner`: the instance that draws it.
  - `revision`: a play-session mesh's upload revision, so a mesh re-uploaded in place invalidates the maps it is in.
- `GameView::collectMeshes` fills all of these.
- A light never shadows itself: meshes whose `owner` is the light's `id` cast nothing for it. See-through meshes (Transparency > 0) cast nothing.

## Rendering

### Frame order

All shadow maps are drawn first, then the G-buffer, then lighting.

On tile-based GPUs (every phone), switching framebuffers in the middle of lighting writes the lighting targets out to memory and reads them back. Drawing every map up front avoids that, and it costs nothing on desktop.

### Planning (`ShadowPlanner`)

`ShadowPlanner` makes every decision with no GL, so it is unit-tested in `sandbox`.

- **Priority.** Each PointLight and SpotLight is ranked by its reach: `tan(asin(Radius / distance)) / tan(FOV/2)`, which is infinite with the camera inside its Radius.
- **Tile size.**
  - The ideal is `reach × pane height × texelsPerPixel`, rounded up to a power of two and clamped to `minTile`–`maxTile`.
  - A light below `minReach` has no shadow.
  - A light keeps its size until the ideal leaves that size's band by 15%, so a light near a boundary does not flip between sizes from frame to frame.
- **Allocation.**
  - The atlas uses a quadtree buddy allocator. In priority order, each light gets tiles of its wanted size, or the biggest smaller size that fits.
  - A light whose wanted size has not changed keeps its tiles, even if they are smaller than wanted.
  - If any light got less than it wanted, the atlas doubles (from `atlasMinSize` up to `atlasMaxSize`, at most once per frame) and everything is reallocated.
- **Caching.**
  - Each light's map is fingerprinted with a 64-bit FNV-1a hash of its kind, position, direction, Radius, FOV, tile size, and every caster within its Radius (mesh, revision, transform).
  - A map whose hash matches is not redrawn.
  - A light without an instance id is never cached.
- **Hidden faces.** A cube face whose pyramid (out to Radius) lies wholly outside the camera's frustum is skipped and stays due. No visible point can sample it, so it is drawn when it comes into view. A SpotLight whose sphere is out of view is skipped the same way.
- **Redraw cap.**
  - Due lights are drawn lights-with-no-map first, then by priority, until `maxTexelsPerFrame` is spent. At least one light is drawn every frame.
  - A light that is not redrawn keeps the map it has, and the lookup uses the matrices that map was drawn with. A moved light's shadow lags a frame or two; it never misreads.
- **Commit after drawing.** The planner's decisions take effect only once the GL side reports the draws were made, so a frame that cannot draw (macOS readies programs a frame late) loses nothing.

### The atlas (`ShadowRenderer`)

- One 2D 24-bit depth texture, with hardware comparison and 2×2 filtering.
- Each tile is drawn with a viewport and scissor, and cleared with a scissored clear.
- PointLight faces are drawn wider than 90° (`faceScale = (size − 4) / size`), so the 3×3 filter taps near a face's edge read real depth. Hardware cube filtering is not needed, and there is no seam.
- The lookup:
  - picks the face from the dominant axis (GL's cube-face table, pinned by tests against the face matrices);
  - clamps the taps two texels inside the tile, so they never read another light's map;
  - turns the distance into the face's perspective depth.
- If the driver will not draw into the atlas, lights are drawn unshadowed, and this is said once.

### Cascades

- Four cascades over `ShadowDistance`, split practically (λ = 0.75), each fitted to the bounding sphere of its frustum slice.
- **Turning the camera:** the sphere's radius depends only on FOV, aspect and depths, so the projection's size never changes.
- **Moving the camera:** the light view only rotates, about the world origin, and the box center is floored to whole texels in that view. Moving the camera slides the map by whole texels.
- **Casters toward the light:** there is no `GL_DEPTH_CLAMP`, since GLES 3.0 lacks it. Instead each cascade's near plane is pulled toward the light far enough to include every caster whose sphere lies in its box's footprint. The pull is capped at 4 × ShadowDistance, so a huge mesh cannot ruin depth precision.
- **Caching:** the cascades are skipped when their matrices and casters hash the same as last frame, which is the common case while the camera is still.
- Only the first shadowed DirectionalLight gets cascades.
- **Lookup:** the first cascade whose box holds the point; it blends into the next over the outer 10% and fades to unshadowed past the last.

### Bias

- Normal offset of 1.5 texels in world space (scaled by distance for atlas tiles, per cascade for the sun).
- Polygon offset (1.5, 3) while drawing.
- Casters are drawn double-sided.

### Settings

`runner::ShadowSettings`, set with `Renderer::setShadowSettings`. Desktop only for now:

| Setting | Default |
| --- | --- |
| Atlas size | 1024², growing to 4096² |
| Atlas depth | 24-bit |
| Tile size | 64–1024 |
| Redraw cap | 8M texels per frame |
| Cascades | 2048 × 4 |

`MaterialBall`'s lights leave `shadows` false.

## GLES 3.0 rules

There is no mobile work now. These rules only keep a later port from having to redo the shadow code.

- No `GL_DEPTH_CLAMP`, `gl_ClipDistance`, geometry shaders, or cube-map arrays.
- Depth uploads use `UNSIGNED_INT` (24-bit) or `UNSIGNED_SHORT` (16-bit), never `FLOAT`.
- Shaders use only GLSL that GLSL ES 3.00 has: `sampler2DShadow`, `sampler2DArrayShadow`, `inverse`, `transpose`, and dynamic indexing of uniform arrays.
- The `#version` line stays `330 core` until a GLES port swaps it.

## Out of scope

- Shadows on see-through surfaces (forward pass).
- A per-object `CastShadow`.
- A Preferences shadow-quality page.
- Any mobile work: a GLES build, a phone settings tier, a 16-bit atlas, or fewer filter taps.
- Upgrading a downsized light's tiles when space frees up without its wanted size changing.
- A spatial index for the casters-per-light test, which is O(lights × casters) on the CPU.
- More than one shadowed DirectionalLight.
- PCSS soft shadows.
