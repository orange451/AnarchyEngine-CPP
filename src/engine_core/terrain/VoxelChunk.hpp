#pragma once

// One chunk of a Terrain's voxels: 32 cells on a side. Each cell is a signed
// distance to the surface (int8, ±4 cells wide, scaled by VoxelSize, negative
// inside) and a material Id (0 the default, 1-255 a TerrainMaterial's Id).
// A chunk is shared by pointer and never changed once shared: an edit clones
// it (clone_dense), changes the copy, and finishes it. Any thread may read a
// shared chunk.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace engine_core::terrain {

inline constexpr int kChunkSize = 32;
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

std::int8_t quantize(float studs, float voxel_size);
float dequantize(std::int8_t stored, float voxel_size);
// The chunk holding cell c, and c's index inside it (x fastest).
ChunkCoord chunk_of(int cx, int cy, int cz);
int cell_index(int lx, int ly, int lz);
// Air keeps no material: (127, m) becomes (127, 0).
Cell normalized(Cell cell);

class ChunkData {
public:
    static std::shared_ptr<const ChunkData> uniform(Cell value);
    static const std::shared_ptr<const ChunkData>& air();
    bool is_uniform() const { return uniform_; }
    Cell cell(int index) const;
    // A dense copy to edit before it is shared.
    std::shared_ptr<ChunkData> clone_dense() const;
    void set(int index, Cell value);   // only on a copy no one else holds
    // Recomputes the Id usage mask; uniform when every cell is equal.
    void finish();
    bool is_air() const { return uniform_ && value_.distance == kAirDistance; }
    // Bit i set when a solid or band cell uses Id i.
    const std::array<std::uint64_t, 4>& ids_used() const { return used_; }
private:
    bool uniform_ = true;
    Cell value_{};
    std::vector<std::int8_t> distances_;
    std::vector<std::uint8_t> materials_;
    std::array<std::uint64_t, 4> used_{};
};
using ChunkPtr = std::shared_ptr<const ChunkData>;

}  // namespace engine_core::terrain
