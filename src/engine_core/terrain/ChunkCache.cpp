#include "terrain/ChunkCache.hpp"

namespace engine_core::terrain {

namespace {
constexpr std::size_t kEntryBytes = static_cast<std::size_t>(kChunkCells) * sizeof(Cell);
}  // namespace

ChunkCache& ChunkCache::global() {
    static ChunkCache cache;
    return cache;
}

void ChunkCache::set_budget(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    budget_ = bytes;
    evict_locked();
}

std::size_t ChunkCache::budget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budget_;
}

std::size_t ChunkCache::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

void ChunkCache::insert(const CellsPtr& cells) {
    if (!cells) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = where_.find(cells.get());
    if (found != where_.end()) {
        order_.splice(order_.begin(), order_, found->second);
        return;
    }
    order_.push_front(cells);
    where_[cells.get()] = order_.begin();
    bytes_ += kEntryBytes;
    evict_locked();
}

void ChunkCache::touch(const CellArray* cells) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = where_.find(cells);
    if (found != where_.end()) {
        order_.splice(order_.begin(), order_, found->second);
    }
}

void ChunkCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    order_.clear();
    where_.clear();
    bytes_ = 0;
}

void ChunkCache::evict_locked() {
    while (bytes_ > budget_ && !order_.empty()) {
        // The chunk's weak reference expires unless a reader still pins it.
        where_.erase(order_.back().get());
        order_.pop_back();
        bytes_ -= kEntryBytes;
    }
}

}  // namespace engine_core::terrain
