# Frame time: 24 ms to under 12 in the 100k forest

**Goal:** the studio's Scene View in `H1Z1 Z1 Vegetation 100k`, at the user's worst view (eye about
(-1838, 265, -1767), looking down across the whole bowl) and the corner-to-corner view, at 10-15 ms
per frame or less, still and moving, on the RTX 3070, without visual loss, and scaling down to
low-end machines.

## Progress (2026-10-13)

The benchmark (`perf_bench.py` in the session scratchpad): the saved 100k place, Release, uncapped
(`JADEFX_MAX_FPS=0`), profiler overlay hidden, a pointer over the view; per paint.

| View | Before | Now |
|---|---|---|
| User's worst, still | 16.8 ms | 6.4-8.6 ms |
| User's worst, moving | 18.3 ms | 8.0-8.3 ms |
| Corner, still | 18.7 ms | 7.2-7.4 ms |
| Corner, moving | 22.3 ms | 7.3-7.7 ms |

Done:
- Phase 0: per-paint numbers in get_profile (`per_call_ms`, `paint`), scopes inside Batches and
  around the upload, mesh collection, GL state, the probe; render stats print upload bytes and cards.
- Phase 1: the card field (95bce546). 143k cards: Batches 8-11 to 0.05 ms, the 10 MB upload to 0.
- Found and fixed on the way: the cursor depth probe read the depth attachment into a pixel buffer,
  which made NVIDIA wait for the whole frame: 3.8 ms whenever the cursor was over the view. It copies
  the texel to a 1x1 float target and reads that behind a fence now (95bce546).
- The far corner draws: kSceneFar 4000 to 16384.
- Phase 2: the cascade plan returns at once when nothing changed (Shadows 0.8 to 0.03 ms still); asset
  files are watched on a thread (the twice-a-second 6-17 ms Snapshot read hitches are gone, max under
  2 ms); TextureCache lookups allocate nothing (Prefab draws 0.45 to 0.2 ms) (8d33543d). The profiler
  overlay draws its rectangles in one call (1.2-1.6 to 1.0 ms while shown) (253121c6).

Left, per paint, at the user's view moving: Visibility 0.5, Cascade plan while moving 0.4-0.7,
Terrain 0.4, Cards 0.3, Snapshot read 0.5, terrain and prefab draws 0.4, the JadeFX UI about 1.2 and
the frame tail 0.5-1.0, Swap about 1 (the present to the Intel adapter), GPU about 3.5.

## Where the time went (before)

Measured with the Release studio, focused, in the user's view. Per paint, not per engine tick: the
profiler's `avg_ms` is per engine tick, so per-paint cost is `avg_ms / calls_per_frame`.

| Per paint | ms |
|---|---|
| **Frame** | **23.9** (42 fps) |
| UI paint | 22.6 |
| - 3D scene | 18.6 |
| -- Batches (BuildBatches: key, sort, 116-byte InstanceData per visible draw) | **10.8** |
| -- unscoped GL, mostly the per-paint `glBufferData` of every visible instance (10-14 MB) | **~4.6** |
| -- Visibility + Frustum | 1.0 |
| -- Shadows (Cascade plan sweeps every caster, even when still) | 1.0 |
| -- Geometry + Terrain submission | 1.0 |
| - Snapshot read (avg; plus twice-a-second file-stat sweeps of +6-16 ms) | 1.2 |
| - Profiler overlay (while shown) | 1.5 |
| - rest of the UI | 1.2 |
| Swap (crosses to the Intel iGPU; stalls of 8-28 ms seen while moving) | 0.7 |
| GPU, in parallel (Terrain 2.1) | 3.6 |

So the frame is bound by CPU work on the UI thread, and about 15 ms of it is the per-instance path:
roughly 100k tree cards are keyed, sorted, written and uploaded again on every paint, even when
nothing moves. The GPU is mostly idle. Stack samples of a RelWithDebInfo build agree:
`BuildBatches`, `InstanceBuffer::upload` in the NVIDIA driver, worker-pool joins, and JadeFX's
one-draw-per-rectangle UI.

Measurement notes: JadeFX caps an unfocused studio at 30 fps by design, so a scripted studio paints
every 33.3 ms whatever it costs. Scripts must run with `JADEFX_MAX_FPS=0` or real focus. The
profiler overlay costs 1.2-1.5 ms while shown.

## Phase 0: measure what we change (half a day)

- `get_profile`: per-paint averages, paints per second, focus and the frame cap in effect.
- Scopes and counters:
  - "Instance upload", with a byte count.
  - Sub-scopes inside BuildBatches (keys, sort, runs, instances).
  - `AppendTerrainDraws`, SavedState and restore.
  - Export Renderer stats (draws, visible, runs) in the profile.
- Release PDBs (`/Zi` + `/DEBUG` with `/OPT:REF,ICF`), so stack samples have names.
- One script, `JADEFX_MAX_FPS=0`, overlay off, three runs each, warmed up:
  - the user's view and the corner view, still and moving;
  - the same views in AnarchyPlayer.

## Phase 1: cards off the per-instance path (1-2 days; about -11 to -13 ms)

Billboard bands are about 99.8% of visible draws in this place. Give them their own path:

- A persistent instance buffer per card slot: 32 bytes per card (origin, up axis times height,
  width, tint), written once and then updated only for the ranges the change notes name.
- The band test moves into `geometry.vert`: a card outside `[near, far)` collapses to nothing. The
  frustum test is left to the rasterizer.
- One draw per card slot, about 95 in this place. Later, texture arrays cut that to 1-3 draws.
- GL 3.3 only (macOS 4.1 too). Picking, outlines and shadows are unaffected; cards cast nothing.

**Expected result:** Batches 10.8 drops to about 0.1, the upload to about 0, and Frustum by 0.3. The
user's view goes from 23.9 to about **11-12 ms**, still and moving.

## Phase 2: the rest of the steady CPU cost and the hitches (1-2 days; about -3 to -5 ms)

| Change | Per paint |
|---|---|
| Cascade plan: return early when camera, sun and casters are unchanged; walk a caster grid when moving | -1.0 |
| Move file-stamp polling (TextureCache, MeshCache, Mesh::skeleton, terrain textures) to one background watcher | removes +6-16 ms spikes twice a second; -0.4 avg |
| Profiler overlay: graph bars and timeline as one batched draw | -1.2 to -1.5 while shown |
| Still camera: reuse visibility, batches and the instance buffer when nothing changed | most of what per-instance work is left |
| What stays per-instance (undergrowth Models, play mode): counting sort by slot instead of ParallelSort; one mapped write instead of `glBufferData`; a smaller InstanceData | -5 to -7 on today's path |
| Terrain: set per-Terrain state once, not per node; validate programs once; TextureCache stops querying GL when nothing landed | -0.3 to -0.6 |

**Expected result:** the user's view at about **7-9 ms**, with no once-a-second hitches.

## Phase 3: it just works for any project (several days)

The user's hand LODs bought a lot. These make that automatic:

- **Automatic far card.** Any Prefab with many instances and no Billboard band gets a baked card
  in the background (BillboardBaker), used where it shrinks under about 24-32 px. Explicit
  LodLayers win. Worth about 6 ms of GPU per paint in content like the tree-cards-only place.
- **Screen-size cull.** Draws under about 1 px fade out with a dither. No setting.
- **Spatial cells for Model-only props**, so CPU stays flat with instance count.
- **Terrain-horizon occlusion of cells.** It must be conservative: occluders use minimum heights.

## Phase 4: GPU floor and low-end (optional for the 3070)

These shorten the frame only once the CPU is under the GPU, but they are what low-end machines need:

- shadow cascades cached per cascade with about 10% slack;
- IBL, sun and merge fused into one pass;
- a sky lookup table;
- separate opaque and cut-out G-buffer programs;
- an automatic render scale with an FSR1-style upscale, used only when the GPU is over budget.

## Decisions for the user

1. **Save the undergrowth LODs into the project?** They only lived in the studio session. Saved,
   they take about 6 ms of GPU and 3 ms of CPU per paint off the place. Phase 3's automatic card
   makes this unnecessary later.
2. **Draw the far corner?** `kSceneFar` = 4000 clips about 19% of the map (about 20k trees) in the
   corner-to-corner view. After Phase 1 it costs almost nothing to extend.
3. **Free test:** plug the monitor into the 3070 for one run. The Swap stalls (8-28 ms while
   moving) come from the cross-adapter present.

## Order

Phase 0, then Phase 1, then measure. Then Phase 2's cascade early-out, file watcher and overlay, and
measure again. Phases 3 and 4 by the numbers after that.
