# Terrain LOD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Controllers: dispatch every implementer and reviewer in the foreground (`run_in_background: false`).

**Goal:** Distant terrain that looks nearly the same as up close, at a cost that lets one Terrain be about 4 km across: an octree of simplified meshes chosen by screen-space error, re-shaded from full-resolution voxels, joined with skirts, cross-faded when switching; full detail and colliders only where they are seen up close or touched by physics; raycasts at any distance.

**Branch:** `terrain` (main + edit-mode physics + terrain data + terrain surface + the Configure Terrain tab once merged). Work in a worktree branched from it.

**Architecture:** Pure LOD code in `src/engine_core/terrain/`: `LodNode` (node key, compact mesh), `LodBuilder` (merge, simplify, measure, re-shade, skirt; the only includer of meshoptimizer), `LodTree` (one Terrain's octree bookkeeping: which nodes exist, are stale, are resident, need building). `TerrainMesher` gains a second job kind (build a node). `TerrainWorld` owns one `LodTree` per Terrain and publishes each Terrain's resident nodes. Selection runs in `GameView` (which knows the pane size and camera) through `TerrainDraws`. `PhysicsWorld` streams chunk colliders around dynamic bodies and ray-marches voxels where colliders are absent.

**Tech Stack:** C++20 (MSVC 14.23), meshoptimizer (MIT), Box3D, OpenGL 3.3/4.1, GLSL 330, Catch2 (`sandbox`), CMake 3.16 + VS 2019.

**Spec:** `docs/superpowers/specs/2026-10-06-terrain-lod-design.md`. This plan records five implementation decisions the spec now needs (Task 1 adds them to the spec):
1. The compact node format (16-bit quantized positions, octahedral 2-byte normals, 4-byte Ids, 4-byte weights, 16-bit indices) is the **RAM** format; a node is unpacked to `amesh::Data` (full `Vertex`, u32 indices) only when uploaded, since `GpuMesh` takes only that.
2. **Selection runs in `GameView`/`TerrainDraws`** before `Renderer::draw`, from the camera row and the Scene View's pane height, because the renderer learns its target size only inside `draw()`.
3. **Shadows draw, for a node mid-fade, only the incoming node**, without dither (the depth shader has no dither and needs none at shadow resolution).
4. LOD builds re-shade from a **chunk-map snapshot** (`std::shared_ptr<const ChunkMap>`, a pointer copy) taken at most once per `TerrainWorld::update`, and only when LOD work is queued.
5. The mesher's job queue holds **two job kinds** (chunk mesh, node build), sharing workers and distance ordering.

## Global Constraints

- Node key: `(level, x, y, z)`; a level-L node holds chunks whose coordinates floor-divide by 2^L to `(x, y, z)`. Level 0 is one chunk. Levels rise until one node covers every chunk of the Terrain.
- A level-L node's target error is `0.25 × VoxelSize × 2^L` studs; its recorded error is meshoptimizer's measured error converted to studs (`result_error × meshopt_simplifyScale`), never more than the target unless simplification cannot reach it (then the measured one).
- Each node is merged from its existing children (level 0: Surface Nets chunk meshes) and simplified with `meshopt_simplify` (borders not locked). Triangle count falls about 4× per level.
- Re-shading: each LOD vertex's normal is the full-resolution distance field's gradient at its position; its Ids and weights are the full-resolution cells' around it (the same rule Surface Nets uses: the lowest-distance corner's Id, weight 1).
- Skirts: for every border edge of a node's mesh (an edge used by one triangle), a quad folded inward along −normal by `max(2 × error, VoxelSize)`, with the edge's vertices' normals and Ids.
- Selection: draw a node when `error × paneHeight / (2 × tan(fovY / 2)) / distance < 1` pixel, distance from the camera to the nearest point of the node's world bounds (0 inside); else test its children; a node whose children are not all ready is drawn instead. Nodes outside the frustum are skipped with their children.
- Switching: when a node replaces its parent or children (or the reverse), both draw for 0.25 s with complementary 4×4 Bayer dither patterns in `terrain.frag`; shadows draw only the incoming one.
- RAM: levels ≥ 2 always resident in compact form; levels 0–1 resident only within `kNearChunks = 6` chunks (L∞, in chunk units) of the camera, dropped beyond `kFarChunks = 8` (hysteresis). GPU uploads not drawn for 5 s are freed.
- Edits: a re-meshed chunk marks all ancestors stale; a stale node rebuilds at most once per 100 ms, bottom-up; a level-1 rebuild re-meshes non-resident children from voxels.
- Colliders: chunk colliders exist only within `kColliderChunks = 3` chunks of a dynamic PhysicsObject or PlayerController; dropped 5 s after the last one leaves; a body with none around it gets them built synchronously in that physics sync.
- `workspace:Raycast` returns the same hit (instance, position within 0.05 × VoxelSize, normal, material) whether or not colliders are loaded where it hits.
- meshoptimizer: FetchContent, pinned tag `v0.22`, built via `cmake/meshoptimizer/CMakeLists.txt` like `cmake/zstd`; only `LodBuilder.cpp` includes `meshoptimizer.h`.
- Budgets (Release): selection < 0.5 ms per frame for a 4 km island (synthetic node set); building every level of a 4,096-chunk island < 5 s; RAM for levels ≥ 2 within the spec's estimate (≤ 12 bytes per compact vertex + 6 per triangle).
- Never `git stash`. Warning-free at /W4; MSVC 14.23 quirks (no std::endian/bit_cast, FLT_MAX literal, getenv). Commit messages: plain imperative sentence ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Cracks between levels** while the camera moves: no background pixels through seams. Pinned in Task 7 (render check) and Task 3 (skirt coverage).
2. **An edit near a level boundary**: both sides catch up; no stale geometry stays drawn longer than the debounce plus a build. Task 5 (LT4).
3. **A camera that sits at a residency boundary**: no rebuild churn. Task 5 (LT5).
4. **A body spawned far from the camera** lands, never falls through. Task 8 (CS2).
5. **A ray at distant terrain** gives the same answer with and without colliders. Task 9 (RM1).

---

### Task 1: meshoptimizer, the node key, and the compact node mesh

**Files:** Create `cmake/meshoptimizer/CMakeLists.txt`, `src/engine_core/terrain/LodNode.{hpp,cpp}`, `sandbox/terrain_lod_tests.cpp`; modify `CMakeLists.txt` (fetch beside zstd ~457-474; sources in the engine_core terrain list ~605-612; test file in the sandbox list; link meshoptimizer PRIVATE like zstd ~634); modify the LOD spec with the five decisions above (a short "Implementation notes" section).

**Interfaces:**

```cpp
namespace engine_core::terrain {
struct NodeKey {
    int level = 0, x = 0, y = 0, z = 0;
    bool operator==(const NodeKey&) const;
};
struct NodeKeyHash { std::size_t operator()(const NodeKey&) const; };
NodeKey node_of(ChunkCoord chunk, int level);          // floor division by 2^level
NodeKey parent_of(const NodeKey&);                     // level + 1
std::array<NodeKey, 8> children_of(const NodeKey&);    // level - 1 (level >= 1)
// Terrain-local bounds of a node, in studs.
void node_bounds(const NodeKey&, float voxel_size, Vec3& min, Vec3& max);

// A node's mesh as RAM keeps it. Positions quantized to the node's bounds.
struct CompactMesh {
    Vec3 origin{}, scale{};                         // position = origin + q / 65535 * scale
    std::vector<std::uint16_t> positions;           // 3 per vertex
    std::vector<std::uint8_t> normals;              // 2 per vertex, octahedral
    std::vector<std::uint8_t> ids;                  // 4 per vertex
    std::vector<std::uint8_t> weights;              // 4 per vertex, /255
    std::vector<std::uint16_t> indices;             // < 65536 vertices; else split (see below)
    std::size_t bytes() const;
};
CompactMesh pack(const anarchy::amesh::Data& mesh, Vec3 bounds_min, Vec3 bounds_max);
anarchy::amesh::Data unpack(const CompactMesh& mesh);   // full Vertex, u32 indices, aabb set
}
```

A node with more than 65,535 vertices after simplification keeps u32 indices in a second vector (`indices32`) instead; `bytes()` counts whichever is used.

- [ ] **Step 1: Tests** (`[terrain][lod]`): LN1 `node_of`/`parent_of`/`children_of` round-trip, negatives included; LN2 `node_bounds` of level 0 equals the chunk's 32-cell box; LN3 `pack`/`unpack` of a Surface Nets ball keeps every position within `scale / 65535` per axis, normals within 0.02 (dot ≥ 0.9998), Ids exact, weights within 1/255; LN4 `bytes()` ≤ 12 × vertices + 6 × triangles for u16 indices.
- [ ] **Step 2:** fail. **Step 3:** add meshoptimizer (FetchContent `https://github.com/zeux/meshoptimizer.git` `v0.22`; the wrapper globs `src/*.cpp` into a STATIC lib, SYSTEM include of `src`, /W3, `/O2` in Debug as zstd does), implement. **Step 4:** pass, plus the full `[terrain]`. **Step 5:** commit "Add LOD node keys and a compact node mesh, and fetch meshoptimizer".

---

### Task 2: LodBuilder: merge, simplify, measure

**Files:** Create `src/engine_core/terrain/LodBuilder.{hpp,cpp}`; tests in `sandbox/terrain_lod_tests.cpp`.

**Interfaces:**

```cpp
namespace engine_core::terrain {
struct LodInput {
    NodeKey key;
    float voxel_size = 1.f;
    std::vector<std::shared_ptr<const anarchy::amesh::Data>> children;   // existing children's meshes (unpacked)
    std::shared_ptr<const ChunkMap> voxels;                               // for re-shading (Task 3)
};
struct LodResult {
    NodeKey key;
    std::shared_ptr<const anarchy::amesh::Data> mesh;   // null: no triangles
    float error = 0.f;                                   // studs
    std::vector<std::uint32_t> border_edges;             // pairs of vertex indices
};
float target_error(int level, float voxel_size);         // 0.25 * voxel_size * 2^level
LodResult build_node(const LodInput& input);              // any thread; Task 3 adds re-shading and skirts
}
```

`build_node`: concatenate children (offset indices), weld exactly-equal positions (`meshopt_generateVertexRemap` on positions only), `meshopt_simplify(dst, indices, count, positions, vcount, stride, target_index_count = count / 4, target_error / meshopt_simplifyScale(...), 0 /*no lock*/, &result_error)`, compact (`meshopt_optimizeVertexFetch`), record `error = result_error * scale`, collect border edges (edges used by exactly one triangle).

- [ ] **Step 1: Tests:** LB1 a level-1 node of a rolling surface has ≤ 30% of its children's triangles and `error ≤ target_error(1, 1)`; LB2 the measured error is honest: every vertex of the children lies within `error + 1e-3` of the simplified surface (point-to-triangle distance against the result; brute force over a small case); LB3 levels 1..4 built bottom-up over a 16×1×16-chunk slab fall about 4× per level (each level between 2.5× and 6× fewer triangles); LB4 an empty child set gives a null mesh.
- [ ] **Steps 2–5:** fail, implement, pass, commit "Merge and simplify terrain LOD nodes with meshoptimizer".

---

### Task 3: Re-shading from full-resolution voxels, and skirts

**Files:** Modify `LodBuilder.{hpp,cpp}`; add a voxel sampler (`src/engine_core/terrain/VoxelSampler.{hpp,cpp}`): distance and gradient at any Terrain-local point from a `ChunkMap` by trilinear interpolation, and the lowest-distance corner's Id. Reuse SurfaceNets' trilinear/gradient math (factor it into the sampler and have SurfaceNets call it only if that keeps SN tests bit-identical; otherwise duplicate with a comment).

- [ ] **Step 1: Tests:** RS1 a level-2 node of a ball: every vertex normal within 0.05 (dot) of the analytic ball normal at that vertex; RS2 a node spanning a two-material boundary: each vertex's Id equals the full-resolution cell's Id at its position (sampled independently); RS3 skirts: every border edge has exactly one skirt quad, its far edge displaced along −normal by `max(2 × error, VoxelSize)` ± 1e-4; RS4 a level-1 node's skirt triangles face consistently with their source edge's triangle.
- [ ] **Steps 2–5:** fail, implement, pass, commit "Shade terrain LOD nodes from full-resolution voxels and add skirts".

---

### Task 4: Node jobs on the mesher pool

**Files:** Modify `src/engine_core/terrain/TerrainMesher.{hpp,cpp}`.

**Interfaces:** `void queue_node(std::uint64_t terrain, std::uint64_t revision, LodInput input, float distance);` results come back through `collect` in a second vector: `void collect(std::vector<MeshResult>& chunks, std::vector<NodeResult>& nodes);` with `struct NodeResult { std::uint64_t terrain; NodeKey key; std::uint64_t revision; LodResult result; };`. Keys are `(terrain, kind, coord-or-node)`, one queued job per key; ordering by distance across both kinds; exceptions caught like chunk jobs. Keep `collect(std::vector<MeshResult>&)` as a wrapper for existing callers that drops nothing (it must not lose node results: make the one-argument form assert no node jobs were ever queued, or migrate callers).

- [ ] **Step 1: Tests:** TM8 a queued node job comes back built; TM9 a newer node revision replaces a queued older one; TM10 node and chunk jobs interleave by distance; TM11 a node job that throws is dropped and counted.
- [ ] **Steps 2–5:** fail, implement, pass `[terrain]` (TM1–TM7 unchanged), commit "Build terrain LOD nodes on the mesher pool".

---

### Task 5: LodTree and TerrainWorld: building, edits, residency

**Files:** Create `src/engine_core/terrain/LodTree.{hpp,cpp}`; modify `src/engine_core/TerrainWorld.{hpp,cpp}`.

**Interfaces:**

```cpp
// TerrainWorld.hpp
struct TerrainNodeView {
    terrain::NodeKey key;
    std::uint64_t revision = 0;
    float error = 0.f;
    Vec3 bounds_min{}, bounds_max{};                         // Terrain-local
    std::shared_ptr<const anarchy::amesh::Data> mesh;        // unpacked for upload; level 0 = the chunk mesh
};
// TerrainView gains:
    std::shared_ptr<const std::vector<TerrainNodeView>> nodes;   // every resident node with a mesh; replaced, never changed
    std::uint64_t nodes_revision = 0;
    int top_level = 0;
```

`LodTree` (pure bookkeeping, unit-tested without threads): per node `{revision, built_revision, stale, last_build_ms, compact mesh, error, resident}`; `chunk_meshed(coord, mesh)`, `chunk_removed(coord)`, `node_built(NodeResult)`, `next_builds(now_ms, camera_chunk, out_inputs)` (stale nodes whose children are all present and whose 100 ms window passed, lowest level first), `update_residency(camera_chunk, out_drop_chunks, out_need_chunks)` with `kNearChunks`/`kFarChunks` hysteresis, `nodes_for_view()`.

`TerrainWorld::update`: feed accepted chunk meshes to the tree; on first sight build every level (chunks near the camera first, as today); queue `next_builds` as node jobs with a chunk-map snapshot (one `std::make_shared<const ChunkMap>(volume.chunks())` per update when any node job is queued); level-1 rebuilds whose children are not resident first queue those chunks; publish `nodes` when anything changed. Unpacked meshes for levels ≥ 2 are made on demand when published (cache the unpacked `shared_ptr` while the node's revision is unchanged).

- [ ] **Step 1: Tests:** LT1 a settled 8×1×8-chunk island has nodes at every level up to the top and one top node; LT2 levels ≥ 2 stay resident when the camera moves 50 chunks away, levels 0–1 near the old camera are dropped, and come back when it returns; LT3 an edit marks every ancestor stale and, after settling, each ancestor's revision advanced exactly once; LT4 50 edits to one chunk within 100 ms rebuild each ancestor at most twice (debounce); LT5 a camera oscillating ±1 chunk across `kNearChunks` does not re-mesh anything after the first settle (hysteresis); LT6 RAM: total `bytes()` of levels ≥ 2 for the island ≤ the per-vertex/triangle budget.
- [ ] **Steps 2–5:** fail, implement, pass `[terrain]` (TW tests unchanged), commit "Keep a terrain LOD octree built, current, and resident near the camera".

---

### Task 6: Selection, drawing, and cross-fades

**Files:** Modify `src/runner/TerrainDraws.{hpp,cpp}` (selection), `src/runner/GameView.cpp` (pass camera world, fov, pane height), `src/runner/MeshCache.{hpp,cpp}` (key by node: `getTerrainNode(terrain, NodeKey, data, revision)`, 5 s grace sweep), `src/runner/Renderer.{hpp,cpp}` (a per-draw fade uniform for terrain runs), `resources/shaders/pipeline/terrain.frag` (dither), `src/engine_core/SnapshotPump.cpp` and `src/runner/SceneFeed.cpp` only if new view fields need copying (they copy `TerrainView` whole today — confirm).

**Interfaces:**

```cpp
// TerrainDraws.hpp
struct TerrainCamera { Matrix4 world; float fov_y_degrees; int pane_height; };
// Pure: which nodes to draw, with fades. state carries fades between frames (keyed by terrain+node).
struct NodeChoice { std::size_t index; float fade; bool incoming; };
void SelectTerrainNodes(const engine_core::TerrainView& view, const TerrainCamera& camera, double now_seconds,
                        TerrainFadeState& state, std::vector<NodeChoice>& out);
float NodePixelError(float error, float distance, float fov_y_degrees, int pane_height);
```

`MeshDraw` gains `float terrainFade = 1.f; bool terrainFadeIn = true;`; the geometry pass sets `uniform float uFade; uniform int uFadeIn;` per terrain run; shadows skip draws with `terrainFadeIn == false`. `terrain.frag`:

```glsl
uniform float uFade;   // 1 = fully drawn
uniform int uFadeIn;   // 1: this draw is fading in; 0: fading out
const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
// in main(), first:
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
    if (uFadeIn == 1 ? threshold > uFade : threshold <= 1.0 - uFade) discard;
```

(The in-going and out-going draws use complementary tests of the same threshold, so together they cover every pixel once.)

- [ ] **Step 1: Tests** (sandbox, pure): SEL1 `NodePixelError` against hand-worked cases; SEL2 a camera far from a synthetic 7-level node set selects only the top node; moving it to the surface selects level 0 under it and coarser nodes away from it, and every leaf region is covered exactly once (no overlap, no hole) when no fades are active; SEL3 frustum culling drops nodes behind the camera; SEL4 a switch produces an incoming and an outgoing choice with fades summing to 1 that reach 1/0 after 0.25 s; SEL5 a node with a child missing is drawn instead of its children; SEL6 (bench, `[.][terrain-bench]`) selection over a synthetic 4 km node set (~16 k resident nodes) < 0.5 ms.
- [ ] **Step 2–3:** fail; implement (GameView passes `viewCamera_`, `viewFov_`, and its pane height into `AppendTerrainDraws`).
- [ ] **Step 4:** pass `[terrain]`; build Release `AnarchyStudio` and `scene-render-check`; `scene-render-check --compare` unchanged for non-terrain frames.
- [ ] **Step 5:** commit "Draw terrain by screen-space error, cross-fading between levels".

---

### Task 7: Seeing it: render check and Studio screenshots

**Files:** Modify `tests/SceneRenderCheck.cpp` (`--terrain-shots`): a large rolling island (e.g. 32×2×32 chunks) drawn through `TerrainWorld` + `SelectTerrainNodes` from a low grazing camera, a high far camera, and a sequence of 30 camera positions moving toward the surface (save every 10th frame), each frame checked for background pixels inside the island's screen-space silhouette (cracks); a debug coloring mode (`--terrain-lod-colors`) that tints each draw by its level, for one extra shot.

- [ ] **Step 1:** add the shots and the crack check; run; Read every PNG; fix anything wrong (cracks, popping visible in the sequence, black or inside-out nodes, missing shadows) in the owning task's code with a regression test.
- [ ] **Step 2:** Studio: drive the Release studio over MCP (the user's notes; a scratch copy of `C:\Users\Andrew\Documents\Anarchy Engine Projects\Terrain Demo`, never the original), build a 2 km island with Lua, and screenshot a far view, a grazing view, and the same far view with `--terrain-lod-colors`' equivalent if available in the studio (else only the render check has it). Save as `lod-*.png` under the scratchpad's screenshots folder.
- [ ] **Step 3:** commit "Show terrain LOD in the render check, with a crack check and level colors".

---

### Task 8: Collider streaming around dynamic bodies

**Files:** Modify `src/engine_core/TerrainWorld.{hpp,cpp}` (build colliders only for requested chunks), `src/engine_core/PhysicsWorld.{hpp,cpp}` (compute interest chunks from dynamic bodies each sync; synchronous build for a body with none).

**Interfaces:** `void TerrainWorld::set_collider_interest(InstanceId terrain, std::vector<terrain::ChunkCoord> chunks);` (called by PhysicsWorld at the start of sync with the chunks within `kColliderChunks` of each dynamic PhysicsObject/PlayerController, converted into each Terrain's local chunk space); `bool TerrainWorld::build_colliders_now(InstanceId terrain, std::span-like vector of coords);` (SimulationThread: meshes + builds those chunks synchronously, for the no-fall-through rule). The mesher's collider builder runs only for chunks in the interest set (pass a flag per chunk job). Colliders for chunks out of interest for 5 s are released.

- [ ] **Step 1: Tests:** CS1 with no dynamic bodies, a settled island has zero chunk shapes; CS2 a box created 2 km from the camera above bare terrain gets shapes under it in the same sync and lands (does not fall below the surface over 2 s); CS3 a body moving across the island keeps shapes only around itself (count stays bounded) and released shapes disappear 5 s after it leaves; CS4 a PlayerController walking keeps ground under it (no fall-through over a 30 s walk across chunk boundaries).
- [ ] **Steps 2–5:** fail, implement, pass `[terrain]` and `[physics]` (TP1–TP5 adjusted only where they assumed a shape for every chunk: give them a dynamic body or call the interest API), commit "Keep terrain colliders only around moving bodies".

---

### Task 9: Raycasts through terrain without colliders

**Files:** Modify `src/engine_core/PhysicsWorld.cpp` (`raycast`), use `VoxelSampler` (Task 3).

`raycast`: after Box3D's cast, for each Terrain in Workspace whose world bounds the ray crosses, march the segment in Terrain-local space over chunks that have no collider (a Terrain with CanCollide false is skipped entirely, as its shapes would be): sphere-trace with step = max(stored distance, 0.25 × VoxelSize) until a sign change, then bisect 8 times; normal from the sampler's gradient; material from the sampler's Id. Take the nearest of Box3D's hit and all march hits.

- [ ] **Step 1: Tests:** RM1 for 200 random rays at a settled island, `raycast` with colliders loaded everywhere (force interest) and with none loaded return the same instance and material and positions within 0.05 × VoxelSize; RM2 a ray at terrain behind a part hits the part; RM3 a CanCollide=false Terrain is not hit by the march; RM4 (bench) a 2 km ray across an island with no colliders < 0.2 ms.
- [ ] **Steps 2–5:** fail, implement, pass `[terrain]`, `[raycast]`, `[physics]`, commit "Ray-march terrain where it has no colliders".

---

### Task 10: Large-island budgets and docs

**Files:** `sandbox/terrain_lod_tests.cpp`, READMEs (`src/engine_core/README.md` if it lists systems, `src/engine_instances/README.md` Terrain paragraph).

- [ ] **Step 1: Benchmarks** (`[.][terrain-bench]`, Release): LB5 building every level of a 4,096-chunk island < 5 s (time from first update to `LodTree` settled, 4 workers); LB6 RAM for levels ≥ 2 of that island within budget, printed; a 16 k-chunk (2 km) island's first view appears progressively: chunks within `kNearChunks` of the camera are drawn within 1 s.
- [ ] **Step 2:** run, fix what they surface, update the READMEs, commit "Check terrain LOD budgets on large islands".
