#pragma once

// A Terrain's voxels: every chunk that is not all air, edited with shapes on
// top of Task 1's chunk data. SimulationThread only; chunks handed out by
// chunks()/cell() are shared and immutable (copy-on-write: an edit clones
// only the chunks it touches).

#include "terrain/ShapeDistance.hpp"
#include "terrain/VoxelChunk.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine_core::terrain {

struct CellCoord {
    int x = 0, y = 0, z = 0;
};

using ChunkMap = std::unordered_map<ChunkCoord, ChunkPtr, ChunkCoordHash>;

// A Terrain's voxels: every chunk that is not all air. SimulationThread only;
// chunks handed out are shared and immutable.
class VoxelVolume {
public:
    static constexpr std::int64_t kMaxCellsPerEdit = 16777216;

    explicit VoxelVolume(float voxel_size = 1.f) : voxel_size_(voxel_size) {}
    float voxel_size() const { return voxel_size_; }

    const ChunkMap& chunks() const { return chunks_; }
    // Replaces every chunk; chunks whose pointer changed, and their neighbors, become dirty.
    void set_chunks(ChunkMap chunks);
    Cell cell(CellCoord c) const;

    // Each returns why it did nothing (too large), else nullopt. shape is in
    // Terrain-local space; prepare_shape is called inside.
    std::optional<std::string> fill(Shape shape, std::uint8_t material);
    std::optional<std::string> subtract(Shape shape);
    std::optional<std::string> paint(Shape shape, std::uint8_t material);
    std::optional<std::string> replace(CellCoord min, CellCoord max, std::uint8_t from, std::uint8_t to);
    // Cells min..max inclusive, x fastest: distances in studs, Ids.
    std::optional<std::string> read(CellCoord min, CellCoord max, std::vector<float>& distances,
                                    std::vector<std::uint8_t>& materials) const;
    std::optional<std::string> write(CellCoord min, CellCoord max, const std::vector<float>& distances,
                                     const std::vector<std::uint8_t>& materials);
    void clear();

    // Ids that some solid or band cell uses; bit i of word i / 64.
    std::array<std::uint64_t, 4> ids_used() const;
    // Chunks changed since the last take_dirty, and every neighbor of each.
    void take_dirty(std::vector<ChunkCoord>& out);
    bool has_dirty() const { return !dirty_.empty(); }

private:
    // Runs change(x, y, z, cell_before) over every cell in min..max, chunk by
    // chunk, cloning a chunk only if some cell in it actually changes.
    template <typename Change>
    void edit(CellCoord min, CellCoord max, Change change);
    // Marks coord and its 26 neighbors dirty.
    void mark_dirty(ChunkCoord coord);

    float voxel_size_;
    ChunkMap chunks_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> dirty_;
};

}  // namespace engine_core::terrain
