#include "terrain/VoxelSampler.hpp"

#include <cmath>

namespace engine_core::terrain {

namespace {

// p's cell-space coordinates (p / voxel_size), split into the lower integer
// cell and the fraction within it -- the same split SurfaceNets' trilinear()
// takes as separate (integer cell, fraction) arguments rather than one
// combined float, which is what keeps fx/fy/fz (and so the result) stable
// regardless of how large the integer part is.
void cell_space(Vec3 p, float voxel_size, int& ix, int& iy, int& iz, float& fx, float& fy, float& fz) {
    const float cx = p.x / voxel_size;
    const float cy = p.y / voxel_size;
    const float cz = p.z / voxel_size;
    ix = static_cast<int>(std::floor(cx));
    iy = static_cast<int>(std::floor(cy));
    iz = static_cast<int>(std::floor(cz));
    fx = cx - static_cast<float>(ix);
    fy = cy - static_cast<float>(iy);
    fz = cz - static_cast<float>(iz);
}

}  // namespace

const ChunkData* VoxelSampler::chunk_at(const ChunkCoord& coord) const {
    if (cached_valid_ && coord == cached_coord_) {
        return cached_chunk_;
    }
    const auto found = chunks_.find(coord);
    cached_chunk_ = (found == chunks_.end()) ? nullptr : found->second.get();
    cached_cells_ = cached_chunk_ != nullptr ? cached_chunk_->cells() : nullptr;
    cached_coord_ = coord;
    cached_valid_ = true;
    return cached_chunk_;
}

Cell VoxelSampler::cell_at(int cx, int cy, int cz) const {
    const ChunkCoord coord = chunk_of(cx, cy, cz);
    const ChunkData* chunk = chunk_at(coord);
    if (!chunk) {
        return Cell{};  // missing chunk: air, exactly as VoxelVolume::cell() treats it
    }
    const int index = cell_index(cx - coord.x * kChunkSize, cy - coord.y * kChunkSize, cz - coord.z * kChunkSize);
    // chunk_at() just pinned this chunk's cells (null when uniform).
    return cached_cells_ ? (*cached_cells_)[static_cast<std::size_t>(index)] : chunk->cell(0);
}

float VoxelSampler::distance(Vec3 p) const {
    int ix = 0, iy = 0, iz = 0;
    float fx = 0.f, fy = 0.f, fz = 0.f;
    cell_space(p, voxel_size_, ix, iy, iz, fx, fy, fz);

    const float c000 = dequantize(cell_at(ix, iy, iz).distance, voxel_size_);
    const float c100 = dequantize(cell_at(ix + 1, iy, iz).distance, voxel_size_);
    const float c010 = dequantize(cell_at(ix, iy + 1, iz).distance, voxel_size_);
    const float c110 = dequantize(cell_at(ix + 1, iy + 1, iz).distance, voxel_size_);
    const float c001 = dequantize(cell_at(ix, iy, iz + 1).distance, voxel_size_);
    const float c101 = dequantize(cell_at(ix + 1, iy, iz + 1).distance, voxel_size_);
    const float c011 = dequantize(cell_at(ix, iy + 1, iz + 1).distance, voxel_size_);
    const float c111 = dequantize(cell_at(ix + 1, iy + 1, iz + 1).distance, voxel_size_);

    const float c00 = c000 + (c100 - c000) * fx;
    const float c10 = c010 + (c110 - c010) * fx;
    const float c01 = c001 + (c101 - c001) * fx;
    const float c11 = c011 + (c111 - c011) * fx;
    const float c0 = c00 + (c10 - c00) * fy;
    const float c1 = c01 + (c11 - c01) * fy;
    return c0 + (c1 - c0) * fz;
}

Vec3 VoxelSampler::gradient(Vec3 p) const {
    const float dx = distance(Vec3{p.x + voxel_size_, p.y, p.z}) - distance(Vec3{p.x - voxel_size_, p.y, p.z});
    const float dy = distance(Vec3{p.x, p.y + voxel_size_, p.z}) - distance(Vec3{p.x, p.y - voxel_size_, p.z});
    const float dz = distance(Vec3{p.x, p.y, p.z + voxel_size_}) - distance(Vec3{p.x, p.y, p.z - voxel_size_});
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(length > 0.f)) {
        return Vec3{0.f, 0.f, 1.f};
    }
    return Vec3{dx / length, dy / length, dz / length};
}

std::uint8_t VoxelSampler::id(Vec3 p) const {
    int ix = 0, iy = 0, iz = 0;
    float fx = 0.f, fy = 0.f, fz = 0.f;
    cell_space(p, voxel_size_, ix, iy, iz, fx, fy, fz);
    (void)fx;
    (void)fy;
    (void)fz;

    const Cell corners[8] = {
        cell_at(ix, iy, iz),         cell_at(ix + 1, iy, iz),         cell_at(ix, iy + 1, iz),
        cell_at(ix + 1, iy + 1, iz), cell_at(ix, iy, iz + 1),         cell_at(ix + 1, iy, iz + 1),
        cell_at(ix, iy + 1, iz + 1), cell_at(ix + 1, iy + 1, iz + 1),
    };
    int lowest = 0;
    for (int c = 1; c < 8; ++c) {
        if (corners[c].distance < corners[lowest].distance) {
            lowest = c;
        }
    }
    return corners[lowest].material;
}

BlendIds VoxelSampler::blend(Vec3 p) const {
    int ix = 0, iy = 0, iz = 0;
    float fx = 0.f, fy = 0.f, fz = 0.f;
    cell_space(p, voxel_size_, ix, iy, iz, fx, fy, fz);
    (void)fx;
    (void)fy;
    (void)fz;

    const Cell corners[8] = {
        cell_at(ix, iy, iz),         cell_at(ix + 1, iy, iz),         cell_at(ix, iy + 1, iz),
        cell_at(ix + 1, iy + 1, iz), cell_at(ix, iy, iz + 1),         cell_at(ix + 1, iy, iz + 1),
        cell_at(ix, iy + 1, iz + 1), cell_at(ix + 1, iy + 1, iz + 1),
    };
    float distances[8];
    std::uint8_t ids[8];
    for (int c = 0; c < 8; ++c) {
        distances[c] = dequantize(corners[c].distance, voxel_size_);
        ids[c] = corners[c].material;
    }
    return blend_weights(distances, ids, voxel_size_);
}

}  // namespace engine_core::terrain
