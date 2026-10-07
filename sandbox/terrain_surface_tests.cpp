// Terrain surfaces: Surface Nets meshing, the mesher pool, TerrainWorld,
// terrain bodies, and what the renderer is handed.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "LuaApi.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "TerrainWorld.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/TerrainMesher.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

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

// Meshes every chunk of volume (plus each one's neighbors), welds all their
// triangles by exact vertex position, and asserts every resulting edge is
// used by exactly two triangles -- the watertight-seam check shared by SN3
// (a ball on a lattice-point seam) and SN3b (off-lattice seams).
void check_watertight(const VoxelVolume& volume) {
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
    check_watertight(volume);
}

TEST_CASE("SN3b an off-lattice ball across chunk seams meshes watertight", "[terrain]") {
    VoxelVolume volume;
    // Centers off any lattice point, spanning seams with non-zero chunk
    // origins on x (64), the y=32 seam, and the z=0 seam; a second ball
    // repeats the check near the origin's own seams.
    REQUIRE_FALSE(volume.fill(ball_at(64.37f, 31.61f, 0.23f, 6.3f), 1));
    REQUIRE_FALSE(volume.fill(ball_at(0.37f, 0.61f, 0.23f, 6.3f), 1));
    check_watertight(volume);
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

TEST_CASE("SN7 triangle winding agrees with vertex normals", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 3));
    const ChunkMesh mesh = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    REQUIRE_FALSE(mesh.triangles.empty());
    for (std::size_t t = 0; t < mesh.triangles.size(); t += 3) {
        const std::uint32_t ia = mesh.triangles[t], ib = mesh.triangles[t + 1], ic = mesh.triangles[t + 2];
        const Vec3& p0 = mesh.positions[ia];
        const Vec3& p1 = mesh.positions[ib];
        const Vec3& p2 = mesh.positions[ic];
        // The engine's CCW-from-outside convention: the cross product of a
        // triangle's edges (in winding order) is its outward geometric normal.
        const Vec3 e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        const Vec3 e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        const Vec3 geometric{
            e1.y * e2.z - e1.z * e2.y,
            e1.z * e2.x - e1.x * e2.z,
            e1.x * e2.y - e1.y * e2.x,
        };
        const auto& v0 = mesh.render->vertices[ia];
        const auto& v1 = mesh.render->vertices[ib];
        const auto& v2 = mesh.render->vertices[ic];
        const Vec3 average{
            v0.n[0] + v1.n[0] + v2.n[0],
            v0.n[1] + v1.n[1] + v2.n[1],
            v0.n[2] + v1.n[2] + v2.n[2],
        };
        const float dot = geometric.x * average.x + geometric.y * average.y + geometric.z * average.z;
        REQUIRE(dot > 0.f);
    }
}

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
            REQUIRE((r.revision == 1u || r.revision == 2u));
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

TEST_CASE("TM4 a revision replaced while still queued leaves exactly one result", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 2));
    TerrainMesher mesher({}, 1);
    // Hold the single worker before it takes a job, so both queue() calls for
    // key 9 land while nothing is running yet -- deterministically exercising
    // the replace-in-the-waiting-set path (as opposed to TM2, where revision 1
    // may already be running by the time revision 2 is queued).
    mesher.pause_for_test(true);
    mesher.queue(9, 1, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.queue(9, 2, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.pause_for_test(false);
    mesher.wait_idle();
    std::vector<MeshResult> results;
    mesher.collect(results);
    REQUIRE(results.size() == 1u);
    REQUIRE(results[0].terrain == 9u);
    REQUIRE(results[0].revision == 2u);
}

TEST_CASE("TM5 a result from a running job loses to a newer one queued while it ran", "[terrain]") {
    // threads=1, deterministic: the collider builder blocks the single
    // worker on job 1 until the test releases it, so job 2's queue() for the
    // same key is guaranteed to land while job 1 is already running (taken
    // off the waiting set), not merely waiting -- the path TM2/TM4 don't
    // reach, since there queue()'s replace-in-place always wins instead.
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 2));
    std::mutex state_mu;
    std::condition_variable state_cv;
    bool started = false;
    bool release = false;
    TerrainMesher mesher(
        [&](const ChunkMesh&) -> std::shared_ptr<void> {
            {
                std::lock_guard<std::mutex> lock(state_mu);
                started = true;
            }
            state_cv.notify_all();
            std::unique_lock<std::mutex> lock(state_mu);
            state_cv.wait(lock, [&] { return release; });
            return nullptr;
        },
        1);
    mesher.queue(9, 1, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    {
        // Blocks (no sleep) until the worker is inside the collider builder,
        // i.e. job 1 is running, not just waiting.
        std::unique_lock<std::mutex> lock(state_mu);
        state_cv.wait(lock, [&] { return started; });
    }
    mesher.queue(9, 2, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    {
        std::lock_guard<std::mutex> lock(state_mu);
        release = true;
    }
    state_cv.notify_all();
    mesher.wait_idle();
    std::vector<MeshResult> results;
    mesher.collect(results);
    REQUIRE(results.size() == 2u);
    // The single worker runs job 1 to completion before it can take job 2
    // (queued after job 1 was already taken), so they come back in order.
    REQUIRE(results[0].revision == 1u);
    REQUIRE(results[1].revision == 2u);
    // TerrainWorld-style filtering: a per-chunk counter that is now at 2
    // keeps only the result whose revision matches it.
    const std::uint64_t current_revision = 2;
    int accepted = 0;
    for (const MeshResult& r : results) {
        if (r.revision == current_revision) ++accepted;
    }
    REQUIRE(accepted == 1);
}

TEST_CASE("TM6 a job whose collider builder throws is dropped, not fatal", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 2));
    TerrainMesher mesher([](const ChunkMesh& mesh) -> std::shared_ptr<void> {
        if (mesh.triangles.empty()) {
            return nullptr;
        }
        throw std::runtime_error("TM6: a deliberately broken collider build");
    });
    mesher.queue(1, 1, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.wait_idle();   // must return: the throw must not wedge the worker or deadlock running_
    std::vector<MeshResult> results;
    mesher.collect(results);
    REQUIRE(results.empty());   // the failed job produced no result
    // The mesher is still usable: a later job for a different chunk (so its
    // mesh has no triangles and the builder above does not throw) comes back.
    mesher.queue(1, 1, mesh_input(volume, ChunkCoord{5, 5, 5}), 0.f);
    mesher.wait_idle();
    mesher.collect(results);
    REQUIRE(results.size() == 1u);
    REQUIRE(results[0].coord.x == 5);
}

TEST_CASE("TM7 the pool meshes every chunk bit-identically to running surface_nets serially", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 1));
    REQUIRE_FALSE(volume.fill(ball_at(40.f, 5.f, 5.f, 2.f), 2));
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 40.f, 5.f, 4.f), 3));
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 40.f, 4.f), 1));

    // Every stored chunk plus its neighbors: the same footprint mesh_all (above) covers.
    std::vector<ChunkCoord> coords;
    for (const auto& [coord, chunk] : volume.chunks()) {
        (void)chunk;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const ChunkCoord c{coord.x + dx, coord.y + dy, coord.z + dz};
                    if (std::find(coords.begin(), coords.end(), c) == coords.end()) coords.push_back(c);
                }
    }
    REQUIRE(coords.size() > 1u);

    TerrainMesher mesher({}, 4);   // several worker threads: pure function of each job's own input
    for (std::size_t i = 0; i < coords.size(); ++i) {
        mesher.queue(1, static_cast<std::uint64_t>(i + 1), mesh_input(volume, coords[i]), 0.f);
    }
    mesher.wait_idle();
    std::vector<MeshResult> results;
    mesher.collect(results);
    REQUIRE(results.size() == coords.size());

    auto same_vertex = [](const anarchy::amesh::Vertex& a, const anarchy::amesh::Vertex& b) {
        for (int i = 0; i < 3; ++i) {
            if (a.p[i] != b.p[i] || a.n[i] != b.n[i]) return false;
        }
        for (int i = 0; i < 4; ++i) {
            if (a.t[i] != b.t[i] || a.rgba[i] != b.rgba[i]) return false;
        }
        return true;
    };
    for (const MeshResult& result : results) {
        const ChunkMesh serial = surface_nets(mesh_input(volume, result.coord));
        REQUIRE(result.mesh.triangles == serial.triangles);
        REQUIRE(result.mesh.triangle_ids == serial.triangle_ids);
        REQUIRE(result.mesh.positions.size() == serial.positions.size());
        for (std::size_t i = 0; i < serial.positions.size(); ++i) {
            REQUIRE(result.mesh.positions[i].x == serial.positions[i].x);
            REQUIRE(result.mesh.positions[i].y == serial.positions[i].y);
            REQUIRE(result.mesh.positions[i].z == serial.positions[i].z);
        }
        REQUIRE((result.mesh.render == nullptr) == (serial.render == nullptr));
        if (result.mesh.render != nullptr) {
            REQUIRE(result.mesh.render->vertices.size() == serial.render->vertices.size());
            for (std::size_t i = 0; i < serial.render->vertices.size(); ++i) {
                REQUIRE(same_vertex(result.mesh.render->vertices[i], serial.render->vertices[i]));
            }
        }
    }
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

namespace {

Terrain& terrain_in_workspace(DataModel& game) {
    auto& t = game.create<Terrain>();
    game.set_parent(t.id(), workspace_of(game));
    return t;
}

void settle(TerrainWorld& world, DataModel& game) {
    for (int i = 0; i < 4; ++i) {
        world.update(game);
        world.wait_idle();
    }
    world.update(game);
}

// Plan 1a's terrain_instance_tests.cpp helper (it lives in that file's own
// anonymous namespace, so this file keeps its own copy, per the brief).
Material& add_material_asset(DataModel& game, const char* name) {
    Material& material = game.create<Material>();
    game.set_name(material.id(), name);
    game.set_parent(material.id(), game.service("Materials"));
    return material;
}

}  // namespace

TEST_CASE("TW1 a Terrain's chunks are meshed and shown", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    // Terrain::edit_volume (plan 1a Task 7) is not merged yet: edit through
    // volume() directly, as VoxelVolume marks its own dirty chunks.
    REQUIRE_FALSE(t.volume().fill(ball_at(5.f, 5.f, 5.f, 4.f), 0));
    TerrainWorld world;
    settle(world, game);
    REQUIRE(world.views().size() == 1u);
    REQUIRE_FALSE(world.views()[0].chunks->empty());
}

TEST_CASE("TW2 a Terrain outside Workspace is not shown", "[terrain]") {
    SimRole role;
    Game game;
    auto& t = game.create<Terrain>();
    game.set_parent(t.id(), game.scene_service("Storage"));
    REQUIRE_FALSE(t.volume().fill(ball_at(5.f, 5.f, 5.f, 4.f), 0));
    TerrainWorld world;
    settle(world, game);
    REQUIRE(world.views().empty());
}

TEST_CASE("TW3 an edit re-meshes only the chunks it touched and their neighbors", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.volume().fill(ball_at(5.f, 5.f, 5.f, 4.f), 0));
    // (208, 16, 16) sits in the middle of chunk {6, 0, 0} (16 studs from every
    // face), so the grown ball's band (radius 6 + the 4-cell band = 10 studs)
    // never reaches a second chunk on any axis -- unlike a center near 0,
    // where the band alone can cross the origin's chunk seam on two axes at
    // once and legitimately dirty more than one chunk's neighborhood.
    REQUIRE_FALSE(t.volume().fill(ball_at(208.f, 16.f, 16.f, 4.f), 0));
    TerrainWorld world;
    settle(world, game);
    const std::uint64_t before = world.meshed_count();
    REQUIRE_FALSE(t.volume().fill(ball_at(208.f, 16.f, 16.f, 6.f), 0));
    settle(world, game);
    REQUIRE(world.meshed_count() - before <= 27u);
}

TEST_CASE("TW4 a TerrainMaterial's Material changes the look, not the meshes", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(t.add_material(0, entry));
    REQUIRE_FALSE(t.volume().fill(ball_at(5.f, 5.f, 5.f, 4.f), 1));
    TerrainWorld world;
    settle(world, game);
    const auto chunks = world.views()[0].chunks;
    const std::uint64_t look = world.views()[0].look->revision;
    // A red Material, under the Assets service, as plan 1a's tests do.
    Material& red = add_material_asset(game, "Red");
    REQUIRE_FALSE(red.set_color(rgb(1.f, 0.f, 0.f)));
    LuaSlot material_slot;
    material_slot.kind = LuaSlot::Kind::Instance;
    material_slot.id = red.id();
    REQUIRE_FALSE(entry->set_material(material_slot));
    settle(world, game);
    REQUIRE(world.views()[0].chunks == chunks);           // same vector: nothing re-meshed
    REQUIRE(world.views()[0].look->revision != look);
    REQUIRE(world.views()[0].look->texels[1 * 4 + 0] == 255);   // Id 1, red
}

TEST_CASE("TW5 Stop re-meshes only what play changed", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.volume().fill(ball_at(5.f, 5.f, 5.f, 4.f), 0));
    REQUIRE_FALSE(t.volume().fill(ball_at(300.f, 5.f, 5.f, 4.f), 0));
    TerrainWorld world;
    settle(world, game);
    ChunkMap saved = t.volume().chunks();
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(t.volume().subtract(ball_at(5.f, 5.f, 5.f, 6.f)));
    settle(world, game);
    const std::uint64_t before = world.meshed_count();
    game.stop_simulation();
    // Plan 1a's place bytes (Task 7) are not merged yet, so Stop does not
    // restore the volume on its own: this does by hand what Terrain's
    // read_place will do once it lands (VoxelVolume::set_chunks with the map
    // captured at Play). Switch to capture_place/stop_simulation alone once
    // that is in.
    t.volume().set_chunks(saved);
    settle(world, game);
    REQUIRE(world.meshed_count() - before <= 27u);
    REQUIRE(world.views()[0].chunks->size() >= 2u);   // the near ball is back
}
