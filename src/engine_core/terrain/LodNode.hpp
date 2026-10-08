#pragma once

// Terrain LOD node keys and the compact mesh a resident node keeps in RAM.
// A level-0 node is one chunk; a level-L node covers 2^L chunks on a side.
// Pure data and format conversions: no octree, no builder, no voxels here
// (see LodBuilder and LodTree for those). See docs/superpowers/specs/
// 2026-10-06-terrain-lod-design.md's Implementation notes for the five
// decisions this header follows.

#include "Vector3.hpp"
#include "amesh.hpp"
#include "terrain/VoxelChunk.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace engine_core::terrain {

// One octree node's address: level 0 is a chunk; level L's (x, y, z) are in
// units of 2^L chunks, floor-divided (not truncated) from a chunk coordinate.
struct NodeKey {
    int level = 0, x = 0, y = 0, z = 0;
    bool operator==(const NodeKey& o) const { return level == o.level && x == o.x && y == o.y && z == o.z; }
};
struct NodeKeyHash {
    std::size_t operator()(const NodeKey&) const;
};

// chunk's node at level, by floor division of chunk's coordinates by 2^level.
NodeKey node_of(ChunkCoord chunk, int level);
// The node one level up that contains key.
NodeKey parent_of(const NodeKey& key);
// key's 8 children, one level down. key.level must be >= 1.
std::array<NodeKey, 8> children_of(const NodeKey& key);

// key's Terrain-local bounds, in units: level 0 is exactly the chunk's
// 32-cell box; level L is 32 * 2^L cells on a side.
void node_bounds(const NodeKey& key, float voxel_size, Vec3& min, Vec3& max);

// A node's mesh as RAM keeps it: positions quantized to [origin, origin +
// scale] per axis over 16 bits, normals octahedral-packed into two bytes,
// material Ids copied exactly from the source vertex's rgba, and weights
// quantized to 8 bits (/255). Indices are 16-bit unless the node has more
// than 65,535 vertices, in which case indices32 holds them instead and
// indices is empty.
struct CompactMesh {
    Vec3 origin{}, scale{};                  // position = origin + q / 65535 * scale, per axis
    std::vector<std::uint16_t> positions;    // 3 per vertex
    std::vector<std::uint8_t> normals;       // 2 per vertex, octahedral
    std::vector<std::uint8_t> ids;           // 4 per vertex
    std::vector<std::uint8_t> weights;       // 4 per vertex, /255
    std::vector<std::uint16_t> indices;      // 3 per triangle; empty when indices32 is used
    std::vector<std::uint32_t> indices32;    // used instead of indices when vertices.size() > 65535
    // R10: the index count (into indices/indices32) of the surface part,
    // before any skirt triangles. Lets a parent build (LodTree, Task 4)
    // merge only a child's real surface and drop its skirts rather than
    // baking them into every coarser level. A single scalar per node, so it
    // is not counted in bytes()'s per-vertex/per-triangle budget (R1).
    std::uint32_t surface_index_count = 0;
    std::size_t bytes() const;
};

// Quantizes mesh's vertices to [bounds_min, bounds_max] per axis. Callers
// pass the union of the node's bounds and the mesh's own AABB (Surface Nets
// boundary vertices and skirts can lie outside the node's box). A degenerate
// axis (bounds_max == bounds_min) quantizes to 0 on that axis rather than
// dividing by zero. surface_index_count (R10) is clamped to mesh.indices.size()
// and stored as-is; the default (omitted) means the whole mesh is surface.
CompactMesh pack(const anarchy::amesh::Data& mesh, Vec3 bounds_min, Vec3 bounds_max,
                  std::uint32_t surface_index_count = std::numeric_limits<std::uint32_t>::max());
// The inverse of pack(): a full Vertex per entry (tangent, uv and bone left
// at Vertex's defaults -- pack() does not carry them), u32 indices, and the
// AABB set from the unpacked positions.
anarchy::amesh::Data unpack(const CompactMesh& mesh);

}  // namespace engine_core::terrain
