#pragma once

// Chunk maps named by a token, so place bytes (Play's capture, undo records)
// carry a Terrain's voxels without copying them: the chunks are shared and
// immutable, so an entry holds pointers only and shares every chunk the live
// Terrain has not edited since. Entries live until the project's place is
// rebuilt (Project's Rebuild::finish clears them).

#include "terrain/VoxelVolume.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace engine_core::terrain {

// SimulationThread writes place bytes, but tests and tools may not; every
// call takes a lock, so any thread may call these.
class TerrainStash {
public:
    // A new token, never 0, naming a copy of chunks. A non-empty data_path
    // (the Terrain's DataPath) remembers it as that path's latest token.
    static std::uint64_t put(ChunkMap chunks, float voxel_size, const std::string& data_path);
    // Overwrites the entry token names in place, as put fills a new one (the
    // edits of a Terrain made in the open recording keep refreshing one entry,
    // not adding one per edit). False, changing nothing, when the token is
    // unknown (0, or cleared).
    static bool replace(std::uint64_t token, ChunkMap chunks, float voxel_size, const std::string& data_path);
    // The latest token put for data_path; 0 when none (or cleared). A paste
    // whose source was cut takes the voxels from the cut's undo record.
    static std::uint64_t latest(const std::string& data_path);
    // False when the token is unknown (0, or cleared); chunks and voxel_size
    // are then left as they were.
    static bool get(std::uint64_t token, ChunkMap& chunks, float& voxel_size);
    // Drops every entry; the project's place was rebuilt.
    static void clear();
    // How many entries are held (for tests).
    static std::size_t size();
};

}  // namespace engine_core::terrain
