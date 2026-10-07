# Terrain Surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make terrain visible and solid: Surface Nets meshes built off-thread from the voxels, one static Box3D body per Terrain with a mesh shape per chunk (in edit and play mode), `workspace:Raycast` reporting terrain materials, and chunk meshes drawn in the deferred and shadow passes, each material as its flat color. LOD-ready, not LOD (that is plan 1d).

**Prerequisites:** both `edit-mode-physics` (sub-project 0: `PhysicsWorld::sync`, `RayHit`, `workspace:Raycast`) and `terrain-data` (plan 1a: `Terrain`, `TerrainMaterial`, `VoxelVolume`, `ChunkData`) are merged into `main`. Branch from that `main`. Where this plan names their types, read the merged code and use the real names if they differ.

**Architecture:** A new sim-side system, `TerrainWorld` (`src/engine_core/TerrainWorld.*`), owned by the Engine like `PhysicsWorld` and `AudioWorld`, keeps every Terrain in Workspace meshed: each tick it takes each Terrain's dirty chunks, hands mesh jobs to `TerrainMesher` (a pool of worker threads running the pure `SurfaceNets` mesher), collects finished meshes, and keeps a per-Terrain "look" (color and surface table built from its TerrainMaterials). `PhysicsWorld` reads the colliders from it; `SnapshotPump` publishes its meshes and looks to the render thread; the renderer draws them with a terrain shader. The voxels stay in `Terrain`; meshes live only in `TerrainWorld`.

**Tech Stack:** C++20 (as far as MSVC 14.23 goes), Box3D, OpenGL 3.3 / 4.1, GLSL 330, Catch2 (`sandbox`), CMake + Visual Studio 2019 generator.

**Spec:** `docs/superpowers/specs/2026-10-06-terrain-core-design.md` ("Meshing, drawing, colliding" and "Play, saving, place bytes"). This plan also records four implementation decisions the spec now reflects: material Ids ride in the vertex color channel (not bones), the look is a texture (not a uniform array), meshing runs on a small worker pool, and meshes live in `TerrainWorld`.

## Global Constraints

- Only `src/engine_core/PhysicsWorld.cpp` includes Box3D. The mesher builds colliders through an opaque `TerrainCollider` that `PhysicsWorld.cpp` defines.
- Chunk geometry is in Terrain-local space; a chunk's draw and body use the Terrain's Transform.
- Surface Nets: one vertex per cell the surface crosses, at the average of its edge crossings; normal from the distance field's gradient (central differences); a quad per surface-crossing lattice edge, owned by the chunk holding the edge's lower endpoint. Neighboring chunks produce bit-identical shared vertices: no cracks.
- Vertex layout (existing `GpuVertex`): position, normal; **material Ids in `color[0..3]`** (byte = Id), **their weights in `tangent[0..3]`**. This plan writes one Id (its most-solid corner sample's) in `color[0]` at weight `tangent[0] = 1`; the rest 0. `uv` 0. (Static uploads overwrite the bone channel, which is why it is not used.)
- Collision triangles: the same positions; each triangle's material Id (in `b3MeshDef::materialIndices`) is the Id of the solid endpoint of the edge that made its quad.
- A mesh result whose chunk revision is older than the chunk's current one is dropped.
- One `b3_staticBody` per Terrain in Workspace at its Transform; one mesh shape per chunk with triangles; each shape's surface materials carry `userMaterialId` = Id; CanCollide false means no shapes. Bodies exist in edit and play mode (sub-project 0's rule).
- `RaycastResult.Material` for a terrain hit is the Material of the TerrainMaterial with the hit triangle's Id; `nil` for Id 0, an unassigned Id, or a TerrainMaterial without a Material.
- The look: a 256 × 2 RGBA8 texture per Terrain. Row 0, texel i: Id i's sRGB color. Row 1: metalness, roughness, reflectivity (0–1 each), 255. Id 0 and unassigned Ids use the Material defaults. Uploaded only when its revision changes.
- Snapshot cost: each Terrain's chunk list and look sit behind `std::shared_ptr<const ...>` with a revision, so the three per-frame snapshot copies copy pointers only.
- Edits never block the simulation thread on meshing. Budgets (Release): meshing one dense surface chunk plus its collider under 2 ms on a worker; a 500-chunk island fully meshed within about 1 s of loading.
- Comments: plain English, say which thread and lock. Warning-free at /W4. Commit messages: a plain imperative sentence ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build: `MSYS_NO_PATHCONV=1 cmake --build build --config Debug --target sandbox --parallel`; run `build/Debug/sandbox.exe "[terrain]"`.

## Review Focus

1. **Seams between chunks.** A ball straddling a chunk corner (8 chunks) must mesh watertight: every edge used by exactly two triangles. Pinned in Task 1 (SN3).
2. **An edit while a job for that chunk is running.** The older result must not replace the newer one, and the newer edit must still reach the screen. Pinned in Task 2 (TM2).
3. **Changing a TerrainMaterial's Material, or its Color.** The look updates; no chunk is re-meshed. Pinned in Task 3 (TW4).
4. **Stop after digging during play.** The island is restored and only the chunks dug are re-meshed. Pinned in Task 3 (TW5).
5. **A body resting on terrain while the chunk under it is re-meshed.** It must not fall through. Pinned in Task 4 (TP4).

## File Structure

| File | Responsibility |
| --- | --- |
| `src/engine_core/terrain/SurfaceNets.{hpp,cpp}` | Pure mesher: 27 chunk pointers in, render mesh + collision triangles out |
| `src/engine_core/terrain/TerrainMesher.{hpp,cpp}` | Worker pool, job queue by camera distance, stale-result dropping |
| `src/engine_core/TerrainWorld.{hpp,cpp}` | Sim-side: which Terrains exist, their dirty chunks to jobs, results to per-Terrain meshes and colliders, looks |
| `src/engine_core/PhysicsWorld.{hpp,cpp}` | Terrain bodies and chunk shapes; `TerrainCollider`; ray materials |
| `src/engine_core/SnapshotPump.{hpp,cpp}` | `VisualTerrain` |
| `src/runner/MeshCache.*`, `Renderer.*`, `GameView.cpp`, `ShadowRenderer.cpp` | Chunk uploads, look textures, terrain program, drawing |
| `resources/shaders/pipeline/terrain.vert`, `terrain.frag` | The terrain program |
| `sandbox/terrain_surface_tests.cpp` | Tasks 1–5 |

---

### Task 1: The Surface Nets mesher

**Files:**
- Create: `src/engine_core/terrain/SurfaceNets.hpp`, `src/engine_core/terrain/SurfaceNets.cpp`
- Create: `sandbox/terrain_surface_tests.cpp`
- Modify: `CMakeLists.txt` (source; test file in the sandbox list)

**Interfaces:**
- Consumes: plan 1a's `ChunkData`, `ChunkPtr`, `ChunkCoord`, `Cell`, `kChunkSize`, `cell_index`, `dequantize`, `VoxelVolume`.
- Produces:

```cpp
namespace engine_core::terrain {

// What a chunk's mesh is made from: the chunk and its 26 neighbors, as shared
// immutable pointers (null is air). neighbors[(dz+1)*9 + (dy+1)*3 + (dx+1)] is
// the chunk at coord + (dx, dy, dz); index 13 is the chunk itself.
struct MeshInput {
    ChunkCoord coord;
    float voxel_size = 1.f;
    std::array<ChunkPtr, 27> neighbors;
};
// Gathers a MeshInput from volume. SimulationThread (it reads the chunk map).
MeshInput mesh_input(const VoxelVolume& volume, ChunkCoord coord);

// One chunk's surface, in Terrain-local space.
struct ChunkMesh {
    std::shared_ptr<const anarchy::amesh::Data> render;   // null when no triangles
    std::vector<Vec3> positions;                          // collision: the same points
    std::vector<std::uint32_t> triangles;                 // three per triangle
    std::vector<std::uint8_t> triangle_ids;               // one per triangle
};
// Any thread: reads only input's immutable chunks.
ChunkMesh surface_nets(const MeshInput& input);

}  // namespace engine_core::terrain
```

**How it works.** Sample `(i, j, k)` (chunk-local, may be −2..33) reads the chunk that holds it from `neighbors` and dequantizes its distance. A cell `(i, j, k)` is the cube between samples `(i..i+1, j..j+1, k..k+1)`. The chunk emits vertices for cells −1..31 on each axis (those touched by its owned edges) and quads for lattice edges whose lower endpoint is in 0..31 on all three axes:
- **Vertex of a cell** (only if its 8 corners do not all share a sign): the average of the points where its 12 edges cross zero (linear interpolation of the two distances). Normal: the normalized central-difference gradient of the trilinearly interpolated distance field at that point, using samples ±1 around it (hence the apron to −2..33). Its Id: the Id of the corner sample with the lowest distance; weight 1.
- **Quad of an edge** from sample `a` to `a + axis` where the signs differ: the four cells sharing that edge, wound so the quad faces from solid (negative) to air. Split into two triangles along the shorter diagonal. Triangle Id: the Id of the edge's negative endpoint.
- Quick reject: if all 27 neighbors are uniform (or null) and all have the same sign, return an empty `ChunkMesh`.
- Vertex positions are `(cell + offset) × voxel_size`, computed from integer chunk-absolute sample coordinates so two chunks computing the same cell's vertex produce the same floats.

- [ ] **Step 1: Write the failing tests**

Create `sandbox/terrain_surface_tests.cpp`:

```cpp
// Terrain surfaces: Surface Nets meshing, the mesher pool, TerrainWorld,
// terrain bodies, and what the renderer is handed.

#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <utility>

using namespace engine_core;
using namespace engine_core::terrain;

namespace {

Shape ball_at(float x, float y, float z, float r) {
    Shape s;
    s.center = Vec3{x, y, z};
    s.radius = r;
    return s;
}

// Every chunk the volume has, plus each one's neighbors, meshed.
std::vector<ChunkMesh> mesh_all(const VoxelVolume& volume) {
    std::vector<ChunkCoord> coords;
    for (const auto& [coord, chunk] : volume.chunks()) {
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const ChunkCoord c{coord.x + dx, coord.y + dy, coord.z + dz};
                    if (std::find(coords.begin(), coords.end(), c) == coords.end()) coords.push_back(c);
                }
    }
    std::vector<ChunkMesh> out;
    for (const ChunkCoord& c : coords) out.push_back(surface_nets(mesh_input(volume, c)));
    return out;
}

}  // namespace

TEST_CASE("SN1 a ball's vertices lie on the ball", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 3));
    const ChunkMesh mesh = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    REQUIRE(mesh.render != nullptr);
    REQUIRE_FALSE(mesh.triangles.empty());
    for (const Vec3& p : mesh.positions) {
        const float r = std::sqrt((p.x - 5.f) * (p.x - 5.f) + (p.y - 5.f) * (p.y - 5.f) + (p.z - 5.f) * (p.z - 5.f));
        REQUIRE(std::fabs(r - 4.f) <= 0.1f);
    }
    for (std::uint8_t id : mesh.triangle_ids) REQUIRE(id == 3);
    for (const auto& v : mesh.render->vertices) {
        REQUIRE(v.rgba[0] == 3);       // check amesh::Vertex's real field names
        REQUIRE(v.t[0] == 1.f);
    }
}

TEST_CASE("SN2 normals point out of the solid", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 1));
    const ChunkMesh mesh = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    for (const auto& v : mesh.render->vertices) {
        const float out = (v.p[0] - 5.f) * v.n[0] + (v.p[1] - 5.f) * v.n[1] + (v.p[2] - 5.f) * v.n[2];
        REQUIRE(out > 0.f);
    }
}

TEST_CASE("SN3 a ball across a chunk corner meshes watertight", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(0.f, 0.f, 0.f, 6.f), 1));   // the corner of 8 chunks
    // Weld all chunks' triangles by exact position, then count each edge's uses.
    std::map<std::tuple<float, float, float>, std::uint32_t> ids;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
    for (const ChunkMesh& mesh : mesh_all(volume)) {
        std::vector<std::uint32_t> welded;
        for (const Vec3& p : mesh.positions) {
            welded.push_back(ids.emplace(std::make_tuple(p.x, p.y, p.z), static_cast<std::uint32_t>(ids.size())).first->second);
        }
        for (std::size_t t = 0; t < mesh.triangles.size(); t += 3) {
            for (int e = 0; e < 3; ++e) {
                std::uint32_t a = welded[mesh.triangles[t + e]], b = welded[mesh.triangles[t + (e + 1) % 3]];
                if (a > b) std::swap(a, b);
                ++edges[{a, b}];
            }
        }
    }
    REQUIRE_FALSE(edges.empty());
    for (const auto& [edge, uses] : edges) REQUIRE(uses == 2);
}

TEST_CASE("SN4 empty and fully solid regions make no triangles", "[terrain]") {
    VoxelVolume volume;
    REQUIRE(surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0})).render == nullptr);
    Shape block;
    block.kind = Shape::Kind::Block;
    block.frame = matrix4_translation(16.f, 16.f, 16.f);
    block.size = Vec3{200.f, 200.f, 200.f};
    REQUIRE_FALSE(volume.fill(block, 1));
    REQUIRE(surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0})).triangles.empty());
}

TEST_CASE("SN5 a wall one cell thick still meshes both faces", "[terrain]") {
    VoxelVolume volume;
    Shape wall;
    wall.kind = Shape::Kind::Block;
    wall.frame = matrix4_translation(16.f, 16.f, 16.f);
    wall.size = Vec3{1.f, 20.f, 20.f};
    REQUIRE_FALSE(volume.fill(wall, 1));
    const ChunkMesh mesh = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    bool plus = false, minus = false;
    for (const auto& v : mesh.render->vertices) {
        plus = plus || v.n[0] > 0.9f;
        minus = minus || v.n[0] < -0.9f;
    }
    REQUIRE(plus);
    REQUIRE(minus);
}

TEST_CASE("SN6 meshing one dense chunk is fast", "[.][terrain-bench]") {
    VoxelVolume volume;
    for (int i = 0; i < 6; ++i) {
        REQUIRE_FALSE(volume.fill(ball_at(5.f + i * 4.f, 10.f + (i % 3) * 5.f, 16.f, 6.f), 1));
    }
    const MeshInput input = mesh_input(volume, ChunkCoord{0, 0, 0});
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) (void)surface_nets(input);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 50.0;
    INFO(ms);
    REQUIRE(ms < 1.5);   // leaves room for the collider build in the 2 ms budget
}
```

(`#include <chrono>`, `<algorithm>`, `<tuple>`. Use the real `amesh::Vertex` field names from `src/amesh/amesh.hpp` — the CPU-side vertex, not `GpuVertex`.)

- [ ] **Step 2: Run to verify they fail** (header missing).
- [ ] **Step 3: Implement** as described under "How it works". Keep the sample lookup branch-light: before the loops, copy the needed 36³ distances into a local `std::vector<float>` (one dequantize per sample) and the Ids into a parallel byte array; the cell and edge loops then index those. Cell vertex indices go in a 33³ table (−1..31) initialized to "none".
- [ ] **Step 4: Run tests** (`[terrain]`; Release `[terrain-bench]` for SN6). **Step 5: Commit** (`Mesh terrain chunks with Surface Nets`).

---

### Task 2: The mesher pool

**Files:**
- Create: `src/engine_core/terrain/TerrainMesher.hpp`, `src/engine_core/terrain/TerrainMesher.cpp`
- Modify: `sandbox/terrain_surface_tests.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1.
- Produces:

```cpp
namespace engine_core::terrain {

// What a worker hands back for one chunk.
struct MeshResult {
    std::uint64_t terrain = 0;            // the caller's key (a Terrain's InstanceId)
    ChunkCoord coord;
    std::uint64_t revision = 0;           // the caller's revision for this chunk when queued
    ChunkMesh mesh;
    std::shared_ptr<void> collider;       // what build_collider made, or null
};

// Meshes chunks on worker threads (hardware threads less one, at least one,
// at most 4). The caller queues jobs and collects results on its own thread
// (SimulationThread); workers read only the jobs' immutable chunks.
class TerrainMesher {
public:
    // Builds a physics collider from a finished mesh, on the worker. Optional.
    using BuildCollider = std::function<std::shared_ptr<void>(const ChunkMesh&)>;
    explicit TerrainMesher(BuildCollider build = {}, unsigned threads = 0);
    ~TerrainMesher();   // drops waiting jobs, finishes running ones, joins

    // Queues input under (terrain, coord); replaces a queued, not yet started
    // job for the same key. distance orders the queue: nearest first.
    void queue(std::uint64_t terrain, std::uint64_t revision, MeshInput input, float distance);
    // Every result finished since the last call.
    void collect(std::vector<MeshResult>& out);
    // No job waiting or running.
    bool idle() const;
    // For tests: blocks until idle.
    void wait_idle();
};

}  // namespace engine_core::terrain
```

The caller (TerrainWorld, Task 3) drops a result whose revision is not the chunk's current one. The mesher itself only guarantees one queued job per key.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("TM1 queued chunks come back meshed", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 2));
    TerrainMesher mesher;
    mesher.queue(7, 1, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.wait_idle();
    std::vector<MeshResult> results;
    mesher.collect(results);
    REQUIRE(results.size() == 1u);
    REQUIRE(results[0].terrain == 7u);
    REQUIRE(results[0].revision == 1u);
    REQUIRE_FALSE(results[0].mesh.triangles.empty());
}

TEST_CASE("TM2 a newer job for a chunk replaces a queued older one", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 2));
    TerrainMesher mesher({}, 1);
    // Keep the one worker busy so the next two jobs wait in the queue.
    for (int i = 0; i < 20; ++i) mesher.queue(1, 1, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.queue(9, 1, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.queue(9, 2, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.wait_idle();
    std::vector<MeshResult> results;
    mesher.collect(results);
    int nine = 0;
    for (const MeshResult& r : results) {
        if (r.terrain == 9) {
            ++nine;
            REQUIRE(r.revision == 2u);
        }
    }
    REQUIRE(nine <= 2);   // revision 1 may have started before 2 was queued; then both come back
    REQUIRE(std::any_of(results.begin(), results.end(), [](const MeshResult& r) { return r.terrain == 9 && r.revision == 2; }));
}

TEST_CASE("TM3 the collider builder runs on the worker and its result comes back", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 2));
    TerrainMesher mesher([](const ChunkMesh& mesh) { return std::make_shared<std::size_t>(mesh.triangles.size()); });
    mesher.queue(1, 1, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.wait_idle();
    std::vector<MeshResult> results;
    mesher.collect(results);
    REQUIRE(*std::static_pointer_cast<std::size_t>(results[0].collider) == results[0].mesh.triangles.size());
}
```

Note TM2's "both come back" case: fix the assertion so it holds — require that revision 2 comes back and that no result for key 9 has a revision other than 1 or 2; then a separate deterministic test with `threads = 1` and a job the worker is blocked on (give the mesher a test-only pause hook, `void pause_for_test(bool)`, that holds workers before they take a job) asserts exactly one result, revision 2.

- [ ] **Step 2–5:** fail, implement (a `std::mutex`, `std::condition_variable`, a map key→job for the waiting set plus a min-heap by distance with lazy deletion, a results vector, a running count; workers loop take→mesh→build collider→push result), pass `[terrain]`, commit (`Mesh terrain chunks on a pool of worker threads`).

---

### Task 3: TerrainWorld

**Files:**
- Create: `src/engine_core/TerrainWorld.hpp`, `src/engine_core/TerrainWorld.cpp`
- Modify: `src/engine_core/DataModel.hpp/.cpp` (a list of Terrains, as `physics_bodies` lists PhysicsBases), `src/engine_instances/Terrain.hpp/.cpp` (the virtual that puts it in that list)
- Modify: `src/engine_core/Engine.hpp/.cpp` (own and step it, playing and stopped; pass it to physics and the snapshot)
- Modify: `sandbox/terrain_surface_tests.cpp`

**Interfaces:**
- Consumes: Tasks 1–2; plan 1a's `Terrain::volume()`, `VoxelVolume::take_dirty`, `Terrain::materials()`, `TerrainMaterial::material_instance()`; `Workspace::current_camera()` (camera position, as `AudioWorld::place_listener`, `AudioWorld.cpp:387-396`).
- Produces:

```cpp
namespace engine_core {

// A Terrain's look: what each material Id draws as. Immutable once published.
struct TerrainLook {
    // 256 x 2 RGBA8, row-major: row 0 sRGB color, row 1 (metalness, roughness,
    // reflectivity, 255). Index 0 and unassigned Ids use the Material defaults.
    std::array<std::uint8_t, 256 * 2 * 4> texels{};
    std::uint64_t revision = 0;   // unique across all looks
};

struct TerrainChunkView {
    terrain::ChunkCoord coord;
    std::uint64_t revision = 0;   // unique across all chunks
    std::shared_ptr<const anarchy::amesh::Data> mesh;
};

// What one Terrain shows, for the renderer and physics.
struct TerrainView {
    InstanceId terrain = 0;
    Matrix4 transform = matrix4_identity();
    bool can_collide = true;
    std::shared_ptr<const std::vector<TerrainChunkView>> chunks;   // replaced, never changed
    std::uint64_t chunks_revision = 0;
    std::shared_ptr<const TerrainLook> look;
};

// Keeps every Terrain in Workspace meshed. SimulationThread, under the write
// lock: the Engine calls update once per tick, playing or stopped.
class TerrainWorld {
public:
    explicit TerrainWorld(terrain::TerrainMesher::BuildCollider build = {}, unsigned threads = 0);
    // Finds Terrains, queues their dirty chunks (all of them the first time a
    // Terrain is seen), collects finished meshes, rebuilds changed looks.
    void update(DataModel& game);
    const std::vector<TerrainView>& views() const;
    // Colliders by chunk, for PhysicsWorld: the latest for each meshed chunk.
    struct ChunkCollider { terrain::ChunkCoord coord; std::uint64_t revision; std::shared_ptr<void> collider; };
    const std::vector<ChunkCollider>* colliders(InstanceId terrain) const;
    // For tests.
    void wait_idle();
    std::uint64_t meshed_count() const;   // results accepted since construction
};

}  // namespace engine_core
```

Revisions: TerrainWorld keeps, per Terrain, a counter per chunk coordinate incremented each time it queues that chunk; a result is accepted only if its revision equals the current counter. A Terrain that leaves Workspace (or is destroyed) drops its record; returning re-queues everything. A Stop's restore changes chunk pointers through `VoxelVolume::set_chunks`, which marks exactly the changed chunks dirty, so only those are re-meshed.

The look is rebuilt when any TerrainMaterial child, its Material reference, or that Material's color, roughness, metalness, or reflectivity changed: compare against the last inputs each update (255 entries; cheap) rather than listening for events. It never touches chunk meshes.

- [ ] **Step 1: Write the failing tests** (add `#include "TerrainWorld.hpp"`, `"Terrain.hpp"`, `"TerrainMaterial.hpp"`, `"AssetInstances.hpp"`, `"support.hpp"`):

```cpp
namespace {
engine_core::Terrain& terrain_in_workspace(engine_core::DataModel& game) {
    auto& t = game.create<engine_core::Terrain>();
    game.set_parent(t.id(), workspace_of(game));
    return t;
}
void settle(engine_core::TerrainWorld& world, engine_core::DataModel& game) {
    for (int i = 0; i < 4; ++i) { world.update(game); world.wait_idle(); }
    world.update(game);
}
}  // namespace

TEST_CASE("TW1 a Terrain's chunks are meshed and shown", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.edit_volume([](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0); }));
    engine_core::TerrainWorld world;
    settle(world, game);
    REQUIRE(world.views().size() == 1u);
    REQUIRE_FALSE(world.views()[0].chunks->empty());
}

TEST_CASE("TW2 a Terrain outside Workspace is not shown", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    auto& t = game.create<engine_core::Terrain>();
    game.set_parent(t.id(), game.scene_service("Storage"));
    REQUIRE_FALSE(t.edit_volume([](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0); }));
    engine_core::TerrainWorld world;
    settle(world, game);
    REQUIRE(world.views().empty());
}

TEST_CASE("TW3 an edit re-meshes only the chunks it touched and their neighbors", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.edit_volume([](VoxelVolume& v) {
        if (auto e = v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0)) return e;
        return v.fill(ball_at(200.f, 5.f, 5.f, 4.f), 0);
    }));
    engine_core::TerrainWorld world;
    settle(world, game);
    const std::uint64_t before = world.meshed_count();
    REQUIRE_FALSE(t.edit_volume([](VoxelVolume& v) { return v.fill(ball_at(200.f, 5.f, 5.f, 6.f), 0); }));
    settle(world, game);
    REQUIRE(world.meshed_count() - before <= 27u);
}

TEST_CASE("TW4 a TerrainMaterial's Material changes the look, not the meshes", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& t = terrain_in_workspace(game);
    engine_core::TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(t.add_material(0, entry));
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 1); }));
    engine_core::TerrainWorld world;
    settle(world, game);
    const auto chunks = world.views()[0].chunks;
    const std::uint64_t look = world.views()[0].look->revision;
    // Give it a red Material (make one under the Assets service as plan 1a's tests do; set Color to red).
    // ... entry->set_material(slot of that Material) ...
    settle(world, game);
    REQUIRE(world.views()[0].chunks == chunks);           // same vector: nothing re-meshed
    REQUIRE(world.views()[0].look->revision != look);
    REQUIRE(world.views()[0].look->texels[1 * 4 + 0] == 255);   // Id 1, red
}

TEST_CASE("TW5 Stop re-meshes only what play changed", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.edit_volume([](VoxelVolume& v) {
        if (auto e = v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0)) return e;
        return v.fill(ball_at(300.f, 5.f, 5.f, 4.f), 0);
    }));
    engine_core::TerrainWorld world;
    settle(world, game);
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(t.edit_volume([](VoxelVolume& v) { return v.subtract(ball_at(5.f, 5.f, 5.f, 6.f)); }));
    settle(world, game);
    const std::uint64_t before = world.meshed_count();
    game.stop_simulation();
    settle(world, game);
    REQUIRE(world.meshed_count() - before <= 27u);
    REQUIRE(world.views()[0].chunks->size() >= 2u);   // the near ball is back
}
```

Fill in TW4's Material creation from plan 1a's `terrain_instance_tests.cpp` helpers (`add_material_asset`) and the Material color setter's real name.

- [ ] **Step 2–3:** fail; implement. `DataModel`: add `virtual bool terrain() const { return false; }` beside `physics_body()`, overridden by `Terrain`, and `void terrains(std::vector<InstanceId>& out) const;` built the way `physics_bodies` is (read its implementation and mirror it, including how it limits to Workspace descendants). Camera distance per job: `|chunk center in world − camera position|`, with the camera from `Workspace::current_camera()`; no camera → 0 for all.
- [ ] **Step 4: Engine wiring.** `Engine` owns `TerrainWorld terrain_` constructed with `PhysicsWorld::build_terrain_collider` (Task 4 adds it; until then pass `{}` and add it in Task 4). Call `terrain_.update(game_)` in the stopped tick (next to `physics_.sync`, under the same write lock) and once per frame while playing, before the first physics substep (find the per-frame, not per-substep, point in `Engine::simulation_loop`). Hand `&terrain_` to the snapshot pump (Task 5) and physics (Task 4).
- [ ] **Step 5:** run `[terrain]` and the full suite; commit (`Keep every Terrain in Workspace meshed in a TerrainWorld`).

---

### Task 4: Terrain bodies, chunk shapes, and ray materials

**Files:**
- Modify: `src/engine_core/PhysicsWorld.hpp`, `src/engine_core/PhysicsWorld.cpp`
- Modify: `src/engine_core/RaycastBindings.cpp` (sub-project 0's file: `RaycastResult.Material`)
- Modify: `src/engine_core/Engine.cpp` (pass the collider builder and `&terrain_`)
- Modify: `sandbox/terrain_surface_tests.cpp`

**Interfaces:**
- Consumes: `TerrainWorld::views()`, `TerrainWorld::colliders(id)` (Task 3); sub-project 0's `sync`, `raycast`, `RayHit`.
- Produces:
  - `static std::shared_ptr<void> PhysicsWorld::build_terrain_collider(const terrain::ChunkMesh& mesh);` — any thread. Builds a `b3MeshDef` from `positions`/`triangles` with `materialIndices = triangle_ids`, `weldVertices` and `identifyEdges` as `build_mesh` does (`PhysicsWorld.cpp:238-252`), and returns a shared pointer to an internal `TerrainCollider { b3MeshData* mesh; ~TerrainCollider() { b3DestroyMesh(mesh); } }`. Null for a mesh with no triangles. Verify `b3CreateMesh` touches no global state (read `mesh.c`); if it does, build colliders on SimulationThread in `sync` instead and note it.
  - `void PhysicsWorld::set_terrain_world(const TerrainWorld* terrains);` — `sync` reconciles a body per `TerrainView`: `b3_staticBody` at `transform` (`b3Body_SetTransform` when it changes), one `b3CreateMeshShape` per chunk collider whose revision changed (destroying the old shape first), none at all when `can_collide` is false. The shape def's `materials` is a 256-entry array of `b3SurfaceMaterial`, each `userMaterialId = index` (friction and restitution at the PhysicsObject defaults), `materialCount = 256`. The record keeps each chunk's `shared_ptr<void>` collider alive as long as its shape exists. `userData` is the Terrain's id, as other bodies'.
  - `RayHit` for a terrain shape: `has_material = true`, `material = userMaterialId` (from the cast callback's `userMaterialId`).
  - `RaycastResult.Material`: when `Instance` is a Terrain and `has_material`, the Material of `terrain->material_by_id(material)` (nil when none).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("TP1 a Terrain has a static body with a shape per meshed chunk, stopped", "[terrain][physics]") {
    // Terrain in Workspace with a ball; TerrainWorld world(PhysicsWorld::build_terrain_collider);
    // PhysicsWorld physics; physics.set_terrain_world(&world); settle(world, game); physics.sync(game);
    // REQUIRE(physics.has_body(terrain.id())); REQUIRE(shape count for it == number of chunks with triangles)
    // (add a test accessor `std::size_t PhysicsWorld::shape_count(InstanceId) const` if shape_frictions does not fit)
}

TEST_CASE("TP2 CanCollide false removes every shape; true brings them back", "[terrain][physics]") { /* as TP1 */ }

TEST_CASE("TP3 a ray hits the terrain and reports its TerrainMaterial's Material", "[terrain][physics]") {
    // TerrainMaterial Id 1 with a Material "Rock"; fill a block whose top is at y = 0 with Id 1.
    // physics.raycast(game, {0,10,0}, {0,-20,0}, {}) -> instance == terrain, has_material, material == 1,
    // position.y within 0.1 of 0.
    // Then through Lua (RaycastRig from sub-project 0's tests, with a TerrainWorld wired):
    //   print(workspace:Raycast(Vector3.new(0,10,0), Vector3.new(0,-20,0)).Material.Name) -> "Rock"
}

TEST_CASE("TP4 a box resting on terrain stays up while its chunk is re-meshed", "[terrain][physics]") {
    // Fill a flat slab (top y = 0). Play. Drop a 1x1x1 PhysicsObject from y = 3; run 2 s of steps
    // (calling world.update(game) once per 1/60 s and physics.step per 1/240 s) -> it rests near y = 0.5.
    // Edit the slab away from the box but inside its chunk (paint a different Id under it, which
    // re-meshes the chunk); run 1 s more: still near y = 0.5.
}
```

Write these concretely, reusing `physics_rig.hpp` (`PhysicsRig`, `at`, `y_of`, `near`) and the `settle` helper.

- [ ] **Step 2–5:** fail, implement, run `[terrain]` and `[physics]` and the full suite, commit (`Give each Terrain a static body with a mesh shape per chunk`).

---

### Task 5: Terrains in the snapshot

**Files:**
- Modify: `src/engine_core/SnapshotPump.hpp/.cpp`, `src/engine_core/Engine.cpp`
- Modify: `sandbox/terrain_surface_tests.cpp`

**Interfaces:**
- Produces in `VisualSnapshot`:

```cpp
    // Every Terrain in Workspace, as TerrainWorld shows it. Pointers only: the
    // chunk list and look are immutable and shared with the simulation.
    std::vector<TerrainView> terrains;
```

  - `void SnapshotPump::set_terrain_world(const TerrainWorld* terrains);` and in `take_changes`, after `resolve_billboards`, `resolve_terrains(game)` copies `terrains->views()` into `base_.terrains` (a vector of small structs of pointers; `blit` copies it like `draggers`).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("TS1 the snapshot carries each Terrain's chunks, transform, and look", "[terrain][render]") {
    // As prefab_render_tests.cpp's Scene (SnapshotPump pump; pump.reserve(...); frame() = prepare_copy + publish),
    // plus a TerrainWorld settled on a Terrain with a ball and Transform at (10,0,0);
    // pump.set_terrain_world(&world); scene.frame();
    // REQUIRE(pump.front().terrains.size() == 1); its chunks pointer == world.views()[0].chunks;
    // its transform x == 10; its look != nullptr.
}
```

- [ ] **Step 2–5:** fail, implement, pass, commit (`Put terrain chunks and looks in the visual snapshot`).

---

### Task 6: Drawing terrain

**Files:**
- Create: `resources/shaders/pipeline/terrain.vert`, `resources/shaders/pipeline/terrain.frag`
- Modify: `src/runner/MeshCache.hpp/.cpp` (chunk uploads), `src/runner/Renderer.hpp/.cpp` (program, look textures, passes), `src/runner/GameView.cpp` (`collectMeshes`), `src/runner/ShadowRenderer.cpp` only if needed
- Modify: `tests/SceneRenderCheck.cpp` (a GL check)

**Interfaces:**
- `MeshDraw` gains `std::uint32_t terrainLook = 0;` (a GL texture; non-zero means "draw with the terrain program").
- `MeshCache::getTerrainChunk(InstanceId terrain, terrain::ChunkCoord coord, const anarchy::amesh::Data& data, std::uint64_t revision)` and `sweepTerrainChunks()`, as `getSession`/`sweepSessions` but keyed by (terrain, coord) and uploading static (`dynamic = false`).
- `Renderer::terrainLookTexture(InstanceId terrain, const TerrainLook& look) -> std::uint32_t`: a 256 × 2 `GL_RGBA8` texture per Terrain, `GL_NEAREST`, re-uploaded with `glTexImage2D`/`glTexSubImage2D` when `look.revision` changes; swept like the chunks.

- [ ] **Step 1: Shaders**

`terrain.vert`:

```glsl
#version 330 core
// A terrain chunk: positions in the Terrain's space, aModel its Transform.
// The vertex's material Ids ride in aColor (one byte each), their weights in
// aTangent. This version draws the first Id alone.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 3) in vec4 aTangent;
layout (location = 4) in vec4 aColor;
layout (location = 7) in mat4 aModel;
layout (location = 11) in mat3 aNormalMatrix;
uniform mat4 uView;
uniform mat4 uProjection;
out vec3 vViewPosition;
out vec3 vViewNormal;
flat out int vMaterial;
void main() {
    vec4 viewPosition = uView * (aModel * vec4(aPosition, 1.0));
    vViewNormal = mat3(uView) * (aNormalMatrix * aNormal);
    vViewPosition = viewPosition.xyz;
    vMaterial = int(aColor.r * 255.0 + 0.5);
    gl_Position = uProjection * viewPosition;
}
```

`terrain.frag` writes the same four G-buffer targets as `deferred.frag`:

```glsl
#version 330 core
// The G-buffer for a terrain chunk: each material Id's look from the
// Terrain's 256 x 2 table (row 0 color, row 1 metalness, roughness,
// reflectivity).
in vec3 vViewPosition;
in vec3 vViewNormal;
flat in int vMaterial;
uniform sampler2D uTerrainLook;
layout (location = 0) out vec4 gAlbedo;
layout (location = 1) out vec4 gNormal;
layout (location = 2) out vec4 gMaterial;
layout (location = 3) out vec4 gEmissive;
vec3 toLinear(vec3 srgb) { return pow(max(srgb, vec3(0.0)), vec3(2.2)); }
void main() {
    vec4 color = texelFetch(uTerrainLook, ivec2(vMaterial, 0), 0);
    vec4 surface = texelFetch(uTerrainLook, ivec2(vMaterial, 1), 0);
    vec3 N = normalize(vViewNormal);
    if (!gl_FrontFacing) N = -N;
    gAlbedo = vec4(toLinear(color.rgb), 1.0);
    gNormal = vec4(N, 1.0);
    gMaterial = vec4(surface.r, max(0.05, surface.g), surface.b, 1.0);
    gEmissive = vec4(0.0, 0.0, 0.0, 1.0);
}
```

Match `deferred.frag`'s output encoding exactly (read it: if `gNormal` or `gMaterial` are packed differently than shown, follow the file). Remember the JadeFX shader-folder copy (`CMakeLists.txt:1003-1007`) can mask a missing shader in a build dir: test from a clean copy of `resources/shaders`.

- [ ] **Step 2: Program and passes**

- `Renderer::initialize`: `buildProgram(terrain_, "Terrain", "pipeline/terrain.vert", "pipeline/terrain.frag", {})`; cache `uView`, `uProjection`, and set `uTerrainLook` to a fixed texture unit once.
- `geometryPass`: when a run's `MeshDraw::terrainLook != 0`, use `terrain_` (set view/projection once when switching to it), bind the look texture, and draw. Give terrain draws their own batch slots (one per chunk; they are never instanced together) and sort them after the other opaque runs so the program switches once.
- Every other pass that draws `MeshDraw`s with a surface program (forward/transparent, reflection or probe passes — search `bindMaterial(` and `geometry_.id`/`forward_.id`) skips terrain draws or draws them with `terrain_`; shadows need no change (depth reads only position and `aModel`) as long as terrain draws are in the caster list.
- `GameView::collectMeshes`: for each `snapshot.terrains` view, get the look texture and, for each chunk with a mesh, `meshes_.getTerrainChunk(...)`; push a `MeshDraw` with `mesh`, `model = view.transform`, `terrainLook`, `owner = view.terrain`, `slot = 0` (drawn alone), white tint. Call `sweepTerrainChunks()` and the look sweep after the loop.

- [ ] **Step 3: GL check.** In `tests/SceneRenderCheck.cpp`, following its cube checks: mesh a ball with `surface_nets`, upload it through `getTerrainChunk`, make a look whose Id 1 is pure red, draw it with `terrainLook` set, and `Expect` the center pixel is red-dominant. Run the check executable from the repo root as its comment says.

- [ ] **Step 4: Run** the full sandbox suite, the SceneRenderCheck executable (with `--compare` to confirm the regression frames are unchanged), and launch the Release studio: insert a Terrain, run `workspace.Terrain:FillBall(Vector3.new(0, 10, 0), 8, workspace.Terrain:AddMaterial(nil))` from the command bar, and confirm a gray ball appears with a shadow. Screenshot it for the report.

- [ ] **Step 5: Commit** (`Draw terrain chunks with their materials' colors`).

---

### Task 7: Loading time, the play loop, and docs

**Files:**
- Modify: `sandbox/terrain_surface_tests.cpp`, `src/engine_instances/README.md` (Terrain paragraph: meshing and colliding), `src/engine_core/README.md` if it lists systems

- [ ] **Step 1: Benchmarks and end-to-end tests**

```cpp
TEST_CASE("TL1 a 500-chunk island is fully meshed about a second after it appears", "[.][terrain-bench]") {
    // Fill a 16 x 2 x 16-chunk rolling slab (sum of balls) in a Terrain in Workspace -> >= 500 chunks
    // with surface. Time from the first world.update to the point where every chunk with a surface
    // has a mesh (world.update + wait_idle loop). REQUIRE < 1.5 s in Release; INFO the time.
}

TEST_CASE("TL2 during play, digging under a resting box drops it", "[terrain][physics]") {
    // Slab, box resting on it (as TP4). During play, edit_volume a SubtractBlock under the box.
    // Step 2 s with updates: the box's y is well below where it rested.
}
```

- [ ] **Step 2:** run (Release for TL1), fix anything they surface, update the READMEs, commit (`Check terrain loading time and digging during play`).
