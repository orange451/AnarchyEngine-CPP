#include "terrain/LodBuilder.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace engine_core::terrain {

namespace {

Vec3 vertex_position(const anarchy::amesh::Data& mesh, std::uint32_t index) {
    const anarchy::amesh::Vertex& v = mesh.vertices[index];
    return Vec3{v.p[0], v.p[1], v.p[2]};
}

float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 sub3(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add3(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale3(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
float length_sq3(Vec3 a) { return dot3(a, a); }

// Closest point on triangle (a, b, c) to p, squared distance. Christer
// Ericson's region test (Real-Time Collision Detection, 5.1.5).
float point_triangle_distance_sq(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = sub3(b, a);
    const Vec3 ac = sub3(c, a);
    const Vec3 ap = sub3(p, a);
    const float d1 = dot3(ab, ap);
    const float d2 = dot3(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) {
        return length_sq3(ap);  // vertex region a
    }

    const Vec3 bp = sub3(p, b);
    const float d3 = dot3(ab, bp);
    const float d4 = dot3(ac, bp);
    if (d3 >= 0.f && d4 <= d3) {
        return length_sq3(bp);  // vertex region b
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
        const float v = d1 / (d1 - d3);
        return length_sq3(sub3(p, add3(a, scale3(ab, v))));  // edge ab
    }

    const Vec3 cp = sub3(p, c);
    const float d5 = dot3(ab, cp);
    const float d6 = dot3(ac, cp);
    if (d6 >= 0.f && d5 <= d6) {
        return length_sq3(cp);  // vertex region c
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
        const float w = d2 / (d2 - d6);
        return length_sq3(sub3(p, add3(a, scale3(ac, w))));  // edge ac
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length_sq3(sub3(p, add3(b, scale3(sub3(c, b), w))));  // edge bc
    }

    const float denom = 1.f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    const Vec3 closest = add3(add3(a, scale3(ab, v)), scale3(ac, w));
    return length_sq3(sub3(p, closest));
}

int cell_coord(float v, float origin, float cell, int n) {
    const int c = static_cast<int>(std::floor((v - origin) / cell));
    return std::clamp(c, 0, n - 1);
}

// A uniform grid over a mesh's triangles (by their AABB), for a fast
// nearest-triangle query. Kept at about one triangle per cell on average, so
// a child vertex's query touches only its own neighborhood rather than the
// whole node: the measurement this backs (max_child_distance, below) must
// stay cheap even on the largest nodes Task 10 builds (a 4,096-chunk island,
// every level, under 5 s on 4 workers).
struct TriangleGrid {
    Vec3 origin{};
    float cell = 1.f;
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::vector<std::uint32_t>> buckets;  // triangle index (mesh.indices[3*t..3*t+2])

    bool empty() const { return buckets.empty(); }
    std::size_t index(int ix, int iy, int iz) const {
        return static_cast<std::size_t>(ix) + static_cast<std::size_t>(nx) * (static_cast<std::size_t>(iy) +
               static_cast<std::size_t>(ny) * static_cast<std::size_t>(iz));
    }
};

TriangleGrid build_triangle_grid(const anarchy::amesh::Data& mesh) {
    TriangleGrid grid;
    const std::size_t triangle_count = mesh.indices.size() / 3;
    if (triangle_count == 0) {
        return grid;
    }

    const Vec3 box_min{mesh.bbox_min[0], mesh.bbox_min[1], mesh.bbox_min[2]};
    const Vec3 box_max{mesh.bbox_max[0], mesh.bbox_max[1], mesh.bbox_max[2]};
    const Vec3 extent{std::max(box_max.x - box_min.x, 1e-4f), std::max(box_max.y - box_min.y, 1e-4f),
                       std::max(box_max.z - box_min.z, 1e-4f)};
    const float volume = extent.x * extent.y * extent.z;
    const float cell = std::max(1e-3f, std::cbrt(volume / static_cast<float>(triangle_count)));

    grid.origin = box_min;
    grid.cell = cell;
    grid.nx = std::clamp(static_cast<int>(std::ceil(extent.x / cell)), 1, 128);
    grid.ny = std::clamp(static_cast<int>(std::ceil(extent.y / cell)), 1, 128);
    grid.nz = std::clamp(static_cast<int>(std::ceil(extent.z / cell)), 1, 128);
    grid.buckets.resize(static_cast<std::size_t>(grid.nx) * static_cast<std::size_t>(grid.ny) *
                         static_cast<std::size_t>(grid.nz));

    for (std::size_t t = 0; t < triangle_count; ++t) {
        const Vec3 a = vertex_position(mesh, mesh.indices[t * 3 + 0]);
        const Vec3 b = vertex_position(mesh, mesh.indices[t * 3 + 1]);
        const Vec3 c = vertex_position(mesh, mesh.indices[t * 3 + 2]);
        const Vec3 tmin{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::min({a.z, b.z, c.z})};
        const Vec3 tmax{std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}), std::max({a.z, b.z, c.z})};
        const int ix0 = cell_coord(tmin.x, grid.origin.x, grid.cell, grid.nx);
        const int ix1 = cell_coord(tmax.x, grid.origin.x, grid.cell, grid.nx);
        const int iy0 = cell_coord(tmin.y, grid.origin.y, grid.cell, grid.ny);
        const int iy1 = cell_coord(tmax.y, grid.origin.y, grid.cell, grid.ny);
        const int iz0 = cell_coord(tmin.z, grid.origin.z, grid.cell, grid.nz);
        const int iz1 = cell_coord(tmax.z, grid.origin.z, grid.cell, grid.nz);
        for (int iz = iz0; iz <= iz1; ++iz) {
            for (int iy = iy0; iy <= iy1; ++iy) {
                for (int ix = ix0; ix <= ix1; ++ix) {
                    grid.buckets[grid.index(ix, iy, iz)].push_back(static_cast<std::uint32_t>(t));
                }
            }
        }
    }
    return grid;
}

// The distance from p to the nearest triangle the grid indexes, found by
// expanding rings of cells (by Chebyshev distance) until no closer triangle
// could lie beyond the searched radius.
float nearest_triangle_distance(const TriangleGrid& grid, const anarchy::amesh::Data& mesh, Vec3 p) {
    if (grid.empty()) {
        return 0.f;
    }
    const int cx = cell_coord(p.x, grid.origin.x, grid.cell, grid.nx);
    const int cy = cell_coord(p.y, grid.origin.y, grid.cell, grid.ny);
    const int cz = cell_coord(p.z, grid.origin.z, grid.cell, grid.nz);

    float best_sq = std::numeric_limits<float>::max();
    for (int radius = 0;; ++radius) {
        const int x0 = std::max(cx - radius, 0), x1 = std::min(cx + radius, grid.nx - 1);
        const int y0 = std::max(cy - radius, 0), y1 = std::min(cy + radius, grid.ny - 1);
        const int z0 = std::max(cz - radius, 0), z1 = std::min(cz + radius, grid.nz - 1);
        for (int iz = z0; iz <= z1; ++iz) {
            for (int iy = y0; iy <= y1; ++iy) {
                for (int ix = x0; ix <= x1; ++ix) {
                    const int chebyshev = std::max(std::abs(ix - cx), std::max(std::abs(iy - cy), std::abs(iz - cz)));
                    if (chebyshev != radius) {
                        continue;  // visited at a smaller radius already
                    }
                    for (std::uint32_t t : grid.buckets[grid.index(ix, iy, iz)]) {
                        const Vec3 a = vertex_position(mesh, mesh.indices[t * 3 + 0]);
                        const Vec3 b = vertex_position(mesh, mesh.indices[t * 3 + 1]);
                        const Vec3 c = vertex_position(mesh, mesh.indices[t * 3 + 2]);
                        best_sq = std::min(best_sq, point_triangle_distance_sq(p, a, b, c));
                    }
                }
            }
        }
        const bool whole_grid = x0 == 0 && x1 == grid.nx - 1 && y0 == 0 && y1 == grid.ny - 1 && z0 == 0 && z1 == grid.nz - 1;
        if (whole_grid) {
            break;  // nothing left outside the searched cells
        }
        if (best_sq < std::numeric_limits<float>::max()) {
            const float safe_radius_distance = static_cast<float>(radius) * grid.cell;
            if (safe_radius_distance * safe_radius_distance >= best_sq) {
                break;  // nothing beyond this radius could be closer
            }
        }
    }
    return std::sqrt(best_sq);
}

// The maximum, over every vertex of children, of its distance to mesh's
// nearest triangle: R8's honest per-level measurement (meshopt's own quadric
// error underestimates this on bumpy terrain, by ~5x in cases checked while
// building this), grid-accelerated so it stays cheap on the largest nodes.
float max_child_distance(const std::vector<std::shared_ptr<const anarchy::amesh::Data>>& children,
                          const anarchy::amesh::Data& mesh) {
    const TriangleGrid grid = build_triangle_grid(mesh);
    float worst = 0.f;
    for (const auto& child : children) {
        if (!child) {
            continue;
        }
        for (std::size_t i = 0; i < child->vertices.size(); ++i) {
            const Vec3 p = vertex_position(*child, static_cast<std::uint32_t>(i));
            worst = std::max(worst, nearest_triangle_distance(grid, mesh, p));
        }
    }
    return worst;
}

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    if (a > b) {
        std::swap(a, b);
    }
    return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
}

// Every edge used by exactly one triangle of mesh, as pairs of vertex indices.
std::vector<std::uint32_t> collect_border_edges(const anarchy::amesh::Data& mesh) {
    struct Edge {
        std::uint32_t a, b;
    };
    std::vector<Edge> edges;
    edges.reserve(mesh.indices.size());
    std::unordered_map<std::uint64_t, int> counts;
    counts.reserve(mesh.indices.size());
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = mesh.indices[t + static_cast<std::size_t>(e)];
            const std::uint32_t b = mesh.indices[t + static_cast<std::size_t>((e + 1) % 3)];
            edges.push_back(Edge{a, b});
            ++counts[edge_key(a, b)];
        }
    }
    std::vector<std::uint32_t> out;
    for (const Edge& edge : edges) {
        if (counts[edge_key(edge.a, edge.b)] == 1) {
            out.push_back(edge.a);
            out.push_back(edge.b);
        }
    }
    return out;
}

}  // namespace

float target_error(int level, float voxel_size) { return 0.25f * voxel_size * static_cast<float>(1 << level); }

LodResult build_node(const LodInput& input) {
    LodResult result;
    result.key = input.key;

    std::size_t total_vertices = 0, total_indices = 0;
    for (const auto& child : input.children) {
        if (child) {
            total_vertices += child->vertices.size();
            total_indices += child->indices.size();
        }
    }
    if (total_indices == 0) {
        return result;  // null mesh: nothing to merge
    }

    // Concatenate children, offsetting each one's indices by the vertices
    // already appended.
    std::vector<anarchy::amesh::Vertex> merged_vertices;
    std::vector<unsigned int> merged_indices;
    merged_vertices.reserve(total_vertices);
    merged_indices.reserve(total_indices);
    for (const auto& child : input.children) {
        if (!child) {
            continue;
        }
        const std::uint32_t offset = static_cast<std::uint32_t>(merged_vertices.size());
        merged_vertices.insert(merged_vertices.end(), child->vertices.begin(), child->vertices.end());
        for (std::uint32_t idx : child->indices) {
            merged_indices.push_back(idx + offset);
        }
    }

    // Weld exactly-equal positions: meshopt_generateVertexRemap on positions
    // only (not the full vertex), so two children's bit-identical Surface
    // Nets border vertices (same cell, same math -- see SurfaceNets.cpp's
    // build_vertices) collapse to one and the seam between them closes.
    std::vector<float> positions(merged_vertices.size() * 3);
    for (std::size_t i = 0; i < merged_vertices.size(); ++i) {
        positions[i * 3 + 0] = merged_vertices[i].p[0];
        positions[i * 3 + 1] = merged_vertices[i].p[1];
        positions[i * 3 + 2] = merged_vertices[i].p[2];
    }
    std::vector<unsigned int> remap(merged_vertices.size());
    const std::size_t unique_count = meshopt_generateVertexRemap(remap.data(), merged_indices.data(), merged_indices.size(),
                                                                   positions.data(), merged_vertices.size(), sizeof(float) * 3);

    std::vector<anarchy::amesh::Vertex> welded(unique_count);
    meshopt_remapVertexBuffer(welded.data(), merged_vertices.data(), merged_vertices.size(), sizeof(anarchy::amesh::Vertex),
                               remap.data());
    std::vector<unsigned int> welded_indices(merged_indices.size());
    meshopt_remapIndexBuffer(welded_indices.data(), merged_indices.data(), merged_indices.size(), remap.data());

    // Simplify to this level's error budget. R8: the recorded error is
    // cumulative (this level's own honest measured distance, plus the worst
    // of the children's own already-recorded errors), so compute that floor
    // before simplifying at all.
    const float target = target_error(input.key.level, input.voxel_size);
    const float scale = meshopt_simplifyScale(&welded[0].p[0], welded.size(), sizeof(anarchy::amesh::Vertex));
    float normalized_target_error = scale > 0.f ? target / scale : 0.f;
    const std::size_t target_index_count = (welded_indices.size() / 4 / 3) * 3;

    float max_child_error = 0.f;
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        const float child_error = i < input.child_errors.size() ? input.child_errors[i] : 0.f;
        max_child_error = std::max(max_child_error, child_error);
    }

    // R8: up to 4 attempts, each halving meshopt's own target error (the
    // index-count target never changes) if the previous attempt's honest
    // recorded error came in over budget. Keep the first attempt within
    // budget; failing that, keep whichever attempt recorded the lowest error.
    std::shared_ptr<const anarchy::amesh::Data> best_mesh;
    float best_error = std::numeric_limits<float>::max();
    constexpr int kMaxAttempts = 4;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::vector<unsigned int> simplified(welded_indices.size());
        float result_error = 0.f;
        const std::size_t simplified_count =
            meshopt_simplify(simplified.data(), welded_indices.data(), welded_indices.size(), &welded[0].p[0],
                              welded.size(), sizeof(anarchy::amesh::Vertex), target_index_count,
                              normalized_target_error, 0, &result_error);
        simplified.resize(simplified_count);

        // Compact: drop vertices simplification no longer uses, and reorder what's left for fetch locality.
        std::vector<anarchy::amesh::Vertex> compacted(welded.size());
        const std::size_t final_vertex_count =
            meshopt_optimizeVertexFetch(compacted.data(), simplified.data(), simplified.size(), welded.data(),
                                         welded.size(), sizeof(anarchy::amesh::Vertex));
        compacted.resize(final_vertex_count);

        anarchy::amesh::Data mesh;
        mesh.vertices = std::move(compacted);
        mesh.indices.assign(simplified.begin(), simplified.end());
        anarchy::amesh::compute_aabb(mesh);
        auto mesh_ptr = std::make_shared<const anarchy::amesh::Data>(std::move(mesh));

        const float measured = max_child_distance(input.children, *mesh_ptr);
        const float candidate_error = measured + max_child_error;
        const bool within_budget = candidate_error <= target;
        if (within_budget || candidate_error < best_error) {
            best_error = candidate_error;
            best_mesh = std::move(mesh_ptr);
        }
        if (within_budget) {
            break;
        }
        normalized_target_error *= 0.5f;
    }

    result.error = best_error;
    result.mesh = best_mesh;
    result.border_edges = collect_border_edges(*best_mesh);
    return result;
}

}  // namespace engine_core::terrain
