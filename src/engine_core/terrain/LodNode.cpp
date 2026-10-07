#include "terrain/LodNode.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace engine_core::terrain {

namespace {

// Floor division by a positive divisor (unlike C++'s truncating /), the same
// shape as VoxelChunk.cpp's file-local floor_div for chunk_of.
int floor_div(int value, int by) { return value >= 0 ? value / by : -((-value + by - 1) / by); }

// Quantizes p in [origin, origin + scale] to 0..65535. scale == 0 is a
// degenerate axis (the node's box is flat on it): quantize to 0 rather than
// divide by zero.
std::uint16_t quantize_axis(float p, float origin, float scale) {
    if (scale == 0.f) {
        return 0;
    }
    const float t = (p - origin) / scale;
    const float clamped = std::clamp(t, 0.f, 1.f);
    return static_cast<std::uint16_t>(std::lround(clamped * 65535.f));
}

std::uint8_t quantize_unit(float v) {
    const float clamped = std::clamp(v, 0.f, 1.f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.f));
}

float sign_or_positive(float v) { return v < 0.f ? -1.f : 1.f; }

// Octahedral normal encoding: a unit vector to two bytes. Standard
// fold-the-octahedron-into-a-square mapping (Meyer et al.), quantized to
// 8 bits per axis.
void encode_octahedral(float x, float y, float z, std::uint8_t& ex, std::uint8_t& ey) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-8f) {
        // No direction to encode: pack as (0, 0, 1), the octahedron's pole.
        ex = quantize_unit(0.5f);
        ey = quantize_unit(0.5f);
        return;
    }
    const float nx = x / len, ny = y / len, nz = z / len;
    const float abs_sum = std::fabs(nx) + std::fabs(ny) + std::fabs(nz);
    float ox = nx / abs_sum;
    float oy = ny / abs_sum;
    if (nz < 0.f) {
        const float wrapped_x = (1.f - std::fabs(oy)) * sign_or_positive(ox);
        const float wrapped_y = (1.f - std::fabs(ox)) * sign_or_positive(oy);
        ox = wrapped_x;
        oy = wrapped_y;
    }
    ex = quantize_unit(ox * 0.5f + 0.5f);
    ey = quantize_unit(oy * 0.5f + 0.5f);
}

void decode_octahedral(std::uint8_t ex, std::uint8_t ey, float& x, float& y, float& z) {
    const float ox = static_cast<float>(ex) / 255.f * 2.f - 1.f;
    const float oy = static_cast<float>(ey) / 255.f * 2.f - 1.f;
    float nx = ox, ny = oy;
    const float nz = 1.f - std::fabs(ox) - std::fabs(oy);
    if (nz < 0.f) {
        nx = (1.f - std::fabs(oy)) * sign_or_positive(ox);
        ny = (1.f - std::fabs(ox)) * sign_or_positive(oy);
    }
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-8f) {
        x = 0.f;
        y = 0.f;
        z = 1.f;
        return;
    }
    x = nx / len;
    y = ny / len;
    z = nz / len;
}

}  // namespace

std::size_t NodeKeyHash::operator()(const NodeKey& k) const {
    std::size_t h = static_cast<std::size_t>(static_cast<std::uint32_t>(k.level)) * 2654435761u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(k.x)) * 73856093u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(k.y)) * 19349663u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(k.z)) * 83492791u;
    return h;
}

NodeKey node_of(ChunkCoord chunk, int level) {
    const int by = 1 << level;
    return NodeKey{level, floor_div(chunk.x, by), floor_div(chunk.y, by), floor_div(chunk.z, by)};
}

NodeKey parent_of(const NodeKey& key) {
    return NodeKey{key.level + 1, floor_div(key.x, 2), floor_div(key.y, 2), floor_div(key.z, 2)};
}

std::array<NodeKey, 8> children_of(const NodeKey& key) {
    assert(key.level >= 1 && "children_of requires key.level >= 1");
    std::array<NodeKey, 8> out;
    const int level = key.level - 1;
    int i = 0;
    for (int dz = 0; dz <= 1; ++dz) {
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                out[static_cast<std::size_t>(i)] = NodeKey{level, key.x * 2 + dx, key.y * 2 + dy, key.z * 2 + dz};
                ++i;
            }
        }
    }
    return out;
}

void node_bounds(const NodeKey& key, float voxel_size, Vec3& min, Vec3& max) {
    const float size = static_cast<float>(kChunkSize * (1 << key.level)) * voxel_size;
    min.x = static_cast<float>(key.x) * size;
    min.y = static_cast<float>(key.y) * size;
    min.z = static_cast<float>(key.z) * size;
    max.x = min.x + size;
    max.y = min.y + size;
    max.z = min.z + size;
}

std::size_t CompactMesh::bytes() const {
    std::size_t total = positions.size() * sizeof(std::uint16_t) + normals.size() * sizeof(std::uint8_t) +
                         ids.size() * sizeof(std::uint8_t) + weights.size() * sizeof(std::uint8_t);
    if (!indices32.empty()) {
        total += indices32.size() * sizeof(std::uint32_t);
    } else {
        total += indices.size() * sizeof(std::uint16_t);
    }
    return total;
}

CompactMesh pack(const anarchy::amesh::Data& mesh, Vec3 bounds_min, Vec3 bounds_max) {
    CompactMesh out;
    out.origin = bounds_min;
    out.scale = Vec3{bounds_max.x - bounds_min.x, bounds_max.y - bounds_min.y, bounds_max.z - bounds_min.z};

    const std::size_t vertex_count = mesh.vertices.size();
    out.positions.resize(vertex_count * 3);
    out.normals.resize(vertex_count * 2);
    out.ids.resize(vertex_count * 4);
    out.weights.resize(vertex_count * 4);

    for (std::size_t i = 0; i < vertex_count; ++i) {
        const anarchy::amesh::Vertex& v = mesh.vertices[i];
        out.positions[i * 3 + 0] = quantize_axis(v.p[0], out.origin.x, out.scale.x);
        out.positions[i * 3 + 1] = quantize_axis(v.p[1], out.origin.y, out.scale.y);
        out.positions[i * 3 + 2] = quantize_axis(v.p[2], out.origin.z, out.scale.z);

        encode_octahedral(v.n[0], v.n[1], v.n[2], out.normals[i * 2 + 0], out.normals[i * 2 + 1]);

        for (int c = 0; c < 4; ++c) {
            out.ids[i * 4 + static_cast<std::size_t>(c)] = v.rgba[c];
            out.weights[i * 4 + static_cast<std::size_t>(c)] = quantize_unit(v.weight[c]);
        }
    }

    if (vertex_count > 65535) {
        out.indices32 = mesh.indices;
    } else {
        out.indices.reserve(mesh.indices.size());
        for (std::uint32_t idx : mesh.indices) {
            out.indices.push_back(static_cast<std::uint16_t>(idx));
        }
    }
    return out;
}

anarchy::amesh::Data unpack(const CompactMesh& mesh) {
    anarchy::amesh::Data out;
    const std::size_t vertex_count = mesh.positions.size() / 3;
    out.vertices.resize(vertex_count);

    for (std::size_t i = 0; i < vertex_count; ++i) {
        anarchy::amesh::Vertex& v = out.vertices[i];
        v.p[0] = mesh.origin.x + static_cast<float>(mesh.positions[i * 3 + 0]) / 65535.f * mesh.scale.x;
        v.p[1] = mesh.origin.y + static_cast<float>(mesh.positions[i * 3 + 1]) / 65535.f * mesh.scale.y;
        v.p[2] = mesh.origin.z + static_cast<float>(mesh.positions[i * 3 + 2]) / 65535.f * mesh.scale.z;

        decode_octahedral(mesh.normals[i * 2 + 0], mesh.normals[i * 2 + 1], v.n[0], v.n[1], v.n[2]);

        for (int c = 0; c < 4; ++c) {
            v.rgba[c] = mesh.ids[i * 4 + static_cast<std::size_t>(c)];
            v.weight[c] = static_cast<float>(mesh.weights[i * 4 + static_cast<std::size_t>(c)]) / 255.f;
        }
    }

    if (!mesh.indices32.empty()) {
        out.indices = mesh.indices32;
    } else {
        out.indices.reserve(mesh.indices.size());
        for (std::uint16_t idx : mesh.indices) {
            out.indices.push_back(idx);
        }
    }

    anarchy::amesh::compute_aabb(out);
    return out;
}

}  // namespace engine_core::terrain
