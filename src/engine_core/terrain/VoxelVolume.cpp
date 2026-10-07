#include "terrain/VoxelVolume.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine_core::terrain {
namespace {

// The cells an edit of shape visits: its bounds grown by the band, in cells.
// Returns false if the shape's bounds are not finite (nothing to edit).
bool cell_box(const Shape& shape, float voxel_size, CellCoord& min, CellCoord& max, std::int64_t& count) {
    Vec3 lo{}, hi{};
    shape_bounds(shape, kBandCells * voxel_size, lo, hi);
    min = CellCoord{static_cast<int>(std::floor(lo.x / voxel_size)), static_cast<int>(std::floor(lo.y / voxel_size)),
                    static_cast<int>(std::floor(lo.z / voxel_size))};
    max = CellCoord{static_cast<int>(std::ceil(hi.x / voxel_size)), static_cast<int>(std::ceil(hi.y / voxel_size)),
                    static_cast<int>(std::ceil(hi.z / voxel_size))};
    count = static_cast<std::int64_t>(max.x - min.x + 1) * (max.y - min.y + 1) * (max.z - min.z + 1);
    return std::isfinite(lo.x) && std::isfinite(hi.x);
}

// A box's cell count, for the coordinate-range operations (replace/read/write).
// False (count left at 0) when max is less than min on any axis: a caller-
// supplied box, unlike a shape's own bounds, can be inverted by mistake, and
// `max - min + 1` would otherwise go negative (or, with two inverted axes,
// wrap back to a misleadingly positive count) and must be rejected instead of
// silently mis-sized or, worse, left to overflow int math.
bool box_count(CellCoord min, CellCoord max, std::int64_t& count) {
    if (max.x < min.x || max.y < min.y || max.z < min.z) {
        return false;
    }
    const std::int64_t dx = static_cast<std::int64_t>(max.x) - static_cast<std::int64_t>(min.x) + 1;
    const std::int64_t dy = static_cast<std::int64_t>(max.y) - static_cast<std::int64_t>(min.y) + 1;
    const std::int64_t dz = static_cast<std::int64_t>(max.z) - static_cast<std::int64_t>(min.z) + 1;
    count = dx * dy * dz;
    return true;
}

const char* kTooLarge = "Terrain edit too large: split it into smaller calls";
const char* kInverted = "max must not be less than min on any axis";

// Whether a shape edit would touch any cell at all: a Ball needs a positive
// radius; a Block or Wedge needs every size component positive; a Cylinder
// needs a positive radius (size.x) and height (size.y). A zero-volume shape
// is a no-op edit (fill/subtract/paint all skip it), since otherwise e.g. a
// zero-radius ball would still "fill" with its surrounding air distance.
bool has_volume(const Shape& shape) {
    switch (shape.kind) {
    case Shape::Kind::Ball:
        return shape.radius > 0.f;
    case Shape::Kind::Cylinder:
        return shape.size.x > 0.f && shape.size.y > 0.f;
    case Shape::Kind::Block:
    case Shape::Kind::Wedge:
    default:
        return shape.size.x > 0.f && shape.size.y > 0.f && shape.size.z > 0.f;
    }
}

}  // namespace

template <typename Change>
void VoxelVolume::edit(CellCoord min, CellCoord max, Change change) {
    const ChunkCoord c0 = chunk_of(min.x, min.y, min.z);
    const ChunkCoord c1 = chunk_of(max.x, max.y, max.z);
    for (int cz = c0.z; cz <= c1.z; ++cz) {
        for (int cy = c0.y; cy <= c1.y; ++cy) {
            for (int cx = c0.x; cx <= c1.x; ++cx) {
                const ChunkCoord coord{cx, cy, cz};
                const auto found = chunks_.find(coord);
                const ChunkPtr& old = found != chunks_.end() ? found->second : ChunkData::air();
                std::shared_ptr<ChunkData> copy;
                // Hoisted once per chunk rather than recomputed for every cell.
                const int bx = cx * kChunkSize, by = cy * kChunkSize, bz = cz * kChunkSize;
                const int x0 = std::max(min.x, bx), x1 = std::min(max.x, bx + kChunkSize - 1);
                const int y0 = std::max(min.y, by), y1 = std::min(max.y, by + kChunkSize - 1);
                const int z0 = std::max(min.z, bz), z1 = std::min(max.z, bz + kChunkSize - 1);
                // Looked up once per chunk, not per cell: while old isn't
                // cloned, a uniform chunk's value never needs an array
                // index at all (no cell_index(), no dense_at() call), since
                // every cell reads the same cached value.
                const bool old_uniform = old->is_uniform();
                // cell(0) rather than dense_at(0): a uniform chunk's dense
                // array is empty (collapsed away by finish()), so dense_at
                // is only ever valid on a chunk known non-uniform.
                const Cell old_value = old_uniform ? old->cell(0) : Cell{};
                // Tracks whether this chunk's Id usage mask can be computed
                // without finish()'s full rescan: written_mask is every Id
                // this edit actually wrote to a solid/band cell (safe to OR
                // in — a material that's now present is present, however
                // it's counted); maybe_removed means some changed cell's OLD
                // value was solid/band and either went to air or changed Id,
                // so that Id *might* have just lost its last cell somewhere
                // in this chunk. Only when that never happens is old's mask
                // (which was itself exact) still exact once written_mask is
                // OR-ed in — nothing could have been removed, so nothing
                // needs re-proving by a full scan.
                std::array<std::uint64_t, 4> written_mask{};
                bool maybe_removed = false;
                for (int z = z0; z <= z1; ++z) {
                    const int zbase = (z - bz) * kChunkSize * kChunkSize;
                    for (int y = y0; y <= y1; ++y) {
                        // index steps by 1 per cell (x is the chunk's fastest
                        // axis): computed once per row rather than via
                        // cell_index()'s multiply-add on every cell.
                        int index = zbase + (y - by) * kChunkSize + (x0 - bx);
                        for (int x = x0; x <= x1; ++x, ++index) {
                            const Cell before = copy ? copy->dense_at(index) : (old_uniform ? old_value : old->dense_at(index));
                            const Cell after = normalized(change(x, y, z, before));
                            if (after == before) {
                                continue;
                            }
                            if (!copy) {
                                copy = old->clone_dense();
                            }
                            copy->set_dense_at(index, after);
                            if (after.distance != kAirDistance) {
                                written_mask[after.material >> 6] |= 1ull << (after.material & 63);
                            }
                            if (before.distance != kAirDistance &&
                                (after.distance == kAirDistance || after.material != before.material)) {
                                maybe_removed = true;
                            }
                        }
                    }
                }
                if (!copy) {
                    continue;
                }
                if (maybe_removed) {
                    copy->finish();
                } else {
                    std::array<std::uint64_t, 4> mask = old->ids_used();
                    for (int w = 0; w < 4; ++w) {
                        mask[w] |= written_mask[w];
                    }
                    copy->finish_with_mask(mask);
                }
                if (copy->is_air()) {
                    chunks_.erase(coord);
                } else {
                    chunks_[coord] = std::move(copy);
                }
                mark_dirty(coord);
            }
        }
    }
}

void VoxelVolume::mark_dirty(ChunkCoord coord) {
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                dirty_.insert(ChunkCoord{coord.x + dx, coord.y + dy, coord.z + dz});
            }
        }
    }
}

Cell VoxelVolume::cell(CellCoord c) const {
    const ChunkCoord coord = chunk_of(c.x, c.y, c.z);
    const auto found = chunks_.find(coord);
    if (found == chunks_.end()) {
        return Cell{};
    }
    const int index = cell_index(c.x - coord.x * kChunkSize, c.y - coord.y * kChunkSize, c.z - coord.z * kChunkSize);
    return found->second->cell(index);
}

std::optional<std::string> VoxelVolume::fill(Shape shape, std::uint8_t material) {
    prepare_shape(shape);
    if (!has_volume(shape)) {
        return std::nullopt;
    }
    CellCoord min{}, max{};
    std::int64_t count = 0;
    if (!cell_box(shape, voxel_size_, min, max, count)) {
        return std::nullopt;
    }
    if (count > kMaxCellsPerEdit) {
        return std::string(kTooLarge);
    }
    const float vs = voxel_size_;
    edit(min, max, [&](int x, int y, int z, Cell cell) {
        const float s = shape_distance(shape, Vec3{x * vs, y * vs, z * vs});
        const float old = dequantize(cell.distance, vs);
        const std::int8_t next = quantize(std::min(old, s), vs);
        Cell out{next, cell.material};
        if (s < old && s < vs) {
            out.material = material;
        }
        return out;
    });
    return std::nullopt;
}

std::optional<std::string> VoxelVolume::subtract(Shape shape) {
    prepare_shape(shape);
    if (!has_volume(shape)) {
        return std::nullopt;
    }
    CellCoord min{}, max{};
    std::int64_t count = 0;
    if (!cell_box(shape, voxel_size_, min, max, count)) {
        return std::nullopt;
    }
    if (count > kMaxCellsPerEdit) {
        return std::string(kTooLarge);
    }
    const float vs = voxel_size_;
    edit(min, max, [&](int x, int y, int z, Cell cell) {
        const float s = shape_distance(shape, Vec3{x * vs, y * vs, z * vs});
        const float old = dequantize(cell.distance, vs);
        const std::int8_t next = quantize(std::max(old, -s), vs);
        return Cell{next, cell.material};
    });
    return std::nullopt;
}

std::optional<std::string> VoxelVolume::paint(Shape shape, std::uint8_t material) {
    prepare_shape(shape);
    if (!has_volume(shape)) {
        return std::nullopt;
    }
    CellCoord min{}, max{};
    std::int64_t count = 0;
    if (!cell_box(shape, voxel_size_, min, max, count)) {
        return std::nullopt;
    }
    if (count > kMaxCellsPerEdit) {
        return std::string(kTooLarge);
    }
    const float vs = voxel_size_;
    edit(min, max, [&](int x, int y, int z, Cell cell) {
        const float s = shape_distance(shape, Vec3{x * vs, y * vs, z * vs});
        const float old = dequantize(cell.distance, vs);
        Cell out = cell;
        if (s <= 0.f && old <= vs) {
            out.material = material;
        }
        return out;
    });
    return std::nullopt;
}

std::optional<std::string> VoxelVolume::replace(CellCoord min, CellCoord max, std::uint8_t from, std::uint8_t to) {
    std::int64_t count = 0;
    if (!box_count(min, max, count)) {
        return std::string(kInverted);
    }
    if (count > kMaxCellsPerEdit) {
        return std::string(kTooLarge);
    }
    edit(min, max, [&](int, int, int, Cell cell) {
        if (cell.distance != kAirDistance && cell.material == from) {
            cell.material = to;
        }
        return cell;
    });
    return std::nullopt;
}

std::optional<std::string> VoxelVolume::read(CellCoord min, CellCoord max, std::vector<float>& distances,
                                              std::vector<std::uint8_t>& materials) const {
    std::int64_t count = 0;
    if (!box_count(min, max, count)) {
        return std::string(kInverted);
    }
    if (count > kMaxCellsPerEdit) {
        return std::string(kTooLarge);
    }
    distances.clear();
    materials.clear();
    distances.reserve(static_cast<std::size_t>(count));
    materials.reserve(static_cast<std::size_t>(count));
    for (int z = min.z; z <= max.z; ++z) {
        for (int y = min.y; y <= max.y; ++y) {
            for (int x = min.x; x <= max.x; ++x) {
                const Cell c = cell(CellCoord{x, y, z});
                distances.push_back(dequantize(c.distance, voxel_size_));
                materials.push_back(c.material);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> VoxelVolume::write(CellCoord min, CellCoord max, const std::vector<float>& distances,
                                               const std::vector<std::uint8_t>& materials) {
    std::int64_t count = 0;
    if (!box_count(min, max, count)) {
        return std::string(kInverted);
    }
    if (count > kMaxCellsPerEdit) {
        return std::string(kTooLarge);
    }
    if (distances.size() != static_cast<std::size_t>(count) || materials.size() != static_cast<std::size_t>(count)) {
        return std::string("distances and materials must each hold one value per cell");
    }
    const float vs = voxel_size_;
    const int width = max.x - min.x + 1;
    const int height = max.y - min.y + 1;
    // edit() walks chunk by chunk, not x-fastest across the whole box, so the
    // cell's own coordinates (not call order) pick its slot in the x-fastest
    // input vectors.
    edit(min, max, [&](int x, int y, int z, Cell) {
        const std::size_t i =
            static_cast<std::size_t>((x - min.x) + width * ((y - min.y) + height * (z - min.z)));
        return Cell{quantize(distances[i], vs), materials[i]};
    });
    return std::nullopt;
}

void VoxelVolume::clear() {
    for (const auto& [coord, chunk] : chunks_) {
        (void)chunk;
        mark_dirty(coord);
    }
    chunks_.clear();
}

void VoxelVolume::set_chunks(ChunkMap chunks) {
    for (const auto& [coord, chunk] : chunks_) {
        const auto found = chunks.find(coord);
        if (found == chunks.end() || found->second != chunk) {
            mark_dirty(coord);
        }
    }
    for (const auto& [coord, chunk] : chunks) {
        (void)chunk;
        if (chunks_.find(coord) == chunks_.end()) {
            mark_dirty(coord);
        }
    }
    chunks_ = std::move(chunks);
}

std::array<std::uint64_t, 4> VoxelVolume::ids_used() const {
    std::array<std::uint64_t, 4> result{};
    for (const auto& [coord, chunk] : chunks_) {
        (void)coord;
        const auto& used = chunk->ids_used();
        for (int i = 0; i < 4; ++i) {
            result[i] |= used[i];
        }
    }
    return result;
}

void VoxelVolume::take_dirty(std::vector<ChunkCoord>& out) {
    out.assign(dirty_.begin(), dirty_.end());
    dirty_.clear();
}

}  // namespace engine_core::terrain
