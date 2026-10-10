#pragma once

// A Terrain's voxels: every chunk that is not all air, edited with shapes on
// top of Task 1's chunk data. SimulationThread only; chunks handed out by
// chunks()/cell() are shared and immutable (copy-on-write: an edit clones
// only the chunks it touches).

#include "terrain/ChunkExtent.hpp"
#include "terrain/ShapeDistance.hpp"
#include "terrain/VoxelChunk.hpp"

#include <array>
#include <cstdint>
#include <mutex>
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

// Hands every dense chunk's cells to the ChunkCache (ChunkData::
// release_cells()), e.g. once a save has encoded them all. Any thread.
void release_all_cells(const ChunkMap& chunks);

// A Terrain's voxels: every chunk that is not all air. SimulationThread only;
// chunks handed out are shared and immutable.
class VoxelVolume {
public:
    static constexpr std::int64_t kMaxCellsPerEdit = 16777216;

    explicit VoxelVolume(float voxel_size = 1.f) : voxel_size_(voxel_size) {}
    // ids_cache_mutex_ cannot be moved, so these move the rest of the fields
    // by hand and leave each side with a fresh mutex of its own. Never copied
    // (a copy would need to decide which side's cache to keep); nothing in
    // the codebase does.
    VoxelVolume(VoxelVolume&& other) noexcept;
    VoxelVolume& operator=(VoxelVolume&& other) noexcept;
    float voxel_size() const { return voxel_size_; }

    const ChunkMap& chunks() const { return chunks_; }
    // The box of chunk coordinates holding every stored chunk, kept up to
    // date as chunks are stored and dropped (no scan): false, leaving lo and
    // hi alone, when nothing is stored.
    bool chunk_extent(ChunkCoord& lo, ChunkCoord& hi) const {
        if (extent_.empty()) {
            return false;
        }
        lo = extent_.lo();
        hi = extent_.hi();
        return true;
    }
    // Replaces every chunk; chunks whose pointer changed, and their neighbors, become dirty.
    void set_chunks(ChunkMap chunks);
    Cell cell(CellCoord c) const;

    // Each returns why it did nothing (too large), else nullopt. shape is in
    // Terrain-local space; prepare_shape is called inside.
    std::optional<std::string> fill(Shape shape, std::uint8_t material);
    std::optional<std::string> subtract(Shape shape);
    std::optional<std::string> paint(Shape shape, std::uint8_t material);
    // Within a ball, each cell's distance moves toward the mean of its 3x3x3
    // neighbourhood, by strength (full at the centre, fading to the rim). A
    // cell it turns solid takes its solid neighbours' Id (a TerrainMaterial's
    // over the default's): its Id as air meant nothing.
    std::optional<std::string> smooth(Vec3 center, float radius, float strength);
    std::optional<std::string> replace(CellCoord min, CellCoord max, std::uint8_t from, std::uint8_t to);
    // Every solid or band cell with Id from takes Id to, across every chunk,
    // with no size limit. Chunks whose Id mask lacks from are skipped
    // untouched. Returns how many chunks changed.
    std::size_t replace_everywhere(std::uint8_t from, std::uint8_t to);
    // Cells min..max inclusive, x fastest: distances in units, Ids.
    std::optional<std::string> read(CellCoord min, CellCoord max, std::vector<float>& distances,
                                    std::vector<std::uint8_t>& materials) const;
    std::optional<std::string> write(CellCoord min, CellCoord max, const std::vector<float>& distances,
                                     const std::vector<std::uint8_t>& materials);
    void clear();

    // Ids that some solid or band cell uses; bit i of word i / 64. Cached:
    // recomputed only when revision() has moved since the last call.
    std::array<std::uint64_t, 4> ids_used() const;
    // Chunks changed since the last take_dirty, and every neighbor of each.
    void take_dirty(std::vector<ChunkCoord>& out);
    // Bumped by every change to the chunk map (edit, set_chunks, clear,
    // replace_everywhere), but only when something actually changed.
    std::uint64_t revision() const { return revision_; }

private:
    // Runs change(x, y, z, cell_before) over every cell in min..max, chunk by
    // chunk, cloning a chunk only if some cell in it actually changes.
    template <typename Change>
    void edit(CellCoord min, CellCoord max, Change change);
    // Marks coord and its 26 neighbors dirty.
    void mark_dirty(ChunkCoord coord);

    float voxel_size_;
    ChunkMap chunks_;
    ChunkExtent extent_;   // chunks_' keys, for chunk_extent()
    std::unordered_set<ChunkCoord, ChunkCoordHash> dirty_;
    std::uint64_t revision_ = 0;
    // ids_used()'s cache: valid when ids_cache_valid_ and ids_cache_revision_
    // matches revision_. Mutable since ids_used() is const but still wants to
    // remember the last scan. Several UI-thread readers may call ids_used()
    // concurrently under the world's shared read lock, so ids_cache_mutex_
    // guards these three together; chunks_ and revision_ themselves are not
    // written while a read lock is held, so they need no lock here.
    mutable std::mutex ids_cache_mutex_;
    mutable std::array<std::uint64_t, 4> ids_cache_{};
    mutable std::uint64_t ids_cache_revision_ = 0;
    mutable bool ids_cache_valid_ = false;
};

}  // namespace engine_core::terrain
