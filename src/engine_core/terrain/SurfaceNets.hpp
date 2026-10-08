#pragma once

// Surface Nets: turns one chunk's voxels, plus its 26 neighbors (for the
// samples its own edge touches), into a render mesh and collision triangles.
// A pure function of its MeshInput: no chunk map, no locking, callable from
// any thread (the mesher pool, later) once the input is gathered.

#include "Vector3.hpp"
#include "amesh.hpp"
#include "terrain/VoxelChunk.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace engine_core::terrain {

class VoxelVolume;

// What a chunk's mesh is made from: the chunk and its 26 neighbors, as shared
// immutable pointers (null is air). neighbors[(dz+1)*9 + (dy+1)*3 + (dx+1)] is
// the chunk at coord + (dx, dy, dz); index 13 is the chunk itself.
struct MeshInput {
    ChunkCoord coord;
    float voxel_size = 1.f;
    std::array<ChunkPtr, 27> neighbors;
    // Task 8: whether the mesher should run its collider builder for this
    // job. True by default so a caller that never sets it (every existing
    // caller, and anything but TerrainWorld) keeps today's "always build
    // one" behavior; TerrainWorld sets this per job from its collider
    // interest set.
    bool build_collider = true;
};

// Gathers a MeshInput from volume. SimulationThread (it reads the chunk map).
MeshInput mesh_input(const VoxelVolume& volume, ChunkCoord coord);

// One chunk's surface, in Terrain-local space.
struct ChunkMesh {
    std::shared_ptr<const anarchy::amesh::Data> render;  // null when no triangles
    std::vector<Vec3> positions;                         // collision: the same points
    std::vector<std::uint32_t> triangles;                // three per triangle
    std::vector<std::uint8_t> triangle_ids;               // one per triangle
};

// Any thread: reads only input's immutable chunks.
ChunkMesh surface_nets(const MeshInput& input);

}  // namespace engine_core::terrain
