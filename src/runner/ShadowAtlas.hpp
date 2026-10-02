#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace runner {

// A square of an atlas, in texels from its bottom left. size 0 is no tile.
struct AtlasTile {
    int x = 0;
    int y = 0;
    int size = 0;
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
    void release(const AtlasTile& tile);
    std::int64_t freeTexels() const;
    int atlasSize() const { return atlasSize_; }

private:
    using Corner = std::pair<int, int>;
    // Free blocks' corners, by size.
    std::map<int, std::vector<Corner>> free_;
    int atlasSize_ = 0;
    int minTile_ = 0;
};

}  // namespace runner
