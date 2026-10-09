#pragma once

// What one Terrain's voxels and meshes hold in RAM, for the Configure Terrain
// tab's header. TerrainWorld::memory() reads it; any thread.

#include <cstddef>

namespace engine_core {

struct TerrainMemory {
    // Every live chunk's zstd frame (all Terrains: the frames live with the
    // chunks, not per Terrain).
    std::size_t compressed_voxels = 0;
    // Decoded chunk cells in the ChunkCache, and the cache's budget (also
    // shared by all Terrains).
    std::size_t decoded_cache = 0;
    std::size_t decoded_budget = 0;
    // This Terrain's level-0 chunk meshes in RAM.
    std::size_t chunk_meshes = 0;
    // This Terrain's level >= 1 LOD meshes in RAM.
    std::size_t far_meshes = 0;
};

}  // namespace engine_core
