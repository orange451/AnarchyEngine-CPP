// Terrain surfaces: Surface Nets meshing, the mesher pool, TerrainWorld,
// terrain bodies, and what the renderer is handed.

#include "physics_rig.hpp"
#include "support.hpp"

#include "AssetInstances.hpp"
#include "LuaApi.hpp"
#include "PhysicsWorld.hpp"
#include "SnapshotPump.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "TerrainWorld.hpp"
#include "terrain/LodBuilder.hpp"
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
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
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
    // TerrainWorld's own revision-filtering (accept_result, which keeps only
    // the result matching a chunk's current counter) is covered end to end
    // by TW7 below, not re-simulated here.
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

TEST_CASE("TM8 a queued node job comes back built", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 2));
    const ChunkMesh child_mesh = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    REQUIRE(child_mesh.render != nullptr);

    LodInput input;
    input.key = NodeKey{1, 0, 0, 0};
    input.voxel_size = 1.f;
    input.children = {child_mesh.render};

    TerrainMesher mesher;
    mesher.queue_node(7, 1, input, 0.f);
    mesher.wait_idle();
    std::vector<MeshResult> chunks;
    std::vector<NodeResult> nodes;
    mesher.collect(chunks, nodes);
    REQUIRE(nodes.size() == 1u);
    REQUIRE(nodes[0].terrain == 7u);
    REQUIRE(nodes[0].revision == 1u);
    REQUIRE((nodes[0].key == input.key));
}

TEST_CASE("TM9 a newer node revision replaces a queued older one", "[terrain]") {
    LodInput input;
    input.key = NodeKey{2, 1, 0, 0};
    TerrainMesher mesher({}, 1);
    // Same shape as TM4: pause before either queue_node() lands, so both
    // reach the waiting set deterministically (the replace-in-place path)
    // rather than racing a worker that might already be running revision 1.
    mesher.pause_for_test(true);
    mesher.queue_node(9, 1, input, 0.f);
    mesher.queue_node(9, 2, input, 0.f);
    mesher.pause_for_test(false);
    mesher.wait_idle();
    std::vector<MeshResult> chunks;
    std::vector<NodeResult> nodes;
    mesher.collect(chunks, nodes);
    REQUIRE(nodes.size() == 1u);
    REQUIRE(nodes[0].terrain == 9u);
    REQUIRE(nodes[0].revision == 2u);
}

TEST_CASE("TM10 node and chunk jobs interleave by distance", "[terrain]") {
    VoxelVolume volume;   // left empty: chunk jobs just need valid MeshInput, not triangles
    std::mutex order_mu;
    std::vector<std::string> order;
    TerrainMesher mesher(
        [&](const ChunkMesh&) -> std::shared_ptr<void> {
            std::lock_guard<std::mutex> lock(order_mu);
            order.push_back("chunk");
            return nullptr;
        },
        1,
        [&](const LodInput& in) -> LodResult {
            std::lock_guard<std::mutex> lock(order_mu);
            order.push_back("node");
            LodResult r;
            r.key = in.key;
            return r;
        });
    mesher.pause_for_test(true);
    LodInput node_a;
    node_a.key = NodeKey{1, 0, 0, 0};
    LodInput node_b;
    node_b.key = NodeKey{1, 1, 0, 0};
    mesher.queue_node(1, 1, node_a, 1.f);                                  // nearest
    mesher.queue_node(1, 1, node_b, 5.f);                                  // 2nd
    mesher.queue(1, 1, mesh_input(volume, ChunkCoord{0, 0, 0}), 10.f);     // 3rd
    mesher.queue(1, 1, mesh_input(volume, ChunkCoord{1, 0, 0}), 20.f);     // farthest
    mesher.pause_for_test(false);
    mesher.wait_idle();
    REQUIRE(order == std::vector<std::string>{"node", "node", "chunk", "chunk"});
}

TEST_CASE("TM11 a node job that throws is dropped and counted", "[terrain]") {
    // LodBuilder's build_node is written to never throw -- it reports a null
    // mesh rather than raising on any input tried (empty children, zero
    // triangles, a fully-collapsed simplification). So, the same way TM6
    // injects a throw through TerrainMesher's existing BuildCollider hook
    // rather than adding a new one, this reuses the symmetric BuildNode
    // constructor hook (added by this task for node jobs, mirroring
    // BuildCollider) instead of adding a test-only mechanism beyond it.
    TerrainMesher mesher({}, 0, [](const LodInput&) -> LodResult {
        throw std::runtime_error("TM11: a deliberately broken node build");
    });
    LodInput input;
    input.key = NodeKey{1, 0, 0, 0};
    mesher.queue_node(1, 1, input, 0.f);
    mesher.wait_idle();   // must return: the throw must not wedge the worker or deadlock running_
    std::vector<MeshResult> chunks;
    std::vector<NodeResult> nodes;
    mesher.collect(chunks, nodes);
    // The failure is reported through collect, so the caller stops waiting on it.
    REQUIRE(chunks.empty());
    REQUIRE(nodes.size() == 1u);
    REQUIRE(nodes[0].failed);
    REQUIRE(nodes[0].terrain == 1u);
    REQUIRE(nodes[0].revision == 1u);
    REQUIRE((nodes[0].key == input.key));
    REQUIRE(nodes[0].result.mesh == nullptr);
    REQUIRE(mesher.failure_count() == 1u);
}

TEST_CASE("TM12 a chunk job that throws comes back through collect(chunks, nodes) flagged failed", "[terrain]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), 2));
    TerrainMesher mesher([](const ChunkMesh& mesh) -> std::shared_ptr<void> {
        if (mesh.triangles.empty()) {
            return nullptr;
        }
        throw std::runtime_error("TM12: a deliberately broken collider build");
    });
    mesher.queue(3, 4, mesh_input(volume, ChunkCoord{0, 0, 0}), 0.f);
    mesher.queue(3, 5, mesh_input(volume, ChunkCoord{5, 5, 5}), 0.f);   // empty: succeeds
    mesher.wait_idle();
    std::vector<MeshResult> chunks;
    std::vector<NodeResult> nodes;
    mesher.collect(chunks, nodes);
    REQUIRE(nodes.empty());
    REQUIRE(chunks.size() == 2u);
    for (const MeshResult& result : chunks) {
        REQUIRE(result.terrain == 3u);
        const bool broken = result.coord == ChunkCoord{0, 0, 0};
        REQUIRE(result.failed == broken);
        REQUIRE(result.revision == (broken ? 4u : 5u));
        if (broken) {
            REQUIRE(result.mesh.render == nullptr);
            REQUIRE(result.collider == nullptr);
        }
    }
    REQUIRE(mesher.failure_count() == 1u);
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

TEST_CASE("SN8 meshing one dense chunk plus its collider is fast", "[.][terrain-bench]") {
    // The spec's worker budget: Surface Nets and the Box3D mesh for one dense
    // surface chunk together under 2 ms, on one mesher worker thread.
    VoxelVolume volume;
    for (int i = 0; i < 6; ++i) {
        REQUIRE_FALSE(volume.fill(ball_at(5.f + i * 4.f, 10.f + (i % 3) * 5.f, 16.f, 6.f), 1));
    }
    const MeshInput input = mesh_input(volume, ChunkCoord{0, 0, 0});
    REQUIRE(PhysicsWorld::build_terrain_collider(surface_nets(input)) != nullptr);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        (void)PhysicsWorld::build_terrain_collider(surface_nets(input));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 50.0;
    INFO(ms);
    // The spec's 2 ms budget awaits a compiler upgrade (MSVC 19.28+, which
    // drops the mesh.c /d2SSAOptimizer- workaround in cmake/box3d) or a
    // custom mesh builder. The user accepted about 4.5 to 5 ms on 2026-10-07.
    REQUIRE(ms < 6.0);
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
    // Edits go through Terrain::edit_volume, as a user's do.
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0); }));
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
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0); }));
    TerrainWorld world;
    settle(world, game);
    REQUIRE(world.views().empty());
}

TEST_CASE("TW3 an edit re-meshes only the chunks it touched and their neighbors", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0); }));
    // (208, 16, 16) sits in the middle of chunk {6, 0, 0} (16 studs from every
    // face), so the grown ball's band (radius 6 + the 4-cell band = 10 studs)
    // never reaches a second chunk on any axis -- unlike a center near 0,
    // where the band alone can cross the origin's chunk seam on two axes at
    // once and legitimately dirty more than one chunk's neighborhood.
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(208.f, 16.f, 16.f, 4.f), 0); }));
    TerrainWorld world;
    settle(world, game);
    const std::uint64_t before = world.meshed_count();
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(208.f, 16.f, 16.f, 6.f), 0); }));
    settle(world, game);
    REQUIRE(world.meshed_count() - before <= 27u);
}

TEST_CASE("TW4 a TerrainMaterial's Material changes the look, not the meshes", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(t.add_material(0, entry));
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 1); }));
    TerrainWorld world;
    settle(world, game);
    const auto chunks = world.views()[0].chunks;
    const std::uint64_t look = world.views()[0].look->revision;
    // A red Material, under the Materials service, as plan 1a's tests do.
    Material& red = add_material_asset(game, "Red");
    REQUIRE_FALSE(red.set_color(rgb(1.f, 0.f, 0.f)));
    LuaSlot material_slot;
    material_slot.kind = LuaSlot::Kind::Instance;
    material_slot.id = red.id();
    REQUIRE_FALSE(entry->set_material(material_slot));
    settle(world, game);
    REQUIRE(world.views()[0].chunks == chunks);           // same vector: nothing re-meshed
    REQUIRE(world.views()[0].look->revision != look);
    REQUIRE(world.views()[0].look->texels[1 * 4 + 0] == 1.f);   // Id 1, red
}

TEST_CASE("TW5 Stop re-meshes only what play changed", "[terrain]") {
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0); }));
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(300.f, 5.f, 5.f, 4.f), 0); }));
    TerrainWorld world;
    settle(world, game);
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.subtract(ball_at(5.f, 5.f, 5.f, 6.f)); }));
    settle(world, game);
    const std::uint64_t before = world.meshed_count();
    // Stop restores the voxels Play captured (Terrain::read_place), marking
    // dirty only the chunks whose pointers play replaced.
    game.stop_simulation();
    settle(world, game);
    REQUIRE(world.meshed_count() - before <= 27u);
    REQUIRE(world.views()[0].chunks->size() >= 2u);
    // The near ball is back: its chunk is meshed again.
    const auto& shown = *world.views()[0].chunks;
    REQUIRE(std::any_of(shown.begin(), shown.end(),
                        [](const TerrainChunkView& chunk) { return chunk.coord == ChunkCoord{0, 0, 0}; }));
}

TEST_CASE("TW6 a Terrain destroyed during play keeps its TerrainTag when Stop restores it", "[terrain]") {
    // DataModel::adopt_slot (DataModelPlace.cpp) rebuilds a captured
    // instance's slot from scratch on Stop. It must add TerrainTag beside
    // physics_body's tag -- exactly as issue_entity does for a brand-new
    // instance -- or terrains() never finds the restored Terrain again and
    // it silently drops out of TerrainWorld (and so out of rendering and
    // collision) even though DataModel still considers it alive.
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 0); }));
    const InstanceId id = t.id();
    TerrainWorld world;
    settle(world, game);
    REQUIRE(world.views().size() == 1u);

    game.capture_place();
    game.start_simulation();
    game.destroy(id);
    settle(world, game);
    REQUIRE(world.views().empty());   // gone while destroyed in play

    game.stop_simulation();   // restores the captured Terrain through adopt_slot
    REQUIRE(game.alive(id));
    settle(world, game);
    REQUIRE(world.views().size() == 1u);   // found again: TerrainTag survived the rebuild
}

TEST_CASE("TW7 a mesh from a Terrain's previous stay in Workspace is dropped, not shown, on its return",
          "[terrain]") {
    // Job revisions and chunks_revision must come from counters that live on
    // TerrainWorld, not on the per-Terrain record: a record is dropped and
    // recreated (fresh, restarting any local counter at 1) every time a
    // Terrain leaves and returns to Workspace, so a value drawn from a local
    // counter can collide with one a previous record already handed out for
    // the very same chunk.
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);

    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool blocked = false;   // the chunk with real geometry is now stuck mid-build
    bool release = false;   // the test says go
    TerrainWorld world(
        [&](const ChunkMesh& mesh) -> std::shared_ptr<void> {
            if (mesh.triangles.empty()) {
                return nullptr;   // one of the 26 air neighbors: never blocks
            }
            std::unique_lock<std::mutex> lock(gate_mu);
            blocked = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release; });
            return nullptr;
        },
        1);   // one worker: exactly one job can ever be "running" at a time

    // First sight while the volume is still empty queues nothing, so the
    // record exists with nothing in flight -- the only window in which
    // set_collider_interest can name a coord before it is ever queued
    // (Task 8: the mesher's collider builder, and so this blocking hook,
    // runs only for a job in collider interest).
    world.update(game);
    world.set_collider_interest(t.id(), {ChunkCoord{0, 0, 0}});
    // Dead center of chunk {0,0,0} (32 studs on a side): radius 4 plus the
    // 4-cell band stays 8 studs clear of every face, so this edit dirties
    // only that one chunk.
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(16.f, 16.f, 16.f, 4.f), 0); }));
    world.update(game);   // the edit's job, in collider interest: blocks
    {
        std::unique_lock<std::mutex> lock(gate_mu);
        gate_cv.wait(lock, [&] { return blocked; });
    }
    // The real chunk's job is now stuck holding its pre-edit mesh and the
    // revision this, its first, stay in Workspace assigned it.
    game.set_parent(t.id(), game.scene_service("Storage"));
    world.update(game);   // leaves Workspace: TerrainWorld drops its record
    // While away, repaint the same solid region -- a visible edit the stuck
    // job's mesh will not reflect once it finally lands.
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.paint(ball_at(16.f, 16.f, 16.f, 4.f), 7); }));
    game.set_parent(t.id(), workspace_of(game));
    world.update(game);   // returns: first sight again, re-queues all 27 coords
    const std::uint64_t before = world.meshed_count();

    {
        std::lock_guard<std::mutex> lock(gate_mu);
        release = true;
    }
    gate_cv.notify_all();
    world.wait_idle();
    world.update(game);
    // This stay's 27 fresh jobs (26 air plus the one real chunk, repainted)
    // are accepted; the one job left over from the previous stay must be
    // dropped as stale, not counted a 28th time.
    REQUIRE(world.meshed_count() - before == 27u);
}

TEST_CASE("TW8 first sight meshes a stored chunk's footprint including an unstored chunk that owns a quad",
          "[terrain]") {
    // queue_dirty's first_seen branch must queue every stored chunk AND its
    // 26 neighbors (deduplicated) -- the same footprint take_dirty gives
    // after an edit -- not just volume.chunks()'s own keys.
    //
    // A chunk's own cell 31 reaches one sample past its own top face, into
    // whichever chunk sits above it on that axis (SurfaceNets.cpp's -1..31
    // cell range): so a quad at a seam is owned by the LOWER chunk's cell 31,
    // not the chunk above, even when the solid material behind it lives
    // entirely in the chunk above. Here a one-cell-thick solid slab is
    // written directly into chunk {0,1,0}'s own first row (world y=32), kept
    // well inside that chunk's x/z extent (cells 4..27, not 0..31) so its
    // four side walls stay owned by {0,1,0} itself rather than spilling into
    // the x/z-neighbor chunks whose own cell 31 would otherwise own the
    // low-side walls; chunk {0,0,0} below it is never written at all, so it
    // stays out of volume.chunks() -- yet it owns the slab's underside.
    // Meshing only volume.chunks()'s own key ({0,1,0}) finds the slab's top
    // face (and its walls) but leaves its underside out: a real, visible gap.
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    const std::size_t side = 24;   // cells 4..27 inclusive
    const std::size_t plane = side * side;
    const std::vector<float> distances(plane, -4.f);   // deep solid
    const std::vector<std::uint8_t> materials(plane, 1);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.write(CellCoord{4, 32, 4}, CellCoord{27, 32, 27}, distances, materials); }));
    REQUIRE(t.volume().chunks().find(ChunkCoord{0, 1, 0}) != t.volume().chunks().end());
    REQUIRE(t.volume().chunks().find(ChunkCoord{0, 0, 0}) == t.volume().chunks().end());   // never touched

    std::size_t reference_count = 0;
    for (const ChunkMesh& mesh : mesh_all(t.volume())) {
        if (!mesh.triangles.empty()) {
            ++reference_count;
        }
    }
    REQUIRE(reference_count == 2u);   // the slab's top face (stored) and its underside (unstored)

    TerrainWorld world;
    settle(world, game);
    REQUIRE(world.views().size() == 1u);
    // TerrainWorld publishes a TerrainChunkView only for a chunk whose last
    // result had a non-null mesh (accept_result erases the rest), so this
    // count is directly comparable to reference_count above.
    REQUIRE(world.views()[0].chunks->size() == reference_count);
}

TEST_CASE("TS1 the snapshot carries each Terrain's chunks, transform, and look", "[terrain][render]") {
    SimRole role;
    Game game;
    SnapshotPump pump;
    pump.reserve(DataModel::kMaxInstances);
    Terrain& t = terrain_in_workspace(game);
    REQUIRE_FALSE(t.set_transform(matrix4_translation(10.f, 0.f, 0.f)));
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 1); }));
    TerrainWorld world;
    settle(world, game);
    REQUIRE(world.views().size() == 1u);

    // set_terrain_world happens once, the way Engine's constructor wires
    // pump_.set_terrain_world(&terrain_). frame() below is prepare_copy + publish,
    // as prefab_render_tests.cpp's Scene::frame() runs it.
    pump.set_terrain_world(&world);
    pump.prepare_copy(game);
    pump.publish();

    const VisualSnapshot& front = pump.front();
    REQUIRE(front.terrains.size() == 1u);
    const TerrainView& view = front.terrains[0];
    // Same shared_ptr as TerrainWorld's own view: a pointer copy, not a chunk copy.
    REQUIRE(view.chunks == world.views()[0].chunks);
    REQUIRE(view.transform.m[12] == 10.f);
    REQUIRE(view.look != nullptr);
}

namespace {

using physics_rig::PhysicsRig;

// A block of solid whose top is at y = 0: size studs across, depth deep.
Shape slab(float size, float depth) {
    Shape s;
    s.kind = Shape::Kind::Block;
    s.frame = matrix4_translation(0.f, -depth * 0.5f, 0.f);
    s.size = Vec3{size, depth, size};
    return s;
}

// How many of the one Terrain's published chunks have triangles: each is
// drawn, so each should collide too.
std::size_t chunks_with_triangles(const TerrainWorld& world) {
    REQUIRE(world.views().size() == 1u);
    return world.views()[0].chunks->size();
}

// Every coord the one Terrain currently shows a mesh for: Task 8's interest
// API, called directly as PhysicsWorld would for a body near all of them.
std::vector<ChunkCoord> meshed_chunk_coords(const TerrainWorld& world) {
    REQUIRE(world.views().size() == 1u);
    std::vector<ChunkCoord> coords;
    for (const TerrainChunkView& chunk : *world.views()[0].chunks) {
        coords.push_back(chunk.coord);
    }
    return coords;
}

// The revision of the collider TerrainWorld holds for coord, or 0.
std::uint64_t collider_revision(const TerrainWorld& world, InstanceId terrain, ChunkCoord coord) {
    const auto* colliders = world.colliders(terrain);
    if (colliders == nullptr) {
        return 0;
    }
    for (const auto& collider : *colliders) {
        if (collider.coord == coord) {
            return collider.revision;
        }
    }
    return 0;
}

// A ScriptRig whose Game has a physics world wired to a TerrainWorld, as an
// Engine's does.
struct TerrainRaycastRig : ScriptRig {
    PhysicsWorld physics;
    TerrainWorld world{PhysicsWorld::build_terrain_collider};
    TerrainRaycastRig() {
        physics.set_terrain_world(&world);
        game.set_physics(&physics);
    }
    ~TerrainRaycastRig() { game.set_physics(nullptr); }

    // Runs source on the command line and returns what it printed.
    std::string run(const char* source) {
        runtime.drain_output();
        runtime.run_chunk(source);
        frames(1);
        std::string out;
        for (const auto& line : runtime.drain_output().lines) {
            out += line.text;
        }
        return out;
    }
};

}  // namespace

TEST_CASE("TW9 an edit to a chunk already in collider interest always refreshes its collider",
          "[terrain]") {
    // Must also fix (Task 7 review): the mesher's per-job collider flag must
    // follow collider_interest for every kind of job, edits included -- an
    // edit must not be treated as if it were out of interest (or as if
    // residency's "not an edit" bit also meant "skip the collider"), and
    // apply_result must decide the collider write from want_collider, not
    // from whether the job also marks LOD ancestors stale.
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    // A wide-enough slab that a level-1 ancestor exists above chunk
    // {0, -1, 0} (a single isolated chunk, as TW3 edits, is its own root:
    // top_level 0, no ancestor to check staleness on).
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(96.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    settle(world, game);
    const ChunkCoord coord{0, -1, 0};
    REQUIRE(world.colliders(t.id()) != nullptr);
    REQUIRE(world.colliders(t.id())->empty());   // nothing has asked for it yet

    world.set_collider_interest(t.id(), {coord});
    settle(world, game);
    const std::uint64_t collider_before = collider_revision(world, t.id(), coord);
    REQUIRE(collider_before != 0u);

    const terrain::LodTree* tree = world.lod_tree(t.id());
    REQUIRE(tree != nullptr);
    const terrain::NodeKey parent = terrain::parent_of(terrain::node_of(coord, 0));
    const auto* parent_before = tree->find(parent);
    REQUIRE(parent_before != nullptr);
    const std::uint64_t ancestor_revision_before = parent_before->revision;

    // An edit well inside the same chunk (8 units clear of its 32-unit
    // faces), still in collider interest.
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.paint(ball_at(8.f, -4.f, 8.f, 3.f), 2); }));
    settle(world, game);

    REQUIRE(collider_revision(world, t.id(), coord) != 0u);
    REQUIRE(collider_revision(world, t.id(), coord) != collider_before);   // refreshed, never skipped
    const auto* parent_after = tree->find(parent);
    REQUIRE(parent_after != nullptr);
    REQUIRE(parent_after->revision != ancestor_revision_before);   // the edit still marked it stale
}

TEST_CASE("TP1 a Terrain has a static body with a shape per chunk asked for, stopped", "[terrain][physics]") {
    // Task 8: colliders exist only within collider interest, so with no
    // dynamic body or PlayerController a meshed Terrain has none at all; the
    // interest API (set_collider_interest), called directly here the way
    // PhysicsWorld would for a body near every chunk, is what CS1 and TP2/
    // TP3/TP5 below also use in place of one.
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(ball_at(5.f, 5.f, 5.f, 4.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    const std::size_t meshed = chunks_with_triangles(world);
    REQUIRE(meshed >= 1u);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.has_body(t.id()));
    REQUIRE(world.colliders(t.id())->empty());
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);

    world.set_collider_interest(t.id(), meshed_chunk_coords(world));
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(world.colliders(t.id())->size() == meshed);
    REQUIRE(rig.physics.shape_count(t.id()) == meshed);

    // Out of Workspace, the body goes.
    rig.game.set_parent(t.id(), rig.game.scene_service("Storage"));
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE_FALSE(rig.physics.has_body(t.id()));
}

TEST_CASE("TP2 CanCollide false removes every shape; true brings them back", "[terrain][physics]") {
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(16.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    const std::size_t meshed = chunks_with_triangles(world);
    // Task 8's interest API, as PhysicsWorld would call it for a body near
    // every meshed chunk.
    world.set_collider_interest(t.id(), meshed_chunk_coords(world));
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == meshed);
    const Vec3 origin{3.f, 10.f, 3.f};
    const Vec3 down{0.f, -20.f, 0.f};
    REQUIRE(rig.physics.raycast(rig.game, origin, down, {}).has_value());

    REQUIRE_FALSE(t.set_can_collide(false));
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.has_body(t.id()));
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);
    REQUIRE_FALSE(rig.physics.raycast(rig.game, origin, down, {}).has_value());

    REQUIRE_FALSE(t.set_can_collide(true));
    // CanCollide false let collider interest lapse (update_collider_interest):
    // ask again, as a body that stayed near it the whole time would have.
    world.set_collider_interest(t.id(), meshed_chunk_coords(world));
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == meshed);
    REQUIRE(rig.physics.raycast(rig.game, origin, down, {}).has_value());
}

TEST_CASE("TP3 a ray hits the terrain and reports its TerrainMaterial's Material", "[terrain][physics]") {
    SECTION("from C++") {
        PhysicsRig rig;
        Terrain& t = terrain_in_workspace(rig.game);
        Material& rock = add_material_asset(rig.game, "Rock");
        TerrainMaterial* entry = nullptr;
        REQUIRE_FALSE(t.add_material(rock.id(), entry));
        REQUIRE(entry->material_id() == 1);
        REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(16.f, 8.f), 1); }));
        TerrainWorld world(PhysicsWorld::build_terrain_collider);
        rig.physics.set_terrain_world(&world);
        settle(world, rig.game);
        world.set_collider_interest(t.id(), meshed_chunk_coords(world));
        settle(world, rig.game);
        const auto hit = rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, {});
        REQUIRE(hit.has_value());
        REQUIRE(hit->instance == t.id());
        REQUIRE(hit->has_material);
        REQUIRE(hit->material == 1);
        REQUIRE(std::fabs(hit->position.y) <= 0.1f);
    }
    SECTION("from Lua") {
        TerrainRaycastRig rig;
        Terrain& t = terrain_in_workspace(rig.game);
        Material& rock = add_material_asset(rig.game, "Rock");
        TerrainMaterial* entry = nullptr;
        REQUIRE_FALSE(t.add_material(rock.id(), entry));
        REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(16.f, 8.f), 1); }));
        settle(rig.world, rig.game);
        rig.world.set_collider_interest(t.id(), meshed_chunk_coords(rig.world));
        settle(rig.world, rig.game);
        const std::string out = rig.run(R"(
            local r = workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0))
            print(r.Instance.ClassName, r.Material.Name)
        )");
        INFO(out);
        REQUIRE(out.find("Terrain\tRock\n") != std::string::npos);
    }
}

TEST_CASE("TP4 a box resting on terrain stays up while its chunk is re-meshed", "[terrain][physics]") {
    using physics_rig::at;
    using physics_rig::kStep;
    using physics_rig::near;
    using physics_rig::y_of;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(48.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    rig.play();
    PhysicsObject& box = rig.body(at(8.f, 3.f, 8.f), Vec3{1.f, 1.f, 1.f}, false);
    // One TerrainWorld update a frame and four physics steps, as the Engine runs them.
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame) {
            world.update(rig.game);
            for (int step = 0; step < 4; ++step) {
                rig.physics.step(rig.game, kStep);
            }
        }
    };
    frames(120);
    {
        INFO(y_of(box.transform()));
        REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));
    }

    // The slab's top face belongs to the chunk below y = 0, which the box
    // sits over. Painting another Id into that chunk, away from the box,
    // meshes it again, and its shape is made again under the box.
    const ChunkCoord under{0, -1, 0};
    const std::uint64_t before = collider_revision(world, t.id(), under);
    REQUIRE(before != 0u);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.paint(ball_at(24.f, -1.f, 24.f, 3.f), 2); }));
    world.update(rig.game);
    world.wait_idle();
    frames(60);
    REQUIRE(collider_revision(world, t.id(), under) != before);
    INFO(y_of(box.transform()));
    REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));
}

TEST_CASE("TP5 a Terrain's body survives a play then Stop round trip", "[terrain][physics]") {
    using physics_rig::kStep;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(16.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    const std::size_t meshed = chunks_with_triangles(world);
    REQUIRE(meshed >= 1u);
    // Task 8's interest API, as PhysicsWorld would call it for a body near
    // every meshed chunk.
    world.set_collider_interest(t.id(), meshed_chunk_coords(world));
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.has_body(t.id()));
    REQUIRE(rig.physics.shape_count(t.id()) == meshed);

    rig.play();
    for (int frame = 0; frame < 8; ++frame) {
        world.update(rig.game);
        for (int step = 0; step < 4; ++step) {
            rig.physics.step(rig.game, kStep);
        }
    }

    rig.game.stop_simulation();
    // Mirrors the Engine's stopped tick (Engine.cpp's simulation_loop): terrain
    // meshes before physics syncs, so a Terrain's shape and collider exist
    // before physics_.sync looks for them, under the same DataModel write lock.
    world.update(rig.game);
    rig.physics.sync(rig.game);

    REQUIRE(rig.physics.has_body(t.id()));
    const std::size_t colliders = world.colliders(t.id())->size();
    REQUIRE(rig.physics.shape_count(t.id()) == colliders);
    const Vec3 origin{3.f, 10.f, 3.f};
    const Vec3 down{0.f, -20.f, 0.f};
    const auto hit = rig.physics.raycast(rig.game, origin, down, {});
    REQUIRE(hit.has_value());
    REQUIRE(hit->instance == t.id());
}

TEST_CASE("CS1 with no dynamic bodies, a settled island has zero chunk shapes", "[terrain][physics]") {
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(48.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    REQUIRE(chunks_with_triangles(world) >= 1u);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.has_body(t.id()));
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);
    REQUIRE(world.colliders(t.id())->empty());

    // Playing changes nothing: nothing ever asks for a collider.
    rig.play();
    for (int i = 0; i < 8; ++i) {
        world.update(rig.game);
        rig.physics.step(rig.game, physics_rig::kStep);
    }
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);
}

TEST_CASE("CS2 a box dropped on bare terrain no camera or edit has ever touched gets colliders "
          "under it in the same sync and lands",
          "[terrain][physics]") {
    using physics_rig::at;
    using physics_rig::kStep;
    using physics_rig::near;
    using physics_rig::y_of;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    // Far from the origin: nothing (no camera, no prior edit) has ever asked
    // TerrainWorld to mesh this area before the box arrives.
    const float cx = 2048.f, cz = 2048.f;
    Shape far_slab;
    far_slab.kind = Shape::Kind::Block;
    far_slab.frame = matrix4_translation(cx, -4.f, cz);
    far_slab.size = Vec3{48.f, 8.f, 48.f};
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(far_slab, 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);   // no body yet: nothing asked for one

    rig.play();
    PhysicsObject& box = rig.body(at(cx, 3.f, cz), Vec3{1.f, 1.f, 1.f}, false);
    // The no-fall-through rule: built right here, before Box3D steps, in
    // this very sync -- not a frame later through the job queue.
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) >= 1u);

    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame) {
            world.update(rig.game);
            for (int step = 0; step < 4; ++step) {
                rig.physics.step(rig.game, kStep);
            }
        }
    };
    frames(120);   // 2 s
    INFO(y_of(box.transform()));
    REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));   // at rest on the slab, not through it
}

TEST_CASE("CS3 colliders follow a moving body, stay bounded, and release 5 s after it leaves",
          "[terrain][physics]") {
    using physics_rig::at;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    // A long, 1-chunk-wide strip (32 chunks along x): far more total chunks
    // than fit in one body's kColliderChunks box, so "bounded" is a real test.
    Shape strip;
    strip.kind = Shape::Kind::Block;
    strip.frame = matrix4_translation(512.f, -4.f, 16.f);
    strip.size = Vec3{1024.f, 8.f, 32.f};
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(strip, 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    const std::size_t total_meshed = chunks_with_triangles(world);
    REQUIRE(total_meshed >= 30u);

    double now_ms = 0.0;
    rig.play();
    PhysicsObject& box = rig.body(at(16.f, 3.f, 16.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.physics.sync(rig.game);         // asks for colliders around x = 16
    world.update(rig.game, now_ms);     // builds the ones the job queue owes
    world.wait_idle();
    world.update(rig.game, now_ms);
    rig.physics.sync(rig.game);

    // The slab's top face belongs to the chunk below y = 0 (as TP4 notes).
    const ChunkCoord near_start{0, -1, 0};
    REQUIRE(collider_revision(world, t.id(), near_start) != 0u);
    const std::size_t near_shapes = rig.physics.shape_count(t.id());
    REQUIRE(near_shapes > 0u);
    REQUIRE(near_shapes < total_meshed / 2);   // bounded: nowhere near every chunk

    // Move far down the strip.
    REQUIRE_FALSE(box.set_transform(at(624.f, 3.f, 16.f)));
    now_ms += 100.0;
    rig.physics.sync(rig.game);         // asks for colliders around x = 624 now
    world.update(rig.game, now_ms);
    world.wait_idle();
    world.update(rig.game, now_ms);
    rig.physics.sync(rig.game);

    const ChunkCoord near_end{19, -1, 0};
    REQUIRE(collider_revision(world, t.id(), near_end) != 0u);
    REQUIRE(rig.physics.shape_count(t.id()) < total_meshed / 2);   // still bounded
    // The old spot's collider stays: the body left less than 5 s ago.
    REQUIRE(collider_revision(world, t.id(), near_start) != 0u);

    // 5 s after the body left it, it is gone.
    now_ms += 5001.0;
    rig.physics.sync(rig.game);         // still only asks around x = 624
    world.update(rig.game, now_ms);     // releases what has not been asked for since
    rig.physics.sync(rig.game);
    REQUIRE(collider_revision(world, t.id(), near_start) == 0u);
    REQUIRE(collider_revision(world, t.id(), near_end) != 0u);
}

TEST_CASE("CS4 a PlayerController walking across chunk boundaries keeps ground under it, no fall-through over 30 s",
          "[terrain][physics]") {
    using physics_rig::at;
    using physics_rig::kStep;
    using physics_rig::x_of;
    using physics_rig::y_of;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    Shape strip;
    strip.kind = Shape::Kind::Block;
    strip.frame = matrix4_translation(160.f, -4.f, 4.f);
    strip.size = Vec3{320.f, 8.f, 8.f};
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(strip, 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);

    rig.play();
    PlayerController& c = rig.controller(at(4.f, 0.5f, 4.f));
    const auto frame = [&] {
        world.update(rig.game);
        for (int step = 0; step < 4; ++step) {
            rig.physics.step(rig.game, kStep);
        }
    };
    for (int i = 0; i < 60; ++i) frame();   // 1 s to settle before walking
    REQUIRE(c.on_ground());

    // Tracks the deepest it ever gets, rather than failing on the first
    // step past some fixed line: a hover's own small settle each time a
    // chunk's shape is remade is not the fall-through this guards against,
    // only a sustained drop (never recovering) is.
    const int walk_frames = static_cast<int>(30.0 / (4.0 * kStep) + 0.5);
    float min_y = 0.f;
    for (int i = 0; i < walk_frames; ++i) {
        world.update(rig.game);
        for (int step = 0; step < 4; ++step) {
            REQUIRE_FALSE(c.set_velocity(Vec3{10.f, c.velocity().y, 0.f}));
            rig.physics.step(rig.game, kStep);
            min_y = std::min(min_y, y_of(c.transform()));
        }
    }
    INFO(x_of(c.transform()) << " " << y_of(c.transform()) << " min_y=" << min_y);
    REQUIRE(x_of(c.transform()) > 100.f);   // it actually crossed several chunk boundaries
    REQUIRE(c.on_ground());
    REQUIRE(min_y > -3.f);
}

namespace {

// A long strip 1 chunk wide (x 0..1024, z 0..32, top at y = 0), as CS3 uses.
void fill_collider_strip(Terrain& t) {
    Shape strip;
    strip.kind = Shape::Kind::Block;
    strip.frame = matrix4_translation(512.f, -4.f, 16.f);
    strip.size = Vec3{1024.f, 8.f, 32.f};
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(strip, 1); }));
}

}  // namespace

TEST_CASE("CS5 a resting body's collider interest settles: no chunk is meshed again once its collider is known",
          "[terrain][physics]") {
    using physics_rig::at;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    fill_collider_strip(t);
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);

    double now_ms = 0.0;
    rig.play();
    rig.body(at(16.f, 3.f, 16.f), Vec3{1.f, 1.f, 1.f}, false);
    const auto round = [&] {
        rig.physics.sync(rig.game);
        world.update(rig.game, now_ms);
        world.wait_idle();
        world.update(rig.game, now_ms);
    };
    for (int i = 0; i < 4; ++i) round();   // every chunk around it known by now
    REQUIRE(collider_revision(world, t.id(), ChunkCoord{0, -1, 0}) != 0u);

    // Most of the kColliderChunks box around the body is empty (air, or
    // solid below the strip): such a chunk has no collider to keep, but once
    // meshed for interest it is known, not meshed again every update.
    const std::uint64_t meshed = world.meshed_count();
    for (int i = 0; i < 10; ++i) round();
    REQUIRE(world.meshed_count() == meshed);
}

TEST_CASE("CS6 a body moved next to a chunk with a collider still gets its own chunk's collider in the same sync",
          "[terrain][physics]") {
    using physics_rig::at;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    fill_collider_strip(t);
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);

    rig.play();
    PhysicsObject& box = rig.body(at(16.f, 3.f, 16.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.physics.sync(rig.game);   // builds the chunks around x = 16 right here
    REQUIRE(collider_revision(world, t.id(), ChunkCoord{0, -1, 0}) != 0u);

    // Into chunk 4 (x 128..160), with no update() in between, so nothing
    // queued for it could have landed: whatever the neighbors hold, the
    // chunk under the body must be built in this sync, before Box3D steps.
    REQUIRE_FALSE(box.set_transform(at(144.f, 3.f, 16.f)));
    rig.physics.sync(rig.game);
    REQUIRE(collider_revision(world, t.id(), ChunkCoord{4, -1, 0}) != 0u);
}

namespace {

// A rolling slab over 16 x 16 chunks (512 x 512 studs), built as a sum of
// balls whose centers rise and fall: its top wanders across y = 32 and its
// bottom across y = 0, so most columns have surface in two or more chunks.
void fill_rolling_slab(VoxelVolume& volume) {
    for (int z = 0; z <= 512; z += 16) {
        for (int x = 0; x <= 512; x += 16) {
            const float fx = static_cast<float>(x), fz = static_cast<float>(z);
            const float y = 20.f + 12.f * std::sin(fx / 40.f) * std::cos(fz / 50.f);
            REQUIRE_FALSE(volume.fill(ball_at(fx, y, fz, 14.f), 1));
        }
    }
}

// How many chunks of volume have triangles, meshed serially here: what the
// TerrainWorld should end up showing.
std::size_t surface_chunk_count(const VoxelVolume& volume) {
    std::size_t count = 0;
    for (const ChunkMesh& mesh : mesh_all(volume)) {
        if (!mesh.triangles.empty()) ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("TL1 a 500-chunk island is fully meshed about a second after it appears", "[.][terrain-bench]") {
    SimRole role;
    Game game;
    Terrain& t = terrain_in_workspace(game);
    fill_rolling_slab(t.volume());
    const std::size_t expected = surface_chunk_count(t.volume());
    INFO("chunks with surface: " << expected);
    REQUIRE(expected >= 500u);

    // This thread stands in for SimulationThread (no other thread touches
    // game, so no DataModel lock) and keeps ticking: one update about every
    // millisecond until every chunk with a surface has a mesh (and a
    // collider, built in the same job on a mesher worker).
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    const auto start = std::chrono::steady_clock::now();
    std::size_t shown = 0;
    while (true) {
        world.update(game);
        shown = world.views().empty() ? 0 : world.views()[0].chunks->size();
        if (shown >= expected) break;
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO("seconds: " << seconds);
    REQUIRE(shown == expected);
    REQUIRE(seconds < 1.5);
}

TEST_CASE("TL2 during play, digging under a resting box drops it", "[terrain][physics]") {
    using physics_rig::at;
    using physics_rig::kStep;
    using physics_rig::near;
    using physics_rig::y_of;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(48.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    rig.play();
    PhysicsObject& box = rig.body(at(8.f, 3.f, 8.f), Vec3{1.f, 1.f, 1.f}, false);
    // One TerrainWorld update a frame and four physics steps, as the Engine
    // runs them during play.
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame) {
            world.update(rig.game);
            for (int step = 0; step < 4; ++step) {
                rig.physics.step(rig.game, kStep);
            }
        }
    };
    frames(120);
    {
        INFO(y_of(box.transform()));
        REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));   // at rest on the slab
    }

    // Dig a hole right through the slab under the box, as a script's
    // Terrain:SubtractBlock does. The box may be asleep by now; the old chunk shapes going must wake it.
    Shape hole;
    hole.kind = Shape::Kind::Block;
    hole.frame = matrix4_translation(8.f, -4.f, 8.f);
    hole.size = Vec3{12.f, 20.f, 12.f};
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.subtract(hole); }));
    // These frames run much faster than real time, so give the mesher's
    // workers the real time a playing Engine would: queue the edit's chunks
    // from this thread (standing in for SimulationThread), wait for the
    // workers, and the next frame collects them -- an edit shows a frame or
    // two later.
    world.update(rig.game);
    world.wait_idle();
    frames(120);   // 2 s
    REQUIRE_FALSE(rig.physics.raycast(rig.game, Vec3{4.f, 10.f, 4.f}, Vec3{0.f, -20.f, 0.f}, {}).has_value());
    INFO(y_of(box.transform()));
    REQUIRE(y_of(box.transform()) < -10.f);
}

// Task 9: workspace:Raycast (PhysicsWorld::raycast) must ray-march a
// Terrain's voxel distance field over any chunk PhysicsWorld holds no
// collider for, so a Raycast hits terrain at any distance and gives the
// same answer whether or not colliders are loaded where it hits.

namespace {

// What RM1 and RM6 compare between two collider states.
struct RayResult {
    bool hit = false;
    InstanceId instance = 0;
    bool has_material = false;
    std::uint8_t material = 0;
    Vec3 position{};
    Vec3 normal{};
};

constexpr float kDegreesToRadians = 3.14159265f / 180.f;

std::vector<RayResult> cast_rays(PhysicsRig& rig, const std::vector<Vec3>& origins,
                                 const std::vector<Vec3>& directions) {
    std::vector<RayResult> results;
    results.reserve(origins.size());
    for (std::size_t i = 0; i < origins.size(); ++i) {
        RayResult r;
        if (const auto hit = rig.physics.raycast(rig.game, origins[i], directions[i], {})) {
            r.hit = true;
            r.instance = hit->instance;
            r.has_material = hit->has_material;
            r.material = hit->material;
            r.position = hit->position;
            r.normal = hit->normal;
        }
        results.push_back(r);
    }
    return results;
}

// The same hits: instance, material, position within 0.05 x VoxelSize (the
// spec's own bound), and normals within kNormalParity of each other. A
// collider's normal is its hit triangle's face normal; the march's is the
// normalized central-difference gradient of the trilinear field. On the
// planar surfaces RM1 and RM6 use, SurfaceNets puts every vertex on the
// plane and the gradient of a trilinear blend of a linear field is that
// plane's normal, so both are exact but for the stored distances' int8
// steps (4/127 of a cell, about 0.03): half a step moves a triangle's
// corners or the gradient's samples a degree or two over a one-cell edge or
// a two-cell difference. 0.98 (about 11 degrees) leaves room for that
// without letting a hit on the wrong side or the wrong surface through.
constexpr float kNormalParity = 0.98f;

// Under 10 degrees to the surface, a ray is grazing: the collider's
// triangles and the march's trilinear zero sit a few hundredths of a cell
// apart even on a plane (the int8 steps again), and a ray at angle a
// meets two surfaces that far apart 1 / sin(a) times further apart along
// itself -- about 0.15 units at 3 degrees for 0.007 units off the plane. So
// for a grazing ray the 0.05 x VoxelSize bound applies along the normal,
// and along the ray it is that bound over sin(a).
const float kGrazingSine = std::sin(10.f * kDegreesToRadians);

void require_same_hits(const std::vector<RayResult>& expected, const std::vector<RayResult>& actual,
                       const std::vector<Vec3>& origins, const std::vector<Vec3>& directions, float voxel_size) {
    REQUIRE(expected.size() == actual.size());
    REQUIRE(origins.size() == expected.size());
    REQUIRE(directions.size() == expected.size());
    const float tolerance = 0.05f * voxel_size;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const RayResult& a = expected[i];
        const RayResult& b = actual[i];
        INFO("from (" << origins[i].x << ", " << origins[i].y << ", " << origins[i].z << ") along (" << directions[i].x
                      << ", " << directions[i].y << ", " << directions[i].z << ")");
        INFO("ray " << i << ": (" << a.position.x << ", " << a.position.y << ", " << a.position.z << ") vs ("
                    << b.position.x << ", " << b.position.y << ", " << b.position.z << ")");
        INFO("normals (" << a.normal.x << ", " << a.normal.y << ", " << a.normal.z << ") vs (" << b.normal.x
                         << ", " << b.normal.y << ", " << b.normal.z << ")");
        REQUIRE(a.hit == b.hit);
        REQUIRE(a.instance == b.instance);
        REQUIRE(a.has_material == b.has_material);
        REQUIRE(a.material == b.material);
        const Vec3 d = directions[i];
        const float d_length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const float sine = std::fabs(d.x * a.normal.x + d.y * a.normal.y + d.z * a.normal.z) / d_length;
        if (sine >= kGrazingSine) {
            REQUIRE(std::fabs(a.position.x - b.position.x) <= tolerance);
            REQUIRE(std::fabs(a.position.y - b.position.y) <= tolerance);
            REQUIRE(std::fabs(a.position.z - b.position.z) <= tolerance);
        } else {
            // Grazing: the two surfaces within tolerance of each other
            // along the normal, and so the hits within tolerance / sine
            // along the ray.
            const Vec3 apart{b.position.x - a.position.x, b.position.y - a.position.y, b.position.z - a.position.z};
            const float off_surface = apart.x * a.normal.x + apart.y * a.normal.y + apart.z * a.normal.z;
            const float distance = std::sqrt(apart.x * apart.x + apart.y * apart.y + apart.z * apart.z);
            INFO("grazing, sine " << sine);
            REQUIRE(std::fabs(off_surface) <= tolerance);
            REQUIRE(distance <= tolerance / sine);
        }
        const float dot = a.normal.x * b.normal.x + a.normal.y * b.normal.y + a.normal.z * b.normal.z;
        REQUIRE(dot >= kNormalParity);
    }
}

// A ray from height h above a plane with unit normal up, heading along the
// plane at a random azimuth and angle below it, reaching 4 units past
// where it meets the plane.
Vec3 ray_into_plane(std::mt19937_64& random, Vec3 up, float h, float angle) {
    std::uniform_real_distribution<float> azimuth(0.f, 6.2831853f);
    const float phi = azimuth(random);
    Vec3 along{std::cos(phi), 0.f, std::sin(phi)};
    const float into = along.x * up.x + along.y * up.y + along.z * up.z;
    along = Vec3{along.x - up.x * into, along.y - up.y * into, along.z - up.z * into};
    const float along_length = std::sqrt(along.x * along.x + along.y * along.y + along.z * along.z);
    const float reach = h / std::sin(angle) + 4.f;
    const float c = std::cos(angle) * reach / along_length;
    const float s = std::sin(angle) * reach;
    return Vec3{along.x * c - up.x * s, along.y * c - up.y * s, along.z * c - up.z * s};
}

// A grazing ray: 2 to 5 degrees below the plane.
Vec3 grazing_ray(std::mt19937_64& random, Vec3 up, float h) {
    std::uniform_real_distribution<float> angle(2.f, 5.f);
    return ray_into_plane(random, up, h, angle(random) * kDegreesToRadians);
}

}  // namespace

TEST_CASE("RM1 a raycast at a settled island returns the same instance, material, position, and normal "
          "whether colliders are loaded everywhere or not loaded at all",
          "[terrain][physics]") {
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    Material& rock = add_material_asset(rig.game, "Rock");
    TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(t.add_material(rock.id(), entry));
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(96.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    // An explicit, controlled clock throughout (CS3's own pattern), not
    // settle()'s real one: the "no colliders" phase below fast-forwards
    // past the 5 s release grace, which a real clock would have to wait out.
    double now_ms = 0.0;
    const auto settle_now = [&] {
        for (int i = 0; i < 4; ++i) {
            world.update(rig.game, now_ms);
            world.wait_idle();
        }
        world.update(rig.game, now_ms);
    };
    settle_now();

    // 200 rays from above, mostly straight down but tilted enough to cross
    // several voxel cells at an angle, all landing well clear of the slab's
    // edges (it spans -48..48 on x and z).
    std::mt19937_64 random(7);
    std::uniform_real_distribution<float> plane(-40.f, 40.f);
    std::uniform_real_distribution<float> tilt(-6.f, 6.f);
    constexpr int kRays = 200;
    std::vector<Vec3> origins;
    std::vector<Vec3> directions;
    for (int i = 0; i < kRays; ++i) {
        origins.push_back(Vec3{plane(random), 10.f, plane(random)});
        directions.push_back(Vec3{tilt(random), -20.f, tilt(random)});
    }
    // And 20 grazing rays, 2 to 5 degrees below the slab's top, starting
    // half a unit above it.
    std::uniform_real_distribution<float> near_middle(-25.f, 25.f);
    for (int i = 0; i < 20; ++i) {
        const Vec3 origin{near_middle(random), 0.5f, near_middle(random)};
        origins.push_back(origin);
        directions.push_back(grazing_ray(random, Vec3{0.f, 1.f, 0.f}, 0.5f));
    }

    // Colliders loaded everywhere: force interest over the whole island.
    world.set_collider_interest(t.id(), meshed_chunk_coords(world));
    settle_now();
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == chunks_with_triangles(world));
    const std::vector<RayResult> with_colliders = cast_rays(rig, origins, directions);
    REQUIRE(std::all_of(with_colliders.begin(), with_colliders.end(), [](const RayResult& r) { return r.hit; }));

    // No colliders loaded anywhere: drop interest, and fast-forward the
    // clock past the 5 s release grace.
    world.set_collider_interest(t.id(), {});
    now_ms += 5001.0;
    settle_now();
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);
    const std::vector<RayResult> without_colliders = cast_rays(rig, origins, directions);

    require_same_hits(with_colliders, without_colliders, origins, directions, static_cast<float>(t.voxel_size()));
}

TEST_CASE("RM6 with colliders loaded around only some chunks, oblique and grazing rays across collided and "
          "uncollided chunks hit exactly where they would with colliders everywhere",
          "[terrain][physics]") {
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    Material& rock = add_material_asset(rig.game, "Rock");
    TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(t.add_material(rock.id(), entry));
    // A tilted slab, its top a plane through the origin, so the surface
    // crosses chunk borders on every axis at an angle and its normal is
    // no axis.
    const Matrix4 rotation = matrix4_axis_angle(Vec3{1.f, 0.f, 0.6f}, 12.f * kDegreesToRadians);
    const Vec3 up = matrix4_vector(rotation, Vec3{0.f, 1.f, 0.f});
    Shape tilted;
    tilted.kind = Shape::Kind::Block;
    tilted.frame = rotation;
    tilted.frame.m[12] = -4.f * up.x;
    tilted.frame.m[13] = -4.f * up.y;
    tilted.frame.m[14] = -4.f * up.z;
    tilted.size = Vec3{200.f, 8.f, 200.f};
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(tilted, 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    double now_ms = 0.0;
    const auto settle_now = [&] {
        for (int i = 0; i < 4; ++i) {
            world.update(rig.game, now_ms);
            world.wait_idle();
        }
        world.update(rig.game, now_ms);
    };
    settle_now();

    // Rays starting just above the plane over x, z in [0, 64), heading
    // outward at every azimuth and 2 to 60 degrees below the plane: they
    // cross out of the collided columns below through faces, edges and
    // corners into uncollided neighbours, and back. A quarter are grazing
    // (2 to 5 degrees).
    std::mt19937_64 random(11);
    std::uniform_real_distribution<float> over(0.f, 64.f);
    std::uniform_real_distribution<float> steep(5.f, 60.f);
    std::uniform_real_distribution<float> height(0.3f, 3.f);
    std::vector<Vec3> origins;
    std::vector<Vec3> directions;
    for (int i = 0; i < 400; ++i) {
        const float x = over(random), z = over(random);
        const float y = -(up.x * x + up.z * z) / up.y;   // on the plane
        const bool grazing = i % 4 == 0;
        const float h = grazing ? 0.3f + 0.7f * height(random) / 3.f : height(random);   // grazing: under 1 unit up
        origins.push_back(Vec3{x + up.x * h, y + up.y * h, z + up.z * h});
        directions.push_back(grazing ? grazing_ray(random, up, h)
                                     : ray_into_plane(random, up, h, steep(random) * kDegreesToRadians));
    }

    // Colliders everywhere: the reference answers.
    world.set_collider_interest(t.id(), meshed_chunk_coords(world));
    settle_now();
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == chunks_with_triangles(world));
    const std::vector<RayResult> everywhere = cast_rays(rig, origins, directions);
    REQUIRE(std::all_of(everywhere.begin(), everywhere.end(),
                        [&](const RayResult& r) { return r.hit && r.instance == t.id(); }));

    // Colliders only in the columns x, z = (0, 0) and (1, 1), diagonal
    // neighbours sharing one vertical edge; every other chunk has none
    // once the 5 s grace has passed.
    std::vector<ChunkCoord> some;
    for (const ChunkCoord& coord : meshed_chunk_coords(world)) {
        if ((coord.x == 0 && coord.z == 0) || (coord.x == 1 && coord.z == 1)) {
            some.push_back(coord);
        }
    }
    REQUIRE(!some.empty());
    world.set_collider_interest(t.id(), some);
    now_ms += 5001.0;
    settle_now();
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == some.size());
    const std::vector<RayResult> mixed = cast_rays(rig, origins, directions);

    require_same_hits(everywhere, mixed, origins, directions, static_cast<float>(t.voxel_size()));
}

TEST_CASE("RM2 a ray toward terrain hits a part in front of it, not the march behind the part",
          "[terrain][physics]") {
    using physics_rig::at;
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(48.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    // No collider interest anywhere: the terrain's own surface, at y = 0, is
    // reached only through the march.
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);

    PhysicsObject& part = rig.body(at(8.f, 4.f, 8.f), Vec3{2.f, 2.f, 2.f}, true);   // spans y in [3, 5]
    rig.physics.sync(rig.game);

    const auto hit = rig.physics.raycast(rig.game, Vec3{8.f, 10.f, 8.f}, Vec3{0.f, -20.f, 0.f}, {});
    REQUIRE(hit.has_value());
    REQUIRE(hit->instance == part.id());
    REQUIRE_FALSE(hit->has_material);
    REQUIRE(hit->position.y > 4.f);   // the part's top, well above the terrain's y = 0
}

TEST_CASE("RM3 a CanCollide false Terrain is never hit by the march", "[terrain][physics]") {
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(48.f, 8.f), 1); }));
    REQUIRE_FALSE(t.set_can_collide(false));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);

    REQUIRE_FALSE(rig.physics.raycast(rig.game, Vec3{8.f, 10.f, 8.f}, Vec3{0.f, -20.f, 0.f}, {}).has_value());
}

TEST_CASE("RM4 a 2 km ray across an island with no colliders marches fast", "[.][terrain-bench]") {
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(96.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);   // no colliders anywhere

    const Vec3 origin{-1000.f, 10.f, 0.f};
    const Vec3 direction{2000.f, -20.f, 0.f};   // 2 km across, crossing the island's surface at its middle
    REQUIRE(rig.physics.raycast(rig.game, origin, direction, {}).has_value());   // sanity: it does hit

    constexpr int kIterations = 50;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        (void)rig.physics.raycast(rig.game, origin, direction, {});
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kIterations;
    WARN("RM4: " << ms << " ms per ray");
    REQUIRE(ms < 0.2);
}

TEST_CASE("RM4b a 2 km ray across a 4,096-chunk island with no colliders marches fast", "[.][terrain-bench]") {
    // Final review: the march's bounds came from a scan of every stored
    // chunk on every raycast; on a large island that scan dominated.
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    // A 2,048 x 8 x 2,048 slab (64 x 64 chunks across, like LB8's island),
    // its top at y = 0, filled in strips so no edit passes kMaxCellsPerEdit.
    for (int strip = 0; strip < 8; ++strip) {
        Shape s;
        s.kind = Shape::Kind::Block;
        s.frame = matrix4_translation(0.f, -4.f, -1024.f + 128.f + 256.f * static_cast<float>(strip));
        s.size = Vec3{2048.f, 8.f, 256.f};
        REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(s, 1); }));
    }
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shape_count(t.id()) == 0u);   // no colliders anywhere
    REQUIRE(t.volume().chunks().size() >= 4096u);

    const Vec3 origin{-1000.f, 10.f, 0.f};
    const Vec3 direction{2000.f, -20.f, 0.f};   // 2 km, crossing the slab's top at its middle
    REQUIRE(rig.physics.raycast(rig.game, origin, direction, {}).has_value());   // sanity: it does hit

    constexpr int kIterations = 50;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        (void)rig.physics.raycast(rig.game, origin, direction, {});
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kIterations;
    WARN("RM4b: " << t.volume().chunks().size() << " stored chunks, " << ms << " ms per ray");
    REQUIRE(ms < 0.2);
}

TEST_CASE("RM5 an edit to a chunk outside collider interest drops its stale collider at once, so a raycast "
          "sees the dug hole right after the update",
          "[terrain][physics]") {
    // Carried from Task 8's review: apply_result must not leave a chunk's
    // pre-edit collider live for up to kColliderReleaseMs after an edit
    // outside collider_interest lands for it -- the march (RM1-RM4) relies
    // on PhysicsWorld's own notion of "no collider" matching the voxels'
    // current state, not a stale one.
    PhysicsRig rig;
    Terrain& t = terrain_in_workspace(rig.game);
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.fill(slab(48.f, 8.f), 1); }));
    TerrainWorld world(PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    settle(world, rig.game);

    // The slab's top face belongs to the chunk below y = 0 (as TP4 notes).
    const ChunkCoord coord{0, -1, 0};
    // Force a collider for this chunk once, as if a body had been near it,
    // then let it fall out of interest -- but still within its 5 s grace --
    // before editing it.
    world.set_collider_interest(t.id(), {coord});
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(collider_revision(world, t.id(), coord) != 0u);
    world.set_collider_interest(t.id(), {});
    settle(world, rig.game);
    rig.physics.sync(rig.game);
    REQUIRE(collider_revision(world, t.id(), coord) != 0u);   // the grace period, not an edit, would keep it

    // Dig a hole straight through the slab at this chunk (TL2's own hole),
    // far from collider interest (nothing asks for it), still well inside
    // the 5 s grace.
    Shape hole;
    hole.kind = Shape::Kind::Block;
    hole.frame = matrix4_translation(8.f, -4.f, 8.f);
    hole.size = Vec3{12.f, 20.f, 12.f};
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.subtract(hole); }));
    world.update(rig.game);
    world.wait_idle();
    world.update(rig.game);

    REQUIRE(collider_revision(world, t.id(), coord) == 0u);   // dropped at once: the edit landed without one
    rig.physics.sync(rig.game);
    REQUIRE_FALSE(rig.physics.raycast(rig.game, Vec3{4.f, 10.f, 4.f}, Vec3{0.f, -20.f, 0.f}, {}).has_value());
}
