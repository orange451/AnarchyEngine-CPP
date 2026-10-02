#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace runner {

// A square of an atlas, in texels from its bottom left, on one of its
// pages (layers). size 0 is no tile.
struct AtlasTile {
    int x = 0;
    int y = 0;
    int size = 0;
    int page = 0;
};

// Hands out square, power-of-two tiles of one square atlas as a quadtree: a
// block is split in four to make a smaller tile, and four free quarters
// join back into their block when the last is released. Needs no GL.
class ShadowAtlasAllocator {
public:
    // Every tile free. atlasSize and minTile are powers of two.
    void reset(int atlasSize, int minTile);
    // A free tile size texels across, or size 0 when size is not a power of
    // two from minTile to the atlas's size, or no block that big is free.
    AtlasTile allocate(int size);
    // Return a tile to the atlas. A tile that is not outstanding (already
    // released, or from before the last reset) is ignored.
    void release(const AtlasTile& tile);
    std::int64_t freeTexels() const;
    int atlasSize() const { return atlasSize_; }

private:
    using Corner = std::pair<int, int>;
    // Free blocks' corners, by size.
    std::map<int, std::vector<Corner>> free_;
    // Tiles currently allocated: (x, y, size).
    std::set<std::tuple<int, int, int>> allocated_;
    int atlasSize_ = 0;
    int minTile_ = 0;
};

// An atlas of pages (a texture array's layers), each one square
// ShadowAtlasAllocator: a tile comes from the first page with a free block
// that big, so page 0 fills first. Needs no GL.
class ShadowAtlasPages {
public:
    // Every tile of every page free. pageSize and minTile are powers of two.
    void reset(int pageSize, int pageCount, int minTile);
    // A free tile size texels across, its page set, or size 0 when no page can give one.
    AtlasTile allocate(int size);
    // Return a tile to its page. One that is not outstanding is ignored.
    void release(const AtlasTile& tile);
    // Free texels over every page.
    std::int64_t freeTexels() const;
    int pageSize() const { return pageSize_; }
    int pageCount() const { return static_cast<int>(pages_.size()); }

private:
    std::vector<ShadowAtlasAllocator> pages_;
    int pageSize_ = 0;
};

}  // namespace runner
