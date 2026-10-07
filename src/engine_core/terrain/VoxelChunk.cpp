#include "terrain/VoxelChunk.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core::terrain {

std::size_t ChunkCoordHash::operator()(const ChunkCoord& c) const {
    std::size_t h = static_cast<std::size_t>(static_cast<std::uint32_t>(c.x)) * 73856093u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(c.y)) * 19349663u;
    h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(c.z)) * 83492791u;
    return h;
}

std::int8_t quantize(float studs, float voxel_size) {
    const float scaled = studs / (kBandCells * voxel_size) * 127.f;
    const float clamped = std::clamp(scaled, -127.f, 127.f);
    return static_cast<std::int8_t>(std::lround(clamped));
}

float dequantize(std::int8_t stored, float voxel_size) {
    return static_cast<float>(stored) / 127.f * kBandCells * voxel_size;
}

namespace {
int floor_div(int value, int by) { return value >= 0 ? value / by : -((-value + by - 1) / by); }
}  // namespace

ChunkCoord chunk_of(int cx, int cy, int cz) {
    return ChunkCoord{floor_div(cx, kChunkSize), floor_div(cy, kChunkSize), floor_div(cz, kChunkSize)};
}

int cell_index(int lx, int ly, int lz) { return lx + kChunkSize * (ly + kChunkSize * lz); }

Cell normalized(Cell cell) {
    if (cell.distance == kAirDistance) {
        cell.material = 0;
    }
    return cell;
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

Cell ChunkData::cell(int index) const {
    if (uniform_) {
        return value_;
    }
    return Cell{distances_[static_cast<std::size_t>(index)], materials_[static_cast<std::size_t>(index)]};
}

std::shared_ptr<ChunkData> ChunkData::clone_dense() const {
    auto copy = std::make_shared<ChunkData>();
    copy->uniform_ = false;
    if (uniform_) {
        copy->distances_.assign(kChunkCells, value_.distance);
        copy->materials_.assign(kChunkCells, value_.material);
    } else {
        copy->distances_ = distances_;
        copy->materials_ = materials_;
    }
    return copy;
}

void ChunkData::set(int index, Cell value) {
    value = normalized(value);
    distances_[static_cast<std::size_t>(index)] = value.distance;
    materials_[static_cast<std::size_t>(index)] = value.material;
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
    const Cell first{distances_[0], materials_[0]};
    for (int i = 0; i < kChunkCells; ++i) {
        const Cell c{distances_[static_cast<std::size_t>(i)], materials_[static_cast<std::size_t>(i)]};
        if (c.distance != kAirDistance) {
            used_[c.material >> 6] |= 1ull << (c.material & 63);
        }
        same = same && c == first;
    }
    if (same) {
        uniform_ = true;
        value_ = first;
        distances_.clear();
        distances_.shrink_to_fit();
        materials_.clear();
        materials_.shrink_to_fit();
    }
}

}  // namespace engine_core::terrain
