#include "terrain/SurfaceNets.hpp"

#include "terrain/BlendWeights.hpp"
#include "terrain/VoxelVolume.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace engine_core::terrain {
namespace {

// Samples run -2..33 (chunk-local): the chunk's own 0..31 plus a 2-sample
// apron on each side, so a cell's gradient (needing samples +/-1 around its
// vertex, which itself needs samples +/-1 around a cell) never runs dry.
constexpr int kApron = kMeshReach;
constexpr int kSampleMin = -kApron;
constexpr int kSampleMax = kChunkSize - 1 + kApron;  // 33
constexpr int kSamplesPerAxis = kChunkSize + 2 * kApron;  // 36

// Cells run -1..31: every cell touched by an edge this chunk owns (0..31)
// plus the one cell outside each owned face, whose vertex a boundary quad needs.
constexpr int kCellMin = -1;
constexpr int kCellMax = kChunkSize - 1;  // 31
constexpr int kCellsPerAxis = kChunkSize + 1;  // 33

constexpr std::uint32_t kNoVertex = std::numeric_limits<std::uint32_t>::max();

std::size_t sample_index(int i, int j, int k) {
    const std::size_t si = static_cast<std::size_t>(i - kSampleMin);
    const std::size_t sj = static_cast<std::size_t>(j - kSampleMin);
    const std::size_t sk = static_cast<std::size_t>(k - kSampleMin);
    return si + kSamplesPerAxis * (sj + kSamplesPerAxis * sk);
}

std::size_t vertex_table_index(int i, int j, int k) {
    const std::size_t vi = static_cast<std::size_t>(i - kCellMin);
    const std::size_t vj = static_cast<std::size_t>(j - kCellMin);
    const std::size_t vk = static_cast<std::size_t>(k - kCellMin);
    return vi + kCellsPerAxis * (vj + kCellsPerAxis * vk);
}

// Which neighbor chunk (offset -1, 0, 1 on each axis) holds chunk-local
// coordinate i, and that chunk's own local index for it.
struct Local {
    int offset;
    int index;
};
Local locate(int i) {
    if (i < 0) return Local{-1, i + kChunkSize};
    if (i >= kChunkSize) return Local{1, i - kChunkSize};
    return Local{0, i};
}

// True if every neighbor is uniform (or absent, i.e. air) and they all agree
// on which side of zero they are: nothing here can ever cross the surface.
bool quick_reject(const MeshInput& input) {
    bool have_sign = false;
    bool solid = false;
    for (const ChunkPtr& neighbor : input.neighbors) {
        if (neighbor && !neighbor->is_uniform()) {
            return false;
        }
        const Cell cell = neighbor ? neighbor->cell(0) : Cell{};
        const bool is_solid = cell.distance < 0;
        if (!have_sign) {
            have_sign = true;
            solid = is_solid;
        } else if (is_solid != solid) {
            return false;
        }
    }
    return true;
}

// Copies the samples this chunk's cells and edges can touch into flat arrays,
// one dequantize per sample, so the hot loops below index memory instead of
// re-deriving which neighbor and which cell each sample belongs to.
void fill_samples(const MeshInput& input, std::vector<float>& distances, std::vector<std::uint8_t>& ids) {
    const std::size_t total = static_cast<std::size_t>(kSamplesPerAxis) * kSamplesPerAxis * kSamplesPerAxis;
    // resize, not assign: every element below is unconditionally overwritten
    // (there is no "leave as default" case), so there is no need to also
    // pay to fill newly-grown elements with 0.f/0 first.
    distances.resize(total);
    ids.resize(total);
    const float voxel_size = input.voxel_size;
    // Each dense neighbor's cells, pinned once for the whole fill rather
    // than per sample (they may live only in the ChunkCache).
    std::array<CellsPtr, 27> pins;
    for (std::size_t n = 0; n < pins.size(); ++n) {
        if (input.neighbors[n]) {
            pins[n] = input.neighbors[n]->cells();
        }
    }
    for (int k = kSampleMin; k <= kSampleMax; ++k) {
        const Local lk = locate(k);
        for (int j = kSampleMin; j <= kSampleMax; ++j) {
            const Local lj = locate(j);
            for (int i = kSampleMin; i <= kSampleMax; ++i) {
                const Local li = locate(i);
                const std::size_t neighbor = static_cast<std::size_t>((lk.offset + 1) * 9 + (lj.offset + 1) * 3 + (li.offset + 1));
                const ChunkPtr& chunk = input.neighbors[neighbor];
                const CellsPtr& cells = pins[neighbor];
                const Cell cell = cells   ? (*cells)[static_cast<std::size_t>(cell_index(li.index, lj.index, lk.index))]
                                  : chunk ? chunk->cell(0)
                                          : Cell{};
                const std::size_t index = sample_index(i, j, k);
                distances[index] = dequantize(cell.distance, voxel_size);
                ids[index] = cell.material;
            }
        }
    }
}

float sample_distance(const std::vector<float>& distances, int i, int j, int k) {
    i = std::clamp(i, kSampleMin, kSampleMax);
    j = std::clamp(j, kSampleMin, kSampleMax);
    k = std::clamp(k, kSampleMin, kSampleMax);
    return distances[sample_index(i, j, k)];
}

// The distance field, trilinearly interpolated between samples, at the point
// (local sample ix,iy,iz) + (fx,fy,fz) (each fraction in [0,1]). Taking the
// integer cell and the fraction as separate arguments -- instead of a single
// combined float coordinate -- means two chunks meshing the same physical
// point with the same fraction (as build_vertices guarantees: fx/fy/fz come
// straight from the edge-crossing average, never combined with a
// chunk-local index first) get bit-identical fx/fy/fz here, regardless of
// how large ix is in either chunk's local frame. Combining first and taking
// floor()/subtracting back out would round fx/fy/fz differently depending on
// ix's magnitude, which is exactly what made two chunks' shared boundary
// vertices (and their normals) disagree.
float trilinear(const std::vector<float>& distances, int ix, int iy, int iz, float fx, float fy, float fz) {
    const float c000 = sample_distance(distances, ix, iy, iz);
    const float c100 = sample_distance(distances, ix + 1, iy, iz);
    const float c010 = sample_distance(distances, ix, iy + 1, iz);
    const float c110 = sample_distance(distances, ix + 1, iy + 1, iz);
    const float c001 = sample_distance(distances, ix, iy, iz + 1);
    const float c101 = sample_distance(distances, ix + 1, iy, iz + 1);
    const float c011 = sample_distance(distances, ix, iy + 1, iz + 1);
    const float c111 = sample_distance(distances, ix + 1, iy + 1, iz + 1);
    const float c00 = c000 + (c100 - c000) * fx;
    const float c10 = c010 + (c110 - c010) * fx;
    const float c01 = c001 + (c101 - c001) * fx;
    const float c11 = c011 + (c111 - c011) * fx;
    const float c0 = c00 + (c10 - c00) * fy;
    const float c1 = c01 + (c11 - c01) * fy;
    return c0 + (c1 - c0) * fz;
}

// The normalized central-difference gradient of the trilinearly interpolated
// field, at (local sample ix,iy,iz) + (fx,fy,fz). Points from solid to air.
// Shifting ix/iy/iz by a whole sample (rather than adding 1 to a combined
// float coordinate) keeps the fraction bit-identical to the one used for the
// vertex position, for the same reason given on trilinear() above.
Vec3 gradient(const std::vector<float>& distances, int ix, int iy, int iz, float fx, float fy, float fz) {
    const float dx = trilinear(distances, ix + 1, iy, iz, fx, fy, fz) - trilinear(distances, ix - 1, iy, iz, fx, fy, fz);
    const float dy = trilinear(distances, ix, iy + 1, iz, fx, fy, fz) - trilinear(distances, ix, iy - 1, iz, fx, fy, fz);
    const float dz = trilinear(distances, ix, iy, iz + 1, fx, fy, fz) - trilinear(distances, ix, iy, iz - 1, fx, fy, fz);
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(length > 0.f)) {
        return Vec3{0.f, 0.f, 1.f};
    }
    return Vec3{dx / length, dy / length, dz / length};
}

// The cube's 8 corners, (x, y, z) offsets from the cell's own sample, in the
// bit order a mask (bit i set when corner i is solid) uses: bit 0 is x, 1 y, 2 z.
constexpr int kCorner[8][3] = {
    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
};
// The 12 edges, as pairs of corner indices: 4 along x (bit 0 differs), then
// 4 along y (bit 1), then 4 along z (bit 2).
constexpr int kEdge[12][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
};
constexpr int kUnit[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

float distance_squared(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

// Every cell's vertex, if its 8 corners don't all share a sign: the average
// of its edges' zero crossings, its Id from the corner with the lowest
// distance, and its normal from the field's gradient there. Fills vtable
// with each meshed cell's vertex index (kNoVertex otherwise).
void build_vertices(const std::vector<float>& distances, const std::vector<std::uint8_t>& ids, const MeshInput& input,
                     anarchy::amesh::Data& render, std::vector<Vec3>& positions, std::vector<std::uint32_t>& vtable) {
    const float voxel_size = input.voxel_size;
    // Chunk-absolute cell coordinates, as integers: computing these (exact,
    // no rounding) before any float arithmetic is what lets two chunks that
    // share a boundary cell agree on its absolute index bit-for-bit.
    const int base_x = input.coord.x * kChunkSize;
    const int base_y = input.coord.y * kChunkSize;
    const int base_z = input.coord.z * kChunkSize;

    for (int k = kCellMin; k <= kCellMax; ++k) {
        for (int j = kCellMin; j <= kCellMax; ++j) {
            for (int i = kCellMin; i <= kCellMax; ++i) {
                float d[8];
                std::uint8_t corner_ids[8];
                int mask = 0;
                for (int c = 0; c < 8; ++c) {
                    const std::size_t index = sample_index(i + kCorner[c][0], j + kCorner[c][1], k + kCorner[c][2]);
                    d[c] = distances[index];
                    corner_ids[c] = ids[index];
                    if (d[c] < 0.f) {
                        mask |= 1 << c;
                    }
                }
                if (mask == 0 || mask == 0xFF) {
                    continue;  // all 8 corners agree: no surface through this cell
                }

                float sum_x = 0.f, sum_y = 0.f, sum_z = 0.f;
                int count = 0;
                for (const auto& edge : kEdge) {
                    const float da = d[edge[0]];
                    const float db = d[edge[1]];
                    if ((da < 0.f) == (db < 0.f)) {
                        continue;
                    }
                    const float t = da / (da - db);
                    const int* a = kCorner[edge[0]];
                    const int* b = kCorner[edge[1]];
                    sum_x += static_cast<float>(a[0]) + t * static_cast<float>(b[0] - a[0]);
                    sum_y += static_cast<float>(a[1]) + t * static_cast<float>(b[1] - a[1]);
                    sum_z += static_cast<float>(a[2]) + t * static_cast<float>(b[2] - a[2]);
                    ++count;
                }
                if (count == 0) {
                    continue;  // unreachable for a cube, but never divide by zero
                }
                // The cell-local fraction, from the edge-crossing average
                // alone -- never combined with the (chunk-sized) cell index
                // i/j/k. Same corner distances in, same fx/fy/fz out,
                // regardless of which chunk's local i/j/k got us here.
                const float fx = sum_x / static_cast<float>(count);
                const float fy = sum_y / static_cast<float>(count);
                const float fz = sum_z / static_cast<float>(count);

                const BlendIds blend = blend_weights(d, corner_ids, voxel_size);
                const Vec3 normal = gradient(distances, i, j, k, fx, fy, fz);

                // Absolute integer cell + fraction, combined in one float
                // addition: for the same physical cell this is the same
                // (ax, fx) pair in every chunk that touches it, so the
                // result is bit-identical no matter which chunk computed it.
                const int ax = base_x + i;
                const int ay = base_y + j;
                const int az = base_z + k;

                anarchy::amesh::Vertex vertex;
                vertex.p[0] = (static_cast<float>(ax) + fx) * voxel_size;
                vertex.p[1] = (static_cast<float>(ay) + fy) * voxel_size;
                vertex.p[2] = (static_cast<float>(az) + fz) * voxel_size;
                vertex.n[0] = normal.x;
                vertex.n[1] = normal.y;
                vertex.n[2] = normal.z;
                for (int s = 0; s < 4; ++s) {
                    vertex.t[s] = blend.weights[s];
                    vertex.rgba[s] = blend.ids[s];
                }

                const std::uint32_t vertex_index = static_cast<std::uint32_t>(render.vertices.size());
                render.vertices.push_back(vertex);
                positions.push_back(Vec3{vertex.p[0], vertex.p[1], vertex.p[2]});
                vtable[vertex_table_index(i, j, k)] = vertex_index;
            }
        }
    }
}

// Every lattice edge this chunk owns (lower endpoint 0..31 on all three
// axes) whose two samples differ in sign becomes a quad of the 4 cells
// sharing it, wound solid-to-air and split along its shorter diagonal.
void build_faces(const std::vector<float>& distances, const std::vector<std::uint8_t>& ids,
                  const std::vector<Vec3>& positions, const std::vector<std::uint32_t>& vtable, ChunkMesh& mesh) {
    for (int k = 0; k < kChunkSize; ++k) {
        for (int j = 0; j < kChunkSize; ++j) {
            for (int i = 0; i < kChunkSize; ++i) {
                for (int axis = 0; axis < 3; ++axis) {
                    const int bi = i + kUnit[axis][0];
                    const int bj = j + kUnit[axis][1];
                    const int bk = k + kUnit[axis][2];
                    const float da = distances[sample_index(i, j, k)];
                    const float db = distances[sample_index(bi, bj, bk)];
                    const bool solid_a = da < 0.f;
                    if (solid_a == (db < 0.f)) {
                        continue;
                    }

                    const int iu = (axis + 1) % 3;
                    const int iv = (axis + 2) % 3;
                    const int c0i = i, c0j = j, c0k = k;
                    const int c1i = i - kUnit[iu][0], c1j = j - kUnit[iu][1], c1k = k - kUnit[iu][2];
                    const int c3i = i - kUnit[iv][0], c3j = j - kUnit[iv][1], c3k = k - kUnit[iv][2];
                    const int c2i = c1i - kUnit[iv][0], c2j = c1j - kUnit[iv][1], c2k = c1k - kUnit[iv][2];

                    const std::uint32_t v0 = vtable[vertex_table_index(c0i, c0j, c0k)];
                    const std::uint32_t v1 = vtable[vertex_table_index(c1i, c1j, c1k)];
                    const std::uint32_t v2 = vtable[vertex_table_index(c2i, c2j, c2k)];
                    const std::uint32_t v3 = vtable[vertex_table_index(c3i, c3j, c3k)];

                    // Solid at the edge's lower sample: wind c0,c1,c2,c3; air
                    // there (so solid at the upper sample): reverse it, so
                    // the quad always faces from solid toward air.
                    const std::uint32_t q0 = v0;
                    const std::uint32_t q1 = solid_a ? v1 : v3;
                    const std::uint32_t q2 = v2;
                    const std::uint32_t q3 = solid_a ? v3 : v1;
                    const std::uint8_t edge_id = solid_a ? ids[sample_index(i, j, k)] : ids[sample_index(bi, bj, bk)];

                    const Vec3& p0 = positions[q0];
                    const Vec3& p1 = positions[q1];
                    const Vec3& p2 = positions[q2];
                    const Vec3& p3 = positions[q3];
                    if (distance_squared(p0, p2) <= distance_squared(p1, p3)) {
                        mesh.triangles.insert(mesh.triangles.end(), {q0, q1, q2, q0, q2, q3});
                    } else {
                        mesh.triangles.insert(mesh.triangles.end(), {q0, q1, q3, q1, q2, q3});
                    }
                    mesh.triangle_ids.push_back(edge_id);
                    mesh.triangle_ids.push_back(edge_id);
                }
            }
        }
    }
}

}  // namespace

MeshInput mesh_input(const VoxelVolume& volume, ChunkCoord coord) {
    MeshInput input;
    input.coord = coord;
    input.voxel_size = volume.voxel_size();
    const ChunkMap& chunks = volume.chunks();
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const ChunkCoord neighbor_coord{coord.x + dx, coord.y + dy, coord.z + dz};
                const auto found = chunks.find(neighbor_coord);
                const std::size_t index = static_cast<std::size_t>((dz + 1) * 9 + (dy + 1) * 3 + (dx + 1));
                input.neighbors[index] = found != chunks.end() ? found->second : nullptr;
            }
        }
    }
    return input;
}

ChunkMesh surface_nets(const MeshInput& input) {
    ChunkMesh mesh;
    if (quick_reject(input)) {
        return mesh;
    }

    // Reused per calling thread: every call fully rewrites these buffers
    // before reading them back (fill_samples overwrites every sample below;
    // vtable is reset to kNoVertex on the next line), so nothing from a
    // previous call ever leaks into this one -- that's what makes sharing
    // them across calls safe. thread_local means each thread that calls
    // surface_nets gets its own copy (no two threads ever touch the same
    // one) and keeps it allocated -- about 377 KB (36^3 floats + 36^3 bytes
    // + 33^3 uint32s) -- until that thread exits, instead of paying a
    // malloc/free pair per chunk when the mesher pool meshes many chunks
    // back-to-back on one thread. Because the buffers are shared per thread
    // across calls, surface_nets must never re-enter itself on the same
    // thread (directly, or indirectly via some callback invoked during its
    // own call) -- a nested call would clobber the outer call's
    // in-progress buffers out from under it.
    thread_local std::vector<float> distances;
    thread_local std::vector<std::uint8_t> ids;
    thread_local std::vector<std::uint32_t> vtable;
    fill_samples(input, distances, ids);
    vtable.assign(static_cast<std::size_t>(kCellsPerAxis) * kCellsPerAxis * kCellsPerAxis, kNoVertex);

    anarchy::amesh::Data render;
    build_vertices(distances, ids, input, render, mesh.positions, vtable);
    build_faces(distances, ids, mesh.positions, vtable, mesh);

    if (mesh.triangles.empty()) {
        return mesh;  // render stays null: nothing to draw or collide with
    }
    render.indices = mesh.triangles;
    anarchy::amesh::compute_aabb(render);
    // Decision 1 (terrain textures): split border triangles -- whose three
    // vertices do not carry the same material set -- into their own
    // vertices, so the shader can blend without a geometry shader. Render
    // mesh only: mesh.positions/mesh.triangles (collision) were already
    // captured above and are untouched by this.
    split_border_triangles(render);
    mesh.render = std::make_shared<const anarchy::amesh::Data>(std::move(render));
    return mesh;
}

}  // namespace engine_core::terrain
