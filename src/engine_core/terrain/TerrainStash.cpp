#include "terrain/TerrainStash.hpp"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace engine_core::terrain {
namespace {

struct Entry {
    ChunkMap chunks;
    float voxel_size = 1.f;
};

struct Stash {
    std::mutex mu;
    std::unordered_map<std::uint64_t, Entry> entries;
    // Never reset by clear, so a token from before a clear stays unknown.
    std::uint64_t next = 1;
};

Stash& stash() {
    static Stash instance;
    return instance;
}

}  // namespace

std::uint64_t TerrainStash::put(ChunkMap chunks, float voxel_size) {
    Stash& s = stash();
    std::lock_guard<std::mutex> guard(s.mu);
    const std::uint64_t token = s.next++;
    Entry& entry = s.entries[token];
    entry.chunks = std::move(chunks);
    entry.voxel_size = voxel_size;
    return token;
}

bool TerrainStash::get(std::uint64_t token, ChunkMap& chunks, float& voxel_size) {
    Stash& s = stash();
    std::lock_guard<std::mutex> guard(s.mu);
    const auto found = s.entries.find(token);
    if (found == s.entries.end()) {
        return false;
    }
    chunks = found->second.chunks;
    voxel_size = found->second.voxel_size;
    return true;
}

void TerrainStash::clear() {
    Stash& s = stash();
    std::lock_guard<std::mutex> guard(s.mu);
    s.entries.clear();
}

}  // namespace engine_core::terrain
