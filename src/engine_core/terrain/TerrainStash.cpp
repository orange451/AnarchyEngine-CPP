#include "terrain/TerrainStash.hpp"

#include <mutex>
#include <string>
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
    // Each DataPath's latest token.
    std::unordered_map<std::string, std::uint64_t> latest;
    // Never reset by clear, so a token from before a clear stays unknown.
    std::uint64_t next = 1;
};

Stash& stash() {
    static Stash instance;
    return instance;
}

}  // namespace

std::uint64_t TerrainStash::put(ChunkMap chunks, float voxel_size, const std::string& data_path) {
    Stash& s = stash();
    std::lock_guard<std::mutex> guard(s.mu);
    const std::uint64_t token = s.next++;
    Entry& entry = s.entries[token];
    entry.chunks = std::move(chunks);
    entry.voxel_size = voxel_size;
    if (!data_path.empty()) {
        s.latest[data_path] = token;
    }
    return token;
}

bool TerrainStash::replace(std::uint64_t token, ChunkMap chunks, float voxel_size, const std::string& data_path) {
    Stash& s = stash();
    std::lock_guard<std::mutex> guard(s.mu);
    const auto found = s.entries.find(token);
    if (found == s.entries.end()) {
        return false;
    }
    found->second.chunks = std::move(chunks);
    found->second.voxel_size = voxel_size;
    if (!data_path.empty()) {
        s.latest[data_path] = token;
    }
    return true;
}

std::uint64_t TerrainStash::latest(const std::string& data_path) {
    Stash& s = stash();
    std::lock_guard<std::mutex> guard(s.mu);
    const auto found = s.latest.find(data_path);
    return found != s.latest.end() ? found->second : 0;
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
    s.latest.clear();
}

std::size_t TerrainStash::size() {
    Stash& s = stash();
    std::lock_guard<std::mutex> guard(s.mu);
    return s.entries.size();
}

}  // namespace engine_core::terrain
