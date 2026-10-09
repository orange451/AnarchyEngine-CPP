#include "terrain/VoxelChunk.hpp"

#include "terrain/ChunkCache.hpp"
#include "terrain/ChunkFrame.hpp"

#include <cstdlib>

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
        copy->owned_ = std::make_shared<CellArray>(static_cast<std::size_t>(kChunkCells), value_);
    } else {
        copy->owned_ = std::make_shared<CellArray>(*cells());
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
    // A flat 256-entry table, one bool per Id: a plain indexed byte store
    // per cell instead of the shift-and-OR into one of 4 uint64 words that
    // ids_used()'s packed form needs. Packed into used_ once, after the
    // scan, since there are only 256 entries to fold regardless of how
    // large the chunk is.
    bool used_flat[256] = {};
    const CellArray& cells = *owned_;
    const Cell first = cells[0];
    bool same = true;
    for (int i = 0; i < kChunkCells; ++i) {
        const Cell c = cells[static_cast<std::size_t>(i)];
        if (c.distance != kAirDistance) {
            used_flat[c.material] = true;
        }
        // Once same is false it stays false; skip the comparison instead of
        // re-ANDing into it for the remaining cells (same && ... is false
        // from here on regardless, but costs a comparison every time).
        if (same && !(c == first)) {
            same = false;
        }
    }
    for (int m = 0; m < 256; ++m) {
        if (used_flat[m]) {
            used_[m >> 6] |= 1ull << (m & 63);
        }
    }
    if (same) {
        uniform_ = true;
        value_ = first;
        owned_.reset();
    }
}

void ChunkData::finish_with_mask(const std::array<std::uint64_t, 4>& mask) {
    if (uniform_) {
        // A uniform chunk's own mask is trivial to recompute; nothing to
        // skip, and the caller's mask isn't meant for this case.
        finish();
        return;
    }
    // The real, exact uniformity check, same as finish()'s, but exiting at
    // the first cell that disagrees with cell 0 instead of visiting all
    // 32,768: the caller already proved the mask, so once this chunk is
    // known non-uniform there's nothing left to compute here.
    const CellArray& cells = *owned_;
    const Cell first = cells[0];
    for (int i = 1; i < kChunkCells; ++i) {
        if (!(cells[static_cast<std::size_t>(i)] == first)) {
            used_ = mask;
            return;
        }
    }
    // Every cell agrees with cell 0: this chunk collapses to uniform.
    uniform_ = true;
    value_ = first;
    owned_.reset();
    used_ = {};
    if (value_.distance != kAirDistance) {
        used_[value_.material >> 6] |= 1ull << (value_.material & 63);
    }
}

const std::vector<std::byte>& ChunkData::encoded() const {
    // Any thread: two readers racing here compress the chunk exactly once,
    // and the chunk's cells never change afterward, so the cached frame
    // stays right forever.
    std::call_once(encoded_once_, [this]() {
        if (!uniform_) {
            encoded_ = encode_chunk_frame(cells()->data());
            ChunkCache::global().add_compressed(encoded_.size());
        }
    });
    return encoded_;
}

void ChunkData::adopt_encoded(std::vector<std::byte> frame) {
    // Marks the flag triggered without running the encoder, so encoded()
    // never recompresses a chunk that was just decoded from this very frame.
    std::call_once(encoded_once_, [] {});
    ChunkCache::global().remove_compressed(encoded_.size());
    encoded_ = std::move(frame);
    ChunkCache::global().add_compressed(encoded_.size());
}

ChunkData::~ChunkData() { ChunkCache::global().remove_compressed(encoded_.size()); }

CellsPtr ChunkData::cells() const {
    if (uniform_) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(cells_mutex_);
    if (owned_) {
        return owned_;
    }
    if (CellsPtr cached = cached_.lock()) {
        ChunkCache::global().touch(cached.get());
        return cached;
    }
    auto decoded = std::make_shared<CellArray>(static_cast<std::size_t>(kChunkCells));
    // The frame was checked when it was read (decode_avox) or made here
    // (encoded()), so failing now means memory was corrupted.
    if (!decode_chunk_frame(encoded_.data(), encoded_.size(), decoded->data())) {
        std::abort();
    }
    CellsPtr shared = std::move(decoded);
    cached_ = shared;
    ChunkCache::global().insert(shared);
    return shared;
}

void ChunkData::release_cells() const {
    if (uniform_) {
        return;
    }
    encoded();   // made from owned_ if this chunk has no frame yet
    std::lock_guard<std::mutex> lock(cells_mutex_);
    if (!owned_) {
        return;
    }
    cached_ = owned_;
    ChunkCache::global().insert(owned_);
    owned_.reset();
}

bool ChunkData::cells_owned() const {
    std::lock_guard<std::mutex> lock(cells_mutex_);
    return owned_ != nullptr;
}

}  // namespace engine_core::terrain
