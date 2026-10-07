// Terrain LOD node keys and the compact node mesh (Task 1 of the terrain
// LOD plan): node_of/parent_of/children_of, node_bounds, and pack()/unpack()
// of a CompactMesh against a Surface Nets ball.

#include "amesh.hpp"
#include "terrain/LodBuilder.hpp"
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
#include <limits>
#include <memory>
#include <unordered_map>
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
                v.weight[c] = static_cast<float>((i + c) % 4) / 3.f;
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
            REQUIRE(std::fabs(rt.weight[c] - original.weight[c]) <= 1.f / 255.f + 1e-6f);
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

TEST_CASE("LN7 pack/unpack preserves non-zero, non-uniform weights", "[terrain][lod]") {
    anarchy::amesh::Data mesh;
    mesh.vertices.resize(2);
    const float weights0[4] = {0.f, 0.33f, 0.99f, 1.f};
    const float weights1[4] = {1.f, 0.99f, 0.33f, 0.f};
    for (int c = 0; c < 4; ++c) {
        mesh.vertices[0].weight[c] = weights0[c];
        mesh.vertices[1].weight[c] = weights1[c];
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
            REQUIRE(std::fabs(round_tripped.vertices[i].weight[c] - mesh.vertices[i].weight[c]) <= 1.f / 255.f + 1e-6f);
        }
    }
}

// --- Task 2: LodBuilder (merge, simplify, measure) -------------------------

namespace {

// A rolling slab over 16 x 1 x 16 chunks (512 x 32 x 512 studs): a smooth,
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
// diagnostics recorded, ~0.579 studs, stayed the same regardless, pinned to
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
// error to mismeasure. Written well past the node's own 64x64-stud footprint
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
// in studs.
void fill_rolling_wave(VoxelVolume& volume, float center_x, float center_z) {
    const int half = 80;  // matches fill_gentle_dome's old 160-stud-wide slab
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
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            const ChunkCoord coord{x, 0, z};
            const ChunkMesh mesh = surface_nets(mesh_input(volume, coord));
            if (mesh.render) {
                const NodeKey key = node_of(coord, 0);
                level[key] = mesh.render;
                level_errors[key] = 0.f;  // a level-0 chunk mesh is exact (R8)
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
        for (const auto& [parent_key, child_keys] : grouped) {
            LodInput input;
            input.key = parent_key;
            input.voxel_size = voxel_size;
            for (const NodeKey& child_key : child_keys) {
                input.children.push_back(level[child_key]);
                input.child_errors.push_back(level_errors[child_key]);
            }
            const LodResult result = build_node(input);
            if (result.mesh) {
                next[parent_key] = result.mesh;
                next_errors[parent_key] = result.error;
            }
        }
        const std::size_t triangles = total_triangles(next);
        REQUIRE(triangles > 0);
        const float ratio = static_cast<float>(previous_triangles) / static_cast<float>(triangles);
        INFO("level " << lvl << " triangles: " << triangles << " (previous level " << previous_triangles
                       << ", ratio " << ratio << ")");
        REQUIRE(ratio >= 2.5f);
        REQUIRE(ratio <= 6.0f);
        previous_triangles = triangles;
        level = std::move(next);
        level_errors = std::move(next_errors);
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
