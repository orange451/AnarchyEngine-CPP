# Project load speed: H1Z1 under 5 seconds

Goal: `AnarchyStudio "<H1Z1 Z1>"` goes from launch to a settled, usable editor in under 5 s, warm or cold.

## What was measured (2026-10-17, Release, main @ 81bfcf92)

Project: `H1Z1 Z1` ("H1Z1 Z1 Vegetation 100k"): 154,489 `.json` files (48 MB) in 327 folders
(153,008 GameObjects), one 4096x4096 Terrain (109 MB `.avox`, 374 MB `.alod`).

Temporary phase timers in `Project::read_into_game`, `PlanReader::read`, `build`,
`IdeLayout::open_project_at` and the first 60 frames after it; removed after measuring.

| Phase | Warm (ms) | Cold (ms) |
|---|---:|---:|
| Process start to first frame | 360-430 | 420 |
| `disk_stamp` walk (size+mtime of every file) | 380-410 | 450 |
| `PlanReader` scan of folders | 260-540 | 340 |
| **Read + parse 154k JSON files (already parallel)** | 1,820-2,200 | **45,300** |
| settle, child order, services | 340-450 | 370 |
| Scratch build (GameObjects 460, set_parent 160) | 480-630 | 600 |
| **Live build: Terrain** | **2,240** | |
| Live build: GameObjects + set_parent | 620 | |
| (live build total) | 2,700-2,860 | 3,900 |
| **`written_doc` for live + scratch, `files_`** | **1,720-1,860** | 2,015 |
| `open_project_at` tail | 310-470 | 410 |
| First ~16 frames after load | ~1,500-2,000 | ~1,600 |
| **Settled** | **~11,000** | **~55,000** |

Post-load frames (profiler): first Snapshot copy/changes 137 ms, first Scene View paint 357 ms
(Snapshot read 213, Prefab draws 162), Terrain LOD 110 ms, then several 110-540 ms frames.

The user's "10 to 30 s" is this range: 11 s with the files in the OS cache, much longer when
they are not (first open after a reboot, or after other work evicted them). Cold, opening 154k
small files is the whole problem; the bytes themselves are 48 MB.

## Is under 5 s possible?

Yes, warm with margin, cold probably. The projected budget after the plan:

| Phase | Target warm (ms) | Target cold (ms) |
|---|---:|---:|
| Process start to first frame | 400 | 450 |
| One folder walk (stamp + scan merged) | 400 | 800 |
| Load cache read + decode (one file, no JSON parse) | 250 | 400 |
| settle / order | 300 | 300 |
| Live build, GameObjects | 620 | 620 |
| Terrain | 300 | 600 |
| Base docs (`files_`) | 0 on the critical path | 0 |
| Open tail | 300 | 300 |
| First frames | 600 | 600 |
| **Total** | **~3.2 s** | **~4.1 s** |

Unknowns to check early: the cold folder walk after a real reboot (only measured with the
folders' metadata cold-ish), and how Windows Defender treats one large cache file versus 154k
small ones.

## Plan, in order of payoff for risk

### 1. Terrain load: 2.2 s to ~0.3 s
`Terrain::read_data_file` (src/engine_instances/Terrain.cpp:330):
- Reads 109 MB through `std::istreambuf_iterator` (byte at a time). Read it in one `read`
  into a sized buffer.
- `content_key_of` hashes all 109 MB on one thread. Hash per chunk on the decode workers, or
  keep the key in the file header.
- Time `decode_avox` on its own; if it is still large, keep each chunk's zstd frame
  (`ChunkData::encoded()` exists) and decode on first touch.

### 2. Base docs off the critical path: ~2.3 s to ~0
`read_into_game` builds the whole place twice (scratch + live) and runs `written_doc` on both
for every instance, only to fill `files_` (what a save and a disk check compare against).
- Run the scratch build and the `files_` fill on a worker after the load returns. Save,
  disk check and anything else reading `files_` waits for it (as saves already wait on
  `save_thread_`).
- `written_doc` on the live world, if still needed, splits across threads (const reads).

### 3. One folder walk instead of two: ~0.3-0.5 s
`disk_stamp()` and `PlanReader::scan` both walk the 154k-file tree. Have the scan record
size+mtime as it goes and build the `DiskStamp` from it.

### 4. Load cache: cold 45 s to ~0.4 s, warm 2 s to ~0.25 s
A single file, `.cache/place.pack` (the project's `.cache` is already ignored by git), written
after each load and save in the background: for every `src` file its relative path, size,
mtime and parsed `PlanNode` (binary, zstd).
- Load: the walk from step 3 gives each file's size+mtime. Files that match the pack come
  from it; only changed, new or racy files (`DiskStamp::racy` already exists) are read and
  parsed from disk. A missing or damaged pack is a normal full read.
- The project format and git diffs do not change: the pack is a cache, never the source of
  truth.
- Tests: an edited file outside the studio, a file added or deleted, an mtime-equal racy
  edit, a damaged pack, a pack from another project version.

### 5. First frames after load: ~1.7 s to ~0.6 s
Profile the first paint with the profiler recording from launch (it starts too late over MCP
today). Known: first Snapshot copy 137 ms, first Prefab draws 162 ms, Terrain LOD 110 ms.
Likely spread over frames or built during the load, but needs its own look.

### Fallback if cold is still over 5 s
Store a folder of many plain instances (the 153k trees) as one file. This changes the project
format and its diffs, so only if the pack is not enough.

## Order of work
Steps 1-3 first (small, local, warm 11 s to ~6 s). Then step 4 (the one that fixes cold
starts, warm to ~4 s). Then step 5. Re-measure H1Z1 warm and after a reboot at each step.
