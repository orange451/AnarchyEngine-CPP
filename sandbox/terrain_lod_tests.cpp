// Terrain LOD node keys and the compact node mesh (Task 1 of the terrain
// LOD plan): node_of/parent_of/children_of, node_bounds, and pack()/unpack()
// of a CompactMesh against a Surface Nets ball.

#include "support.hpp"

#include "Camera.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"
#include "TerrainWorld.hpp"
#include "amesh.hpp"
#include "terrain/BlendWeights.hpp"
#include "terrain/LodBuilder.hpp"
#include "terrain/LodNode.hpp"
#include "terrain/LodTree.hpp"
#include "terrain/ShapeDistance.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelSampler.hpp"
#include "terrain/VoxelVolume.hpp"

#include <cstring>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

        // R7: blend weights live in v.t[0..3] (the tangent channel,
        // repurposed for terrain -- a Surface Nets mesh carries no skinning
        // weights, so v.weight stays untouched and uninteresting here), and
        // pack()/unpack() must carry them within 1/255, same as the Ids.
        for (int c = 0; c < 4; ++c) {
            REQUIRE(std::fabs(rt.t[c] - original.t[c]) <= 1.f / 255.f + 1e-6f);
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

namespace {

// A synthetic grid mesh with more than 65,535 vertices, to exercise pack()'s
// indices32 path (LN4's u16-index budget only covers the common case).
anarchy::amesh::Data synthetic_grid_mesh(int width, int height) {
    anarchy::amesh::Data out;
    out.vertices.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            const std::size_t idx = static_cast<std::size_t>(j) * static_cast<std::size_t>(width) + static_cast<std::size_t>(i);
            anarchy::amesh::Vertex& v = out.vertices[idx];
            v.p[0] = static_cast<float>(i);
            v.p[1] = static_cast<float>(j);
            v.p[2] = 0.1f * static_cast<float>((i + j) % 7);
            v.n[0] = 0.f;
            v.n[1] = 0.f;
            v.n[2] = 1.f;
            for (int c = 0; c < 4; ++c) {
                v.rgba[c] = static_cast<std::uint8_t>((i * 7 + j * 13 + c * 29) % 256);
                // R7: pack()/unpack() carry the terrain blend weight channel
                // (v.t), not v.weight (skinning, which a terrain mesh never has).
                v.t[c] = static_cast<float>((i + c) % 4) / 3.f;
            }
        }
    }

    for (int j = 0; j < height - 1; ++j) {
        for (int i = 0; i < width - 1; ++i) {
            const std::uint32_t tl = static_cast<std::uint32_t>(j * width + i);
            const std::uint32_t tr = tl + 1;
            const std::uint32_t bl = static_cast<std::uint32_t>((j + 1) * width + i);
            const std::uint32_t br = bl + 1;
            out.indices.push_back(tl);
            out.indices.push_back(bl);
            out.indices.push_back(tr);
            out.indices.push_back(tr);
            out.indices.push_back(bl);
            out.indices.push_back(br);
        }
    }

    anarchy::amesh::compute_aabb(out);
    return out;
}

}  // namespace

TEST_CASE("LN5 pack/unpack of a >65535-vertex mesh uses indices32", "[terrain][lod]") {
    // 256 x 257 = 65,792 vertices: just over the 16-bit index limit.
    const int width = 256, height = 257;
    const anarchy::amesh::Data grid = synthetic_grid_mesh(width, height);
    REQUIRE(grid.vertices.size() > 65535u);

    const Vec3 bounds_min{0.f, 0.f, 0.f};
    const Vec3 bounds_max{static_cast<float>(width - 1), static_cast<float>(height - 1), 1.f};
    const CompactMesh compact = pack(grid, bounds_min, bounds_max);

    // indices32 is used instead of indices once the node exceeds 65,535 vertices.
    REQUIRE_FALSE(compact.indices32.empty());
    REQUIRE(compact.indices.empty());
    REQUIRE(compact.indices32.size() == grid.indices.size());
    for (std::size_t i = 0; i < grid.indices.size(); ++i) {
        REQUIRE(compact.indices32[i] == grid.indices[i]);
    }

    const std::size_t vertex_count = grid.vertices.size();
    const std::size_t triangle_count = grid.indices.size() / 3;
    // R1: 16 B/vertex + 12 B/triangle (u32 indices) when indices32 is in use.
    REQUIRE(compact.bytes() == 16 * vertex_count + 12 * triangle_count);

    const anarchy::amesh::Data round_tripped = unpack(compact);
    REQUIRE(round_tripped.vertices.size() == grid.vertices.size());
    REQUIRE(round_tripped.indices.size() == grid.indices.size());
    for (std::size_t i = 0; i < grid.indices.size(); ++i) {
        REQUIRE(round_tripped.indices[i] == grid.indices[i]);
    }

    const float scale_x = bounds_max.x - bounds_min.x;
    const float scale_y = bounds_max.y - bounds_min.y;
    const float scale_z = bounds_max.z - bounds_min.z;
    const float tol_x = scale_x / 65535.f;
    const float tol_y = scale_y / 65535.f;
    const float tol_z = scale_z / 65535.f;

    for (std::size_t i = 0; i < grid.vertices.size(); ++i) {
        const anarchy::amesh::Vertex& original = grid.vertices[i];
        const anarchy::amesh::Vertex& rt = round_tripped.vertices[i];
        REQUIRE(std::fabs(rt.p[0] - original.p[0]) <= tol_x + 1e-5f);
        REQUIRE(std::fabs(rt.p[1] - original.p[1]) <= tol_y + 1e-5f);
        REQUIRE(std::fabs(rt.p[2] - original.p[2]) <= tol_z + 1e-5f);
        for (int c = 0; c < 4; ++c) {
            REQUIRE(rt.rgba[c] == original.rgba[c]);
            REQUIRE(std::fabs(rt.t[c] - original.t[c]) <= 1.f / 255.f + 1e-6f);
        }
    }
}

TEST_CASE("LN6 pack/unpack of a degenerate (flat) axis round-trips without NaN or Inf", "[terrain][lod]") {
    // A flat quad: all four vertices share the same Z, so bounds_min.z ==
    // bounds_max.z and pack()'s quantize_axis must take the scale == 0
    // branch instead of dividing by zero.
    anarchy::amesh::Data flat;
    flat.vertices.resize(4);
    const float flat_z = 5.f;
    const float xs[4] = {0.f, 4.f, 0.f, 4.f};
    const float ys[4] = {0.f, 0.f, 4.f, 4.f};
    for (int i = 0; i < 4; ++i) {
        anarchy::amesh::Vertex& v = flat.vertices[static_cast<std::size_t>(i)];
        v.p[0] = xs[i];
        v.p[1] = ys[i];
        v.p[2] = flat_z;
        v.n[0] = 0.f;
        v.n[1] = 0.f;
        v.n[2] = 1.f;
        for (int c = 0; c < 4; ++c) {
            v.rgba[c] = static_cast<std::uint8_t>(10 * i + c);
            v.weight[c] = 0.25f * static_cast<float>(c);
        }
    }
    flat.indices = {0, 1, 2, 1, 3, 2};
    anarchy::amesh::compute_aabb(flat);

    const Vec3 bounds_min{0.f, 0.f, flat_z};
    const Vec3 bounds_max{4.f, 4.f, flat_z};  // degenerate on Z
    const CompactMesh compact = pack(flat, bounds_min, bounds_max);
    const anarchy::amesh::Data round_tripped = unpack(compact);

    REQUIRE(round_tripped.vertices.size() == flat.vertices.size());
    for (std::size_t i = 0; i < flat.vertices.size(); ++i) {
        const anarchy::amesh::Vertex& rt = round_tripped.vertices[i];
        REQUIRE(std::isfinite(rt.p[0]));
        REQUIRE(std::isfinite(rt.p[1]));
        REQUIRE(std::isfinite(rt.p[2]));
        // The degenerate axis quantizes to 0, which unpacks back to origin.z
        // exactly (origin.z + 0 / 65535 * 0 == origin.z == flat_z).
        REQUIRE(std::fabs(rt.p[2] - flat_z) <= 1e-5f);
        REQUIRE(std::fabs(rt.p[0] - flat.vertices[i].p[0]) <= 4.f / 65535.f + 1e-5f);
        REQUIRE(std::fabs(rt.p[1] - flat.vertices[i].p[1]) <= 4.f / 65535.f + 1e-5f);
    }
}

TEST_CASE("LN7 pack/unpack preserves non-zero, non-uniform blend weights (v.t)", "[terrain][lod]") {
    anarchy::amesh::Data mesh;
    mesh.vertices.resize(2);
    const float weights0[4] = {0.f, 0.33f, 0.99f, 1.f};
    const float weights1[4] = {1.f, 0.99f, 0.33f, 0.f};
    for (int c = 0; c < 4; ++c) {
        mesh.vertices[0].t[c] = weights0[c];
        mesh.vertices[1].t[c] = weights1[c];
    }
    mesh.vertices[0].p[0] = 0.f;
    mesh.vertices[0].p[1] = 0.f;
    mesh.vertices[0].p[2] = 0.f;
    mesh.vertices[1].p[0] = 1.f;
    mesh.vertices[1].p[1] = 1.f;
    mesh.vertices[1].p[2] = 1.f;
    mesh.vertices[0].n[2] = 1.f;
    mesh.vertices[1].n[2] = 1.f;
    mesh.indices = {0, 1, 0};
    anarchy::amesh::compute_aabb(mesh);

    const Vec3 bounds_min{0.f, 0.f, 0.f};
    const Vec3 bounds_max{1.f, 1.f, 1.f};
    const CompactMesh compact = pack(mesh, bounds_min, bounds_max);
    const anarchy::amesh::Data round_tripped = unpack(compact);

    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            REQUIRE(std::fabs(round_tripped.vertices[i].t[c] - mesh.vertices[i].t[c]) <= 1.f / 255.f + 1e-6f);
        }
    }
}

// --- Task 2: LodBuilder (merge, simplify, measure) -------------------------

namespace {

// A rolling slab over 16 x 1 x 16 chunks (512 x 32 x 512 units): a smooth,
// low-curvature height field (R9's style; see fill_rolling_wave below),
// written directly as signed distances, with the same below-the-floor
// margin fill_rolling_wave uses and for the same reason (no spurious,
// disconnected floor-cap surface at the bottom of chunk-y 0). This used to
// be a union of balls whose centers rose and fell along the same wave
// instead: every adjacent pair's saddle-shaped valley sat exactly where
// meshoptimizer's quadric error most underestimates true distance
// (confirmed while chasing LB1's budget failure -- locking every border
// vertex changed nothing, and neither did halving the wave's amplitude or
// widening the balls' own overlap: the level-1 error this test's own
// diagnostics recorded, ~0.579 units, stayed the same regardless, pinned to
// the ball-packing's own geometry rather than to the terrain's curvature).
// A true height field has no seam between pieces for that error to hide in.
void fill_rolling_slab(VoxelVolume& volume) {
    const int x0 = 0, x1 = 511, z0 = 0, z1 = 511;  // the full 16x16-chunk footprint
    const int y0 = -8, y1 = kChunkSize - 1;         // a margin below chunk-y 0's own floor
    const int width = x1 - x0 + 1, height = y1 - y0 + 1, depth = z1 - z0 + 1;

    std::vector<float> distances(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                  static_cast<std::size_t>(depth));
    std::vector<std::uint8_t> materials(distances.size(), 1);
    for (int z = z0; z <= z1; ++z) {
        const float fz = static_cast<float>(z);
        for (int x = x0; x <= x1; ++x) {
            const float fx = static_cast<float>(x);
            const float surface_y = 16.f + 2.f * std::sin(fx / 40.f) * std::cos(fz / 50.f);
            for (int y = y0; y <= y1; ++y) {
                const std::size_t i = static_cast<std::size_t>((x - x0) + width * ((y - y0) + height * (z - z0)));
                distances[i] = static_cast<float>(y) - surface_y;
            }
        }
    }
    REQUIRE_FALSE(volume.write(CellCoord{x0, y0, z0}, CellCoord{x1, y1, z1}, distances, materials));
}

// A genuinely rolling surface (R9): a smooth, low-curvature height field --
// z = A*sin(x/P)*cos(z/P), amplitude a few voxels, period tens of voxels --
// written directly as signed distances. Unlike a ball union (fill_rolling_
// slab, below), this has no seam between balls for meshoptimizer's quadric
// error to mismeasure. Written well past the node's own 64x64-unit footprint
// (matching the margin the flat Block this replaced used) so the written
// area's own lateral perimeter -- where the terrain would otherwise meet
// untouched air and stand a tall, artificial cliff -- falls outside every
// one of the node's own children chunks. The y range is extended a few
// cells below the chunk's own floor (y=0) for the same reason on the
// vertical axis: SurfaceNets samples one cell past each chunk's own border,
// and leaving y<0 at its unwritten default (air) manufactures a second,
// disconnected, perfectly flat "floor cap" surface at the bottom of the
// chunk -- found by printing children's/the result's bboxes while chasing
// LB1's budget failure: the result mesh's own bbox started at y=14.8 while
// a child vertex sat at y=-0.5, meaning meshopt_simplify (lock_border=0, as
// R9/the interface call for) had collapsed that whole separate, entirely
// open-bordered, zero-quadric-cost patch away before spending any of its
// triangle budget on the wave, leaving the floor's vertices with nothing
// nearby at all. Continuing the real height field downward instead keeps
// the solid genuinely solid at every sampled cell, so there is no second
// surface to begin with. center_x/center_z should be the node's own center
// in units.
void fill_rolling_wave(VoxelVolume& volume, float center_x, float center_z) {
    const int half = 80;  // matches fill_gentle_dome's old 160-unit-wide slab
    const int x0 = static_cast<int>(center_x) - half, x1 = x0 + 2 * half - 1;
    const int z0 = static_cast<int>(center_z) - half, z1 = z0 + 2 * half - 1;
    const int y0 = -8, y1 = kChunkSize - 1;  // a margin below chunk-y 0's own floor
    const int width = x1 - x0 + 1, height = y1 - y0 + 1, depth = z1 - z0 + 1;

    std::vector<float> distances(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                  static_cast<std::size_t>(depth));
    std::vector<std::uint8_t> materials(distances.size(), 1);
    for (int z = z0; z <= z1; ++z) {
        const float fz = static_cast<float>(z);
        for (int x = x0; x <= x1; ++x) {
            const float fx = static_cast<float>(x);
            const float surface_y = 16.f + 3.f * std::sin(fx / 40.f) * std::cos(fz / 50.f);
            for (int y = y0; y <= y1; ++y) {
                const std::size_t i = static_cast<std::size_t>((x - x0) + width * ((y - y0) + height * (z - z0)));
                distances[i] = static_cast<float>(y) - surface_y;
            }
        }
    }
    REQUIRE_FALSE(volume.write(CellCoord{x0, y0, z0}, CellCoord{x1, y1, z1}, distances, materials));
}

// level 1's 8 children (children_of's level-0 keys are chunk coordinates
// directly, by node_of's definition), meshed, non-null ones only.
std::vector<std::shared_ptr<const anarchy::amesh::Data>> mesh_level1_children(VoxelVolume& volume,
                                                                               const NodeKey& level1_key,
                                                                               std::size_t& triangle_count) {
    std::vector<std::shared_ptr<const anarchy::amesh::Data>> children;
    triangle_count = 0;
    for (const NodeKey& child_key : children_of(level1_key)) {
        const ChunkCoord coord{child_key.x, child_key.y, child_key.z};
        const ChunkMesh mesh = surface_nets(mesh_input(volume, coord));
        if (mesh.render) {
            triangle_count += mesh.render->indices.size() / 3;
            children.push_back(mesh.render);
        }
    }
    return children;
}

std::size_t total_triangles(const std::unordered_map<NodeKey, std::shared_ptr<const anarchy::amesh::Data>, NodeKeyHash>& level) {
    std::size_t total = 0;
    for (const auto& [key, mesh] : level) {
        if (mesh) {
            total += mesh->indices.size() / 3;
        }
    }
    return total;
}

// Independent (not meshoptimizer's, not LodBuilder.cpp's) brute-force
// point-to-triangle squared distance, for LB2's honesty check: the standard
// barycentric-region closest point, written from scratch here rather than
// shared with the implementation under test.
float brute_point_triangle_distance_sq(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    // Project p onto the triangle's plane, clamp to the triangle if the
    // projection falls outside it (nearest edge or vertex).
    const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 ac{c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 n{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
    const float n_len_sq = n.x * n.x + n.y * n.y + n.z * n.z;
    Vec3 planar = p;
    if (n_len_sq > 1e-12f) {
        const Vec3 ap{p.x - a.x, p.y - a.y, p.z - a.z};
        const float dist_along_normal = (ap.x * n.x + ap.y * n.y + ap.z * n.z) / n_len_sq;
        planar = Vec3{p.x - dist_along_normal * n.x, p.y - dist_along_normal * n.y, p.z - dist_along_normal * n.z};
    }

    // Barycentric coordinates of planar within triangle (a, b, c).
    const Vec3 ap{planar.x - a.x, planar.y - a.y, planar.z - a.z};
    const float d00 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    const float d01 = ab.x * ac.x + ab.y * ac.y + ab.z * ac.z;
    const float d11 = ac.x * ac.x + ac.y * ac.y + ac.z * ac.z;
    const float d20 = ap.x * ab.x + ap.y * ab.y + ap.z * ab.z;
    const float d21 = ap.x * ac.x + ap.y * ac.y + ap.z * ac.z;
    const float denom = d00 * d11 - d01 * d01;

    auto point_segment_distance_sq = [](Vec3 q, Vec3 s0, Vec3 s1) {
        const Vec3 d{s1.x - s0.x, s1.y - s0.y, s1.z - s0.z};
        const float len_sq = d.x * d.x + d.y * d.y + d.z * d.z;
        const Vec3 w{q.x - s0.x, q.y - s0.y, q.z - s0.z};
        float t = len_sq > 1e-12f ? (w.x * d.x + w.y * d.y + w.z * d.z) / len_sq : 0.f;
        t = std::clamp(t, 0.f, 1.f);
        const Vec3 closest{s0.x + t * d.x, s0.y + t * d.y, s0.z + t * d.z};
        const float dx = q.x - closest.x, dy = q.y - closest.y, dz = q.z - closest.z;
        return dx * dx + dy * dy + dz * dz;
    };

    if (std::fabs(denom) > 1e-12f) {
        const float v = (d11 * d20 - d01 * d21) / denom;
        const float w = (d00 * d21 - d01 * d20) / denom;
        const float u = 1.f - v - w;
        if (u >= 0.f && v >= 0.f && w >= 0.f) {
            // Inside the triangle: distance is just the out-of-plane offset.
            const float dx = p.x - planar.x, dy = p.y - planar.y, dz = p.z - planar.z;
            return dx * dx + dy * dy + dz * dz;
        }
    }
    // Outside: nearest point lies on one of the three edges (or their shared vertices).
    float best = point_segment_distance_sq(p, a, b);
    best = std::min(best, point_segment_distance_sq(p, b, c));
    best = std::min(best, point_segment_distance_sq(p, c, a));
    return best;
}

Vec3 mesh_vertex_position(const anarchy::amesh::Data& mesh, std::uint32_t index) {
    const anarchy::amesh::Vertex& v = mesh.vertices[index];
    return Vec3{v.p[0], v.p[1], v.p[2]};
}

// Brute force (every vertex against every triangle): the max distance from
// any vertex of children to mesh's nearest triangle.
float brute_max_child_distance(const std::vector<std::shared_ptr<const anarchy::amesh::Data>>& children,
                                const anarchy::amesh::Data& mesh) {
    const std::size_t triangle_count = mesh.indices.size() / 3;
    float worst = 0.f;
    for (const auto& child : children) {
        for (const anarchy::amesh::Vertex& cv : child->vertices) {
            const Vec3 p{cv.p[0], cv.p[1], cv.p[2]};
            float best_sq = std::numeric_limits<float>::max();
            for (std::size_t t = 0; t < triangle_count; ++t) {
                const Vec3 a = mesh_vertex_position(mesh, mesh.indices[t * 3 + 0]);
                const Vec3 b = mesh_vertex_position(mesh, mesh.indices[t * 3 + 1]);
                const Vec3 c = mesh_vertex_position(mesh, mesh.indices[t * 3 + 2]);
                best_sq = std::min(best_sq, brute_point_triangle_distance_sq(p, a, b, c));
            }
            worst = std::max(worst, std::sqrt(best_sq));
        }
    }
    return worst;
}

}  // namespace

TEST_CASE("LB1 a level-1 node of a rolling surface has <= 30% of its children's triangles, within budget",
          "[terrain][lod]") {
    // A true height-field wave over the node's own 2x2 chunks (R9; see
    // fill_rolling_wave), margined well past the node itself so the
    // written area's own edge-of-world cliff falls outside every one of
    // the node's children: unlike fill_rolling_slab (below, used by LB3,
    // which checks only the triangle-count ratio and so can tolerate a few
    // edge chunks' genuine edge-of-world error), this test also checks the
    // error budget, so it needs every one of its own children clear of any
    // cliff, not just mostly.
    VoxelVolume volume;
    const NodeKey level1_key{1, 3, 0, 3};
    Vec3 node_min, node_max;
    node_bounds(level1_key, 1.f, node_min, node_max);
    fill_rolling_wave(volume, (node_min.x + node_max.x) * 0.5f, (node_min.z + node_max.z) * 0.5f);
    const float voxel_size = volume.voxel_size();

    std::size_t children_triangles = 0;
    const std::vector<std::shared_ptr<const anarchy::amesh::Data>> children =
        mesh_level1_children(volume, level1_key, children_triangles);
    REQUIRE(children_triangles > 0);

    LodInput input;
    input.key = level1_key;
    input.voxel_size = voxel_size;
    input.children = children;
    const LodResult result = build_node(input);

    REQUIRE(result.mesh != nullptr);
    const std::size_t result_triangles = result.mesh->indices.size() / 3;
    INFO("children triangles: " << children_triangles << ", result triangles: " << result_triangles
                                 << ", error: " << result.error << ", budget: " << target_error(1, voxel_size));
    REQUIRE(static_cast<double>(result_triangles) <= 0.30 * static_cast<double>(children_triangles));
    REQUIRE(result.error <= target_error(1, voxel_size));
}

TEST_CASE("LB2 the recorded error is honest: every child vertex lies within error + 1e-3 of the simplified surface",
          "[terrain][lod]") {
    // A small case, deliberately: one ball straddling the x boundary between
    // chunks (0,0,0) and (1,0,0), kept well clear of every other chunk
    // boundary. A few hundred triangles per child keeps the brute-force
    // child-vertex-against-every-result-triangle check (below) fast even in
    // an unoptimized Debug build.
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(32.f, 16.f, 16.f, 6.f), 1));
    const float voxel_size = volume.voxel_size();
    const NodeKey level1_key{1, 0, 0, 0};

    std::size_t children_triangles = 0;
    const std::vector<std::shared_ptr<const anarchy::amesh::Data>> children =
        mesh_level1_children(volume, level1_key, children_triangles);
    REQUIRE(children.size() >= 2);  // must actually straddle the boundary
    REQUIRE(children_triangles > 0);

    LodInput input;
    input.key = level1_key;
    input.voxel_size = voxel_size;
    input.children = children;
    const LodResult result = build_node(input);
    REQUIRE(result.mesh != nullptr);

    const float measured = brute_max_child_distance(children, *result.mesh);
    INFO("children triangles: " << children_triangles << ", result triangles: " << result.mesh->indices.size() / 3
                                 << ", recorded error: " << result.error << ", brute-force measured max distance: "
                                 << measured);
    REQUIRE(measured <= result.error + 1e-3f);
}

TEST_CASE("LB3 levels 1..4 built bottom-up over a 16x1x16-chunk slab fall about 4x per level", "[terrain][lod]") {
    VoxelVolume volume;
    fill_rolling_slab(volume);
    const float voxel_size = volume.voxel_size();

    std::unordered_map<NodeKey, std::shared_ptr<const anarchy::amesh::Data>, NodeKeyHash> level;
    std::unordered_map<NodeKey, float, NodeKeyHash> level_errors;  // R8: passed up as child_errors
    // R10: passed up as child_surface_index_counts; a level-0 chunk mesh has
    // no skirts, so its whole index buffer is surface.
    std::unordered_map<NodeKey, std::uint32_t, NodeKeyHash> level_surface_index_counts;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            const ChunkCoord coord{x, 0, z};
            const ChunkMesh mesh = surface_nets(mesh_input(volume, coord));
            if (mesh.render) {
                const NodeKey key = node_of(coord, 0);
                level[key] = mesh.render;
                level_errors[key] = 0.f;  // a level-0 chunk mesh is exact (R8)
                level_surface_index_counts[key] = static_cast<std::uint32_t>(mesh.render->indices.size());
            }
        }
    }
    std::size_t previous_triangles = total_triangles(level);
    REQUIRE(previous_triangles > 0);
    INFO("level 0 triangles: " << previous_triangles);

    for (int lvl = 1; lvl <= 4; ++lvl) {
        std::unordered_map<NodeKey, std::vector<NodeKey>, NodeKeyHash> grouped;
        for (const auto& [key, mesh] : level) {
            grouped[parent_of(key)].push_back(key);
        }
        std::unordered_map<NodeKey, std::shared_ptr<const anarchy::amesh::Data>, NodeKeyHash> next;
        std::unordered_map<NodeKey, float, NodeKeyHash> next_errors;
        std::unordered_map<NodeKey, std::uint32_t, NodeKeyHash> next_surface_index_counts;
        for (const auto& [parent_key, child_keys] : grouped) {
            LodInput input;
            input.key = parent_key;
            input.voxel_size = voxel_size;
            for (const NodeKey& child_key : child_keys) {
                input.children.push_back(level[child_key]);
                input.child_errors.push_back(level_errors[child_key]);
                input.child_surface_index_counts.push_back(level_surface_index_counts[child_key]);
            }
            const LodResult result = build_node(input);
            if (result.mesh) {
                next[parent_key] = result.mesh;
                next_errors[parent_key] = result.error;
                next_surface_index_counts[parent_key] = result.surface_index_count;
            }
        }
        const std::size_t triangles = total_triangles(next);
        REQUIRE(triangles > 0);
        const float ratio = static_cast<float>(previous_triangles) / static_cast<float>(triangles);
        INFO("level " << lvl << " triangles: " << triangles << " (previous level " << previous_triangles
                       << ", ratio " << ratio << ")");
        WARN("LB3 level " << lvl << ": " << triangles << " triangles, ratio " << ratio);
        REQUIRE(ratio >= 2.5f);
        REQUIRE(ratio <= 6.0f);
        previous_triangles = triangles;
        level = std::move(next);
        level_errors = std::move(next_errors);
        level_surface_index_counts = std::move(next_surface_index_counts);
    }
}

TEST_CASE("LB4 an empty child set gives a null mesh", "[terrain][lod]") {
    LodInput input;
    input.key = NodeKey{1, 0, 0, 0};
    input.voxel_size = 1.f;

    const LodResult result = build_node(input);

    REQUIRE(result.key == input.key);
    REQUIRE(result.mesh == nullptr);
    REQUIRE(result.error == 0.f);
    REQUIRE(result.border_edges.empty());
}

// Task 3: re-shading LOD node vertices from full-resolution voxels
// (VoxelSampler), and skirts along a node's border edges.

namespace {

// Builds key's node bottom-up from volume: level 0 is the chunk's own exact
// Surface Nets mesh (no simplification; build_node is never called on it);
// level >= 1 recurses into children_of(key) first, then calls build_node
// with every non-null child's mesh and recorded error, and voxels threaded
// through at every level (re-shading always reads the full-resolution
// field, regardless of which level is being built).
LodResult build_lod_node(VoxelVolume& volume, const NodeKey& key, const std::shared_ptr<const ChunkMap>& voxels) {
    if (key.level == 0) {
        const ChunkCoord coord{key.x, key.y, key.z};
        const ChunkMesh mesh = surface_nets(mesh_input(volume, coord));
        LodResult result;
        result.key = key;
        result.mesh = mesh.render;
        return result;
    }
    LodInput input;
    input.key = key;
    input.voxel_size = volume.voxel_size();
    input.voxels = voxels;
    for (const NodeKey& child_key : children_of(key)) {
        const LodResult child = build_lod_node(volume, child_key, voxels);
        if (child.mesh) {
            input.children.push_back(child.mesh);
            input.child_errors.push_back(child.error);
            // R10: a level-0 child's LodResult never goes through build_node
            // (see above), so it leaves surface_index_count at its default
            // (0); the whole mesh is surface, so pass its real index count
            // instead of that default.
            input.child_surface_index_counts.push_back(
                child_key.level == 0 ? static_cast<std::uint32_t>(child.mesh->indices.size()) : child.surface_index_count);
        }
    }
    return build_node(input);
}

// add_skirts (LodBuilder.cpp) appends exactly 2 new vertices per border edge
// to the tail of mesh->vertices, in border_edges' own order, after whatever
// vertices the simplified (pre-skirt) surface already had -- so the
// pre-skirt vertex count is recoverable from the result alone, letting a
// test tell a node's original surface vertices apart from its skirts'.
std::size_t original_vertex_count(const LodResult& result) {
    return result.mesh->vertices.size() - result.border_edges.size();
}

}  // namespace

TEST_CASE("RS1 a level-2 node of a ball re-shades every original vertex's normal to the analytic gradient",
          "[terrain][lod]") {
    VoxelVolume volume;
    const Vec3 center{64.f, 64.f, 64.f};
    const float radius = 40.f;
    REQUIRE_FALSE(volume.fill(ball_at(center.x, center.y, center.z, radius), 1));
    const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());

    const NodeKey level2_key{2, 0, 0, 0};
    const LodResult result = build_lod_node(volume, level2_key, voxels);
    REQUIRE(result.mesh != nullptr);

    const std::size_t original_count = original_vertex_count(result);
    REQUIRE(original_count > 0);

    int checked = 0;
    for (std::size_t i = 0; i < original_count; ++i) {
        const anarchy::amesh::Vertex& v = result.mesh->vertices[i];
        const Vec3 to_vertex{v.p[0] - center.x, v.p[1] - center.y, v.p[2] - center.z};
        const float r = std::sqrt(to_vertex.x * to_vertex.x + to_vertex.y * to_vertex.y + to_vertex.z * to_vertex.z);
        REQUIRE(r > 0.f);
        const Vec3 analytic{to_vertex.x / r, to_vertex.y / r, to_vertex.z / r};

        const float n_len = std::sqrt(v.n[0] * v.n[0] + v.n[1] * v.n[1] + v.n[2] * v.n[2]);
        REQUIRE(n_len > 0.f);
        const float dot = (v.n[0] * analytic.x + v.n[1] * analytic.y + v.n[2] * analytic.z) / n_len;
        INFO("vertex " << i << " position (" << v.p[0] << ", " << v.p[1] << ", " << v.p[2] << "), dot " << dot);
        REQUIRE(dot >= 0.95);
        ++checked;
    }
    REQUIRE(checked > 0);
}

namespace {

// Independent of VoxelSampler: the same blend_weights() rule
// VoxelSampler::blend() (and so reshade_vertices) now uses for a vertex's
// dominant Id -- Task 2 of the terrain textures plan superseded the old
// "lowest-distance corner's Id alone" rule this helper used to mirror, with
// one that votes over every corner within voxel_size of the surface and
// keeps the top Id (ties by lower Id); rgba[0] is that top Id. Read straight
// from volume.cell() (VoxelVolume's own chunk-map lookup, not VoxelSampler's)
// rather than calling the production sampler under test.
std::uint8_t expected_id_at(const VoxelVolume& volume, Vec3 p) {
    const float voxel_size = volume.voxel_size();
    const int ix = static_cast<int>(std::floor(p.x / voxel_size));
    const int iy = static_cast<int>(std::floor(p.y / voxel_size));
    const int iz = static_cast<int>(std::floor(p.z / voxel_size));

    float distances[8];
    std::uint8_t ids[8];
    int c = 0;
    for (int dz = 0; dz <= 1; ++dz) {
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                const Cell cell = volume.cell(CellCoord{ix + dx, iy + dy, iz + dz});
                distances[c] = dequantize(cell.distance, voxel_size);
                ids[c] = cell.material;
                ++c;
            }
        }
    }
    return blend_weights(distances, ids, voxel_size).ids[0];
}

}  // namespace

TEST_CASE("RS2 a node spanning a two-material boundary assigns each vertex the full-resolution cell's "
          "independently-sampled Id",
          "[terrain][lod]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(32.f, 32.f, 32.f, 25.f), 2));
    Shape half;
    half.kind = Shape::Kind::Block;
    half.frame = matrix4_translation(48.f, 32.f, 32.f);  // x in [32, 64): the ball's +x half
    half.size = Vec3{32.f, 64.f, 64.f};
    REQUIRE_FALSE(volume.paint(half, 5));
    const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());

    const NodeKey level1_key{1, 0, 0, 0};
    const LodResult result = build_lod_node(volume, level1_key, voxels);
    REQUIRE(result.mesh != nullptr);

    const std::size_t original_count = original_vertex_count(result);
    REQUIRE(original_count > 0);

    bool saw_material_a = false, saw_material_b = false;
    for (std::size_t i = 0; i < original_count; ++i) {
        const anarchy::amesh::Vertex& v = result.mesh->vertices[i];
        const Vec3 p{v.p[0], v.p[1], v.p[2]};
        const std::uint8_t expected = expected_id_at(volume, p);
        INFO("vertex " << i << " position (" << p.x << ", " << p.y << ", " << p.z << "), rgba[0] " << int(v.rgba[0])
                        << ", expected " << int(expected));
        // Decision 1 (terrain textures): a vertex whose triangle spans more
        // than one material set gets split, carrying that triangle's merged
        // Ids (top 4 by summed weight *over the triangle*) rather than only
        // this one corner's own independently-sampled Id -- rgba[0] is then
        // the triangle's dominant Id, not necessarily this corner's own.
        // The corner's own Id is still carried (projected and renormalized
        // onto the merged set), just not necessarily in slot 0, so check
        // for it among all 4 slots rather than slot 0 alone.
        const bool carries_expected =
            v.rgba[0] == expected || v.rgba[1] == expected || v.rgba[2] == expected || v.rgba[3] == expected;
        REQUIRE(carries_expected);
        saw_material_a = saw_material_a || v.rgba[0] == 2;
        saw_material_b = saw_material_b || v.rgba[0] == 5;
    }
    // The boundary must actually fall inside this node's mesh, or the test
    // would pass trivially with only one material ever seen.
    REQUIRE(saw_material_a);
    REQUIRE(saw_material_b);
}

TEST_CASE("RS3 every border edge gets exactly one skirt quad, its far edge displaced by max(2*error, VoxelSize) and flanged out",
          "[terrain][lod]") {
    VoxelVolume volume;
    const NodeKey level1_key{1, 2, 0, 2};
    Vec3 node_min, node_max;
    node_bounds(level1_key, 1.f, node_min, node_max);
    fill_rolling_wave(volume, (node_min.x + node_max.x) * 0.5f, (node_min.z + node_max.z) * 0.5f);
    const float voxel_size = volume.voxel_size();

    std::size_t children_triangles = 0;
    const std::vector<std::shared_ptr<const anarchy::amesh::Data>> children =
        mesh_level1_children(volume, level1_key, children_triangles);
    REQUIRE(children_triangles > 0);

    LodInput input;
    input.key = level1_key;
    input.voxel_size = voxel_size;
    input.children = children;
    input.voxels = std::make_shared<const ChunkMap>(volume.chunks());
    const LodResult result = build_node(input);
    REQUIRE(result.mesh != nullptr);

    const std::size_t edge_count = result.border_edges.size() / 2;
    REQUIRE(edge_count > 0);  // the node's own footprint cuts the (wider) wave: there must be a rim

    const float depth = std::max(2.f * result.error, voxel_size);
    const std::size_t original_count = original_vertex_count(result);
    REQUIRE(result.mesh->vertices.size() == original_count + edge_count * 2);

    // R21: the far edge is flanged outward by depth / 2 along the unit vector
    // perpendicular to the edge, in its own triangle's plane, pointing away
    // from that triangle. Find each border edge's own triangle (the one that
    // stores (a, b) in order) among the surface's triangles.
    std::unordered_map<std::uint64_t, std::uint32_t> third_of;
    for (std::size_t t = 0; t < result.surface_index_count; t += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint64_t a = result.mesh->indices[t + k], b = result.mesh->indices[t + (k + 1) % 3];
            third_of[(a << 32) | b] = result.mesh->indices[t + (k + 2) % 3];
        }
    }

    for (std::size_t e = 0; e < edge_count; ++e) {
        const std::uint32_t a_index = result.border_edges[e * 2 + 0];
        const std::uint32_t b_index = result.border_edges[e * 2 + 1];
        const anarchy::amesh::Vertex& a = result.mesh->vertices[a_index];
        const anarchy::amesh::Vertex& b = result.mesh->vertices[b_index];
        // add_skirts appends a' then b' per edge, in border_edges' own order.
        const anarchy::amesh::Vertex& a_prime = result.mesh->vertices[original_count + e * 2 + 0];
        const anarchy::amesh::Vertex& b_prime = result.mesh->vertices[original_count + e * 2 + 1];

        const auto third = third_of.find((static_cast<std::uint64_t>(a_index) << 32) | b_index);
        REQUIRE(third != third_of.end());
        const Vec3 pa = mesh_vertex_position(*result.mesh, a_index);
        const Vec3 pb = mesh_vertex_position(*result.mesh, b_index);
        const Vec3 pc = mesh_vertex_position(*result.mesh, third->second);
        const Vec3 edge{pb.x - pa.x, pb.y - pa.y, pb.z - pa.z};
        const Vec3 to_c{pc.x - pa.x, pc.y - pa.y, pc.z - pa.z};
        // Away from c, in the triangle's plane: c's part across the edge, negated.
        const float along = (to_c.x * edge.x + to_c.y * edge.y + to_c.z * edge.z) /
                            (edge.x * edge.x + edge.y * edge.y + edge.z * edge.z);
        Vec3 out{-(to_c.x - along * edge.x), -(to_c.y - along * edge.y), -(to_c.z - along * edge.z)};
        const float out_length = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
        REQUIRE(out_length > 0.f);
        out = Vec3{out.x / out_length * depth * 0.5f, out.y / out_length * depth * 0.5f, out.z / out_length * depth * 0.5f};

        const float expected_ax = a.p[0] - a.n[0] * depth + out.x;
        const float expected_ay = a.p[1] - a.n[1] * depth + out.y;
        const float expected_az = a.p[2] - a.n[2] * depth + out.z;
        REQUIRE(std::fabs(a_prime.p[0] - expected_ax) <= 1e-4f);
        REQUIRE(std::fabs(a_prime.p[1] - expected_ay) <= 1e-4f);
        REQUIRE(std::fabs(a_prime.p[2] - expected_az) <= 1e-4f);

        const float expected_bx = b.p[0] - b.n[0] * depth + out.x;
        const float expected_by = b.p[1] - b.n[1] * depth + out.y;
        const float expected_bz = b.p[2] - b.n[2] * depth + out.z;
        REQUIRE(std::fabs(b_prime.p[0] - expected_bx) <= 1e-4f);
        REQUIRE(std::fabs(b_prime.p[1] - expected_by) <= 1e-4f);
        REQUIRE(std::fabs(b_prime.p[2] - expected_bz) <= 1e-4f);

        // Normal and Id carried over unchanged from the edge's own vertex.
        for (int c = 0; c < 3; ++c) {
            REQUIRE(a_prime.n[c] == a.n[c]);
            REQUIRE(b_prime.n[c] == b.n[c]);
        }
        for (int c = 0; c < 4; ++c) {
            REQUIRE(a_prime.rgba[c] == a.rgba[c]);
            REQUIRE(b_prime.rgba[c] == b.rgba[c]);
        }
    }

    // Exactly one skirt quad (2 triangles) per border edge: every triangle
    // beyond the pre-skirt surface's own count references at least one
    // appended (>= original_count) vertex, and there are exactly 2 per edge.
    std::size_t skirt_triangles = 0;
    for (std::size_t t = 0; t < result.mesh->indices.size(); t += 3) {
        const bool touches_skirt = result.mesh->indices[t + 0] >= original_count ||
                                    result.mesh->indices[t + 1] >= original_count ||
                                    result.mesh->indices[t + 2] >= original_count;
        if (touches_skirt) {
            ++skirt_triangles;
        }
    }
    REQUIRE(skirt_triangles == edge_count * 2);
}

TEST_CASE("RS4 a level-1 node's skirt triangles face consistently with their source edge's triangle",
          "[terrain][lod]") {
    VoxelVolume volume;
    const NodeKey level1_key{1, 2, 0, 2};
    Vec3 node_min, node_max;
    node_bounds(level1_key, 1.f, node_min, node_max);
    fill_rolling_wave(volume, (node_min.x + node_max.x) * 0.5f, (node_min.z + node_max.z) * 0.5f);
    const float voxel_size = volume.voxel_size();

    std::size_t children_triangles = 0;
    const std::vector<std::shared_ptr<const anarchy::amesh::Data>> children =
        mesh_level1_children(volume, level1_key, children_triangles);
    REQUIRE(children_triangles > 0);

    LodInput input;
    input.key = level1_key;
    input.voxel_size = voxel_size;
    input.children = children;
    input.voxels = std::make_shared<const ChunkMap>(volume.chunks());
    const LodResult result = build_node(input);
    REQUIRE(result.mesh != nullptr);

    const std::size_t edge_count = result.border_edges.size() / 2;
    REQUIRE(edge_count > 0);
    const std::size_t original_count = original_vertex_count(result);

    // For a border edge (a, b) extracted (by LodBuilder.cpp's
    // collect_border_edges) in the same forward order its one owning
    // triangle stores it in, cross(b - a, c - a) is outward for that
    // triangle's real third vertex c (Surface Nets winds every triangle
    // solid-to-air, and neither simplification nor compaction change that).
    // t = cross(b - a, (n_a + n_b)) approximates that same outward
    // direction without needing c (n is parallel to it to first order), so
    // a correctly-wound skirt triangle's own face normal should point the
    // same way as t, not away from it.
    int checked = 0;
    for (std::size_t e = 0; e < edge_count; ++e) {
        const std::uint32_t a_index = result.border_edges[e * 2 + 0];
        const std::uint32_t b_index = result.border_edges[e * 2 + 1];
        const anarchy::amesh::Vertex& a = result.mesh->vertices[a_index];
        const anarchy::amesh::Vertex& b = result.mesh->vertices[b_index];
        const anarchy::amesh::Vertex& a_prime = result.mesh->vertices[original_count + e * 2 + 0];
        const anarchy::amesh::Vertex& b_prime = result.mesh->vertices[original_count + e * 2 + 1];

        const Vec3 pa{a.p[0], a.p[1], a.p[2]};
        const Vec3 pb{b.p[0], b.p[1], b.p[2]};
        const Vec3 pap{a_prime.p[0], a_prime.p[1], a_prime.p[2]};
        const Vec3 pbp{b_prime.p[0], b_prime.p[1], b_prime.p[2]};

        const Vec3 e_dir{pb.x - pa.x, pb.y - pa.y, pb.z - pa.z};
        const Vec3 n_sum{a.n[0] + b.n[0], a.n[1] + b.n[1], a.n[2] + b.n[2]};
        const Vec3 t{e_dir.y * n_sum.z - e_dir.z * n_sum.y, e_dir.z * n_sum.x - e_dir.x * n_sum.z,
                      e_dir.x * n_sum.y - e_dir.y * n_sum.x};
        const float t_len = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
        if (!(t_len > 1e-6f)) {
            continue;  // a degenerate (near-zero) edge direction x normal sum: no reliable reference here
        }

        auto face_normal = [](Vec3 p0, Vec3 p1, Vec3 p2) {
            const Vec3 u{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
            const Vec3 v{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
            return Vec3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        };
        // The two skirt triangles LodBuilder.cpp's add_skirts appends: (a, b', b) and (a, a', b').
        const Vec3 n1 = face_normal(pa, pbp, pb);
        const Vec3 n2 = face_normal(pa, pap, pbp);

        INFO("edge " << e << ": dot(n1,t) " << (n1.x * t.x + n1.y * t.y + n1.z * t.z) << ", dot(n2,t) "
                      << (n2.x * t.x + n2.y * t.y + n2.z * t.z));
        REQUIRE(n1.x * t.x + n1.y * t.y + n1.z * t.z > 0.f);
        REQUIRE(n2.x * t.x + n2.y * t.y + n2.z * t.z > 0.f);
        ++checked;
    }
    REQUIRE(checked > 0);
}

// Task 3 fix round 1 (findings-r1.md), R10: a child's own skirts must never
// be baked into a coarser level's merge.

namespace {

// result's mesh with its skirts physically cut off: just the vertices and
// indices result.surface_index_count/border_edges say are the real surface
// (see original_vertex_count, above, for the same split).
std::shared_ptr<const anarchy::amesh::Data> strip_skirts(const LodResult& result) {
    anarchy::amesh::Data out;
    const std::size_t original_count = original_vertex_count(result);
    out.vertices.assign(result.mesh->vertices.begin(),
                         result.mesh->vertices.begin() + static_cast<std::ptrdiff_t>(original_count));
    out.indices.assign(result.mesh->indices.begin(),
                        result.mesh->indices.begin() + static_cast<std::ptrdiff_t>(result.surface_index_count));
    anarchy::amesh::compute_aabb(out);
    return std::make_shared<const anarchy::amesh::Data>(std::move(out));
}

}  // namespace

TEST_CASE("RS5 a level-2 node merges skirted level-1 children the same as the same children with skirts "
          "physically stripped",
          "[terrain][lod]") {
    VoxelVolume volume;
    const Vec3 center{64.f, 64.f, 64.f};
    const float radius = 40.f;
    REQUIRE_FALSE(volume.fill(ball_at(center.x, center.y, center.z, radius), 1));
    const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());

    const NodeKey level2_key{2, 0, 0, 0};
    std::vector<LodResult> level1_results;
    for (const NodeKey& child_key : children_of(level2_key)) {
        LodResult child = build_lod_node(volume, child_key, voxels);
        if (child.mesh) {
            REQUIRE(child.border_edges.size() > 0);  // this node's own rim must actually have skirts to strip
            level1_results.push_back(std::move(child));
        }
    }
    REQUIRE(level1_results.size() >= 2);  // the ball must straddle more than one level-1 node

    // Path A: build_node sees the children's full (skirted) meshes, and
    // relies on child_surface_index_counts (R10) to drop the skirts itself.
    LodInput skirted_input;
    skirted_input.key = level2_key;
    skirted_input.voxel_size = volume.voxel_size();
    skirted_input.voxels = voxels;

    // Path B: the children are pre-stripped of their skirts entirely (no
    // skirt vertices or triangles exist in the input at all), with no
    // child_surface_index_counts entry -- the "missing entry" default
    // ("whole mesh is surface") is exactly right since there is no skirt left.
    LodInput stripped_input;
    stripped_input.key = level2_key;
    stripped_input.voxel_size = volume.voxel_size();
    stripped_input.voxels = voxels;

    for (const LodResult& child : level1_results) {
        skirted_input.children.push_back(child.mesh);
        skirted_input.child_errors.push_back(child.error);
        skirted_input.child_surface_index_counts.push_back(child.surface_index_count);

        stripped_input.children.push_back(strip_skirts(child));
        stripped_input.child_errors.push_back(child.error);
    }

    const LodResult with_skirts = build_node(skirted_input);
    const LodResult stripped = build_node(stripped_input);

    REQUIRE(with_skirts.mesh != nullptr);
    REQUIRE(stripped.mesh != nullptr);
    INFO("with_skirts surface_index_count " << with_skirts.surface_index_count << ", stripped "
                                             << stripped.surface_index_count << "; with_skirts error "
                                             << with_skirts.error << ", stripped error " << stripped.error);
    // Same triangle budget and the same honestly-measured error: a child's
    // own skirts (now excluded from both the merge and the error
    // measurement, per R10) must not change either outcome.
    REQUIRE(with_skirts.surface_index_count == stripped.surface_index_count);
    REQUIRE(with_skirts.mesh->indices.size() == stripped.mesh->indices.size());
    REQUIRE(with_skirts.error == Approx(stripped.error).margin(1e-5f));
}

TEST_CASE("RS6 a fully-collapsed simplification yields a null mesh, same as an empty input",
          "[terrain][lod]") {
    // A tiny, isolated, fully open-bordered quad (2 triangles, 4 distinct
    // vertices a thousandth of a unit apart): meshopt_simplify (lock_border
    // == 0, as build_node always calls it -- see the comment on the
    // fill_rolling_wave helper above, found by watching exactly this happen
    // to an unwanted "floor cap" patch while chasing LB1's budget) is free
    // to collapse an open patch like this away entirely for a cost far
    // under any level's target_error, and target_index_count (0, since the
    // input's own 6 indices are below build_node's /4/3*3 floor) asks it to.
    // No two vertices are exactly equal, so weld() does not pre-collapse
    // this itself -- the test exercises meshopt's own full collapse, then
    // build_node's "0 triangles left -> null mesh" handling below it.
    anarchy::amesh::Data quad;
    quad.vertices.resize(4);
    const float positions[4][3] = {
        {5.f, 5.f, 5.f}, {5.001f, 5.f, 5.f}, {5.f, 5.001f, 5.f}, {5.001f, 5.001f, 5.f}};
    for (int i = 0; i < 4; ++i) {
        quad.vertices[static_cast<std::size_t>(i)].p[0] = positions[i][0];
        quad.vertices[static_cast<std::size_t>(i)].p[1] = positions[i][1];
        quad.vertices[static_cast<std::size_t>(i)].p[2] = positions[i][2];
        quad.vertices[static_cast<std::size_t>(i)].n[2] = 1.f;
    }
    quad.indices = {0, 1, 2, 1, 3, 2};
    anarchy::amesh::compute_aabb(quad);

    LodInput input;
    input.key = NodeKey{1, 0, 0, 0};
    input.voxel_size = 1.f;
    input.children.push_back(std::make_shared<const anarchy::amesh::Data>(std::move(quad)));

    const LodResult result = build_node(input);

    REQUIRE(result.mesh == nullptr);
    REQUIRE(result.border_edges.empty());
    REQUIRE(result.surface_index_count == 0);
}

// Task 7 crack fix (R21): seams between LOD nodes.

namespace {

// key's node from compact level-1 children, the way LodTree builds it: each
// level-1 child is built from its exact chunk meshes, packed to the union of
// its node box and its mesh's AABB (LodTree's covering_bounds, R2), and
// unpacked again for the parent's build (R12).
LodResult build_from_compact_children(VoxelVolume& volume, const NodeKey& key, const std::shared_ptr<const ChunkMap>& voxels) {
    LodInput input;
    input.key = key;
    input.voxel_size = volume.voxel_size();
    input.voxels = voxels;
    for (const NodeKey& child_key : children_of(key)) {
        const LodResult child = build_lod_node(volume, child_key, voxels);
        if (!child.mesh) {
            continue;
        }
        Vec3 box_min, box_max;
        node_bounds(child_key, volume.voxel_size(), box_min, box_max);
        box_min = Vec3{std::min(box_min.x, child.mesh->bbox_min[0]), std::min(box_min.y, child.mesh->bbox_min[1]),
                       std::min(box_min.z, child.mesh->bbox_min[2])};
        box_max = Vec3{std::max(box_max.x, child.mesh->bbox_max[0]), std::max(box_max.y, child.mesh->bbox_max[1]),
                       std::max(box_max.z, child.mesh->bbox_max[2])};
        const CompactMesh compact = pack(*child.mesh, box_min, box_max, child.surface_index_count);
        input.children.push_back(std::make_shared<const anarchy::amesh::Data>(unpack(compact)));
        input.child_errors.push_back(child.error);
        input.child_surface_index_counts.push_back(compact.surface_index_count);
    }
    return build_node(input);
}

// The border edges of result (its surface, before skirts) whose midpoint
// lies farther than tolerance from every face of key's node box: open
// seams inside the node.
std::size_t interior_border_edges(const LodResult& result, const NodeKey& key, float voxel_size, float tolerance) {
    Vec3 box_min, box_max;
    node_bounds(key, voxel_size, box_min, box_max);
    std::size_t interior = 0;
    for (std::size_t e = 0; e + 1 < result.border_edges.size(); e += 2) {
        const Vec3 a = mesh_vertex_position(*result.mesh, result.border_edges[e]);
        const Vec3 b = mesh_vertex_position(*result.mesh, result.border_edges[e + 1]);
        const Vec3 m{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
        const float to_face = std::min({m.x - box_min.x, box_max.x - m.x, m.y - box_min.y, box_max.y - m.y,
                                        m.z - box_min.z, box_max.z - m.z});
        if (to_face > tolerance) {
            ++interior;
        }
    }
    return interior;
}

}  // namespace

TEST_CASE("LB5 a level-2 node built from compact level-1 siblings has no open seam inside it", "[terrain][lod]") {
    VoxelVolume volume;
    fill_rolling_slab(volume);
    const float voxel_size = volume.voxel_size();
    const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());

    std::size_t total_interior = 0, total_border = 0;
    for (const NodeKey& key : {NodeKey{2, 1, 0, 1}, NodeKey{2, 2, 0, 1}, NodeKey{2, 1, 0, 2}}) {
        const LodResult result = build_from_compact_children(volume, key, voxels);
        REQUIRE(result.mesh != nullptr);
        const std::size_t interior = interior_border_edges(result, key, voxel_size, 2.f * voxel_size);
        WARN("LB5 L2(" << key.x << "," << key.y << "," << key.z << "): " << result.border_edges.size() / 2
                       << " border edges, " << interior << " inside the node");
        total_interior += interior;
        total_border += result.border_edges.size() / 2;
    }
    REQUIRE(total_border > 0);   // the node's own rim is a border
    REQUIRE(total_interior == 0);
}

namespace {

// result's mesh packed to its covering box and unpacked again, as a node is
// drawn (R2, R12).
anarchy::amesh::Data compact_round_trip(const LodResult& result, const NodeKey& key, float voxel_size) {
    Vec3 box_min, box_max;
    node_bounds(key, voxel_size, box_min, box_max);
    box_min = Vec3{std::min(box_min.x, result.mesh->bbox_min[0]), std::min(box_min.y, result.mesh->bbox_min[1]),
                   std::min(box_min.z, result.mesh->bbox_min[2])};
    box_max = Vec3{std::max(box_max.x, result.mesh->bbox_max[0]), std::max(box_max.y, result.mesh->bbox_max[1]),
                   std::max(box_max.z, result.mesh->bbox_max[2])};
    return unpack(pack(*result.mesh, box_min, box_max, result.surface_index_count));
}

// Double-sided Moller-Trumbore: does the ray origin + s * direction (s > 0)
// cross triangle (a, b, c)?
bool ray_hits_triangle(Vec3 origin, Vec3 direction, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 p{direction.y * e2.z - direction.z * e2.y, direction.z * e2.x - direction.x * e2.z,
                 direction.x * e2.y - direction.y * e2.x};
    const float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    const float inv = 1.f / det;
    const Vec3 s{origin.x - a.x, origin.y - a.y, origin.z - a.z};
    const float u = (s.x * p.x + s.y * p.y + s.z * p.z) * inv;
    if (u < 0.f || u > 1.f) {
        return false;
    }
    const Vec3 q{s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
    const float v = (direction.x * q.x + direction.y * q.y + direction.z * q.z) * inv;
    if (v < 0.f || u + v > 1.f) {
        return false;
    }
    return (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inv > 0.f;
}


// Diagonal ridges, steep enough (slope up to about 0.85) that a border's
// Surface Nets vertices wander across the node face rather than lining up on
// one plane, so two simplifications of it leave a gap seen from above.
void fill_steep_ridges(VoxelVolume& volume) {
    const int x0 = 0, x1 = 319, z0 = 64, z1 = 319;
    const int y0 = -8, y1 = kChunkSize - 1;
    const int width = x1 - x0 + 1, height = y1 - y0 + 1, depth = z1 - z0 + 1;
    std::vector<float> distances(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                  static_cast<std::size_t>(depth));
    std::vector<std::uint8_t> materials(distances.size(), 1);
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float surface_y = 16.f + 9.f * std::sin(static_cast<float>(x + z) / 15.f);
            const float slope = std::sqrt(1.f + 2.f * std::pow(9.f / 15.f * std::cos(static_cast<float>(x + z) / 15.f), 2.f));
            for (int y = y0; y <= y1; ++y) {
                const std::size_t i = static_cast<std::size_t>((x - x0) + width * ((y - y0) + height * (z - z0)));
                distances[i] = (static_cast<float>(y) - surface_y) / slope;
            }
        }
    }
    REQUIRE_FALSE(volume.write(CellCoord{x0, y0, z0}, CellCoord{x1, y1, z1}, distances, materials));
}

}  // namespace

TEST_CASE("RS8 skirts cover the gap between neighbor nodes at different levels, seen head-on",
          "[terrain][lod]") {
    // A level-1 node (x 64..128) beside a level-2 node (x 128..256) on steep
    // ridges: each simplified the shared border at x = 128 its own way, so
    // their border polylines differ and there is a sliver between them
    // unless a skirt fills it. Rays cast down the surface normal (the head-on
    // view, where a skirt hanging along -normal is seen edge-on) across the
    // shared border must all hit one node or the other.
    VoxelVolume volume;
    fill_steep_ridges(volume);
    const float voxel_size = volume.voxel_size();
    const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());
    const VoxelSampler sampler(*voxels, voxel_size);

    const NodeKey fine_key{1, 1, 0, 2};    // chunks x 2..3, z 4..5
    const NodeKey coarse_key{2, 1, 0, 1};  // chunks x 4..7, z 4..7
    const LodResult fine = build_lod_node(volume, fine_key, voxels);
    const LodResult coarse = build_from_compact_children(volume, coarse_key, voxels);
    REQUIRE(fine.mesh != nullptr);
    REQUIRE(coarse.mesh != nullptr);

    const float seam_x = 128.f, strip = 8.f;
    struct Triangle {
        Vec3 a, b, c;
    };
    std::vector<Triangle> near_seam;
    for (const anarchy::amesh::Data& mesh : {compact_round_trip(fine, fine_key, voxel_size),
                                             compact_round_trip(coarse, coarse_key, voxel_size)}) {
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const Triangle tri{mesh_vertex_position(mesh, mesh.indices[t]), mesh_vertex_position(mesh, mesh.indices[t + 1]),
                               mesh_vertex_position(mesh, mesh.indices[t + 2])};
            if (std::max({tri.a.x, tri.b.x, tri.c.x}) >= seam_x - strip &&
                std::min({tri.a.x, tri.b.x, tri.c.x}) <= seam_x + strip) {
                near_seam.push_back(tri);
            }
        }
    }
    REQUIRE_FALSE(near_seam.empty());

    int rays = 0, misses = 0;
    for (float z = 129.f; z <= 191.f; z += 0.25f) {
        const Vec3 seam{seam_x, 16.f + 9.f * std::sin((seam_x + z) / 15.f), z};
        const Vec3 n = sampler.gradient(seam);
        // Across the seam, perpendicular to the view: x with its n part removed.
        Vec3 across{1.f - n.x * n.x, -n.x * n.y, -n.x * n.z};
        const float across_length = std::sqrt(across.x * across.x + across.y * across.y + across.z * across.z);
        across = Vec3{across.x / across_length, across.y / across_length, across.z / across_length};
        for (float s = -1.5f; s <= 1.5f; s += 0.01f) {
            ++rays;
            const Vec3 origin{seam.x + across.x * s + n.x * 4.f, seam.y + across.y * s + n.y * 4.f,
                              seam.z + across.z * s + n.z * 4.f};
            const Vec3 direction{-n.x, -n.y, -n.z};
            bool hit = false;
            for (const Triangle& tri : near_seam) {
                if (ray_hits_triangle(origin, direction, tri.a, tri.b, tri.c)) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                if (misses < 5) {
                    WARN("RS8 ray from (" << origin.x << ", " << origin.y << ", " << origin.z << ") along -normal ("
                                          << direction.x << ", " << direction.y << ", " << direction.z
                                          << ") misses both nodes");
                }
                ++misses;
            }
        }
    }
    WARN("RS8: " << misses << " of " << rays << " rays miss");
    REQUIRE(misses == 0);
}

// Fix round 1 (finding 1): stitch_seams' cost is bounded by edge length.

namespace {

// Writes field(x, y, z) (a signed distance, in units) over the cells
// lo..hi, material 1.
template <typename Field>
void fill_field(VoxelVolume& volume, CellCoord lo, CellCoord hi, const Field& field) {
    const int width = hi.x - lo.x + 1, height = hi.y - lo.y + 1, depth = hi.z - lo.z + 1;
    std::vector<float> distances(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                  static_cast<std::size_t>(depth));
    std::vector<std::uint8_t> materials(distances.size(), 1);
    for (int z = lo.z; z <= hi.z; ++z) {
        for (int y = lo.y; y <= hi.y; ++y) {
            for (int x = lo.x; x <= hi.x; ++x) {
                const std::size_t i = static_cast<std::size_t>((x - lo.x) + width * ((y - lo.y) + height * (z - lo.z)));
                distances[i] = field(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            }
        }
    }
    REQUIRE_FALSE(volume.write(lo, hi, distances, materials));
}

// The signed distance to a box of half extents half, centered at center,
// turned by yaw about y and then pitch about x (radians).
float rotated_box_distance(Vec3 p, Vec3 center, Vec3 half, float yaw, float pitch) {
    const Vec3 d{p.x - center.x, p.y - center.y, p.z - center.z};
    // Into the box's frame: undo pitch (about x), then yaw (about y).
    const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    const Vec3 u{d.x, cp * d.y + sp * d.z, -sp * d.y + cp * d.z};
    const Vec3 q{cy * u.x - sy * u.z, u.y, sy * u.x + cy * u.z};
    const Vec3 e{std::fabs(q.x) - half.x, std::fabs(q.y) - half.y, std::fabs(q.z) - half.z};
    const Vec3 outside{std::max(e.x, 0.f), std::max(e.y, 0.f), std::max(e.z, 0.f)};
    return std::sqrt(outside.x * outside.x + outside.y * outside.y + outside.z * outside.z) +
           std::min(std::max({e.x, e.y, e.z}), 0.f);
}

// key's level-2 build from compact level-1 children (built first, untimed),
// and how long build_node alone took, in seconds.
LodResult timed_level2_build(VoxelVolume& volume, const NodeKey& key, double& seconds) {
    const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());
    LodInput input;
    input.key = key;
    input.voxel_size = volume.voxel_size();
    input.voxels = voxels;
    for (const NodeKey& child_key : children_of(key)) {
        const LodResult child = build_lod_node(volume, child_key, voxels);
        if (!child.mesh) {
            continue;
        }
        const anarchy::amesh::Data round_tripped = compact_round_trip(child, child_key, volume.voxel_size());
        input.children.push_back(std::make_shared<const anarchy::amesh::Data>(round_tripped));
        input.child_errors.push_back(child.error);
        input.child_surface_index_counts.push_back(child.surface_index_count);
    }
    REQUIRE(input.children.size() >= 2);
    const auto start = std::chrono::steady_clock::now();
    LodResult result = build_node(input);
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

}  // namespace

TEST_CASE("LB6 a level-2 node of a planar 45-degree slope or a turned block builds within a per-node budget",
          "[terrain][lod]") {
    // Planar surfaces simplify to long edges, and their children's errors
    // are near 0, so the seam stitch's tolerance is tiny (about 0.004 units,
    // the weld step): a search whose cells were that size scanned every cell
    // of a long diagonal edge's box. Budget per level-2 build: 0.5 s in
    // Release (about 0.06 s measured; the old search took 0.1 s on the flat
    // floor), 5 s in Debug, which is about 20x slower and noisy under load.
    // The error measurement's grid was fixed too: its
    // cells, sized from the box's volume and clamped to 128 per axis, covered
    // a tenth of a flat mesh (2.4 s of Debug time on the floor).
#ifdef NDEBUG
    constexpr double kBudgetSeconds = 0.5;
#else
    constexpr double kBudgetSeconds = 5.0;
#endif
    const NodeKey key{2, 0, 0, 0};   // chunks 0..3 on each axis: 128 units

    SECTION("a 45-degree plane rising along x and z") {
        VoxelVolume volume;
        fill_field(volume, CellCoord{-8, -8, -8}, CellCoord{135, 135, 135}, [](float x, float y, float z) {
            return (y - 0.5f * (x + z) - 2.f) / std::sqrt(1.5f);
        });
        double seconds = 0.0;
        const LodResult result = timed_level2_build(volume, key, seconds);
        REQUIRE(result.mesh != nullptr);
        WARN("LB6 plane: build_node took " << seconds << " s, " << result.mesh->indices.size() / 3
                                           << " triangles, error " << result.error);
        REQUIRE(seconds < kBudgetSeconds);
        WARN("LB6 open seam edges inside the node: " << interior_border_edges(result, key, volume.voxel_size(), 2.f * volume.voxel_size()));
    }
    SECTION("a flat floor") {
        VoxelVolume volume;
        fill_field(volume, CellCoord{-8, -8, -8}, CellCoord{135, 40, 135},
                   [](float, float y, float) { return y - 16.5f; });
        double seconds = 0.0;
        const LodResult result = timed_level2_build(volume, key, seconds);
        REQUIRE(result.mesh != nullptr);
        WARN("LB6 floor: build_node took " << seconds << " s, " << result.mesh->indices.size() / 3
                                           << " triangles, error " << result.error);
        REQUIRE(seconds < kBudgetSeconds);
        WARN("LB6 open seam edges inside the node: " << interior_border_edges(result, key, volume.voxel_size(), 2.f * volume.voxel_size()));
    }
    SECTION("a block turned 30 degrees about y and 20 about x") {
        VoxelVolume volume;
        fill_field(volume, CellCoord{-8, -8, -8}, CellCoord{135, 135, 135}, [](float x, float y, float z) {
            return rotated_box_distance(Vec3{x, y, z}, Vec3{61.f, 59.f, 66.f}, Vec3{38.f, 30.f, 34.f}, 0.5236f, 0.3491f);
        });
        double seconds = 0.0;
        const LodResult result = timed_level2_build(volume, key, seconds);
        REQUIRE(result.mesh != nullptr);
        WARN("LB6 block: build_node took " << seconds << " s, " << result.mesh->indices.size() / 3
                                           << " triangles, error " << result.error);
        REQUIRE(seconds < kBudgetSeconds);
        WARN("LB6 open seam edges inside the node: " << interior_border_edges(result, key, volume.voxel_size(), 2.f * volume.voxel_size()));
    }
}

// Fix round 1 (finding 5): the seam stitch never folds a thin feature.

namespace {

// key's node built bottom-up the way LodTree builds it (each level >= 1
// child packed to its covering box and unpacked for its parent, R2/R12),
// skipping children whose box misses lo..hi (where the field has surface).
// max_child_error gets the largest error among key's own children.
LodResult build_compact_tree(VoxelVolume& volume, const NodeKey& key, const std::shared_ptr<const ChunkMap>& voxels,
                             Vec3 lo, Vec3 hi, float* max_child_error = nullptr) {
    if (key.level == 0) {
        LodResult result;
        result.key = key;
        result.mesh = surface_nets(mesh_input(volume, ChunkCoord{key.x, key.y, key.z})).render;
        if (result.mesh) result.surface_index_count = static_cast<std::uint32_t>(result.mesh->indices.size());
        return result;
    }
    LodInput input;
    input.key = key;
    input.voxel_size = volume.voxel_size();
    input.voxels = voxels;
    for (const NodeKey& child_key : children_of(key)) {
        Vec3 box_min, box_max;
        node_bounds(child_key, volume.voxel_size(), box_min, box_max);
        if (box_max.x < lo.x || box_min.x > hi.x || box_max.y < lo.y || box_min.y > hi.y || box_max.z < lo.z ||
            box_min.z > hi.z) {
            continue;
        }
        const LodResult child = build_compact_tree(volume, child_key, voxels, lo, hi);
        if (!child.mesh) continue;
        if (child_key.level == 0) {
            input.children.push_back(child.mesh);
            input.child_errors.push_back(0.f);
            input.child_surface_index_counts.push_back(child.surface_index_count);
        } else {
            input.children.push_back(
                std::make_shared<const anarchy::amesh::Data>(compact_round_trip(child, child_key, volume.voxel_size())));
            input.child_errors.push_back(child.error);
            input.child_surface_index_counts.push_back(child.surface_index_count);
        }
        if (max_child_error != nullptr) *max_child_error = std::max(*max_child_error, child.error);
    }
    return build_node(input);
}

// The surface triangles of result (before its skirts) whose face normal
// opposes the field's gradient at the triangle's centroid: their cosine is
// under -0.5. A triangle folded across a feature is turned over (cosines of
// -0.93 to -0.98 were seen); one lying along a narrow tube's curve can be
// near perpendicular to the gradient at its centroid, cosine about 0 either way.
int faces_against_gradient(const LodResult& result, const VoxelSampler& sampler, int& total) {
    const anarchy::amesh::Data& mesh = *result.mesh;
    int against = 0;
    total = 0;
    for (std::size_t t = 0; t + 2 < result.surface_index_count; t += 3) {
        const Vec3 a = mesh_vertex_position(mesh, mesh.indices[t]);
        const Vec3 b = mesh_vertex_position(mesh, mesh.indices[t + 1]);
        const Vec3 c = mesh_vertex_position(mesh, mesh.indices[t + 2]);
        const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z}, ac{c.x - a.x, c.y - a.y, c.z - a.z};
        const Vec3 n{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
        const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (!(length > 1e-6f)) continue;
        ++total;
        const Vec3 g = sampler.gradient(Vec3{(a.x + b.x + c.x) / 3.f, (a.y + b.y + c.y) / 3.f, (a.z + b.z + c.z) / 3.f});
        const float cosine = (n.x * g.x + n.y * g.y + n.z * g.z) / length;
        if (cosine < -0.5f) {
            if (against < 5) {
                WARN("face against the gradient (cosine " << cosine << "): (" << a.x << ", " << a.y << ", " << a.z
                                                          << ") (" << b.x << ", " << b.y << ", " << b.z << ") (" << c.x
                                                          << ", " << c.y << ", " << c.z << ")");
            }
            ++against;
        }
    }
    return against;
}

}  // namespace

TEST_CASE("LB7 a thin wall and a cave crossing level-3 and level-4 seams stitch without folds", "[terrain][lod]") {
    // The stitch inserts another child's border vertex into an edge when it
    // lies within 2 x the children's error of it. A wall or cave thinner
    // than that has its other side's border vertices within reach: inserted,
    // one would fold a triangle across the feature. Every face of the built
    // node must still point the way the field's gradient does.
    VoxelVolume volume;
    SECTION("a bumpy wall 2.5 units thick, across x = 256 (between level-3 nodes) inside level-4 (0, 0, 0)") {
        fill_field(volume, CellCoord{150, 8, 80}, CellCoord{362, 90, 124}, [](float x, float y, float z) {
            const float middle = 100.f + 3.f * std::sin(x / 11.f);
            const float half = 1.25f + 0.6f * std::sin(x / 5.f) * std::cos(y / 4.f);
            const float side = std::fabs(z - middle) - half;
            const float ends = std::max({158.f - x, x - 354.f, 16.f - y, y - 82.f});
            return std::max(side, ends);
        });
        const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());
        float child_error = 0.f;
        const LodResult result =
            build_compact_tree(volume, NodeKey{4, 0, 0, 0}, voxels, Vec3{150, 8, 80}, Vec3{362, 90, 124}, &child_error);
        REQUIRE(result.mesh != nullptr);
        int total = 0;
        const int against = faces_against_gradient(result, VoxelSampler(*voxels, volume.voxel_size()), total);
        WARN("LB7 wall: level-4 error " << result.error << ", children's largest " << child_error << ", " << against
                                        << " of " << total << " faces against the gradient");
        REQUIRE(against == 0);
    }
    SECTION("a closed cave of radius 2 inside a block, across x = 256 inside level-4 (0, 0, 0)") {
        fill_field(volume, CellCoord{150, 8, 150}, CellCoord{362, 70, 230}, [](float x, float y, float z) {
            const float block = std::max({158.f - x, x - 354.f, 16.f - y, y - 62.f, 158.f - z, z - 222.f});
            const float cy = 40.f + 4.f * std::sin(x / 13.f), cz = 190.f + 3.f * std::cos(x / 9.f);
            const float past = std::max({0.f, 180.f - x, x - 332.f});   // closed ends, inside the block
            const float tunnel = 2.f - std::sqrt((y - cy) * (y - cy) + (z - cz) * (z - cz) + past * past);
            return std::max(block, tunnel);
        });
        const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());
        float child_error = 0.f;
        const LodResult result = build_compact_tree(volume, NodeKey{4, 0, 0, 0}, voxels, Vec3{150, 8, 150},
                                                    Vec3{362, 70, 230}, &child_error);
        REQUIRE(result.mesh != nullptr);
        int total = 0;
        const int against = faces_against_gradient(result, VoxelSampler(*voxels, volume.voxel_size()), total);
        WARN("LB7 cave: level-4 error " << result.error << ", children's largest " << child_error << ", " << against
                                        << " of " << total << " faces against the gradient");
        REQUIRE(against == 0);
    }
}

// Task 10 of the terrain LOD plan: large-island budgets for the whole
// TerrainWorld loop (chunks, LodTree levels, residency ordering) rather than
// one node's build. [.][terrain-bench]: opt-in, Release only.
namespace {

// A rolling island over n x n chunks (one y layer), its walls and floor kept
// 4 cells inside the outer chunks so every one of the n x n columns has
// surface (fill_island's own shape, scaled up), with two materials banded
// across x. Written one z-row of chunks (one VoxelVolume::write call) at a
// time so no call passes VoxelVolume::kMaxCellsPerEdit.
std::optional<std::string> fill_big_island(VoxelVolume& volume, int n) {
    const int x1 = kChunkSize * n - 1, z1 = x1, y1 = kChunkSize - 1;
    const int lo = 4, hi = kChunkSize * n - 5;
    const int width = x1 + 1, height = y1 + 1;
    std::vector<float> distances(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                                  static_cast<std::size_t>(kChunkSize));
    std::vector<std::uint8_t> materials(distances.size());
    for (int cz = 0; cz < n; ++cz) {
        const int z0 = cz * kChunkSize;
        for (int dz = 0; dz < kChunkSize; ++dz) {
            const int z = z0 + dz;
            for (int x = 0; x <= x1; ++x) {
                const float surface = 16.f + 2.f * std::sin(static_cast<float>(x) / 40.f) *
                                                  std::cos(static_cast<float>(z) / 50.f);
                const auto material = static_cast<std::uint8_t>(1 + (x / 128) % 2);   // a couple of materials
                for (int y = 0; y <= y1; ++y) {
                    const float d = std::max({static_cast<float>(y) - surface, static_cast<float>(lo - y),
                                              static_cast<float>(lo - x), static_cast<float>(x - hi),
                                              static_cast<float>(lo - z), static_cast<float>(z - hi)});
                    const std::size_t i = static_cast<std::size_t>(x + width * (y + height * dz));
                    distances[i] = d;
                    materials[i] = material;
                }
            }
        }
        std::optional<std::string> error =
            volume.write(CellCoord{0, 0, z0}, CellCoord{x1, y1, z0 + kChunkSize - 1}, distances, materials);
        if (error) return error;
    }
    return std::nullopt;
}

Terrain& big_island_terrain(Game& game, int n) {
    auto& t = game.create<Terrain>();
    game.set_parent(t.id(), workspace_of(game));
    REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return fill_big_island(v, n); }));
    return t;
}

Camera& bench_camera_at(DataModel& game, Vec3 at) {
    Camera& camera = game.create<Camera>();
    camera.set_transform(matrix4_translation(at.x, at.y, at.z));
    game.set_parent(camera.id(), workspace_of(game));
    auto* workspace = dynamic_cast<Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->set_current_camera(camera.id()));
    return camera;
}

// 1 (level 0) + 1 per coarser level, as LT1 counts per level, down to the
// single top-level root: n^2 + (n/2)^2 + (n/4)^2 + ... + 1.
std::size_t expected_node_total(int n) {
    std::size_t total = 0;
    for (int level = 0; (n >> level) >= 1; ++level) {
        const std::size_t count = static_cast<std::size_t>(n >> level);
        total += count * count;
    }
    return total;
}

// Every node exists, level 0 has no chunk job in flight, and levels >= 1 are
// built and resident: nothing left for next_builds or a chunk job to do.
bool lod_tree_settled(const terrain::LodTree& tree, std::size_t expected_nodes) {
    const auto& nodes = tree.nodes();
    if (nodes.size() != expected_nodes) return false;
    for (const auto& [key, node] : nodes) {
        if (key.level == 0) {
            if (node.in_flight) return false;
        } else if (node.stale() || !node.resident) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("LB8 building every level of a 4,096-chunk island settles under 5 s with 4 workers",
          "[.][terrain-bench]") {
    SimRole role;
    Game game;
    constexpr int n = 64;   // 64 x 64 chunks, one y layer: 4,096 chunks total
    Terrain& t = big_island_terrain(game, n);
    TerrainWorld world({}, 4);   // no collider builder: this budget is about LOD levels, not colliders
    const std::size_t expected_nodes = expected_node_total(n);

    const auto start = std::chrono::steady_clock::now();
    const terrain::LodTree* tree = nullptr;
    bool settled = false;
    double meshed_seconds = -1.0;   // profiling: when every chunk (level 0) finished meshing
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
        world.update(game);
        tree = world.lod_tree(t.id());
        if (meshed_seconds < 0.0 && !world.views().empty() &&
            world.views()[0].chunks->size() == static_cast<std::size_t>(n * n)) {
            meshed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        if (tree != nullptr && lod_tree_settled(*tree, expected_nodes)) {
            settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(world.views().size() == 1u);
    const std::size_t surface_chunks = world.views()[0].chunks->size();
    INFO("nodes built: " << (tree != nullptr ? tree->nodes().size() : 0u) << " of " << expected_nodes);
    WARN("LB8: " << n * n << "-chunk island (" << surface_chunks << " with surface, " << expected_nodes
                 << " LOD nodes) settled in " << seconds << " s with 4 workers (level 0 chunks alone meshed in "
                 << meshed_seconds << " s)");
    REQUIRE(settled);
    REQUIRE(surface_chunks == static_cast<std::size_t>(n * n));
    REQUIRE(seconds < 5.0);
}

TEST_CASE("LB9 levels >= 2 of a 4,096-chunk island stay within the compact RAM budget", "[.][terrain-bench]") {
    SimRole role;
    Game game;
    constexpr int n = 64;   // same island as LB8
    Terrain& t = big_island_terrain(game, n);
    TerrainWorld world({}, 4);
    const std::size_t expected_nodes = expected_node_total(n);

    const auto start = std::chrono::steady_clock::now();
    const terrain::LodTree* tree = nullptr;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
        world.update(game);
        tree = world.lod_tree(t.id());
        if (tree != nullptr && lod_tree_settled(*tree, expected_nodes)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(tree != nullptr);
    REQUIRE(lod_tree_settled(*tree, expected_nodes));

    // R1, as LT6 computes it: 16 B/vertex (6+2+4+4) + 6 B/triangle (12 with indices32).
    std::size_t bytes = 0, vertices = 0, triangles = 0, nodes = 0;
    for (const auto& [key, node] : tree->nodes()) {
        if (key.level < 2) continue;
        REQUIRE(node.resident);
        REQUIRE(node.compact != nullptr);
        const CompactMesh& mesh = *node.compact;
        const std::size_t v = mesh.positions.size() / 3;
        const std::size_t tris = (mesh.indices.size() + mesh.indices32.size()) / 3;
        const std::size_t held = mesh.positions.size() * sizeof(std::uint16_t) + mesh.normals.size() +
                                 mesh.ids.size() + mesh.weights.size() +
                                 mesh.indices.size() * sizeof(std::uint16_t) +
                                 mesh.indices32.size() * sizeof(std::uint32_t);
        REQUIRE(mesh.bytes() == held);
        bytes += held;
        vertices += v;
        triangles += tris;
        ++nodes;
    }
    std::size_t budget = 16 * vertices;
    for (const auto& [key, node] : tree->nodes()) {
        if (key.level < 2) continue;
        const std::size_t tris = (node.compact->indices.size() + node.compact->indices32.size()) / 3;
        budget += (node.compact->indices32.empty() ? 6 : 12) * tris;
    }
    WARN("LB9 levels >= 2 of the " << n * n << "-chunk island: " << nodes << " nodes, " << vertices
                                   << " vertices, " << triangles << " triangles, " << bytes << " bytes (budget "
                                   << budget << " B)");
    REQUIRE(bytes > 0u);
    REQUIRE(bytes <= budget);
}

TEST_CASE("LB10 a 16,384-chunk (2 km) island's first view appears progressively: near chunks publish within 1 s",
          "[.][terrain-bench]") {
    SimRole role;
    Game game;
    constexpr int n = 128;   // 128 x 128 chunks, one y layer: 16,384 chunks total
    Terrain& t = big_island_terrain(game, n);
    const float span = static_cast<float>(kChunkSize) * static_cast<float>(t.volume().voxel_size());
    const int center = n / 2;
    // A camera near the island's middle, placed before the first update so
    // queue_dirty's first-sight jobs are already ordered by distance to it.
    bench_camera_at(game, Vec3{(static_cast<float>(center) + 0.5f) * span, 20.f,
                               (static_cast<float>(center) + 0.5f) * span});

    std::vector<ChunkCoord> near_chunks;
    for (int dz = -kNearChunks; dz <= kNearChunks; ++dz) {
        for (int dx = -kNearChunks; dx <= kNearChunks; ++dx) {
            near_chunks.push_back(ChunkCoord{center + dx, 0, center + dz});
        }
    }

    TerrainWorld world({}, 4);
    const auto start = std::chrono::steady_clock::now();
    bool all_near_published = false;
    double seconds = 0.0;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        world.update(game);
        if (!world.views().empty()) {
            std::unordered_set<ChunkCoord, ChunkCoordHash> published;
            for (const TerrainChunkView& chunk : *world.views()[0].chunks) published.insert(chunk.coord);
            all_near_published = std::all_of(near_chunks.begin(), near_chunks.end(),
                                             [&](const ChunkCoord& c) { return published.count(c) != 0; });
        }
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (all_near_published) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    WARN("LB10: " << near_chunks.size() << " near chunks (kNearChunks = " << kNearChunks << ") of the " << n * n
                  << "-chunk island published in " << seconds << " s");
    REQUIRE(all_near_published);
    REQUIRE(seconds < 1.0);
}

TEST_CASE("LB11 one edit's updates on a settled 4,096-chunk island's tree stay cheap", "[.][terrain-bench]") {
    // Final review: every empty neighbor chunk an edit queues lands with no
    // surface (chunk_removed), and each removal used to rescan every node for
    // the level-0 extent -- a hitch per edit that grows with the island.
    SimRole role;
    Game game;
    constexpr int n = 64;   // LB8's island
    Terrain& t = big_island_terrain(game, n);
    TerrainWorld world({}, 4);
    const std::size_t expected_nodes = expected_node_total(n);
    const auto start = std::chrono::steady_clock::now();
    const terrain::LodTree* tree = nullptr;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
        world.update(game);
        tree = world.lod_tree(t.id());
        if (tree != nullptr && lod_tree_settled(*tree, expected_nodes)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(tree != nullptr);
    REQUIRE(lod_tree_settled(*tree, expected_nodes));

    // Ten small digs at the surface, spread over the island. Timed: the
    // update that queues the edit's chunks plus the one that applies their
    // results (workers waited on in between, not timed).
    constexpr int kEdits = 10;
    double total_ms = 0.0, worst_ms = 0.0;
    for (int i = 0; i < kEdits; ++i) {
        Shape ball;
        ball.center = Vec3{(5.5f + 5.f * static_cast<float>(i)) * kChunkSize, 16.f,
                           (7.5f + 5.f * static_cast<float>(i)) * kChunkSize};
        ball.radius = 3.f;
        REQUIRE_FALSE(t.edit_volume([&](VoxelVolume& v) { return v.subtract(ball); }));
        const auto queue_start = std::chrono::steady_clock::now();
        world.update(game);
        const double queue_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - queue_start).count();
        world.wait_idle();
        const auto apply_start = std::chrono::steady_clock::now();
        world.update(game);
        const double apply_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - apply_start).count();
        const double ms = queue_ms + apply_ms;
        total_ms += ms;
        worst_ms = std::max(worst_ms, ms);
        // Let the edit's node rebuilds land before the next one.
        for (int j = 0; j < 4; ++j) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            world.update(game);
            world.wait_idle();
        }
    }
    WARN("LB11: one edit's queue + apply updates on the " << n * n << "-chunk island's settled tree: "
                                                           << total_ms / kEdits << " ms on average, " << worst_ms
                                                           << " ms at worst");
    // Most of what is left is republishing the whole node set (nodes_for_view,
    // 5,461 nodes) once per update the tree changed in; LB12 times the
    // tree's own share.
    REQUIRE(total_ms / kEdits < 12.0);
}

TEST_CASE("LV1 a node built from only the chunks around it matches one built from the whole map", "[terrain][lod]") {
    // A node job reads voxels only to re-shade its own vertices: the chunks
    // in its box and one around it are all it needs, so a brush stroke need
    // not copy a huge Terrain's whole chunk map for every rebuild.
    VoxelVolume volume(1.f);
    fill_rolling_slab(volume);
    const auto full = std::make_shared<const ChunkMap>(volume.chunks());
    for (const NodeKey& key : {NodeKey{1, 1, 0, 1}, NodeKey{2, 1, 0, 1}, NodeKey{3, 0, 0, 0}}) {
        INFO("level " << key.level << " (" << key.x << ", " << key.y << ", " << key.z << ")");
        LodInput input;
        input.key = key;
        input.voxel_size = volume.voxel_size();
        for (const NodeKey& child_key : children_of(key)) {
            const LodResult child = build_lod_node(volume, child_key, full);
            if (child.mesh) {
                input.children.push_back(child.mesh);
                input.child_errors.push_back(child.error);
                input.child_surface_index_counts.push_back(child_key.level == 0
                                                               ? static_cast<std::uint32_t>(child.mesh->indices.size())
                                                               : child.surface_index_count);
            }
        }
        REQUIRE_FALSE(input.children.empty());
        input.voxels = full;
        const LodResult whole = build_node(input);
        const std::shared_ptr<const ChunkMap> near = node_voxels(volume.chunks(), key);
        REQUIRE(near->size() < full->size());
        input.voxels = near;
        const LodResult part = build_node(input);
        REQUIRE(whole.mesh != nullptr);
        REQUIRE(part.mesh != nullptr);
        REQUIRE(part.mesh->indices == whole.mesh->indices);
        REQUIRE(part.mesh->vertices.size() == whole.mesh->vertices.size());
        for (std::size_t v = 0; v < whole.mesh->vertices.size(); ++v) {
            REQUIRE(std::memcmp(&part.mesh->vertices[v], &whole.mesh->vertices[v], sizeof(whole.mesh->vertices[v])) == 0);
        }
    }
}
