#include "terrain/LodBuilder.hpp"

#include "terrain/VoxelSampler.hpp"

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

Vec3 vertex_position_of(const anarchy::amesh::Vertex& v) { return Vec3{v.p[0], v.p[1], v.p[2]}; }

float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 sub3(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add3(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale3(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
float length_sq3(Vec3 a) { return dot3(a, a); }
Vec3 cross3(Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

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
    // At most 128 cells per axis must still span the box: a flat mesh (no
    // y extent) would otherwise get cells so small that the clamped grid
    // covered a tenth of it, piling every other triangle into its edge cells.
    const float span = std::max({extent.x, extent.y, extent.z});
    const float cell = std::max({1e-3f, std::cbrt(volume / static_cast<float>(triangle_count)), span / 128.f});

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
//
// R10: a child's skirt vertices (indexed only by the triangles beyond its
// own child_surface_index_counts[i], if given) are excluded -- a skirt
// vertex is displaced inward along -normal purely to hide a rendering
// crack, so measuring this (coarser) level's error against it would charge
// for a discrepancy that was never part of the real surface to begin with,
// and would make this measurement (and so the recorded error) depend on
// whether a child happened to carry skirts, rather than on the child's own
// real geometry. Mirrors the same first-N-indices slice build_node's own
// merge uses below, so a child merged with vs. without its skirts already
// physically stripped measures identically.
float max_child_distance(const std::vector<std::shared_ptr<const anarchy::amesh::Data>>& children,
                          const std::vector<std::uint32_t>& child_surface_index_counts,
                          const anarchy::amesh::Data& mesh) {
    const TriangleGrid grid = build_triangle_grid(mesh);
    float worst = 0.f;
    for (std::size_t c = 0; c < children.size(); ++c) {
        const auto& child = children[c];
        if (!child) {
            continue;
        }
        const std::size_t use_count = c < child_surface_index_counts.size()
                                           ? std::min<std::size_t>(child_surface_index_counts[c], child->indices.size())
                                           : child->indices.size();
        std::vector<bool> used(child->vertices.size(), false);
        for (std::size_t i = 0; i < use_count; ++i) {
            used[child->indices[i]] = true;
        }
        for (std::size_t i = 0; i < child->vertices.size(); ++i) {
            if (!used[i]) {
                continue;
            }
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

// Every edge used by exactly one triangle of mesh, as pairs of vertex indices,
// and (in out_thirds, parallel: one per edge) the third vertex of the edge's
// own triangle.
std::vector<std::uint32_t> collect_border_edges(const anarchy::amesh::Data& mesh, std::vector<std::uint32_t>& out_thirds) {
    struct Edge {
        std::uint32_t a, b, c;
    };
    std::vector<Edge> edges;
    edges.reserve(mesh.indices.size());
    std::unordered_map<std::uint64_t, int> counts;
    counts.reserve(mesh.indices.size());
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = mesh.indices[t + static_cast<std::size_t>(e)];
            const std::uint32_t b = mesh.indices[t + static_cast<std::size_t>((e + 1) % 3)];
            const std::uint32_t c = mesh.indices[t + static_cast<std::size_t>((e + 2) % 3)];
            edges.push_back(Edge{a, b, c});
            ++counts[edge_key(a, b)];
        }
    }
    std::vector<std::uint32_t> out;
    out_thirds.clear();
    for (const Edge& edge : edges) {
        if (counts[edge_key(edge.a, edge.b)] == 1) {
            out.push_back(edge.a);
            out.push_back(edge.b);
            out_thirds.push_back(edge.c);
        }
    }
    return out;
}

// Re-shades every vertex of mesh from the full-resolution field sampler
// reads: normal = the field's gradient at the vertex's (unchanged) position;
// Id = the lowest-distance corner's Id of the full-resolution cell around
// it. Mirrors exactly what SurfaceNets.cpp's build_vertices writes for a
// vertex's n/rgba (rgba[1..3] stay 0; weight[] is untouched -- a Surface
// Nets mesh carries no skinning weights to begin with).
void reshade_vertices(anarchy::amesh::Data& mesh, const VoxelSampler& sampler) {
    for (anarchy::amesh::Vertex& v : mesh.vertices) {
        const Vec3 p{v.p[0], v.p[1], v.p[2]};
        const Vec3 n = sampler.gradient(p);
        v.n[0] = n.x;
        v.n[1] = n.y;
        v.n[2] = n.z;
        v.rgba[0] = sampler.id(p);
        v.rgba[1] = 0;
        v.rgba[2] = 0;
        v.rgba[3] = 0;
    }
}

// terrain.frag takes a triangle's material from its provoking (last) vertex
// (flat in). A level-0 triangle spans one voxel, so which corner's Id wins
// hardly shows; a simplified triangle can span many, and one fanned from a
// vertex at a material boundary (a clay-Id vertex at a mound's base, out
// across a flat grass floor) drew wholly in that vertex's material. Rotates
// each triangle's indices (the cyclic order, so winding and border edges
// are unchanged) so its last vertex carries the material the full-resolution
// field has at the triangle's centroid, when any of its vertices does.
void orient_triangle_materials(anarchy::amesh::Data& mesh, const VoxelSampler& sampler) {
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::uint32_t i0 = mesh.indices[t], i1 = mesh.indices[t + 1], i2 = mesh.indices[t + 2];
        const anarchy::amesh::Vertex& v0 = mesh.vertices[i0];
        const anarchy::amesh::Vertex& v1 = mesh.vertices[i1];
        const anarchy::amesh::Vertex& v2 = mesh.vertices[i2];
        if (v0.rgba[0] == v1.rgba[0] && v1.rgba[0] == v2.rgba[0]) {
            continue;   // one material: nothing to choose
        }
        const Vec3 centroid{(v0.p[0] + v1.p[0] + v2.p[0]) / 3.f, (v0.p[1] + v1.p[1] + v2.p[1]) / 3.f,
                            (v0.p[2] + v1.p[2] + v2.p[2]) / 3.f};
        const std::uint8_t id = sampler.id(centroid);
        if (v2.rgba[0] == id) {
            continue;
        }
        if (v0.rgba[0] == id) {
            mesh.indices[t] = i1;
            mesh.indices[t + 1] = i2;
            mesh.indices[t + 2] = i0;
        } else if (v1.rgba[0] == id) {
            mesh.indices[t] = i2;
            mesh.indices[t + 1] = i0;
            mesh.indices[t + 2] = i1;
        }
    }
}

// Appends, for every border edge (a, b) in border_edges (pairs of vertex
// indices into mesh, in the forward order collect_border_edges extracted
// them: the same direction their one owning triangle's winding visits them
// in; thirds holds that triangle's third vertex c, one per edge), a flanged
// quad (R21): two new vertices
//   a' = a - n_a * depth + u * depth / 2,  b' = b - n_b * depth + u * depth / 2
// (copying a/b's own normal and Id), and two triangles, where u is the unit
// vector perpendicular to the edge, in the plane of its triangle, pointing
// away from that triangle: normalize(cross(e, f)) with e = b - a and
// f = cross(b - a, c - a), the triangle's own (outward) face normal. Hanging
// straight down -normal, a skirt is seen edge-on by a viewer looking down the
// normal, so it hid nothing from that view; leaning outward under the
// neighbor, it covers a lateral gap from above too.
//
// Winding: for any triangle (p0, p1, p2) this mesh stores, cross(p1 - p0,
// p2 - p0) points the same way the vertex normals do (outward -- Surface
// Nets winds every quad "from solid toward air", and simplification,
// compaction and stitching never change that), and (a, b) is in its
// triangle's own order. Let n be the edge's normal (n_a == n_b == n, and
// u = cross(e, n) / |e|, in the planar case). Triangles (a, b', b) and
// (a, a', b') both come out to cross(second - a, third - a) ==
// depth * |e| * (u + n / 2): the depth^2 terms of the second cancel -- so
// both face away from the solid side of the edge and partly up its normal,
// the same way the border edge's own triangle does, never flipped.
void add_skirts(anarchy::amesh::Data& mesh, const std::vector<std::uint32_t>& border_edges,
                const std::vector<std::uint32_t>& thirds, float depth) {
    const std::size_t edge_count = border_edges.size() / 2;
    for (std::size_t e = 0; e < edge_count; ++e) {
        const std::uint32_t a_index = border_edges[e * 2 + 0];
        const std::uint32_t b_index = border_edges[e * 2 + 1];
        const anarchy::amesh::Vertex a = mesh.vertices[a_index];
        const anarchy::amesh::Vertex b = mesh.vertices[b_index];
        const Vec3 pa = vertex_position_of(a);
        const Vec3 pb = vertex_position_of(b);
        const Vec3 pc = vertex_position_of(mesh.vertices[thirds[e]]);

        const Vec3 edge = sub3(pb, pa);
        const Vec3 face = cross3(edge, sub3(pc, pa));
        Vec3 out = cross3(edge, face);
        const float out_length = std::sqrt(length_sq3(out));
        out = out_length > 0.f ? scale3(out, 0.5f * depth / out_length) : Vec3{};

        anarchy::amesh::Vertex a_prime = a;
        a_prime.p[0] = a.p[0] - a.n[0] * depth + out.x;
        a_prime.p[1] = a.p[1] - a.n[1] * depth + out.y;
        a_prime.p[2] = a.p[2] - a.n[2] * depth + out.z;

        anarchy::amesh::Vertex b_prime = b;
        b_prime.p[0] = b.p[0] - b.n[0] * depth + out.x;
        b_prime.p[1] = b.p[1] - b.n[1] * depth + out.y;
        b_prime.p[2] = b.p[2] - b.n[2] * depth + out.z;

        const auto a_prime_index = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(a_prime);
        const auto b_prime_index = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(b_prime);

        mesh.indices.insert(mesh.indices.end(), {a_index, b_prime_index, b_index});
        mesh.indices.insert(mesh.indices.end(), {a_index, a_prime_index, b_prime_index});
    }
}

// How far apart (per axis) two children's copies of one shared vertex can
// be. Level-0 children are exact chunk meshes (Surface Nets writes a shared
// border vertex bit for bit the same in both chunks), so only a tiny step is
// needed. Coarser children were unpacked from CompactMeshes quantized to
// their own box (R2: the union of the child's node box and its mesh's AABB),
// each axis in steps of extent / 65535, so a vertex is off by at most half a
// step and two copies by at most one coarsest step. LodInput carries no box,
// so this bounds the child's box by the union of the parent's node box (which
// holds the child's) and the child mesh's AABB, and welds within 2 steps.
float weld_tolerance(const LodInput& input) {
    if (input.key.level <= 1) {
        return input.voxel_size / 4096.f;
    }
    Vec3 parent_min, parent_max;
    node_bounds(input.key, input.voxel_size, parent_min, parent_max);
    float extent = 0.f;
    for (const auto& child : input.children) {
        if (!child || child->vertices.empty()) {
            continue;
        }
        Vec3 lo = parent_min, hi = parent_max;
        for (const anarchy::amesh::Vertex& v : child->vertices) {
            lo = Vec3{std::min(lo.x, v.p[0]), std::min(lo.y, v.p[1]), std::min(lo.z, v.p[2])};
            hi = Vec3{std::max(hi.x, v.p[0]), std::max(hi.y, v.p[1]), std::max(hi.z, v.p[2])};
        }
        extent = std::max({extent, hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    }
    return 2.f * extent / 65535.f;
}

struct WeldCell {
    std::int64_t x, y, z;
    bool operator==(const WeldCell& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct WeldCellHash {
    std::size_t operator()(const WeldCell& c) const {
        std::uint64_t h = static_cast<std::uint64_t>(c.x) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(c.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint64_t>(c.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

// Welds vertices whose positions lie within tolerance of each other on every
// axis, visiting vertices in first-use order through indices (so a vertex no
// index uses -- a child's skirt vertex, R10 -- is dropped), each one joining
// the lowest-numbered earlier vertex in range or starting a new one. A hash
// grid of tolerance-sized cells keeps it to a 27-cell look-up per vertex.
// Triangles left with a repeated vertex are dropped.
void weld_vertices(const std::vector<anarchy::amesh::Vertex>& vertices, const std::vector<unsigned int>& indices,
                   const std::vector<std::uint32_t>& children, float tolerance,
                   std::vector<anarchy::amesh::Vertex>& out_vertices, std::vector<unsigned int>& out_indices,
                   std::vector<std::uint32_t>& out_children) {
    constexpr unsigned int kUnset = std::numeric_limits<unsigned int>::max();
    const float cell = std::max(tolerance, 1e-6f);
    std::vector<unsigned int> remap(vertices.size(), kUnset);
    std::unordered_map<WeldCell, std::vector<unsigned int>, WeldCellHash> grid;
    grid.reserve(vertices.size());
    out_vertices.clear();
    out_vertices.reserve(vertices.size());
    out_children.clear();
    for (unsigned int index : indices) {
        if (remap[index] != kUnset) {
            continue;
        }
        const anarchy::amesh::Vertex& v = vertices[index];
        const WeldCell home{static_cast<std::int64_t>(std::floor(v.p[0] / cell)),
                            static_cast<std::int64_t>(std::floor(v.p[1] / cell)),
                            static_cast<std::int64_t>(std::floor(v.p[2] / cell))};
        unsigned int match = kUnset;
        for (std::int64_t dz = -1; dz <= 1; ++dz) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const auto found = grid.find(WeldCell{home.x + dx, home.y + dy, home.z + dz});
                    if (found == grid.end()) {
                        continue;
                    }
                    for (unsigned int candidate : found->second) {
                        const anarchy::amesh::Vertex& w = out_vertices[candidate];
                        if (candidate < match && std::fabs(w.p[0] - v.p[0]) <= tolerance &&
                            std::fabs(w.p[1] - v.p[1]) <= tolerance && std::fabs(w.p[2] - v.p[2]) <= tolerance) {
                            match = candidate;
                        }
                    }
                }
            }
        }
        if (match == kUnset) {
            match = static_cast<unsigned int>(out_vertices.size());
            out_vertices.push_back(v);
            out_children.push_back(0);
            grid[home].push_back(match);
        }
        remap[index] = match;
        out_children[match] |= children[index];
    }
    out_indices.clear();
    out_indices.reserve(indices.size());
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const unsigned int a = remap[indices[t]], b = remap[indices[t + 1]], c = remap[indices[t + 2]];
        if (a == b || b == c || a == c) {
            continue;
        }
        out_indices.insert(out_indices.end(), {a, b, c});
    }
}

// Zips the open seams between siblings. Each sibling simplified its own
// border, keeping its own subset of the border's vertices, so after welding a
// seam is two different polylines through the same original border vertices.
// For every border edge (a, b), every border vertex of another child that
// lies within tolerance of the segment, strictly between a and b, is
// inserted into it: its triangle (a, b, c) becomes the fan (a, v1, c),
// (v1, v2, c), ..., (vk, b, c), winding unchanged. Done on both sides, the
// two polylines become the same one and the seam's edges pair up. children
// holds a bit per child for each vertex; a vertex is only inserted into a
// triangle of a child it does not belong to, and only when its normal is on
// the edge's side (not the far face of a wall or cave thinner than
// tolerance). The search grid's cells are at least voxel_size, so an edge
// costs (length / cell + 1) look-ups of a few cells however small tolerance is.
void stitch_seams(const std::vector<anarchy::amesh::Vertex>& vertices, std::vector<unsigned int>& indices,
                  const std::vector<std::uint32_t>& children, float tolerance, float voxel_size) {
    struct BorderEdge {
        unsigned int a, b, c;
    };
    std::unordered_map<std::uint64_t, int> counts;
    counts.reserve(indices.size());
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            ++counts[edge_key(indices[t + static_cast<std::size_t>(e)], indices[t + static_cast<std::size_t>((e + 1) % 3)])];
        }
    }
    std::vector<BorderEdge> borders;
    std::unordered_map<std::uint64_t, std::size_t> owner;  // directed border edge (a << 32 | b) -> triangle start
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            const unsigned int a = indices[t + static_cast<std::size_t>(e)];
            const unsigned int b = indices[t + static_cast<std::size_t>((e + 1) % 3)];
            const unsigned int c = indices[t + static_cast<std::size_t>((e + 2) % 3)];
            if (counts[edge_key(a, b)] == 1) {
                borders.push_back(BorderEdge{a, b, c});
                owner[(static_cast<std::uint64_t>(a) << 32) | b] = t;
            }
        }
    }
    if (borders.empty()) {
        return;
    }

    // Cells at least a voxel across: border vertices are about a voxel apart
    // or more, so a cell holds a few, and an edge's search below walks
    // (length / cell + 1) samples of a few cells each, however small the
    // tolerance (near 0 on planar terrain, where children simplify exactly).
    const float cell = std::max({tolerance, voxel_size, 1e-4f});
    auto cell_of = [cell](float v) { return static_cast<std::int64_t>(std::floor(v / cell)); };
    std::unordered_map<WeldCell, std::vector<unsigned int>, WeldCellHash> grid;
    std::vector<bool> on_border(vertices.size(), false);
    for (const BorderEdge& edge : borders) {
        for (unsigned int v : {edge.a, edge.b}) {
            if (!on_border[v]) {
                on_border[v] = true;
                const Vec3 p = vertex_position_of(vertices[v]);
                grid[WeldCell{cell_of(p.x), cell_of(p.y), cell_of(p.z)}].push_back(v);
            }
        }
    }

    const float tolerance_sq = tolerance * tolerance;
    std::vector<std::pair<float, unsigned int>> splits;
    std::vector<unsigned int> candidates;
    for (const BorderEdge& edge : borders) {
        const std::uint32_t own = children[edge.a] & children[edge.b] & children[edge.c];
        if (own == 0) {
            continue;   // not one child's triangle: nothing to tell siblings apart by
        }
        const Vec3 a = vertex_position_of(vertices[edge.a]);
        const Vec3 b = vertex_position_of(vertices[edge.b]);
        const Vec3 ab = sub3(b, a);
        const float length_sq = length_sq3(ab);
        if (!(length_sq > 0.f)) {
            continue;
        }
        splits.clear();
        candidates.clear();
        // Samples along the edge at most a cell apart; every point within
        // tolerance of the edge lies within half a step plus tolerance of
        // a sample on each axis, so the cells that box spans hold every
        // candidate. Cost: (length / cell + 1) samples of at most 4^3 cells.
        const float length = std::sqrt(length_sq);
        const int steps = std::max(1, static_cast<int>(std::ceil(length / cell)));
        const float reach = 0.5f * length / static_cast<float>(steps) + tolerance;
        for (int i = 0; i <= steps; ++i) {
            const Vec3 at = add3(a, scale3(ab, static_cast<float>(i) / static_cast<float>(steps)));
            const std::int64_t x0 = cell_of(at.x - reach), x1 = cell_of(at.x + reach);
            const std::int64_t y0 = cell_of(at.y - reach), y1 = cell_of(at.y + reach);
            const std::int64_t z0 = cell_of(at.z - reach), z1 = cell_of(at.z + reach);
            for (std::int64_t z = z0; z <= z1; ++z) {
                for (std::int64_t y = y0; y <= y1; ++y) {
                    for (std::int64_t x = x0; x <= x1; ++x) {
                        const auto found = grid.find(WeldCell{x, y, z});
                        if (found != grid.end()) {
                            candidates.insert(candidates.end(), found->second.begin(), found->second.end());
                        }
                    }
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        // The side of the surface the edge is on: a vertex whose normal
        // points against it is the far side of a feature thinner than the
        // tolerance (a wall, a cave), and inserting it would fold a triangle
        // across that feature.
        const anarchy::amesh::Vertex& va = vertices[edge.a];
        const anarchy::amesh::Vertex& vb = vertices[edge.b];
        const Vec3 edge_normal{va.n[0] + vb.n[0], va.n[1] + vb.n[1], va.n[2] + vb.n[2]};
        for (unsigned int v : candidates) {
            if (v == edge.a || v == edge.b || v == edge.c || (children[v] & own) != 0) {
                continue;
            }
            const anarchy::amesh::Vertex& vv = vertices[v];
            if (dot3(edge_normal, Vec3{vv.n[0], vv.n[1], vv.n[2]}) <= 0.f) {
                continue;
            }
            const Vec3 p = vertex_position_of(vertices[v]);
            const float s = dot3(sub3(p, a), ab) / length_sq;
            if (s <= 1e-4f || s >= 1.f - 1e-4f) {
                continue;
            }
            if (length_sq3(sub3(p, add3(a, scale3(ab, s)))) <= tolerance_sq) {
                splits.emplace_back(s, v);
            }
        }
        if (splits.empty()) {
            continue;
        }
        std::sort(splits.begin(), splits.end());

        const auto found = owner.find((static_cast<std::uint64_t>(edge.a) << 32) | edge.b);
        if (found == owner.end()) {
            continue;
        }
        const std::size_t t = found->second;
        // The owning triangle may have been re-fanned by an earlier split of
        // another of its edges; it still holds (a, b) in order with some
        // third vertex c.
        bool holds_edge = false;
        unsigned int c = 0;
        for (int e = 0; e < 3; ++e) {
            if (indices[t + static_cast<std::size_t>(e)] == edge.a &&
                indices[t + static_cast<std::size_t>((e + 1) % 3)] == edge.b) {
                c = indices[t + static_cast<std::size_t>((e + 2) % 3)];
                holds_edge = true;
            }
        }
        splits.erase(std::remove_if(splits.begin(), splits.end(),
                                    [c](const std::pair<float, unsigned int>& split) { return split.second == c; }),
                     splits.end());
        if (!holds_edge || splits.empty()) {
            continue;
        }
        unsigned int previous = splits.front().second;
        indices[t] = edge.a;
        indices[t + 1] = previous;
        indices[t + 2] = c;
        const std::uint64_t ca = (static_cast<std::uint64_t>(c) << 32) | edge.a;
        if (owner.count(ca) != 0) {
            owner[ca] = t;
        }
        for (std::size_t i = 1; i < splits.size(); ++i) {
            indices.insert(indices.end(), {previous, splits[i].second, c});
            previous = splits[i].second;
        }
        const std::size_t last = indices.size();
        indices.insert(indices.end(), {previous, edge.b, c});
        const std::uint64_t bc = (static_cast<std::uint64_t>(edge.b) << 32) | c;
        if (owner.count(bc) != 0) {
            owner[bc] = last;
        }
        owner.erase(found);
    }
}

}  // namespace

float target_error(int level, float voxel_size) { return 0.25f * voxel_size * static_cast<float>(1 << level); }

LodResult build_node(const LodInput& input) {
    if (!input.compact_children.empty()) {
        // Final review: unpacked here, on the worker, not by LodTree on
        // SimulationThread under the write lock (R12: the job owns the
        // unpacked copies and they go when the build is done).
        LodInput unpacked = input;
        unpacked.compact_children.clear();
        unpacked.children.reserve(input.children.size() + input.compact_children.size());
        for (const auto& compact : input.compact_children) {
            unpacked.children.push_back(compact != nullptr
                                            ? std::make_shared<const anarchy::amesh::Data>(unpack(*compact))
                                            : nullptr);
        }
        return build_node(unpacked);
    }
    LodResult result;
    result.key = input.key;

    std::size_t total_vertices = 0, total_indices = 0;
    for (const auto& child : input.children) {
        if (child) {
            total_vertices += child->vertices.size();
            total_indices += child->indices.size();  // upper bound (pre-R10-slice); fine for reserve()
        }
    }
    if (total_indices == 0) {
        return result;  // null mesh: nothing to merge
    }

    // Concatenate children, offsetting each one's indices by the vertices
    // already appended. R10: merge only a child's surface part -- its first
    // child_surface_index_counts[i] indices (or, missing that entry, the
    // whole mesh: a level-0 chunk mesh has no skirts). A child's skirt
    // vertices, now referenced by nothing in merged_indices, are dropped
    // for free by weld_vertices below (it only assigns a
    // destination index to a vertex actually visited through the index
    // buffer), so a coarser level never inherits a finer level's skirts.
    std::vector<anarchy::amesh::Vertex> merged_vertices;
    std::vector<unsigned int> merged_indices;
    std::vector<std::uint32_t> merged_children;  // per merged vertex: a bit for the child it came from
    merged_vertices.reserve(total_vertices);
    merged_indices.reserve(total_indices);
    for (std::size_t c = 0; c < input.children.size(); ++c) {
        const auto& child = input.children[c];
        if (!child) {
            continue;
        }
        const std::uint32_t offset = static_cast<std::uint32_t>(merged_vertices.size());
        merged_vertices.insert(merged_vertices.end(), child->vertices.begin(), child->vertices.end());
        merged_children.insert(merged_children.end(), child->vertices.size(), std::uint32_t{1} << (c % 32));
        const std::size_t use_count = c < input.child_surface_index_counts.size()
                                           ? std::min<std::size_t>(input.child_surface_index_counts[c], child->indices.size())
                                           : child->indices.size();
        for (std::size_t i = 0; i < use_count; ++i) {
            merged_indices.push_back(child->indices[i] + offset);
        }
    }
    if (merged_indices.empty()) {
        return result;  // every child was pure skirt (shouldn't happen in practice): null mesh, same contract
    }

    // Weld positions within weld_tolerance (R21): siblings' shared border
    // vertices come back from their own CompactMesh boxes quantized
    // differently, so they no longer match bit for bit.
    std::vector<anarchy::amesh::Vertex> welded;
    std::vector<unsigned int> welded_indices;
    std::vector<std::uint32_t> welded_children;
    const float weld_step = weld_tolerance(input);
    weld_vertices(merged_vertices, merged_indices, merged_children, weld_step, welded, welded_indices, welded_children);
    if (welded_indices.empty()) {
        return result;
    }

    float max_child_error = 0.f;
    for (std::size_t i = 0; i < input.children.size(); ++i) {
        const float child_error = i < input.child_errors.size() ? input.child_errors[i] : 0.f;
        max_child_error = std::max(max_child_error, child_error);
    }

    // Close the seams between simplified siblings (R21): borders simplify
    // freely, so two siblings keep different subsets of their shared
    // border's vertices, and welding alone leaves a sliver open between the
    // two polylines. Exact (level-0) children share every border vertex, so
    // there is nothing to stitch for them.
    if (max_child_error > 0.f) {
        stitch_seams(welded, welded_indices, welded_children, 2.f * max_child_error + weld_step, input.voxel_size);
    }

    // Simplify to this level's error budget. R8: the recorded error is
    // cumulative (this level's own honest measured distance, plus the worst
    // of the children's own already-recorded errors), so compute that floor
    // before simplifying at all.
    const float target = target_error(input.key.level, input.voxel_size);
    const float scale = meshopt_simplifyScale(&welded[0].p[0], welded.size(), sizeof(anarchy::amesh::Vertex));
    float normalized_target_error = scale > 0.f ? target / scale : 0.f;
    const std::size_t target_index_count = (welded_indices.size() / 4 / 3) * 3;

    // R8: up to 4 attempts, each halving meshopt's own target error (the
    // index-count target never changes) if the previous attempt's honest
    // recorded error came in over budget. Keep the first attempt within
    // budget; failing that, keep whichever attempt recorded the lowest error.
    // Kept as a plain Data (not yet a shared_ptr<const ...>) since the kept
    // attempt still needs re-shading and skirts appended below.
    anarchy::amesh::Data best_mesh;
    bool have_best = false;
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

        const float measured = max_child_distance(input.children, input.child_surface_index_counts, mesh);
        const float candidate_error = measured + max_child_error;
        const bool within_budget = candidate_error <= target;
        if (within_budget || candidate_error < best_error) {
            best_error = candidate_error;
            best_mesh = std::move(mesh);
            have_best = true;
        }
        if (within_budget) {
            break;
        }
        normalized_target_error *= 0.5f;
    }

    // have_best is always true here: the very first attempt always satisfies
    // "candidate_error < best_error" (best_error starts at float max).
    result.error = best_error;
    // A fully-collapsed attempt (every attempt simplified away to 0
    // triangles) reports a null mesh, same as an empty input -- the
    // contract is that null means no triangles, full stop.
    if (!have_best || best_mesh.indices.empty()) {
        return result;
    }

    // Re-shade from the full-resolution voxel field, if given: positions
    // stay put, but every vertex's normal and Id are read back from voxels
    // rather than kept from whichever child contributed that welded vertex
    // (which, after simplification, may not even be one of this mesh's own
    // positions' original sources any more).
    if (input.voxels) {
        const VoxelSampler sampler(*input.voxels, input.voxel_size);
        reshade_vertices(best_mesh, sampler);
        orient_triangle_materials(best_mesh, sampler);
    }

    // Border edges, as the simplified (and possibly re-shaded) surface
    // stands before skirts are appended -- this is what the result reports.
    std::vector<std::uint32_t> border_thirds;
    result.border_edges = collect_border_edges(best_mesh, border_thirds);
    // R10: the surface's own index count, before add_skirts() appends more
    // below -- reported so a parent build can merge just this part.
    result.surface_index_count = static_cast<std::uint32_t>(best_mesh.indices.size());

    // Skirts: one flanged quad per border edge (R21), hanging down -normal by
    // this node's own recorded error (R8) and the input voxel size, and
    // leaning out by half that.
    const float skirt_depth = std::max(2.f * result.error, input.voxel_size);
    add_skirts(best_mesh, result.border_edges, border_thirds, skirt_depth);
    anarchy::amesh::compute_aabb(best_mesh);  // skirt vertices can lie outside the pre-skirt AABB

    result.mesh = std::make_shared<const anarchy::amesh::Data>(std::move(best_mesh));
    return result;
}

}  // namespace engine_core::terrain
