#include "terrain/SurfaceNets.hpp"

#include "terrain/VoxelVolume.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine_core::terrain {
namespace {

// Samples run -2..33 (chunk-local): the chunk's own 0..31 plus a 2-sample
// apron on each side, so a cell's gradient (needing samples +/-1 around its
// vertex, which itself needs samples +/-1 around a cell) never runs dry.
constexpr int kApron = 2;
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
    distances.assign(total, 0.f);
    ids.assign(total, 0);
    const float voxel_size = input.voxel_size;
    for (int k = kSampleMin; k <= kSampleMax; ++k) {
        const Local lk = locate(k);
        for (int j = kSampleMin; j <= kSampleMax; ++j) {
            const Local lj = locate(j);
            for (int i = kSampleMin; i <= kSampleMax; ++i) {
                const Local li = locate(i);
                const std::size_t neighbor = static_cast<std::size_t>((lk.offset + 1) * 9 + (lj.offset + 1) * 3 + (li.offset + 1));
                const ChunkPtr& chunk = input.neighbors[neighbor];
                const Cell cell = chunk ? chunk->cell(cell_index(li.index, lj.index, lk.index)) : Cell{};
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

// The distance field, trilinearly interpolated between samples, at an
// arbitrary point in chunk-local sample space.
float trilinear(const std::vector<float>& distances, float x, float y, float z) {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const float fz = z - static_cast<float>(iz);
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
// field, at a point in chunk-local sample space. Points from solid to air.
Vec3 gradient(const std::vector<float>& distances, float x, float y, float z) {
    const float dx = trilinear(distances, x + 1.f, y, z) - trilinear(distances, x - 1.f, y, z);
    const float dy = trilinear(distances, x, y + 1.f, z) - trilinear(distances, x, y - 1.f, z);
    const float dz = trilinear(distances, x, y, z + 1.f) - trilinear(distances, x, y, z - 1.f);
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
    const float abs_x = static_cast<float>(input.coord.x * kChunkSize);
    const float abs_y = static_cast<float>(input.coord.y * kChunkSize);
    const float abs_z = static_cast<float>(input.coord.z * kChunkSize);

    for (int k = kCellMin; k <= kCellMax; ++k) {
        for (int j = kCellMin; j <= kCellMax; ++j) {
            for (int i = kCellMin; i <= kCellMax; ++i) {
                float d[8];
                int mask = 0;
                for (int c = 0; c < 8; ++c) {
                    d[c] = distances[sample_index(i + kCorner[c][0], j + kCorner[c][1], k + kCorner[c][2])];
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
                const float px = static_cast<float>(i) + sum_x / static_cast<float>(count);
                const float py = static_cast<float>(j) + sum_y / static_cast<float>(count);
                const float pz = static_cast<float>(k) + sum_z / static_cast<float>(count);

                int lowest = 0;
                for (int c = 1; c < 8; ++c) {
                    if (d[c] < d[lowest]) {
                        lowest = c;
                    }
                }
                const std::uint8_t id = ids[sample_index(i + kCorner[lowest][0], j + kCorner[lowest][1], k + kCorner[lowest][2])];
                const Vec3 normal = gradient(distances, px, py, pz);

                anarchy::amesh::Vertex vertex;
                vertex.p[0] = (abs_x + px) * voxel_size;
                vertex.p[1] = (abs_y + py) * voxel_size;
                vertex.p[2] = (abs_z + pz) * voxel_size;
                vertex.n[0] = normal.x;
                vertex.n[1] = normal.y;
                vertex.n[2] = normal.z;
                vertex.t[0] = 1.f;
                vertex.t[1] = 0.f;
                vertex.t[2] = 0.f;
                vertex.t[3] = 0.f;
                vertex.rgba[0] = id;
                vertex.rgba[1] = 0;
                vertex.rgba[2] = 0;
                vertex.rgba[3] = 0;

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

    // Reused per calling thread, so back-to-back chunks (the mesher pool's
    // normal case) pay no repeated malloc/free for these scratch buffers.
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
    mesh.render = std::make_shared<const anarchy::amesh::Data>(std::move(render));
    return mesh;
}

}  // namespace engine_core::terrain
