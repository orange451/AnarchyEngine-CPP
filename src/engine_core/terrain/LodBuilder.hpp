#pragma once

// Task 2 of the terrain LOD plan: LodBuilder merges a node's children's
// meshes, simplifies the result with meshoptimizer to a level-appropriate
// error budget, and collects the merged mesh's border edges (for Task 3's
// skirts). Pure function of its LodInput: no octree, no voxel reads yet
// (voxels is accepted but unused until Task 3's re-shading). See docs/
// superpowers/specs/2026-10-06-terrain-lod-design.md's Implementation notes.

#include "amesh.hpp"
#include "terrain/LodNode.hpp"
#include "terrain/VoxelVolume.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace engine_core::terrain {

// What build_node merges: key's existing children's meshes (unpacked), and
// (for Task 3's re-shading) the voxels they came from.
struct LodInput {
    NodeKey key;
    float voxel_size = 1.f;
    std::vector<std::shared_ptr<const anarchy::amesh::Data>> children;  // existing children's meshes
    // children's own recorded error (R8), parallel to children; an entry
    // missing (vector shorter than children, or empty altogether) counts as
    // 0, which is always correct for a level-0 child (an exact chunk mesh).
    std::vector<float> child_errors;
    std::shared_ptr<const ChunkMap> voxels;                              // for re-shading (Task 3)
};

struct LodResult {
    NodeKey key;
    std::shared_ptr<const anarchy::amesh::Data> mesh;  // null: no triangles
    float error = 0.f;                                  // studs
    std::vector<std::uint32_t> border_edges;            // pairs of vertex indices
};

// The level's simplification budget, in studs: 0.25 * voxel_size * 2^level.
float target_error(int level, float voxel_size);

// Concatenates input's children (offsetting indices), welds exactly-equal
// positions, simplifies to target_error(input.key.level, input.voxel_size)
// with meshoptimizer, compacts, and collects border edges (edges used by
// exactly one triangle). The recorded error is a cumulative bound: this
// level's own honest measured distance (every child vertex's distance to the
// simplified surface) plus the worst of input.child_errors (R8). If that
// exceeds target_error, build_node retries from the welded mesh with
// meshopt's own target error halved, up to 4 attempts in total; it keeps the
// first attempt within budget, or, failing that, the attempt with the
// lowest recorded error. Callable from any thread. Task 3 adds re-shading
// from input.voxels and skirts.
LodResult build_node(const LodInput& input);

}  // namespace engine_core::terrain
