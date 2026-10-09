// Task 2 of the terrain textures plan: up to 4 blended material Ids per
// terrain render vertex (BlendWeights.{hpp,cpp}), wired into SurfaceNets'
// build_vertices and LodBuilder's reshade_vertices (via VoxelSampler::blend),
// plus the border-triangle split that lets the (future) shader blend without
// a geometry shader. See docs/superpowers/specs/2026-10-07-terrain-textures-
// design.md, "Blend weights in the mesh" and implementation note 1.

#include "support.hpp"

#include "amesh.hpp"
#include "terrain/BlendWeights.hpp"
#include "terrain/LodBuilder.hpp"
#include "terrain/LodNode.hpp"
#include "terrain/ShapeDistance.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelSampler.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
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

// The set of Ids v carries with nonzero weight, sorted and deduplicated --
// the same "Identical sets" rule split_border_triangles uses, reimplemented
// here (independent of BlendWeights.cpp's own, unexported copy) so a test
// can check a triangle's three corners agree or disagree.
std::vector<std::uint8_t> id_set(const anarchy::amesh::Vertex& v) {
    std::vector<std::uint8_t> ids;
    for (int k = 0; k < 4; ++k) {
        if (v.t[k] > 0.f) {
            ids.push_back(v.rgba[k]);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

float weight_sum(const anarchy::amesh::Vertex& v) {
    return v.t[0] + v.t[1] + v.t[2] + v.t[3];
}

// v's rgba slots, as a sorted set -- ignoring weight, since a slot a split
// triangle's corner has 0 weight for still carries that triangle's merged
// Id (so the shader reads the same Id per slot at all three corners; only
// the weight varies). Used to check a *post-split* triangle's three corners
// agree; the pre-split trigger rule compares each corner's exact ordered
// slot layout (BlendWeights.cpp's own, internal ordered_ids_of), not just
// the set -- see BW9.
std::vector<std::uint8_t> raw_ids(const anarchy::amesh::Vertex& v) {
    std::vector<std::uint8_t> ids(v.rgba, v.rgba + 4);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// blend_weights(), independently fed from VoxelVolume::cell() (not
// VoxelSampler) over the 8 corners of p's cell -- the same corner selection
// VoxelSampler::blend() and SurfaceNets' build_vertices both use.
BlendIds expected_blend_at(const VoxelVolume& volume, Vec3 p) {
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
    return blend_weights(distances, ids, voxel_size);
}

anarchy::amesh::Vertex make_vertex(std::initializer_list<std::pair<std::uint8_t, float>> entries) {
    anarchy::amesh::Vertex v;
    int k = 0;
    for (const auto& [id, weight] : entries) {
        v.rgba[k] = id;
        v.t[k] = weight;
        ++k;
    }
    for (; k < 4; ++k) {
        v.rgba[k] = 0;
        v.t[k] = 0.f;
    }
    return v;
}

}  // namespace

TEST_CASE("BW1 a vertex deep in one material: one Id at weight 1", "[terrain][textures]") {
    const float voxel_size = 1.f;
    const float distances[8] = {-5.f, -5.f, -5.f, -5.f, -5.f, -5.f, -5.f, -5.f};
    const std::uint8_t ids[8] = {7, 7, 7, 7, 7, 7, 7, 7};
    const BlendIds blend = blend_weights(distances, ids, voxel_size);
    REQUIRE(blend.ids[0] == 7);
    REQUIRE(blend.weights[0] == 1.f);
    for (int k = 1; k < 4; ++k) {
        REQUIRE(blend.ids[k] == 0);
        REQUIRE(blend.weights[k] == 0.f);
    }
}

TEST_CASE("BW2 a vertex on a two-material border: both Ids, weights sum to 1, each > 0", "[terrain][textures]") {
    const float voxel_size = 1.f;
    // All 8 corners within voxel_size of the surface: 4 vote material 2, 4 vote material 5.
    const float distances[8] = {0.2f, -0.3f, 0.4f, -0.1f, 0.05f, -0.5f, 0.3f, -0.2f};
    const std::uint8_t ids[8] = {2, 2, 2, 2, 5, 5, 5, 5};
    const BlendIds blend = blend_weights(distances, ids, voxel_size);

    bool saw2 = false, saw5 = false;
    float sum = 0.f;
    for (int k = 0; k < 4; ++k) {
        sum += blend.weights[k];
        if (blend.weights[k] > 0.f) {
            REQUIRE(blend.weights[k] > 0.f);
            if (blend.ids[k] == 2) saw2 = true;
            if (blend.ids[k] == 5) saw5 = true;
        }
    }
    REQUIRE(saw2);
    REQUIRE(saw5);
    REQUIRE(std::fabs(sum - 1.f) <= 1e-6f);
}

TEST_CASE("BW3 never more than 4 Ids, sorted by descending weight, sum 1", "[terrain][textures]") {
    const float voxel_size = 1.f;
    // 8 distinct Ids on 8 solid corners near the surface, each a single
    // vote: ties broken by lower Id keep 1,2,3,4.
    const float distances[8] = {-0.1f, -0.15f, -0.2f, -0.25f, -0.3f, -0.35f, -0.05f, -0.4f};
    const std::uint8_t ids[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const BlendIds blend = blend_weights(distances, ids, voxel_size);

    float sum = 0.f;
    for (int k = 0; k < 4; ++k) {
        sum += blend.weights[k];
        if (k > 0) {
            REQUIRE(blend.weights[k] <= blend.weights[k - 1] + 1e-6f);
        }
    }
    REQUIRE(std::fabs(sum - 1.f) <= 1e-6f);
    REQUIRE(blend.ids[0] == 1);
    REQUIRE(blend.ids[1] == 2);
    REQUIRE(blend.ids[2] == 3);
    REQUIRE(blend.ids[3] == 4);
    REQUIRE(std::fabs(blend.weights[3] - blend.weights[0]) <= 1e-6f);  // the 4 ties stay equal after renormalizing
}

TEST_CASE("BW4 a Surface Nets ball half grass half rock: weights sum to 1, boundary vertices carry both",
          "[terrain][textures]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(16.f, 16.f, 16.f, 10.f), 2));  // grass
    Shape half;
    half.kind = Shape::Kind::Block;
    half.frame = matrix4_translation(24.f, 16.f, 16.f);  // x in [16, 32): the ball's +x half
    half.size = Vec3{16.f, 32.f, 32.f};
    REQUIRE_FALSE(volume.paint(half, 5));  // rock

    const ChunkMesh mesh = surface_nets(mesh_input(volume, ChunkCoord{0, 0, 0}));
    REQUIRE(mesh.render != nullptr);
    REQUIRE_FALSE(mesh.render->vertices.empty());

    bool saw_both = false;
    for (const auto& v : mesh.render->vertices) {
        REQUIRE(std::fabs(weight_sum(v) - 1.f) <= 1e-4f);
        const std::vector<std::uint8_t> ids = id_set(v);
        const bool has_grass = std::find(ids.begin(), ids.end(), std::uint8_t{2}) != ids.end();
        const bool has_rock = std::find(ids.begin(), ids.end(), std::uint8_t{5}) != ids.end();
        if (has_grass && has_rock) {
            saw_both = true;
        }
    }
    REQUIRE(saw_both);
}

TEST_CASE("BW5 a LOD node's re-shaded vertices match blend_weights from the full-resolution field",
          "[terrain][textures][lod]") {
    VoxelVolume volume;
    REQUIRE_FALSE(volume.fill(ball_at(32.f, 32.f, 32.f, 20.f), 3));
    const auto voxels = std::make_shared<const ChunkMap>(volume.chunks());
    const float voxel_size = volume.voxel_size();

    const NodeKey level1_key{1, 0, 0, 0};
    std::vector<std::shared_ptr<const anarchy::amesh::Data>> children;
    for (const NodeKey& child_key : children_of(level1_key)) {
        const ChunkCoord coord{child_key.x, child_key.y, child_key.z};
        const ChunkMesh mesh = surface_nets(mesh_input(volume, coord));
        if (mesh.render) {
            children.push_back(mesh.render);
        }
    }
    REQUIRE_FALSE(children.empty());

    LodInput input;
    input.key = level1_key;
    input.voxel_size = voxel_size;
    input.children = children;
    input.voxels = voxels;
    const LodResult result = build_node(input);
    REQUIRE(result.mesh != nullptr);

    int checked = 0;
    for (const auto& v : result.mesh->vertices) {
        const Vec3 p{v.p[0], v.p[1], v.p[2]};
        const BlendIds expected = expected_blend_at(volume, p);
        for (int k = 0; k < 4; ++k) {
            REQUIRE(static_cast<int>(v.rgba[k]) == static_cast<int>(expected.ids[k]));
            REQUIRE(std::fabs(v.t[k] - expected.weights[k]) <= 1e-5f);
        }
        ++checked;
    }
    REQUIRE(checked > 0);
}

TEST_CASE("BW6 a triangle with {grass}, {grass,rock}, {rock,sand} corners splits into one merged set",
          "[terrain][textures]") {
    anarchy::amesh::Vertex v0 = make_vertex({{1, 1.f}});               // grass
    anarchy::amesh::Vertex v1 = make_vertex({{1, 0.6f}, {2, 0.4f}});   // grass, rock
    anarchy::amesh::Vertex v2 = make_vertex({{2, 0.5f}, {3, 0.5f}});   // rock, sand
    v0.p[0] = 0.f; v0.p[1] = 0.f; v0.p[2] = 0.f;
    v1.p[0] = 1.f; v1.p[1] = 0.f; v1.p[2] = 0.f;
    v2.p[0] = 0.f; v2.p[1] = 1.f; v2.p[2] = 0.f;

    anarchy::amesh::Data render;
    render.vertices = {v0, v1, v2};
    render.indices = {0, 1, 2};

    split_border_triangles(render);

    // All three original (shared) corners were split off into their own copies.
    REQUIRE(render.vertices.size() == 3);
    REQUIRE(render.indices.size() == 3);

    // Every corner carries the same rgba layout -- the triangle's merged
    // set {grass, rock, sand} plus one padded (unused) slot -- so the
    // shader reads a consistent Id per slot across the triangle; only each
    // corner's own weight (t[]) varies.
    const anarchy::amesh::Vertex& a = render.vertices[render.indices[0]];
    const anarchy::amesh::Vertex& b = render.vertices[render.indices[1]];
    const anarchy::amesh::Vertex& c = render.vertices[render.indices[2]];
    REQUIRE(raw_ids(a) == raw_ids(b));
    REQUIRE(raw_ids(b) == raw_ids(c));

    std::vector<std::uint8_t> used_ids = raw_ids(a);
    used_ids.erase(std::remove(used_ids.begin(), used_ids.end(), std::uint8_t{0}), used_ids.end());
    REQUIRE(used_ids == std::vector<std::uint8_t>{1, 2, 3});

    for (const anarchy::amesh::Vertex& v : {a, b, c}) {
        REQUIRE(std::fabs(weight_sum(v) - 1.f) <= 1e-6f);
    }
}

// R9 (fix round 2): two corners can carry the same Id SET in different slot
// ORDER -- each vertex's own weight-descending sort, computed from that
// vertex's own neighborhood, not from the triangle as a whole. Comparing
// only the set (as split_border_triangles once did) let such a triangle
// report "already agrees" and skip splitting; terrain.vert's `flat` ids
// (one corner) plus `smooth` weights (interpolated per slot number across
// all three) then silently blended slot 0's weight between two different
// materials. v0 and v1 below carry {grass, sand} in opposite slot order; v2
// carries it in v0's order, so two of the three corners "agree" and the old
// (set-only) rule would have left this triangle unsplit.
TEST_CASE("BW9 a triangle whose corners share an Id set in different slot order still splits",
          "[terrain][textures]") {
    anarchy::amesh::Vertex v0 = make_vertex({{1, 0.7f}, {3, 0.3f}});   // grass, sand (grass first)
    anarchy::amesh::Vertex v1 = make_vertex({{3, 0.6f}, {1, 0.4f}});   // sand, grass (sand first)
    anarchy::amesh::Vertex v2 = make_vertex({{1, 0.55f}, {3, 0.45f}});  // grass, sand (grass first, like v0)
    v0.p[0] = 0.f; v0.p[1] = 0.f; v0.p[2] = 0.f;
    v1.p[0] = 1.f; v1.p[1] = 0.f; v1.p[2] = 0.f;
    v2.p[0] = 0.f; v2.p[1] = 1.f; v2.p[2] = 0.f;

    anarchy::amesh::Data render;
    render.vertices = {v0, v1, v2};
    render.indices = {0, 1, 2};

    split_border_triangles(render);

    // Split: each corner got its own copy (3 vertices in, 3 out, but not
    // necessarily shared -- BW6 already covers the "stays at 3" shape; the
    // real assertion here is the ordered layout, below).
    REQUIRE(render.vertices.size() == 3);
    REQUIRE(render.indices.size() == 3);

    const anarchy::amesh::Vertex& a = render.vertices[render.indices[0]];
    const anarchy::amesh::Vertex& b = render.vertices[render.indices[1]];
    const anarchy::amesh::Vertex& c = render.vertices[render.indices[2]];

    // Every corner now carries the SAME ORDERED layout (not just the same
    // set) -- the whole point: slot 0 is the same material (by Id) at every
    // corner, so terrain.vert's smooth weights interpolate one material's
    // weight per slot, consistently, across the triangle.
    for (int k = 0; k < 4; ++k) {
        REQUIRE(a.rgba[k] == b.rgba[k]);
        REQUIRE(b.rgba[k] == c.rgba[k]);
    }
    for (const anarchy::amesh::Vertex& v : {a, b, c}) {
        REQUIRE(std::fabs(weight_sum(v) - 1.f) <= 1e-6f);
    }
    // Each corner's own weight for an Id it originally carried round-trips
    // (projected onto the merged order, same values since both corners'
    // sets were already {grass, sand}).
    REQUIRE(std::fabs(a.t[static_cast<std::size_t>(std::find(a.rgba, a.rgba + 4, std::uint8_t{1}) - a.rgba)] -
                      0.7f) <= 1e-6f);
    REQUIRE(std::fabs(b.t[static_cast<std::size_t>(std::find(b.rgba, b.rgba + 4, std::uint8_t{1}) - b.rgba)] -
                      0.4f) <= 1e-6f);
}

TEST_CASE("BW7 a single-material region keeps shared vertices (vertex count unchanged)", "[terrain][textures]") {
    // A small quad (2 triangles, 4 shared vertices), all one material.
    anarchy::amesh::Vertex v0 = make_vertex({{4, 1.f}});
    anarchy::amesh::Vertex v1 = make_vertex({{4, 1.f}});
    anarchy::amesh::Vertex v2 = make_vertex({{4, 1.f}});
    anarchy::amesh::Vertex v3 = make_vertex({{4, 1.f}});
    v0.p[0] = 0.f; v0.p[1] = 0.f;
    v1.p[0] = 1.f; v1.p[1] = 0.f;
    v2.p[0] = 1.f; v2.p[1] = 1.f;
    v3.p[0] = 0.f; v3.p[1] = 1.f;

    anarchy::amesh::Data render;
    render.vertices = {v0, v1, v2, v3};
    render.indices = {0, 1, 2, 0, 2, 3};

    split_border_triangles(render);

    REQUIRE(render.vertices.size() == 4);
    REQUIRE(render.indices.size() == 6);
    REQUIRE(render.indices[0] == 0);
    REQUIRE(render.indices[1] == 1);
    REQUIRE(render.indices[2] == 2);
    REQUIRE(render.indices[3] == 0);
    REQUIRE(render.indices[4] == 2);
    REQUIRE(render.indices[5] == 3);
}

namespace {

// A rolling island over n x n chunks (one y layer), banded by x into two
// materials, its walls and floor kept a margin inside the outer chunks
// (fill_big_island's style, see terrain_lod_tests.cpp) so every written
// column has a real floor/wall rather than an artificial cliff where an
// outer chunk's own unwritten apron would read back as air. Written one
// z-row of chunks at a time, as fill_big_island does, so no call exceeds
// VoxelVolume::kMaxCellsPerEdit.
void fill_rolling_island(VoxelVolume& volume, int n) {
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
                const float surface = 16.f + 6.f * std::sin(static_cast<float>(x) / 20.f) *
                                                  std::cos(static_cast<float>(z) / 24.f);
                const auto material = static_cast<std::uint8_t>(x <= x1 / 2 ? 1 : 2);
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
        REQUIRE_FALSE(volume.write(CellCoord{0, 0, z0}, CellCoord{x1, y1, z0 + kChunkSize - 1}, distances, materials));
    }
}

}  // namespace

TEST_CASE("BW8 a rolling two-material island grows render vertices by at most 30%, every triangle agrees",
          "[terrain][textures]") {
    VoxelVolume volume;
    const int n = 3;  // a 3x3-chunk island: enough area that the one x-band
                       // boundary is a modest fraction of it, not most of it.
    fill_rolling_island(volume, n);

    std::size_t before = 0, after = 0;
    bool saw_any_mesh = false;
    std::vector<std::shared_ptr<const anarchy::amesh::Data>> meshes;
    for (int cx = 0; cx < n; ++cx) {
        for (int cz = 0; cz < n; ++cz) {
            const ChunkMesh mesh = surface_nets(mesh_input(volume, ChunkCoord{cx, 0, cz}));
            if (!mesh.render) {
                continue;
            }
            saw_any_mesh = true;
            before += mesh.positions.size();
            after += mesh.render->vertices.size();
            meshes.push_back(mesh.render);
        }
    }
    REQUIRE(saw_any_mesh);
    REQUIRE(before > 0);

    const double growth_percent = 100.0 * (static_cast<double>(after) - static_cast<double>(before)) /
                                   static_cast<double>(before);
    WARN("BW8 render vertices " << before << " -> " << after << " (" << growth_percent << "% growth)");
    REQUIRE(growth_percent <= 30.0);
    REQUIRE(after != before);  // the boundary must actually cross some chunk, or the test is trivial

    for (const auto& render : meshes) {
        for (std::size_t t = 0; t + 2 < render->indices.size(); t += 3) {
            const anarchy::amesh::Vertex& a = render->vertices[render->indices[t]];
            const anarchy::amesh::Vertex& b = render->vertices[render->indices[t + 1]];
            const anarchy::amesh::Vertex& c = render->vertices[render->indices[t + 2]];
            // Same rgba layout at all three corners (split triangles: by
            // construction; single-material ones: trivially, one Id in slot 0).
            const std::vector<std::uint8_t> set_a = raw_ids(a);
            const std::vector<std::uint8_t> set_b = raw_ids(b);
            const std::vector<std::uint8_t> set_c = raw_ids(c);
            REQUIRE(set_a == set_b);
            REQUIRE(set_b == set_c);
        }
    }
}

TEST_CASE("BW10 only solid corners vote: air corners' Ids (0 from WriteVoxels, or a fill's leftover) never blend in",
          "[terrain][textures]") {
    const float voxel_size = 1.f;
    // A surface cell: four solid grass (3) corners below, four air corners
    // above holding Id 0, as a WriteVoxels caller writes them.
    const float distances[8] = {-0.4f, -0.6f, -0.3f, -0.5f, 0.6f, 0.4f, 0.7f, 0.5f};
    const std::uint8_t air_zero[8] = {3, 3, 3, 3, 0, 0, 0, 0};
    BlendIds blend = blend_weights(distances, air_zero, voxel_size);
    REQUIRE(blend.ids[0] == 3);
    REQUIRE(blend.weights[0] == 1.f);
    REQUIRE(blend.weights[1] == 0.f);
    // The same with rock (7) left on the air side by an earlier fill that was
    // dug away: the wall shows the ground it cuts into.
    const std::uint8_t air_rock[8] = {3, 3, 3, 3, 7, 7, 7, 7};
    blend = blend_weights(distances, air_rock, voxel_size);
    REQUIRE(blend.ids[0] == 3);
    REQUIRE(blend.weights[0] == 1.f);
}
