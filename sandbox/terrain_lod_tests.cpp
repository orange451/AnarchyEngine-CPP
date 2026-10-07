// Terrain LOD node keys and the compact node mesh (Task 1 of the terrain
// LOD plan): node_of/parent_of/children_of, node_bounds, and pack()/unpack()
// of a CompactMesh against a Surface Nets ball.

#include "amesh.hpp"
#include "terrain/LodNode.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

using Catch::Approx;
using namespace engine_core;
using namespace engine_core::terrain;

namespace {

Shape ball_at(float x, float y, float z, float r) {
    Shape s;
    s.center = Vec3{x, y, z};
    s.radius = r;
    return s;
}

}  // namespace

TEST_CASE("LN1 node_of/parent_of/children_of round-trip, negatives included", "[terrain][lod]") {
    // A chunk at level 0 is its own node.
    REQUIRE(node_of(ChunkCoord{3, -5, 7}, 0) == NodeKey{0, 3, -5, 7});

    // Floor division by 2^level, not truncation: negative coordinates round
    // down (toward -infinity), not toward zero.
    REQUIRE(node_of(ChunkCoord{-1, -1, -1}, 1) == NodeKey{1, -1, -1, -1});
    REQUIRE(node_of(ChunkCoord{-2, -2, -2}, 1) == NodeKey{1, -1, -1, -1});
    REQUIRE(node_of(ChunkCoord{-3, 0, 1}, 1) == NodeKey{1, -2, 0, 0});

    // parent_of(node_of(chunk, L)) == node_of(chunk, L + 1), for positive and
    // negative chunks.
    for (int cx = -9; cx <= 9; cx += 3) {
        for (int cy = -9; cy <= 9; cy += 5) {
            for (int cz = -9; cz <= 9; cz += 7) {
                const ChunkCoord chunk{cx, cy, cz};
                for (int level = 0; level <= 3; ++level) {
                    REQUIRE(parent_of(node_of(chunk, level)) == node_of(chunk, level + 1));
                }
            }
        }
    }

    // children_of(parent_of(key)) contains key.
    for (int x = -4; x <= 4; ++x) {
        for (int y = -4; y <= 4; ++y) {
            const NodeKey key{2, x, y, -3};
            const NodeKey parent = parent_of(key);
            const std::array<NodeKey, 8> children = children_of(parent);
            REQUIRE(std::find(children.begin(), children.end(), key) != children.end());
        }
    }

    // children_of at level 1 recovers the 8 level-0 chunks it was built from.
    const NodeKey node1 = node_of(ChunkCoord{-4, -4, -4}, 1);
    const std::array<NodeKey, 8> children = children_of(node1);
    for (int dz = 0; dz <= 1; ++dz) {
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                const ChunkCoord chunk{-4 + dx, -4 + dy, -4 + dz};
                REQUIRE(std::find(children.begin(), children.end(), node_of(chunk, 0)) != children.end());
            }
        }
    }
}

TEST_CASE("LN2 node_bounds of level 0 equals the chunk's 32-cell box", "[terrain][lod]") {
    const float voxel_size = 1.5f;
    const NodeKey key = node_of(ChunkCoord{2, -3, 1}, 0);
    Vec3 min, max;
    node_bounds(key, voxel_size, min, max);

    const float size = static_cast<float>(kChunkSize) * voxel_size;
    REQUIRE(min.x == Approx(2.f * size));
    REQUIRE(min.y == Approx(-3.f * size));
    REQUIRE(min.z == Approx(1.f * size));
    REQUIRE(max.x == Approx(min.x + size));
    REQUIRE(max.y == Approx(min.y + size));
    REQUIRE(max.z == Approx(min.z + size));

    // A level-1 node is 2x a level-0 node's size on each side.
    const NodeKey level1 = parent_of(key);
    Vec3 min1, max1;
    node_bounds(level1, voxel_size, min1, max1);
    REQUIRE((max1.x - min1.x) == Approx(2.f * size));
}

namespace {

// A Surface Nets ball, meshed from one chunk: enough vertices, normals and
// material Ids to exercise pack()/unpack() meaningfully.
std::shared_ptr<const anarchy::amesh::Data> surface_nets_ball() {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(16.f, 16.f, 16.f, 10.f), 7));
    const ChunkMesh mesh = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    REQUIRE(mesh.render != nullptr);
    REQUIRE_FALSE(mesh.render->vertices.empty());
    return mesh.render;
}

}  // namespace

TEST_CASE("LN3 pack/unpack of a Surface Nets ball round-trips within tolerance", "[terrain][lod]") {
    const std::shared_ptr<const anarchy::amesh::Data> ball = surface_nets_ball();

    Vec3 bounds_min{0.f, 0.f, 0.f};
    Vec3 bounds_max{static_cast<float>(kChunkSize), static_cast<float>(kChunkSize), static_cast<float>(kChunkSize)};
    const CompactMesh compact = pack(*ball, bounds_min, bounds_max);
    const anarchy::amesh::Data round_tripped = unpack(compact);

    REQUIRE(round_tripped.vertices.size() == ball->vertices.size());

    const float scale_x = bounds_max.x - bounds_min.x;
    const float scale_y = bounds_max.y - bounds_min.y;
    const float scale_z = bounds_max.z - bounds_min.z;
    const float tol_x = scale_x / 65535.f;
    const float tol_y = scale_y / 65535.f;
    const float tol_z = scale_z / 65535.f;

    for (std::size_t i = 0; i < ball->vertices.size(); ++i) {
        const anarchy::amesh::Vertex& original = ball->vertices[i];
        const anarchy::amesh::Vertex& rt = round_tripped.vertices[i];

        // Position: within one quantization step per axis.
        REQUIRE(std::fabs(rt.p[0] - original.p[0]) <= tol_x + 1e-5f);
        REQUIRE(std::fabs(rt.p[1] - original.p[1]) <= tol_y + 1e-5f);
        REQUIRE(std::fabs(rt.p[2] - original.p[2]) <= tol_z + 1e-5f);

        // Normal: within 0.02 (dot >= 0.9998) of the original direction.
        const float len = std::sqrt(original.n[0] * original.n[0] + original.n[1] * original.n[1] +
                                     original.n[2] * original.n[2]);
        REQUIRE(len > 0.f);
        const float ox = original.n[0] / len, oy = original.n[1] / len, oz = original.n[2] / len;
        const float rt_len = std::sqrt(rt.n[0] * rt.n[0] + rt.n[1] * rt.n[1] + rt.n[2] * rt.n[2]);
        REQUIRE(rt_len > 0.f);
        const float rx = rt.n[0] / rt_len, ry = rt.n[1] / rt_len, rz = rt.n[2] / rt_len;
        const float dot = ox * rx + oy * ry + oz * rz;
        REQUIRE(dot >= 0.9998);

        // Ids: copied from rgba exactly, no quantization.
        for (int c = 0; c < 4; ++c) {
            REQUIRE(rt.rgba[c] == original.rgba[c]);
        }

        // Weights: within 1/255 of the original (all 0 for a Surface Nets
        // mesh, which has no skinning, so this also covers the degenerate case).
        for (int c = 0; c < 4; ++c) {
            REQUIRE(std::fabs(rt.weight[c] - original.weight[c]) <= 1.f / 255.f + 1e-6f);
        }
    }

    REQUIRE(round_tripped.indices.size() == ball->indices.size());
    for (std::size_t i = 0; i < ball->indices.size(); ++i) {
        REQUIRE(round_tripped.indices[i] == ball->indices[i]);
    }

    // unpack() sets the AABB from the unpacked positions.
    REQUIRE(round_tripped.bbox_min[0] <= round_tripped.bbox_max[0]);
    REQUIRE(round_tripped.bbox_min[1] <= round_tripped.bbox_max[1]);
    REQUIRE(round_tripped.bbox_min[2] <= round_tripped.bbox_max[2]);
}

TEST_CASE("LN4 bytes() stays within the per-vertex/per-triangle compact RAM budget", "[terrain][lod]") {
    const std::shared_ptr<const anarchy::amesh::Data> ball = surface_nets_ball();
    const Vec3 bounds_min{0.f, 0.f, 0.f};
    const Vec3 bounds_max{static_cast<float>(kChunkSize), static_cast<float>(kChunkSize), static_cast<float>(kChunkSize)};
    const CompactMesh compact = pack(*ball, bounds_min, bounds_max);

    REQUIRE(compact.indices32.empty());  // the test ball has far fewer than 65536 vertices
    const std::size_t vertex_count = ball->vertices.size();
    const std::size_t triangle_count = ball->indices.size() / 3;
    // R1: 16 B/vertex (6 position + 2 normal + 4 Ids + 4 weights) + 6 B/triangle (u16 indices).
    const std::size_t budget = 16 * vertex_count + 6 * triangle_count;
    REQUIRE(compact.bytes() <= budget);
}
