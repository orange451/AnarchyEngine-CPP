#include "terrain/VoxelChunk.hpp"

namespace engine_core::terrain {

std::size_t ChunkCoordHash::operator()(const ChunkCoord& c) const {
    std::size_t h = static_cast<std::size_t>(static_cast<std::uint32_t>(c.x)) * 73856093u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(c.y)) * 19349663u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(c.z)) * 83492791u;
    return h;
}

namespace {
int floor_div(int value, int by) { return value >= 0 ? value / by : -((-value + by - 1) / by); }
}  // namespace

ChunkCoord chunk_of(int cx, int cy, int cz) {
    return ChunkCoord{floor_div(cx, kChunkSize), floor_div(cy, kChunkSize), floor_div(cz, kChunkSize)};
}

std::shared_ptr<const ChunkData> ChunkData::uniform(Cell value) {
    auto chunk = std::make_shared<ChunkData>();
    chunk->value_ = normalized(value);
    chunk->finish();
    return chunk;
}

const std::shared_ptr<const ChunkData>& ChunkData::air() {
    static const std::shared_ptr<const ChunkData> empty = uniform(Cell{});
    return empty;
}

std::shared_ptr<ChunkData> ChunkData::clone_dense() const {
    auto copy = std::make_shared<ChunkData>();
    copy->uniform_ = false;
    if (uniform_) {
        copy->cells_.assign(kChunkCells, value_);
    } else {
        copy->cells_ = cells_;
    }
    return copy;
}

void ChunkData::finish() {
    used_ = {};
    if (uniform_) {
        if (value_.distance != kAirDistance) {
            used_[value_.material >> 6] |= 1ull << (value_.material & 63);
        }
        return;
    }
    bool same = true;
    const Cell first = cells_[0];
    for (int i = 0; i < kChunkCells; ++i) {
        const Cell c = cells_[static_cast<std::size_t>(i)];
        if (c.distance != kAirDistance) {
            used_[c.material >> 6] |= 1ull << (c.material & 63);
        }
        same = same && c == first;
    }
    if (same) {
        uniform_ = true;
        value_ = first;
        cells_.clear();
        cells_.shrink_to_fit();
    }
}

}  // namespace engine_core::terrain
