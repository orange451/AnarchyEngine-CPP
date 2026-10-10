# Texture Streaming Design

Date: 2026-10-09

## Goal

Get textures on screen fast, then sharpen them. Above all: **terrain must
never sit white while its textures load.** Low resolution for a while is fine;
white is not.

Scope is "option 1": a baked compressed mip cache plus progressive
small-to-large loading. Priority by screen size and a memory budget with
eviction are later work; the interfaces leave room for them.

Hard constraint: **OpenGL 3.3 core.** Everything below uses 3.3 core features
or `GL_EXT_texture_compression_s3tc` (universal on desktop, checked at startup
with an RGBA8 fallback). No BC7.

## 1. Texture.Streaming

New enum on `Texture` (`src/engine_instances/AssetInstances.hpp`), saved with
the place, shown in Properties:

```
Enum.TextureStreaming
  Automatic     -- default: smallest mips first, sharpens to full resolution
  AlwaysLoaded  -- stays on its placeholder until every mip is uploaded
```

Textures used by GUI never stream and are never compressed (raw RGBA8, no
mips), whatever the setting.

## 2. Baked format and cache

- `.atex` file: header (magic, format version, pixel format, size, level
  count, per-level offset and length), then levels **smallest first**, already
  compressed. Any level is one seek and one read.
- Path: `<project>/.cache/textures/<hash>.atex`, outside `resources/`, in
  `.gitignore`. The hash covers source paths, their file times, build settings
  (usage, size, terrain layer packing) and the format version. Editing a
  source changes the key and re-bakes. Orphans are safe to delete.
- Packaging copies `.cache/textures`; Player never bakes a texture the Studio
  already baked.
- Encoder: `rgbcx.h` (bc7enc project, MIT; confirm license before adding to
  `third_party`) for BC1/BC3/BC4/BC5.
- Without s3tc, the same cache stores RGBA8 (pixel format is part of the key).

## 3. What each texture bakes as

**Mesh/material textures (`TextureCache`).** `get()` gains a usage argument:

| Usage  | Format                                   |
|--------|------------------------------------------|
| Color  | BC1 if fully opaque, else BC3            |
| Normal | BC5 (XY; shader rebuilds Z)              |
| Gui    | RGBA8, no mips, not baked                |

Mips are built on the CPU by the baker; `glGenerateMipmap` is no longer used.

**Terrain layers.** `terrain::build_layer` is unchanged (Toksvig mips and the
height chain stay as they are); its output is compressed per mip:

| Array | Contents                     | Format |
|-------|------------------------------|--------|
| A     | color RGB + height           | BC3    |
| B     | normal XY                    | BC5    |
| C     | roughness + metalness        | BC5    |

3 bytes/texel instead of 8 (~2.7x less memory). The terrain shader samples one
more array.

## 4. Terrain first: no white

This section takes priority over everything else in the design.

1. **Layers publish one at a time.** `TerrainTextures` no longer waits for
   every layer of a request before publishing; each layer appears as soon as
   it has anything to show. This removes today's all-or-nothing wait.
2. **Parallel builds.** Layer work runs on the shared `TextureStreamer` pool,
   not the single `worker_` thread.
3. **Terrain jobs jump the queue.** Every terrain job outranks every mesh
   texture job, at each stage below.
4. **Warm path (cache exists):** read only each layer's levels at 64² and
   below (a few KB per layer) and upload them in the first frames. Terrain is
   textured, blurry, almost immediately. Larger levels follow.
5. **Cold path (no cache): a preview pass before the full bake.** For each
   layer, first job: decode only the diffuse source, box-downsample to 64²,
   build its tiny mip chain, upload it with flat normal / default
   roughness+metalness / 0.5 height. One decode per layer, layers in parallel,
   so the terrain shows its colors within roughly one image decode time. The
   full bake (all five sources, `build_layer`, compression, `.atex` write)
   runs after every layer's preview, and its levels replace the preview's.
6. **Pending layers draw neutral grey, not white.** Before a layer has even a
   preview, it draws as mid grey with flat normal, so an unloaded terrain
   reads as "loading", not as a white material. (Layer 0, the untextured
   default, stays white: that is its real look.)

Terrain arrays share one `GL_TEXTURE_BASE_LEVEL`. It is set to the finest
level every displayed layer has; layers sharpen together, level by level. A
layer whose full bake lands later is uploaded in full over a few frames.

## 5. Pipeline

- **`TextureStreamer`** (new, `src/runner`): worker pool of
  `max(1, cores - 2)` threads, one priority queue. Jobs are *preview* (terrain
  only), *bake* (decode, mips, compress, write `.atex`), or *read levels*
  (byte ranges from an `.atex`). Workers never touch GL.
- **GL thread:** each frame, uploads finished level data within ~16 MB, via
  `glCompressedTexSubImage2D/3D`, then lowers `GL_TEXTURE_BASE_LEVEL` to the
  lowest level uploaded. Textures are allocated with every level up front
  (`GL_TEXTURE_MAX_LEVEL` set), so uploads never reallocate.
- **Order:** terrain previews, then every texture's levels <= 64², then larger
  levels one at a time, smallest first, terrain before mesh textures.
- **Placeholders:** before any level arrives, `TextureCache::get` returns a
  1x1 texture: white for Color, flat for Normal.
- **AlwaysLoaded:** keeps the placeholder until all levels are uploaded, then
  swaps in. It still uses the cache.
- **Hot reload:** the once-a-second file-time check stays. A changed file is a
  new key; the old texture keeps drawing until the new one is ready.
- **Future hooks:** queue entries carry a priority (constant per job kind for
  now) and each texture a "wanted base level" (0 for now), for screen-size
  priority and eviction later.

## 6. Errors

- Corrupt or old-version `.atex`: delete and re-bake.
- Source fails to decode: today's once-per-file-version warning through
  `report`; placeholder stays.
- Cache folder not writable: bake in memory, upload anyway, warn once.

## 7. Testing

- `.atex` write/read round-trip; levels stored smallest first; cache key
  changes with file time and settings; corrupt-file fallback.
- BC round-trip error under a fixed threshold per format.
- Headless streamer tests: small levels before large; AlwaysLoaded never
  visible part-loaded; terrain jobs ahead of mesh jobs; on a cold cache every
  terrain layer gets a preview before any full bake starts.
- `TerrainTextures`: a layer publishes without waiting for the others.
- Live, Release, warmed up, H1Z1 demo:
  - Cold cache: terrain shows colors (no white) within ~0.5 s.
  - Warm cache: textured within a few frames, sharp in under 1 s.
  - Terrain VRAM ~2.7x lower than today.
