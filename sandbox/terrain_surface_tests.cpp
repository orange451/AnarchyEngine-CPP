// Terrain surfaces: Surface Nets meshing, the mesher pool, TerrainWorld,
// terrain bodies, and what the renderer is handed.

#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <tuple>
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
