# Terrain Streaming — Design

## Problem

The H1Z1 Z1 project's terrain (`Z1.*.avox`, 104 MB on disk, 55,359 chunks — 13.5× the
4,096-chunk budget in the terrain core spec) drives Studio to tens of GB of RAM on open:

1. `Terrain::read_data_file` decodes every chunk to its dense 64 KB form (~3.6 GB) and
   keeps each chunk's zstd frame too.
2. `TerrainWorld` queues every chunk for meshing on first sight; in-flight jobs pin
   neighbour chunks and meshes accumulate during the build.
3. LOD levels 2+ for the whole map stay in RAM, and are rebuilt on every open.

Textures are not the problem (34 PNGs, 11.5 MB).

## Goal

Terrain uses **at most ~2 GB of RAM** for any map, including H1Z1 Z1, on both a cold open
(no mesh cache) and a warm open. Near terrain appears first; far terrain may fill in over a
few seconds. Editing, saving, undo, colliders, raycasts, and sampling behave as today.

## Non-goals

- Reading chunks directly from disk (compressed voxels fit the budget).
- Texture streaming or GPU texture compression.
- Changing the `.avox` format.

## 1. Voxels stay compressed

- On open, `read_data_file` parses the index and adopts each chunk's zstd frame **without
  decoding it**. A chunk is either uniform (no frame, no cost) or *compressed-only*.
- New `ChunkCache` (`src/engine_core/terrain/ChunkCache.hpp/.cpp`): a thread-safe LRU of
  decoded `ChunkData` keyed by chunk identity, with a byte budget (default 256 MB). A miss
  decodes from the chunk's frame. Callers get a `ChunkPtr` they may hold; eviction only
  drops the cache's reference.
- All readers of voxel cells — meshing jobs, collider builds, `VoxelSampler`,
  `HeightDerive`, raycasts, brushes, copy/paste — go through the cache.
- Edited chunks stay decoded and pinned (outside the LRU budget) until saved; save encodes
  them and they become ordinary compressed chunks.
- Undo history and `TerrainStash` store compressed frames, not dense cells.

## 2. Meshing streams

- On first sight of a Terrain, `TerrainWorld` builds a work list ordered by distance to the
  camera instead of queueing every chunk. At most `2 × mesher threads` chunk jobs are in
  flight; finishing a job admits the next. Camera moves re-sort the remainder.
- Jobs fetch the chunk and its neighbours from `ChunkCache` and release them on completion,
  so first-build memory is flat.

## 3. Far LOD mesh cache

- Built `CompactMesh` nodes (level ≥ 2) are written to `<name>.<hash>.alod` beside the
  `.avox`. The header records the terrain content hash and a format version; a mismatch
  means the cache is ignored and rebuilt.
- On open with a valid cache, far nodes load from the file and the full-map meshing pass
  is skipped (near chunks still mesh on demand).
- `LodTree` keeps far nodes under a byte budget (default 512 MB): nodes far from the camera
  and not drawn recently are dropped and reloaded from the `.alod` on demand. The coarsest
  level covering the whole map is always resident, so the horizon never has holes.
- Edits dirty nodes as today; dirty nodes rebuild in memory and the `.alod` is rewritten
  on save.

## Budgets and UI

| Part | Budget |
|---|---|
| Compressed voxels | size of map (~104 MB here) |
| Decoded chunk cache | 256 MB (setting) |
| Far LOD meshes | 512 MB (setting) |
| Near meshes, colliders, textures | as today |

Total expected ~1–1.5 GB. Budgets are engine settings, not constants. The Configure
Terrain tab shows current usage against each budget.

## Testing

- Unit: cache LRU eviction respects the budget; pinned edited chunks never evict; decode on
  demand matches a full `decode_avox`; `.alod` round-trips; stale `.alod` is rejected;
  first build never exceeds the in-flight cap.
- Existing terrain tests (sampling, raycast, brushes, undo, save/load) pass unchanged.
- Acceptance: open H1Z1 Z1 in Release, peak working set < 2 GB on both cold and warm
  open; terrain renders fully; an edit + save + reopen round-trips.
