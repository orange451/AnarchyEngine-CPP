# Terrain Streaming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Terrain of any size (H1Z1 Z1: 55,359 chunks) opens and runs in at most ~2 GB of RAM.

**Architecture:** Dense chunks keep only their zstd frame resident; decoded cells live in a process-wide LRU `ChunkCache` and are pinned by readers. `TerrainWorld` admits first-build chunk jobs a few at a time, nearest first. LOD nodes of level ≥ 2 are appended to an `.alod` file beside the `.avox` as they build; far ones are evicted and reloaded from it, and a warm open adopts the whole tree from it instead of re-meshing the map.

**Tech Stack:** C++20, MSVC, zstd (already vendored), Catch2 sandbox suite.

**Spec:** `docs/superpowers/specs/2026-10-09-terrain-streaming-design.md`

## Global Constraints

- Terrain RAM at most ~2 GB for H1Z1 Z1, cold and warm open (spec "Goal").
- `.avox` format unchanged.
- Engine distances are "units" in comments and UI, never "studs" in new text.
- Decoded chunk cache default budget: 256 MB.
- Far LOD meshes: bounded by a per-level distance ring (this plan's refinement of the spec's 512 MB budget — a ring bounds memory independent of map size; the spec is updated in Task 6).
- Editing, saving, undo, colliders, raycasts, and sampling behave as today; every existing `[terrain]` sandbox test still passes.

## Build and test commands

Configure once if needed: `MSYS_NO_PATHCONV=1 cmake -S . -B build` (Bash) — see memory note on MSVC.

- Build tests: `cmake --build build --config Debug --target sandbox`
- Run terrain tests: `build/Debug/sandbox.exe "[terrain]"`
- Run one test: `build/Debug/sandbox.exe "CC1*"`
- Release studio (acceptance): `cmake --build build --config Release --target AnarchyStudio`

If the Release link fails with LNK1104 the user's studio is open: say so, don't kill it.

## Review Focus

1. **A chunk evicted from the cache while a mesher job reads it** — the job's pin must keep the cells alive; no use-after-free, no wrong cells. (Task 1 test CC4, run under the threaded test CC5.)
2. **An edit made before the first build reaches that chunk** — the chunk meshes once, with the edited voxels, and the first-build list does not mesh it again with stale input. (Task 3 test TW-S3.)
3. **A stale or truncated `.alod`** (studio killed mid-build, `.avox` edited by another save, version bump) — it is ignored and the terrain rebuilds cold; never a crash or wrong mesh. (Task 4 tests AL3, AL4.)
4. **Camera flying back toward an evicted region** — evicted nodes reload from the `.alod` and the horizon never shows holes while they load (parent stays drawn). (Task 5 test LR3.)
5. **Edit then save on a warm-opened terrain** — rebuilt nodes are persisted, a reopen shows the edit, not the old cached meshes. (Task 6 test TW-A2.)

---

### Task 1: ChunkCache and compressed-resident chunks

**Files:**
- Create: `src/engine_core/terrain/ChunkCache.hpp`, `src/engine_core/terrain/ChunkCache.cpp`
- Modify: `src/engine_core/terrain/VoxelChunk.hpp`, `src/engine_core/terrain/VoxelChunk.cpp`
- Modify: `src/engine_core/terrain/ChunkFrame.hpp` (declare `decode_chunk_frame`), `src/engine_core/terrain/AvoxFile.cpp:128` (expose it; release cells after decode at ~line 447)
- Modify: `src/engine_core/terrain/VoxelVolume.cpp` (edit loop ~line 180–225, `cell` ~283, `replace_everywhere` ~550)
- Modify: `CMakeLists.txt:634` (add `ChunkCache.cpp`)
- Create: `sandbox/terrain_streaming_tests.cpp`; add to the `sandbox` target in `CMakeLists.txt:944`

**Interfaces:**
- Produces:
  - `using CellArray = std::vector<Cell>; using CellsPtr = std::shared_ptr<const CellArray>;` (VoxelChunk.hpp)
  - `CellsPtr ChunkData::cells() const` — dense: the 32,768 cells, pinned while held (decodes on a miss); uniform: nullptr.
  - `void ChunkData::release_cells() const` — encodes if needed, then hands the chunk's own cells to the cache so they can be evicted.
  - `bool ChunkData::cells_owned() const` — test hook: the chunk holds its cells outside the cache.
  - `class ChunkCache { static ChunkCache& global(); void set_budget(std::size_t bytes); std::size_t budget() const; std::size_t bytes() const; std::size_t compressed_bytes() const; void insert(const CellsPtr&); void touch(const CellArray*); void clear(); }`
  - `bool decode_chunk_frame(const std::byte* frame, std::size_t size, Cell* out)` (ChunkFrame.hpp)

Design notes for the implementer:
- `ChunkData` replaces `std::vector<Cell> cells_` with `mutable std::mutex cells_mutex_; mutable std::shared_ptr<CellArray> owned_; mutable std::weak_ptr<const CellArray> cached_;`. `clone_dense()` fills `owned_`. `set`, `dense_at`, `set_dense_at`, `finish`, `finish_with_mask` operate on `*owned_` (valid only on an unshared copy, as today).
- `cells()`: lock `cells_mutex_`; if `owned_` return it; else if `cached_.lock()` → `ChunkCache::global().touch(p.get())`, return it; else decode `encoded_` into a fresh `CellArray`, set `cached_`, `insert` it, return it.
- `release_cells()`: no-op for uniform or already released; `encoded()` first (it reads `owned_`), then under lock `cached_ = owned_; ChunkCache::global().insert(owned_); owned_.reset();`.
- `cell(int)` stays for single lookups: `uniform_ ? value_ : (*cells())[index]`.
- `ChunkCache` is an LRU: `std::list<CellsPtr>` plus `std::unordered_map<const CellArray*, list::iterator>`, under one mutex. `insert` pushes front and evicts from the back while `bytes() > budget`. Bytes per entry: `kChunkCells * sizeof(Cell)`. `compressed_bytes()` is an atomic the chunk adds `encoded_.size()` to when a frame is adopted or made and subtracts in `~ChunkData`.
- VoxelVolume's edit loop: `old->dense_at(index)` on a shared chunk must become a read through a `CellsPtr old_cells = old->cells();` taken once per chunk.
- `decode_avox`: after `adopt_encoded`, call `chunk->release_cells()`. Peak decode memory becomes budget + one chunk per thread.

- [ ] **Step 1: Write the failing tests** in `sandbox/terrain_streaming_tests.cpp`

```cpp
// Terrain streaming: the decoded-chunk cache, first-build admission, and the
// .alod far-mesh cache. No instances.

#include "terrain/AvoxFile.hpp"
#include "terrain/ChunkCache.hpp"
#include "terrain/ShapeDistance.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

using namespace engine_core;
using namespace engine_core::terrain;

namespace {
constexpr std::size_t kChunkBytes = static_cast<std::size_t>(kChunkCells) * sizeof(Cell);

// A dense, shared chunk whose cell 0 reads (d, m) and the rest air.
ChunkPtr dense_chunk(std::int8_t d, std::uint8_t m) {
    std::shared_ptr<ChunkData> chunk = ChunkData::air()->clone_dense();
    chunk->set(0, Cell{d, m});
    chunk->set(1, Cell{-5, m});
    chunk->finish();
    return chunk;
}

struct BudgetGuard {
    std::size_t saved = ChunkCache::global().budget();
    explicit BudgetGuard(std::size_t bytes) { ChunkCache::global().clear(); ChunkCache::global().set_budget(bytes); }
    ~BudgetGuard() { ChunkCache::global().clear(); ChunkCache::global().set_budget(saved); }
};
}  // namespace

TEST_CASE("CC1 a released chunk reads the same cells after eviction", "[terrain]") {
    BudgetGuard guard(kChunkBytes);   // room for one
    ChunkPtr a = dense_chunk(-20, 7);
    ChunkPtr b = dense_chunk(-30, 9);
    a->release_cells();
    b->release_cells();   // evicts a's cells
    REQUIRE_FALSE(a->cells_owned());
    REQUIRE(ChunkCache::global().bytes() <= kChunkBytes);
    REQUIRE(a->cell(0) == Cell{-20, 7});   // decoded again from its frame
    REQUIRE(b->cell(0) == Cell{-30, 9});
}

TEST_CASE("CC2 the cache never holds more than its budget", "[terrain]") {
    BudgetGuard guard(3 * kChunkBytes);
    std::vector<ChunkPtr> chunks;
    for (int i = 0; i < 20; ++i) {
        chunks.push_back(dense_chunk(static_cast<std::int8_t>(-i - 1), 1));
        chunks.back()->release_cells();
        REQUIRE(ChunkCache::global().bytes() <= 3 * kChunkBytes);
    }
    for (int i = 0; i < 20; ++i) {
        REQUIRE(chunks[static_cast<std::size_t>(i)]->cell(0).distance == -i - 1);
        REQUIRE(ChunkCache::global().bytes() <= 3 * kChunkBytes);
    }
}

TEST_CASE("CC3 an edited chunk keeps its cells until released", "[terrain]") {
    BudgetGuard guard(0);   // the cache keeps nothing
    ChunkPtr edited = dense_chunk(-12, 3);
    REQUIRE(edited->cells_owned());
    REQUIRE(edited->cell(0) == Cell{-12, 3});
    edited->release_cells();
    REQUIRE_FALSE(edited->cells_owned());
    REQUIRE(edited->cell(0) == Cell{-12, 3});
}

TEST_CASE("CC4 a pin outlives eviction", "[terrain]") {
    BudgetGuard guard(kChunkBytes);
    ChunkPtr a = dense_chunk(-20, 7);
    a->release_cells();
    CellsPtr pinned = a->cells();
    for (int i = 0; i < 4; ++i) {
        dense_chunk(-1, 1)->release_cells();   // churn: a's entry is evicted
    }
    REQUIRE((*pinned)[0] == Cell{-20, 7});
}

TEST_CASE("CC5 many threads read evicting chunks correctly", "[terrain]") {
    BudgetGuard guard(4 * kChunkBytes);
    std::vector<ChunkPtr> chunks;
    for (int i = 0; i < 64; ++i) {
        chunks.push_back(dense_chunk(static_cast<std::int8_t>(-1 - (i % 100)), static_cast<std::uint8_t>(i)));
        chunks.back()->release_cells();
    }
    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int pass = 0; pass < 50; ++pass) {
                for (int i = 0; i < 64; ++i) {
                    const ChunkPtr& c = chunks[static_cast<std::size_t>((i + t * 7) % 64)];
                    const int index = (i + t * 7) % 64;
                    if (c->cell(0) != Cell{static_cast<std::int8_t>(-1 - (index % 100)), static_cast<std::uint8_t>(index)}) {
                        ++wrong;
                    }
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();
    REQUIRE(wrong == 0);
    REQUIRE(ChunkCache::global().bytes() <= 4 * kChunkBytes);
}

TEST_CASE("CC6 a decoded .avox keeps only frames resident", "[terrain]") {
    BudgetGuard guard(2 * kChunkBytes);
    VoxelVolume volume(1.f);
    REQUIRE_FALSE(volume.fill(Shape::ball(Vec3{0.f, 0.f, 0.f}, 100.f), 1).has_value());
    const std::vector<std::byte> bytes = encode_avox(volume);
    VoxelVolume loaded(1.f);
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), loaded).has_value());
    REQUIRE(ChunkCache::global().bytes() <= 2 * kChunkBytes);
    for (const auto& [coord, chunk] : loaded.chunks()) {
        REQUIRE_FALSE(chunk->cells_owned());
        const ChunkPtr& original = volume.chunks().at(coord);
        for (int i = 0; i < kChunkCells; i += 997) {
            REQUIRE(chunk->cell(i) == original->cell(i));
        }
    }
}

TEST_CASE("CC7 editing a released chunk changes only the copy", "[terrain]") {
    BudgetGuard guard(kChunkBytes);
    VoxelVolume volume(1.f);
    REQUIRE_FALSE(volume.fill(Shape::ball(Vec3{0.f, 0.f, 0.f}, 40.f), 1).has_value());
    const std::vector<std::byte> bytes = encode_avox(volume);
    VoxelVolume loaded(1.f);
    REQUIRE_FALSE(decode_avox(bytes.data(), bytes.size(), loaded).has_value());
    const ChunkMap before = loaded.chunks();
    REQUIRE_FALSE(loaded.paint(Shape::ball(Vec3{0.f, 0.f, 0.f}, 10.f), 2).has_value());
    REQUIRE(loaded.cell(CellCoord{0, 0, 0}).material == 2);
    // The chunks the paint replaced still read as they did.
    for (const auto& [coord, chunk] : before) {
        for (int i = 0; i < kChunkCells; i += 1013) {
            REQUIRE(chunk->cell(i) == volume.chunks().at(coord)->cell(i));
        }
    }
}
```

(Check `Shape::ball` against `ShapeDistance.hpp` and adapt the factory name if it differs; keep the radii.)

- [ ] **Step 2: Run to verify they fail** — `cmake --build build --config Debug --target sandbox` fails to compile: `ChunkCache.hpp` not found.

- [ ] **Step 3: Implement** `ChunkCache` and the `ChunkData` changes per the design notes; expose `decode_chunk_frame` (make the existing static one in AvoxFile.cpp:128 write into a `Cell*` and keep a thin `ChunkData&` wrapper for current callers); call `release_cells()` in `decode_avox`; fix VoxelVolume's three `dense_at` sites on shared chunks to read through one `cells()` pin per chunk.

`ChunkCache.hpp`:

```cpp
#pragma once

// Decoded cells of dense chunks that are not being edited: one process-wide
// LRU under a byte budget. A chunk keeps only a weak reference to its cells;
// readers pin them (ChunkData::cells()) for as long as they read. Any thread.

#include "terrain/VoxelChunk.hpp"

#include <atomic>
#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>

namespace engine_core::terrain {

class ChunkCache {
public:
    static constexpr std::size_t kDefaultBudget = 256u * 1024u * 1024u;
    static ChunkCache& global();

    void set_budget(std::size_t bytes);
    std::size_t budget() const;
    // Decoded bytes the cache holds now.
    std::size_t bytes() const;
    // Every live chunk's zstd frame, resident or not.
    std::size_t compressed_bytes() const { return compressed_.load(std::memory_order_relaxed); }
    void add_compressed(std::ptrdiff_t delta) { compressed_.fetch_add(static_cast<std::size_t>(delta), std::memory_order_relaxed); }

    // Most recently used first; evicts from the back while over budget.
    void insert(const CellsPtr& cells);
    // Marks cells most recently used, if the cache still holds them.
    void touch(const CellArray* cells);
    void clear();

private:
    void evict_locked();
    mutable std::mutex mutex_;
    std::size_t budget_ = kDefaultBudget;
    std::size_t bytes_ = 0;
    std::list<CellsPtr> order_;
    std::unordered_map<const CellArray*, std::list<CellsPtr>::iterator> where_;
    std::atomic<std::size_t> compressed_{0};
};

}  // namespace engine_core::terrain
```

`ChunkCache.cpp`:

```cpp
#include "terrain/ChunkCache.hpp"

namespace engine_core::terrain {

namespace {
constexpr std::size_t kEntryBytes = static_cast<std::size_t>(kChunkCells) * sizeof(Cell);
}

ChunkCache& ChunkCache::global() {
    static ChunkCache cache;
    return cache;
}

void ChunkCache::set_budget(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    budget_ = bytes;
    evict_locked();
}

std::size_t ChunkCache::budget() const {
    std::lock_guard lock(mutex_);
    return budget_;
}

std::size_t ChunkCache::bytes() const {
    std::lock_guard lock(mutex_);
    return bytes_;
}

void ChunkCache::insert(const CellsPtr& cells) {
    if (!cells) return;
    std::lock_guard lock(mutex_);
    const auto found = where_.find(cells.get());
    if (found != where_.end()) {
        order_.splice(order_.begin(), order_, found->second);
        return;
    }
    order_.push_front(cells);
    where_[cells.get()] = order_.begin();
    bytes_ += kEntryBytes;
    evict_locked();
}

void ChunkCache::touch(const CellArray* cells) {
    std::lock_guard lock(mutex_);
    const auto found = where_.find(cells);
    if (found != where_.end()) {
        order_.splice(order_.begin(), order_, found->second);
    }
}

void ChunkCache::clear() {
    std::lock_guard lock(mutex_);
    order_.clear();
    where_.clear();
    bytes_ = 0;
}

void ChunkCache::evict_locked() {
    while (bytes_ > budget_ && !order_.empty()) {
        where_.erase(order_.back().get());
        order_.pop_back();   // the chunk's weak_ptr expires unless a reader pins it
        bytes_ -= kEntryBytes;
    }
}

}  // namespace engine_core::terrain
```

Core of the `ChunkData` change (VoxelChunk.cpp):

```cpp
CellsPtr ChunkData::cells() const {
    if (uniform_) return nullptr;
    std::lock_guard lock(cells_mutex_);
    if (owned_) return owned_;
    if (CellsPtr cached = cached_.lock()) {
        ChunkCache::global().touch(cached.get());
        return cached;
    }
    auto decoded = std::make_shared<CellArray>(kChunkCells);
    // The frame was checked when it was read (decode_avox) or made here
    // (encoded()), so a failure now means memory corruption.
    if (!decode_chunk_frame(encoded_.data(), encoded_.size(), decoded->data())) {
        std::abort();
    }
    CellsPtr shared = std::move(decoded);
    cached_ = shared;
    ChunkCache::global().insert(shared);
    return shared;
}

void ChunkData::release_cells() const {
    if (uniform_) return;
    encoded();   // made from owned_ if this chunk has no frame yet
    std::lock_guard lock(cells_mutex_);
    if (!owned_) return;
    cached_ = owned_;
    ChunkCache::global().insert(owned_);
    owned_.reset();
}
```

Add a `~ChunkData()` that calls `ChunkCache::global().add_compressed(-static_cast<std::ptrdiff_t>(encoded_.size()))`, and add the positive delta wherever `encoded_` is assigned (`encoded()` and `adopt_encoded`).

- [ ] **Step 4: Run** `build/Debug/sandbox.exe "[terrain]"` — CC1–CC7 and every existing terrain test pass.
- [ ] **Step 5: Commit** — `git add src/engine_core/terrain sandbox/terrain_streaming_tests.cpp CMakeLists.txt && git commit -m "Terrain: chunks stay compressed, decoded cells live in an LRU cache"`

---

### Task 2: Readers pin once per chunk; a save releases edited chunks

Per-cell `cell()` now takes a mutex. The hot readers must pin once.

**Files:**
- Modify: `src/engine_core/terrain/SurfaceNets.cpp` (`quick_reject` ~line 60, `fill_samples` ~line 80)
- Modify: `src/engine_core/terrain/VoxelSampler.hpp/.cpp` (cache the pinned `CellsPtr` next to `cached_chunk_`)
- Modify: `src/engine_instances/Terrain.cpp` (after a successful `write_avox` and on `saved_chunks_ = ...` after save: `release_cells()` on each chunk)
- Test: `sandbox/terrain_streaming_tests.cpp`

**Interfaces:**
- Consumes: `ChunkData::cells()`, `release_cells()`, `cells_owned()` (Task 1).
- Produces: `void release_all_cells(const ChunkMap&)` in `VoxelVolume.hpp` — calls `release_cells()` on every chunk.

- [ ] **Step 1: Failing tests**

```cpp
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelSampler.hpp"

TEST_CASE("CC8 meshing a released volume matches meshing it decoded", "[terrain]") {
    VoxelVolume volume(1.f);
    REQUIRE_FALSE(volume.fill(Shape::ball(Vec3{0.f, 0.f, 0.f}, 50.f), 1).has_value());
    const ChunkMesh reference = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    BudgetGuard guard(kChunkBytes);   // fewer than the 27 neighbours
    release_all_cells(volume.chunks());
    const ChunkMesh streamed = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    REQUIRE(streamed.mesh.positions == reference.mesh.positions);
    REQUIRE(streamed.mesh.indices == reference.mesh.indices);
}

TEST_CASE("CC9 a sampler reads released chunks", "[terrain]") {
    VoxelVolume volume(1.f);
    REQUIRE_FALSE(volume.fill(Shape::ball(Vec3{0.f, 0.f, 0.f}, 50.f), 1).has_value());
    const float inside = VoxelSampler(volume.chunks(), 1.f).distance(Vec3{0.f, 0.f, 49.f});
    BudgetGuard guard(kChunkBytes);
    release_all_cells(volume.chunks());
    REQUIRE(VoxelSampler(volume.chunks(), 1.f).distance(Vec3{0.f, 0.f, 49.f}) == inside);
}
```

(Match `ChunkMesh`'s field names in `SurfaceNets.hpp`; compare the mesh's positions and indices.)

- [ ] **Step 2: Run** — fails to compile: `release_all_cells` undefined.
- [ ] **Step 3: Implement.**
  - `fill_samples` and `quick_reject`: build `std::array<CellsPtr, 27> pins` once from `input.neighbors[n]->cells()` (null for absent or uniform). Read `pins[n] ? (*pins[n])[index] : (neighbor ? neighbor->cell(0) : Cell{})`.
  - `VoxelSampler::chunk_at`: also keep `mutable CellsPtr cached_cells_` set from `chunk->cells()`, and have `cell_at` read through it.
  - In `Terrain.cpp` save, after `write_avox` succeeds for this Terrain's chunks, call `terrain::release_all_cells(volume_.chunks())`.
- [ ] **Step 4: Run** `build/Debug/sandbox.exe "[terrain]"` — all pass. Also run `build/Debug/terrain-editor-tests.exe`.
- [ ] **Step 5: Commit** — `git commit -am "Terrain: meshing and sampling pin each chunk once; saving releases edited chunks"`

---

### Task 3: First build admits chunks a few at a time

**Files:**
- Modify: `src/engine_core/terrain/TerrainMesher.hpp` (add `unsigned thread_count() const { return thread_count_; }`)
- Modify: `src/engine_core/TerrainWorld.hpp` (`TerrainRecord`: `std::vector<terrain::ChunkCoord> first_build; terrain::ChunkCoord first_build_camera{}; bool first_build_sorted = false;`)
- Modify: `src/engine_core/TerrainWorld.cpp` (`queue_dirty`, lines 232–304; new `admit_first_build`, called from `update` after `queue_dirty`)
- Test: `sandbox/terrain_streaming_tests.cpp`

**Interfaces:**
- Produces: `std::size_t TerrainWorld::first_build_remaining(InstanceId) const` (test hook); `static constexpr unsigned kFirstBuildJobsPerThread = 2;`

Behavior:
- First sight: compute the same footprint, call `record.tree->chunk_queued(coord, true)` for every coord as today, so parents wait for all their children. Store the coords in `record.first_build` and queue **no** mesher jobs.
- `admit_first_build`, run every update:
  - Do nothing while `record.pending_jobs.size() >= mesher_.thread_count() * kFirstBuildJobsPerThread`.
  - Re-sort `first_build` by distance to the camera chunk (farthest first, so `pop_back` takes the nearest) whenever the camera chunk has changed since the last sort.
  - Pop coords and queue them exactly as `queue_dirty` does today: new revision, `pending_jobs` entry with `edited = true`, `mesh_input`, `mesher_.queue`. Stop when the cap is reached.
  - Skip a popped coord that already has a `pending_jobs` entry, or whose `chunk_revisions` entry was set after first sight. That means an edit queued it already.
- `TerrainWorld` with no camera (tests): the cap still applies; order is the stored order.

- [ ] **Step 1: Failing tests**

```cpp
#include "DataModel.hpp"
#include "TerrainWorld.hpp"
#include "Terrain.hpp"
// Reuse the Terrain/DataModel fixture from sandbox/terrain_lod_tests.cpp (copy its
// make_game()/add_terrain() helpers into an anonymous namespace here).

TEST_CASE("TW-S1 first build never has more jobs in flight than the cap", "[terrain]") {
    // A volume of ~200 surface chunks, a TerrainWorld with 2 threads, the mesher paused.
    // After update(): pending jobs <= 2 * kFirstBuildJobsPerThread, and
    // first_build_remaining() == footprint size - pending.
}

TEST_CASE("TW-S2 first build finishes and meshes every surface chunk", "[terrain]") {
    // update()+wait_idle() in a loop until first_build_remaining()==0 and the mesher is idle;
    // meshed_count() equals the count from a world built before this change
    // (compute by calling surface_nets on every footprint coord directly).
}

TEST_CASE("TW-S3 an edit during first build meshes that chunk once, with the edit", "[terrain]") {
    // Pause mesher, first update, paint a far chunk, update, unpause, drain.
    // The far chunk's final mesh has the painted Id; it was queued once by the edit
    // (count MeshResults for that coord via a BuildCollider hook that counts calls).
}
```

Write each test body in full, modelled on the fixtures in `sandbox/terrain_lod_tests.cpp`. Use `pause_for_test` for determinism.

- [ ] **Step 2: Run** — fails: `first_build_remaining` undefined.
- [ ] **Step 3: Implement** per the behavior list.
- [ ] **Step 4: Run** `build/Debug/sandbox.exe "[terrain]"` — all pass. The existing LOD world tests still pass, because they drain to idle.
- [ ] **Step 5: Commit** — `git commit -am "TerrainWorld: the first build admits chunks nearest first, a few per mesher thread"`

---

### Task 4: The .alod far-mesh store

An append-only file of built LOD nodes, read by random access.

**Files:**
- Create: `src/engine_core/terrain/AlodStore.hpp`, `src/engine_core/terrain/AlodStore.cpp`; add to `CMakeLists.txt` beside `LodTree.cpp`
- Test: `sandbox/terrain_streaming_tests.cpp`

**Format** (little-endian; this is new, so document it in the header comment):
- **Header (32 bytes):** magic `"ALOD"`, u32 version = 1, u64 `content_key`, f32 `voxel_size`, 12 bytes zero.
- **Records, appended in any order.** Each record is:
  - i32 level, x, y, z
  - f32 error, then f32×6 bounds
  - u32 surface_index_count
  - Vec3 origin and Vec3 scale (6×f32)
  - u32 counts: vertices, indices16, indices32
  - the CompactMesh arrays, raw
- **Footer:** the index. It starts with u32 count of entries; each entry is `NodeKey` (4×i32) + u64 offset + u32 size + f32 error + f32×6 bounds. Then u32 count of level-0 coords with surface, followed by those coords (3×i32 each).
- **Trailer (16 bytes):** u64 footer offset, magic `"ALDE"`, u32 crc32 of the footer.

A file whose trailer is missing, has a bad CRC, a wrong version, or a different `content_key`/`voxel_size` is invalid. Appending writes new records over the old footer position, then a new footer and trailer.

**Interfaces:**
- Produces:

```cpp
namespace engine_core::terrain {
struct AlodEntry { NodeKey key; std::uint64_t offset = 0; std::uint32_t size = 0; float error = 0.f; Vec3 bounds_min{}, bounds_max{}; };
class AlodStore {
public:
    // Opens path if it is a valid store for (content_key, voxel_size); else nullopt.
    static std::optional<AlodStore> open(const std::filesystem::path& path, std::uint64_t content_key, float voxel_size);
    // Starts an empty store at path (replacing any file there).
    static std::optional<AlodStore> create(const std::filesystem::path& path, std::uint64_t content_key, float voxel_size);
    const std::unordered_map<NodeKey, AlodEntry, NodeKeyHash>& entries() const;
    const std::vector<ChunkCoord>& surface_chunks() const;
    std::shared_ptr<const CompactMesh> load(const NodeKey& key) const;   // null if absent or unreadable
    // Appends or replaces key's record (the index points at the newest).
    bool put(const NodeKey& key, const CompactMesh& mesh, float error, Vec3 bounds_min, Vec3 bounds_max);
    void set_surface_chunks(std::vector<ChunkCoord> coords);
    // Writes the footer and trailer; until then a reopen sees the previous footer (or nothing).
    bool commit();
    std::uint64_t content_key() const;
};
// FNV-1a 64 over bytes: the content key of an .avox file.
std::uint64_t content_key_of(const std::byte* data, std::size_t size);
}
```

- [ ] **Step 1: Failing tests:**
  - **AL1:** `put` 3 nodes, `commit`, `open` → `entries()` has 3 and `load` round-trips each `CompactMesh` field-for-field. Build meshes with `pack` from a small `amesh::Data`, following `terrain_lod_tests.cpp`.
  - **AL2:** `put` the same key twice, `commit`, reopen → `load` returns the second mesh.
  - **AL3:** truncate the file by 1 byte → `open` returns nullopt. Then flip one footer byte → nullopt.
  - **AL4:** `open` with a different `content_key` → nullopt; with a different `voxel_size` → nullopt.
  - **AL5:** `put` without `commit`, reopen → the previously committed state only.

  Write each test in full, using a temp path under `std::filesystem::temp_directory_path()`, and remove it at the end.
- [ ] **Step 2: Run** — fails to compile.
- [ ] **Step 3: Implement.**
  - Keep one `std::fstream` open for read+write.
  - `put` seeks to the end of the last record (the old footer start), writes the record, and updates the in-memory index.
  - `commit` writes the footer, the trailer, and flushes. Reads reuse the `get_*`/`put_*` helpers; move them from AvoxFile.cpp into a small internal `terrain/ByteIo.hpp` and include it from both files.
- [ ] **Step 4: Run** `build/Debug/sandbox.exe "AL*"` — pass.
- [ ] **Step 5: Commit** — `git commit -am "Terrain: .alod store of built LOD nodes"`

---

### Task 5: LodTree persists, evicts, and adopts far nodes

**Files:**
- Modify: `src/engine_core/terrain/LodTree.hpp/.cpp`
- Test: `sandbox/terrain_streaming_tests.cpp` (and existing `sandbox/terrain_lod_tree_tests.cpp` must still pass)

**Interfaces:**
- Consumes: `AlodStore` (Task 4).
- Produces:

```cpp
// Level >= 2 nodes are kept within kFarRingNodes nodes of the camera at their
// own level (L-infinity, in that level's node units) and evicted beyond
// kFarRingNodes + 2, but only once persisted and with a resident parent; the
// top level never leaves.
inline constexpr int kFarRingNodes = 4;
void LodTree::attach_store(AlodStore* store);   // null: nothing persisted, nothing evicted
// Warm start: creates every node in the store (built, current, not resident)
// and a level-0 node per surface chunk (has_surface, no mesh). Call on an empty tree.
void LodTree::adopt_store();
// Called by update_residency's caller: nodes to read from the store this update.
void LodTree::take_loads(std::vector<NodeKey>& out);
void LodTree::node_loaded(const NodeKey& key, std::shared_ptr<const CompactMesh> mesh);
std::size_t LodTree::compact_bytes() const;   // resident level >= 1 meshes
```

Behavior:
- `node_built` for level ≥ 2 with a store attached calls `store->put(...)` and marks the node `persisted = true`, a new `Node` field. Marking a node stale clears `persisted`.
- `update_residency` evicts. A level ≥ 2 node that is resident, persisted, not at `top_`, has a resident parent, and is beyond `kFarRingNodes + 2` loses its `compact` and gets `resident = false`.
- A level ≥ 2 node that is built, persisted, not resident, and within `kFarRingNodes`, or at `top_`, is pushed to the load list once (`loading = true`). `node_loaded` restores `compact`, `resident = true`, and a new `mesh_revision`.
- `adopt_store`: every entry becomes a `Node` with `built = true`, `built_revision = revision = min_revision = next_revision()`, `has_surface = true`, the bounds and error from the entry, `persisted = true`, and `resident = false`. Each surface chunk gets a level-0 node with `has_surface = true` and no mesh. Then `refresh_top()`.
  - The usual residency pass afterwards asks for near chunk meshes and near or top nodes. Far chunks stay unmeshed, because their parents are built and current.
- **Level 1 is not persisted.** It is cheap to rebuild from chunks near the camera. Level-1 nodes are not in the store, so after `adopt_store` the level-1 nodes near the camera are created stale by `ensure_ancestors` and rebuilt.
- `child_mask` semantics are unchanged. An evicted child is "surfaced" but not published, so selection draws its resident parent. That keeps the horizon free of holes.

- [ ] **Step 1: Failing tests** (full bodies, using the `LodTree` test helpers in `sandbox/terrain_lod_tree_tests.cpp`):
  - **LR1:** build a tree over a 64×1×64 chunk sheet with a store, with the camera at the origin. After all builds, `compact_bytes()` with the camera at one corner is below what it is with no store attached (nothing evicted).
  - **LR2:** after LR1's eviction, every evicted node's parent is resident, and every top-level node is resident.
  - **LR3:** move the camera to the far corner and run `update_residency`. `take_loads` lists the now-near evicted nodes; after `node_loaded`, they appear in `nodes_for_view()`.
  - **LR4:** `adopt_store` on a fresh tree from LR1's committed store. `update_residency` with the camera at the origin asks only for chunks within `kNearChunks` (count ≤ (2·6+1)²·1 for the sheet), and `nodes_for_view()` is non-empty after the loads.
  - **LR5:** an edit (`chunk_queued(c, true)`) clears `persisted` up the ancestor chain, so those nodes are not evicted until rebuilt and re-put.
- [ ] **Step 2: Run** — fails to compile.
- [ ] **Step 3: Implement** per the behavior list.
- [ ] **Step 4: Run** `build/Debug/sandbox.exe "[terrain]"` — all pass, including the existing `terrain_lod_tree_tests`.
- [ ] **Step 5: Commit** — `git commit -am "LodTree: far nodes persist to the .alod store, evict beyond a ring, and reload"`

---

### Task 6: Wire the store into Terrain and TerrainWorld

**Files:**
- Modify: `src/engine_instances/Terrain.hpp/.cpp`:
  - Add `std::uint64_t content_key_ = 0;`, set from `content_key_of(bytes)` in `read_data_file` and from the encoded bytes after a save.
  - Add `std::filesystem::path lod_cache_path() const` (`resources_root() / data_path_` with its extension replaced by `.alod`; empty without a project).
  - Add `std::uint64_t content_key() const`.
- Modify: `src/engine_core/TerrainWorld.hpp/.cpp`:
  - `TerrainRecord` gains `std::optional<terrain::AlodStore> store; std::uint64_t store_key = 0;`.
  - On first sight with a non-empty `lod_cache_path()` and a non-zero `content_key()`: `AlodStore::open`. If that succeeds, `tree->attach_store(&*store); tree->adopt_store();` and skip the first-build list. Otherwise `AlodStore::create` and run the cold first build (Task 3) with the store attached.
  - Each update, `tree->take_loads(keys)` and, for at most 16 keys per update, `store->load(key)` → `tree->node_loaded`.
  - Commit the store when the first build finishes (`first_build` empty, no pending jobs, no stale node), and again whenever `terrain.content_key()` changes (a save). On a key change, before committing, rewrite the header key: create a new store at a `.alod.tmp` path, `put` every node that is currently persisted (loading the evicted ones from the old store), `commit`, and rename it over the old file.
  - `store->set_surface_chunks` gets every level-0 node with `has_surface` before each commit.
- Modify: `docs/superpowers/specs/2026-10-09-terrain-streaming-design.md`, section 3: replace the 512 MB budget with the ring rule (`kFarRingNodes`), and note that level 1 is not persisted.
- Test: `sandbox/terrain_streaming_tests.cpp`

- [ ] **Step 1: Failing tests** (full bodies; temp project folder with a `resources/terrain/` path, following `sandbox/project_tests.cpp` for making a project root):
  - **TW-A1:** a Terrain with a ~200-chunk surface. A cold world drains, which writes the `.alod`. A second `TerrainWorld` over the same reopened project meshes at most (2·kNearChunks+1)³ chunks (`meshed_count()`), and its view has nodes.
  - **TW-A2:** warm-open, paint a far region, save, drain. A third world (warm again) shows the painted Id in that region's node meshes (unpack a resident ancestor and check `ids`), and the `.alod` content key equals the new `.avox` key.
  - **TW-A3:** corrupt the `.alod` (truncate it), reopen → a cold build (`meshed_count()` equals the full footprint) and a fresh valid `.alod`.
- [ ] **Step 2: Run** — fails.
- [ ] **Step 3: Implement** per the file list, and update the spec section.
- [ ] **Step 4: Run** `build/Debug/sandbox.exe "[terrain]"` and `build/Debug/terrain-editor-tests.exe` — all pass.
- [ ] **Step 5: Commit** — `git commit -am "Terrain: warm opens adopt the .alod far-mesh cache instead of re-meshing"`

---

### Task 7: Memory readout and acceptance on H1Z1 Z1

**Files:**
- Modify: `src/engine_core/TerrainWorld.hpp/.cpp`: `struct TerrainMemory { std::size_t compressed_voxels, decoded_cache, decoded_budget, chunk_meshes, far_meshes; }; TerrainMemory memory(InstanceId) const;`. `chunk_meshes` sums the vertex and index bytes of `record.meshes`; `far_meshes` = `tree->compact_bytes()`; the voxel numbers come from `ChunkCache::global()`.
- Modify: `src/ide/IdeTerrainEditor.cpp`: one "Memory" line in the Configure Terrain tab, beside the existing texture memory line, e.g. `Voxels 104 MB · cache 212/256 MB · meshes 380 MB`.
- Test: `tests/TerrainEditorTest.cpp`: the line renders with the numbers from a stub `TerrainMemory`. Follow how that file tests the texture memory line.

- [ ] **Step 1: Failing test** for the readout, written in full in the style of the existing texture-memory test in `tests/TerrainEditorTest.cpp`.
- [ ] **Step 2: Run** `build/Debug/terrain-editor-tests.exe` — fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — passes.
- [ ] **Step 5: Acceptance on the real project.** Build Release `AnarchyStudio`. Delete any `.alod` in `H1Z1 Z1/resources/terrain/`, then open the project. Sample the peak working set every second until the terrain stops building:

  ```powershell
  $p = Start-Process build/Release/AnarchyStudio.exe -ArgumentList '"C:\Users\Andrew\Documents\Anarchy Engine Projects\H1Z1 Z1"' -PassThru
  $peak = 0; while (-not $p.HasExited -and $i++ -lt 180) { $p.Refresh(); $peak = [math]::Max($peak, $p.PeakWorkingSet64); Start-Sleep 1 }
  "{0:N0} MB" -f ($peak / 1MB)
  ```

  Expected: under 2,048 MB. Close Studio, reopen (warm, `.alod` present): under 2,048 MB, and the terrain is visible within a few seconds. Check the Memory line, and take one screenshot of the island.
  - If the cold peak is over budget, read the Memory line to see which part is over, and fix that part before claiming done. Lowering `kFirstBuildJobsPerThread` or `kFarRingNodes` is the expected lever.
- [ ] **Step 6: Commit** — `git commit -am "Configure Terrain: terrain memory readout"`. Then refresh the Release package per the rebuild-release memory note.
