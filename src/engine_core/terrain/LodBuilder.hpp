#pragma once

// Tasks 2-3 of the terrain LOD plan: LodBuilder merges a node's children's
// meshes, simplifies the result with meshoptimizer to a level-appropriate
// error budget, re-shades the kept attempt's vertices from the full-
// resolution voxel field (input.voxels, via VoxelSampler), and hangs a
// flanged skirt quad from every border edge (an edge used by exactly one
// triangle) to hide cracks against a lower-detail neighbor. Pure function of its
// LodInput: no octree. See docs/superpowers/specs/2026-10-06-terrain-lod-
// design.md's Implementation notes.

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
    // Compact (level >= 1) children, unpacked by build_node itself (on the
    // worker) and appended after children: the parallel vectors below index
    // children followed by these. LodTree fills this, not children, for a
    // level >= 2 node.
    std::vector<std::shared_ptr<const CompactMesh>> compact_children;
    // children's own recorded error (R8), parallel to children; an entry
    // missing (vector shorter than children, or empty altogether) counts as
    // 0, which is always correct for a level-0 child (an exact chunk mesh).
    std::vector<float> child_errors;
    // R10 (binding): each child's own surface_index_count (LodResult's
    // field below), parallel to children. An entry missing (vector shorter
    // than children) means the whole child mesh is surface -- always
    // correct for a level-0 chunk mesh, which has no skirts. build_node
    // merges only a child's first child_surface_index_counts[i] indices, so
    // a non-leaf child's own skirt triangles are never baked into this
    // (coarser) level; vertices referenced only by skirts are then dropped
    // by the weld/compaction step since nothing still indexes them.
    std::vector<std::uint32_t> child_surface_index_counts;
    std::shared_ptr<const ChunkMap> voxels;  // full-resolution voxels, for re-shading; null skips it
};

struct LodResult {
    NodeKey key;
    std::shared_ptr<const anarchy::amesh::Data> mesh;  // null: no triangles (includes skirts, once added)
    float error = 0.f;                                  // units
    // The simplified surface's border edges, pairs of vertex indices, as it
    // stood before skirts were appended to mesh (so these indices still
    // address mesh's first vertices/triangles; skirt geometry follows).
    std::vector<std::uint32_t> border_edges;
    // R10: mesh->indices.size() as it stood just before add_skirts()
    // appended the skirt triangles' indices to the tail -- i.e. the index
    // count of this node's real surface. A caller merging this result as a
    // child of a coarser node (via LodInput::child_surface_index_counts)
    // passes this back so the parent can drop the skirts instead of
    // merging them in as if they were real geometry.
    std::uint32_t surface_index_count = 0;
};

// The level's simplification budget, in units: 0.25 * voxel_size * 2^level.
float target_error(int level, float voxel_size);

// Concatenates input's children (offsetting indices), welds positions within
// the children's quantization tolerance and zips the seams between
// simplified siblings (R21: borders simplify freely, so siblings keep
// different vertices of a shared border; each one's border vertices are
// inserted into the other's border edges),
// simplifies to target_error(input.key.level, input.voxel_size)
// with meshoptimizer, and compacts. The recorded error is a cumulative
// bound: this level's own honest measured distance (every child vertex's
// distance to the simplified surface) plus the worst of input.child_errors
// (R8). If that exceeds target_error, build_node retries from the welded
// mesh with meshopt's own target error halved, up to 4 attempts in total; it
// keeps the first attempt within budget, or, failing that, the attempt with
// the lowest recorded error. A fully-collapsed attempt (0 triangles left)
// reports a null mesh, same as an empty input.
//
// On the kept attempt, if input.voxels is non-null, re-shades every vertex
// (positions untouched) from the full-resolution field: normal = the
// distance field's gradient at the vertex's position, and Id = the lowest-
// distance corner's Id of the full-resolution cell around it (both via
// VoxelSampler) -- the same rule Surface Nets itself uses, just read back at
// whatever resolution the simplified vertex landed at. Then collects border
// edges (edges used by exactly one triangle) and, for each one, appends a
// skirt quad down -normal by depth = max(2 * result.error, input.voxel_size)
// and out by depth / 2 (perpendicular to the edge, in its triangle's plane,
// away from the triangle: R21),
// copying the edge's own two vertices' normals and Ids.
// border_edges and surface_index_count are reported as they stood before
// skirts were appended (R10): a caller recursing to a coarser level passes
// surface_index_count back in child_surface_index_counts so this level's own
// skirts aren't merged into the next one up.
//
// Callable from any thread (voxels, like children, is only ever read).
LodResult build_node(const LodInput& input);

// The chunks of chunks a build of key re-shades from: those in its box and
// one chunk around it (its vertices, skirts, and gradient taps all fall
// there). Far fewer than a huge Terrain's whole map, so each node job can
// carry its own instead of a copy of the map. SimulationThread (it reads chunks).
std::shared_ptr<const ChunkMap> node_voxels(const ChunkMap& chunks, const NodeKey& key);

}  // namespace engine_core::terrain
