#include "ShadowAtlas.hpp"

#include <algorithm>

namespace runner {

void ShadowAtlasAllocator::reset(int atlasSize, int minTile) {
    free_.clear();
    allocated_.clear();
    atlasSize_ = atlasSize;
    minTile_ = minTile;
    if (atlasSize > 0) {
        free_[atlasSize].push_back({0, 0});
    }
}

AtlasTile ShadowAtlasAllocator::allocate(int size) {
    if (size < minTile_ || size > atlasSize_ || size <= 0 || (size & (size - 1)) != 0) {
        return {};
    }
    // The smallest free block at least that big.
    int block = size;
    while (block <= atlasSize_ && free_[block].empty()) {
        block *= 2;
    }
    if (block > atlasSize_) {
        return {};
    }
    const Corner corner = free_[block].back();
    free_[block].pop_back();
    // Split down to size, keeping the bottom-left quarter each time.
    while (block > size) {
        block /= 2;
        free_[block].push_back({corner.first + block, corner.second});
        free_[block].push_back({corner.first, corner.second + block});
        free_[block].push_back({corner.first + block, corner.second + block});
    }
    const AtlasTile tile{corner.first, corner.second, size};
    allocated_.insert(std::make_tuple(tile.x, tile.y, tile.size));
    return tile;
}

void ShadowAtlasAllocator::release(const AtlasTile& tile) {
    if (tile.size <= 0) {
        return;
    }
    // Only release tiles that are currently allocated.
    if (allocated_.find(std::make_tuple(tile.x, tile.y, tile.size)) == allocated_.end()) {
        return;
    }
    allocated_.erase(std::make_tuple(tile.x, tile.y, tile.size));
    int size = tile.size;
    int x = tile.x;
    int y = tile.y;
    // Join with the other three quarters of its block while they are all free.
    while (size < atlasSize_) {
        const int parent = size * 2;
        const int px = x - x % parent;
        const int py = y - y % parent;
        std::vector<Corner>& list = free_[size];
        const Corner quarters[4] = {{px, py}, {px + size, py}, {px, py + size}, {px + size, py + size}};
        int found = 0;
        for (const Corner& quarter : quarters) {
            if (quarter != Corner{x, y} && std::find(list.begin(), list.end(), quarter) != list.end()) {
                ++found;
            }
        }
        if (found < 3) {
            break;
        }
        for (const Corner& quarter : quarters) {
            if (quarter != Corner{x, y}) {
                list.erase(std::find(list.begin(), list.end(), quarter));
            }
        }
        size = parent;
        x = px;
        y = py;
    }
    free_[size].push_back({x, y});
}

std::int64_t ShadowAtlasAllocator::freeTexels() const {
    std::int64_t total = 0;
    for (const auto& [size, corners] : free_) {
        total += static_cast<std::int64_t>(size) * size * static_cast<std::int64_t>(corners.size());
    }
    return total;
}

}  // namespace runner
