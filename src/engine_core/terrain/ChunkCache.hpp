#pragma once

// Decoded cells of dense chunks that are not being edited: one process-wide
// LRU under a byte budget. A chunk keeps only a weak reference to its cells
// once released (ChunkData::release_cells()); readers pin them
// (ChunkData::cells()) for as long as they read, so eviction never frees
// cells someone is reading. Any thread.

#include "terrain/VoxelChunk.hpp"

#include <atomic>
#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>

namespace engine_core::terrain {

class ChunkCache {
public:
    static constexpr std::size_t kDefaultBudget = 256u * 1024u * 1024u;
    static ChunkCache& global();

    void set_budget(std::size_t bytes);
    std::size_t budget() const;
    // Decoded bytes the cache holds now.
    std::size_t bytes() const;
    // Every live chunk's zstd frame, whether its cells are decoded or not.
    std::size_t compressed_bytes() const { return compressed_.load(std::memory_order_relaxed); }
    void add_compressed(std::size_t bytes) { compressed_.fetch_add(bytes, std::memory_order_relaxed); }
    void remove_compressed(std::size_t bytes) { compressed_.fetch_sub(bytes, std::memory_order_relaxed); }

    // Most recently used first; evicts from the back while over budget.
    void insert(const CellsPtr& cells);
    // Marks cells most recently used, if the cache still holds them.
    void touch(const CellArray* cells);
    void clear();

private:
    void evict_locked();

    mutable std::mutex mutex_;
    std::size_t budget_ = kDefaultBudget;
    std::size_t bytes_ = 0;
    std::list<CellsPtr> order_;
    std::unordered_map<const CellArray*, std::list<CellsPtr>::iterator> where_;
    std::atomic<std::size_t> compressed_{0};
};

}  // namespace engine_core::terrain
