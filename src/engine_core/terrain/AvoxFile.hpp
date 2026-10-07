#pragma once

// The .avox file: a 32-byte header, then a 32-byte index entry per non-air
// chunk sorted by (z, y, x), then one zstd frame per dense chunk. See
// docs/superpowers/specs/2026-10-06-terrain-core-design.md, "The .avox
// file", for the exact byte layout; this header only declares the two
// functions that read and write it.

#include "terrain/VoxelVolume.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace engine_core::terrain {

// The bytes of an .avox file holding volume's chunks. Reuses each chunk's
// cached frame (ChunkData::encoded()); compresses only chunks that have
// none yet. Any thread, since it only reads volume's shared, immutable
// chunks.
std::vector<std::byte> encode_avox(const VoxelVolume& volume);

// Reads bytes into out, decoding dense chunks on up to `threads` worker
// threads (0: hardware threads less one, at least 1). Returns why they are
// not a valid .avox file; out is then left empty. Call on SimulationThread:
// the worker threads this spawns only decompress frames, never touch out.
std::optional<std::string> decode_avox(const std::byte* data, std::size_t size, VoxelVolume& out,
                                       unsigned threads = 0);

}  // namespace engine_core::terrain
