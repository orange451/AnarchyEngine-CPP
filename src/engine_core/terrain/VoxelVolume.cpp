#include "terrain/VoxelVolume.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine_core::terrain {

VoxelVolume::VoxelVolume(VoxelVolume&& other) noexcept {
    *this = std::move(other);
}

VoxelVolume& VoxelVolume::operator=(VoxelVolume&& other) noexcept {
    if (this != &other) {
        voxel_size_ = other.voxel_size_;
        chunks_ = std::move(other.chunks_);
        dirty_ = std::move(other.dirty_);
        revision_ = other.revision_;
        ids_cache_ = other.ids_cache_;
        ids_cache_revision_ = other.ids_cache_revision_;
        ids_cache_valid_ = other.ids_cache_valid_;
    }
    return *this;
}

namespace {

// Cell bounds past this are refused before they are cast to int. Far inside
// int's range, so max - min + 1 on any axis cannot overflow either.
constexpr double kMaxBoundCell = 1073741824.0;  // 2^30

// dx * dy * dz, but any product past VoxelVolume::kMaxCellsPerEdit comes back
// as kMaxCellsPerEdit + 1, so no product can overflow and wrap below the limit.
// Each extent is positive and at most 2^32.
std::int64_t saturated_count(std::int64_t dx, std::int64_t dy, std::int64_t dz) {
    constexpr std::int64_t over = VoxelVolume::kMaxCellsPerEdit + 1;
    if (dx >= over || dy >= over || dz >= over) {
        return over;
    }
    const std::int64_t xy = dx * dy;
    return xy >= over ? over : std::min(xy * dz, over);
}

// The cells an edit of shape visits: its bounds grown by the band, in cells.
// Returns false, setting nothing, when a bound is not finite or lies past
// kMaxBoundCell: such an edit is too large.
bool cell_box(const Shape& shape, float voxel_size, CellCoord& min, CellCoord& max, std::int64_t& count) {
    Vec3 lo{}, hi{};
    shape_bounds(shape, kBandCells * voxel_size, lo, hi);
    const double bounds[6] = {std::floor(static_cast<double>(lo.x) / voxel_size),
                              std::floor(static_cast<double>(lo.y) / voxel_size),
                              std::floor(static_cast<double>(lo.z) / voxel_size),
                              std::ceil(static_cast<double>(hi.x) / voxel_size),
                              std::ceil(static_cast<double>(hi.y) / voxel_size),
                              std::ceil(static_cast<double>(hi.z) / voxel_size)};
    for (const double bound : bounds) {
        // Also false for NaN, which fails both comparisons.
        if (!(bound >= -kMaxBoundCell && bound <= kMaxBoundCell)) {
            return false;
        }
    }
    min = CellCoord{static_cast<int>(bounds[0]), static_cast<int>(bounds[1]), static_cast<int>(bounds[2])};
    max = CellCoord{static_cast<int>(bounds[3]), static_cast<int>(bounds[4]), static_cast<int>(bounds[5])};
    count = saturated_count(static_cast<std::int64_t>(max.x) - min.x + 1, static_cast<std::int64_t>(max.y) - min.y + 1,
                            static_cast<std::int64_t>(max.z) - min.z + 1);
    return true;
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
    count = saturated_count(dx, dy, dz);
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
    // Set only when some chunk actually changes, so a no-op edit (nothing in
    // range, or every cell already at its new value) leaves revision_ alone.
    bool changed = false;
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
                changed = true;
            }
        }
    }
    if (changed) {
        ++revision_;
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
    if (!cell_box(shape, voxel_size_, min, max, count) || count > kMaxCellsPerEdit) {
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
    if (!cell_box(shape, voxel_size_, min, max, count) || count > kMaxCellsPerEdit) {
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
    if (!cell_box(shape, voxel_size_, min, max, count) || count > kMaxCellsPerEdit) {
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
    if (chunks_.empty()) {
        return;
    }
    for (const auto& [coord, chunk] : chunks_) {
        (void)chunk;
        mark_dirty(coord);
    }
    chunks_.clear();
    ++revision_;
}

void VoxelVolume::set_chunks(ChunkMap chunks) {
    bool changed = false;
    for (const auto& [coord, chunk] : chunks_) {
        const auto found = chunks.find(coord);
        if (found == chunks.end() || found->second != chunk) {
            mark_dirty(coord);
            changed = true;
        }
    }
    for (const auto& [coord, chunk] : chunks) {
        (void)chunk;
        if (chunks_.find(coord) == chunks_.end()) {
            mark_dirty(coord);
            changed = true;
        }
    }
    chunks_ = std::move(chunks);
    if (changed) {
        ++revision_;
    }
}

std::size_t VoxelVolume::replace_everywhere(std::uint8_t from, std::uint8_t to) {
    if (from == to) {
        return 0;
    }
    std::size_t changed_chunks = 0;
    for (auto& [coord, chunk] : chunks_) {
        const std::array<std::uint64_t, 4>& used = chunk->ids_used();
        if (((used[from >> 6] >> (from & 63)) & 1ull) == 0ull) {
            continue;   // from isn't used anywhere in this chunk: leave it alone
        }
        std::shared_ptr<ChunkData> copy = chunk->clone_dense();
        for (int i = 0; i < kChunkCells; ++i) {
            const Cell before = copy->dense_at(i);
            if (before.distance != kAirDistance && before.material == from) {
                copy->set_dense_at(i, Cell{before.distance, to});
            }
        }
        copy->finish();
        chunk = std::move(copy);
        mark_dirty(coord);
        ++changed_chunks;
    }
    if (changed_chunks > 0) {
        ++revision_;
    }
    return changed_chunks;
}

std::array<std::uint64_t, 4> VoxelVolume::ids_used() const {
    // chunks_ and revision_ are stable while this runs (no writer runs
    // concurrently with a reader), but several readers may call this at
    // once, so the cache fields themselves need their own lock.
    std::lock_guard<std::mutex> lock(ids_cache_mutex_);
    if (ids_cache_valid_ && ids_cache_revision_ == revision_) {
        return ids_cache_;
    }
    std::array<std::uint64_t, 4> result{};
    for (const auto& [coord, chunk] : chunks_) {
        (void)coord;
        const auto& used = chunk->ids_used();
        for (int i = 0; i < 4; ++i) {
            result[i] |= used[i];
        }
    }
    ids_cache_ = result;
    ids_cache_revision_ = revision_;
    ids_cache_valid_ = true;
    return result;
}

void VoxelVolume::take_dirty(std::vector<ChunkCoord>& out) {
    out.assign(dirty_.begin(), dirty_.end());
    dirty_.clear();
}

}  // namespace engine_core::terrain
