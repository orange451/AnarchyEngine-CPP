#pragma once

// Task 3 of the terrain LOD plan: reads a Terrain's full-resolution voxel
// field at any Terrain-local point, for re-shading a simplified LOD node's
// vertices (distance/gradient/Id) and, later (Task 9), for ray marching. The
// same trilinear/central-difference math SurfaceNets.cpp uses per chunk,
// generalized to an arbitrary point rather than one chunk's own cells, and
// to the whole ChunkMap rather than one chunk's 26 neighbors.

#include "Vector3.hpp"
#include "terrain/BlendWeights.hpp"
#include "terrain/VoxelChunk.hpp"
#include "terrain/VoxelVolume.hpp"

#include <cstdint>

namespace engine_core::terrain {

// Allocation-free per sample: holds only a reference to the ChunkMap and the
// voxel size, both of which must outlive every call. Reads only the
// immutable chunks the map already holds; never mutates the map or any
// chunk. NOT thread-safe, though: a sampler instance caches the last chunk
// it resolved (chunk_at, below) to cut repeated unordered_map lookups, so
// two threads must each construct and keep their own VoxelSampler rather
// than share one (LodBuilder::build_node already does this -- a fresh
// instance local to each call).
// A chunk coordinate absent from the map reads as air -- the same default
// VoxelVolume::cell() and SurfaceNets' fill_samples() give a missing
// neighbor (Cell{}: kAirDistance, material 0).
class VoxelSampler {
public:
    VoxelSampler(const ChunkMap& chunks, float voxel_size) : chunks_(chunks), voxel_size_(voxel_size) {}

    // The signed distance field at p (Terrain-local units), trilinearly
    // interpolated between the 8 cells surrounding it. Negative inside.
    float distance(Vec3 p) const;
    // The normalized gradient of distance() at p, by central differences one
    // voxel_size either side (points from solid to air; {0,0,1} where the
    // field is locally flat, matching SurfaceNets' gradient() fallback).
    Vec3 gradient(Vec3 p) const;
    // The material Id of the cell containing p whose own (unfiltered)
    // distance is lowest among its cell's 8 corners -- the same rule
    // SurfaceNets' build_vertices uses to pick a vertex's Id.
    std::uint8_t id(Vec3 p) const;
    // Task 2 (terrain textures): the same blend_weights() rule, over the 8
    // corners of p's cell (the same 8 cells id() and distance() read).
    BlendIds blend(Vec3 p) const;

private:
    Cell cell_at(int cx, int cy, int cz) const;
    // The chunk holding cell coord, or nullptr if none is mapped (read as
    // air). NOT thread-safe: caches the single most recently resolved
    // chunk, since consecutive lookups overwhelmingly repeat it -- a
    // distance() call's own 8 corners, a gradient() call's 6
    // central-difference taps, and successive vertices reshade_vertices()
    // visits in a row, all typically land in the same chunk away from a
    // chunk boundary. Construct one VoxelSampler per thread/call (build_node
    // already does: a fresh instance local to each build_node call).
    const ChunkData* chunk_at(const ChunkCoord& coord) const;

    const ChunkMap& chunks_;
    float voxel_size_;
    mutable ChunkCoord cached_coord_{};
    mutable const ChunkData* cached_chunk_ = nullptr;
    // cached_chunk_'s cells, pinned while it stays the cached chunk (null
    // for a uniform one).
    mutable CellsPtr cached_cells_;
    mutable bool cached_valid_ = false;
};

}  // namespace engine_core::terrain
