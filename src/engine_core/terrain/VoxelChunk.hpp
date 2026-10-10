#pragma once

// One chunk of a Terrain's voxels: 32 cells on a side. Each cell is a signed
// distance to the surface (int8, ±4 cells wide, scaled by VoxelSize, negative
// inside) and a material Id (0 the default, 1-255 a TerrainMaterial's Id).
// A chunk is shared by pointer and never changed once shared: an edit clones
// it (clone_dense), changes the copy, and finishes it. Any thread may read a
// shared chunk.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace engine_core::terrain {

inline constexpr int kChunkSize = 32;
// How many cells past its own a chunk's mesh reads on every side (Surface
// Nets' sample apron): a change to a cell can alter the meshes of chunks
// this far away, and no farther.
inline constexpr int kMeshReach = 2;
inline constexpr int kChunkCells = kChunkSize * kChunkSize * kChunkSize;
// The distance band either side of the surface, in cells.
inline constexpr float kBandCells = 4.f;
inline constexpr std::int8_t kAirDistance = 127;
inline constexpr std::int8_t kSolidDistance = -127;

struct ChunkCoord {
    int x = 0, y = 0, z = 0;
    bool operator==(const ChunkCoord& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct ChunkCoordHash { std::size_t operator()(const ChunkCoord& c) const; };

struct Cell {
    std::int8_t distance = kAirDistance;
    std::uint8_t material = 0;
    bool operator==(const Cell& o) const { return distance == o.distance && material == o.material; }
};

// Hot per-cell helpers: inline so every call site (VoxelVolume's edit loop,
// run per cell of every edit) can fold them in rather than crossing a
// translation unit for a few bytes of math each time.

inline std::int8_t quantize(float distance, float voxel_size) {
    const float scaled = distance / (kBandCells * voxel_size) * 127.f;
    const float clamped = std::clamp(scaled, -127.f, 127.f);
    return static_cast<std::int8_t>(std::lround(clamped));
}

inline float dequantize(std::int8_t stored, float voxel_size) {
    return static_cast<float>(stored) / 127.f * kBandCells * voxel_size;
}

// The chunk holding cell c, and c's index inside it (x fastest).
ChunkCoord chunk_of(int cx, int cy, int cz);

inline int cell_index(int lx, int ly, int lz) { return lx + kChunkSize * (ly + kChunkSize * lz); }

// Air keeps no material: (127, m) becomes (127, 0).
inline Cell normalized(Cell cell) {
    if (cell.distance == kAirDistance) {
        cell.material = 0;
    }
    return cell;
}

// A dense chunk's 32,768 cells, x fastest.
using CellArray = std::vector<Cell>;
using CellsPtr = std::shared_ptr<const CellArray>;

// A dense chunk's cells need not stay decoded: once it has its zstd frame
// (encoded()), release_cells() hands its cells to the ChunkCache, which may
// evict them, and cells() decodes them again from the frame when asked.
// Readers pin what cells() returns for as long as they read.
class ChunkData {
public:
    ChunkData() = default;
    ~ChunkData();
    ChunkData(const ChunkData&) = delete;
    ChunkData& operator=(const ChunkData&) = delete;

    static std::shared_ptr<const ChunkData> uniform(Cell value);
    static const std::shared_ptr<const ChunkData>& air();
    bool is_uniform() const { return uniform_; }
    // One cell. A dense chunk pins its cells for the call, so a reader of
    // many cells should hold cells() instead.
    Cell cell(int index) const { return uniform_ ? value_ : (*cells())[static_cast<std::size_t>(index)]; }
    // A dense chunk's cells, decoded from its frame if the cache let them
    // go; null for a uniform chunk. Any thread.
    CellsPtr cells() const;
    // Hands a dense chunk's own cells to the ChunkCache (encoding its frame
    // first if it has none), so they can be evicted. Any thread; only once
    // the chunk is shared, i.e. no longer being edited.
    void release_cells() const;
    // The chunk holds its cells itself: dense and not released.
    bool cells_owned() const;
    // A dense copy to edit before it is shared.
    std::shared_ptr<ChunkData> clone_dense() const;
    // only on a copy no one else holds
    void set(int index, Cell value) { (*owned_)[static_cast<std::size_t>(index)] = normalized(value); }
    // Direct access to a dense chunk's own array: valid only when the chunk
    // is known dense already (e.g. right after clone_dense(), or any chunk
    // that is_uniform() says false for). Skips the is_uniform() branch that
    // cell()/set() pay on every call, for callers (VoxelVolume's edit loop)
    // that have already cloned and so know which case applies.
    Cell dense_at(int index) const { return (*owned_)[static_cast<std::size_t>(index)]; }
    void set_dense_at(int index, Cell value) { (*owned_)[static_cast<std::size_t>(index)] = value; }
    // The array itself, under the same rule as dense_at().
    Cell* dense_data() { return owned_->data(); }
    // Recomputes the Id usage mask; uniform when every cell is equal.
    void finish();
    // Same collapse-to-uniform check as finish(), but the Id usage mask is
    // taken from the caller (who has proven it exact some cheaper way, e.g.
    // VoxelVolume::edit() tracking it incrementally) rather than rescanned.
    // Still does the real, exact uniformity check: only the mask rescan is
    // skipped.
    void finish_with_mask(const std::array<std::uint64_t, 4>& mask);
    bool is_air() const { return uniform_ && value_.distance == kAirDistance; }
    // Bit i set when a solid or band cell uses Id i.
    const std::array<std::uint64_t, 4>& ids_used() const { return used_; }
    // This chunk's zstd frame as .avox stores it, made on first use and kept:
    // the chunk never changes once shared, so neither does its frame. Empty
    // for a uniform chunk. Any thread.
    const std::vector<std::byte>& encoded() const;
    // A decoded chunk gets the frame it was read from, so an unchanged chunk
    // is never compressed again by the next save.
    void adopt_encoded(std::vector<std::byte> frame);
private:
    bool uniform_ = true;
    Cell value_{};
    // One array of 32,768 cells rather than parallel distance/material
    // arrays: half the allocations per clone_dense(). owned_ holds them
    // while the chunk is edited and until release_cells(); after that only
    // cached_ refers to them, and the ChunkCache owns them.
    mutable std::mutex cells_mutex_;
    mutable std::shared_ptr<CellArray> owned_;
    mutable std::weak_ptr<const CellArray> cached_;
    std::array<std::uint64_t, 4> used_{};
    // clone_dense() builds its copy field by field rather than copying *this,
    // so a clone starts with its own unset flag and never inherits a frame
    // that belonged to the chunk it was cloned from.
    mutable std::once_flag encoded_once_;
    mutable std::vector<std::byte> encoded_;
};
using ChunkPtr = std::shared_ptr<const ChunkData>;

}  // namespace engine_core::terrain
