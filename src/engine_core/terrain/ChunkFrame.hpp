#pragma once

// The zstd-free half of the .avox frame codec. Only this declaration is
// visible outside AvoxFile.cpp, so VoxelChunk.cpp can cache a chunk's
// encoded bytes (ChunkData::encoded()) without pulling zstd.h into every
// translation unit that includes VoxelChunk.hpp. Any thread.

#include "terrain/VoxelChunk.hpp"

#include <cstddef>
#include <vector>

namespace engine_core::terrain {

// Encodes one dense chunk's 32,768 cells (x fastest) as an .avox frame: a
// zstd frame, level 3, with its content checksum on, holding the
// Lorenzo-predicted distance residuals (32,768 bytes) then the material Ids
// (32,768 bytes).
std::vector<std::byte> encode_chunk_frame(const Cell* cells);

// The reverse: writes the frame's 32,768 cells into cells. False when the
// frame is damaged.
bool decode_chunk_frame(const std::byte* frame, std::size_t frame_size, Cell* cells);

}  // namespace engine_core::terrain
