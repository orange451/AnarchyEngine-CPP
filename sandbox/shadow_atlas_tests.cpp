// ShadowAtlasAllocator: square power-of-two tiles in one square atlas, as a
// quadtree, so freed tiles join back into bigger ones.

#include "runner/ShadowAtlas.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <utility>
#include <vector>

using runner::AtlasTile;
using runner::ShadowAtlasAllocator;

TEST_CASE("AT1 the atlas hands out tiles until it is full, and takes them back whole", "[shadow]") {
    ShadowAtlasAllocator atlas;
    atlas.reset(512, 64);
    std::vector<AtlasTile> tiles;
    std::set<std::pair<int, int>> corners;
    for (int i = 0; i < 64; ++i) {
        const AtlasTile tile = atlas.allocate(64);
        REQUIRE(tile.size == 64);
        REQUIRE(tile.x % 64 == 0);
        REQUIRE(tile.y % 64 == 0);
        REQUIRE(tile.x + 64 <= 512);
        REQUIRE(tile.y + 64 <= 512);
        corners.insert({tile.x, tile.y});
        tiles.push_back(tile);
    }
    REQUIRE(corners.size() == 64);
    REQUIRE(atlas.allocate(64).size == 0);
    REQUIRE(atlas.freeTexels() == 0);
    for (const AtlasTile& tile : tiles) {
        atlas.release(tile);
    }
    REQUIRE(atlas.freeTexels() == 512 * 512);
    REQUIRE(atlas.allocate(512).size == 512);
}

TEST_CASE("AT2 sizes mix without overlapping, and a size it cannot give is refused", "[shadow]") {
    ShadowAtlasAllocator atlas;
    atlas.reset(256, 32);
    const AtlasTile big = atlas.allocate(128);
    const AtlasTile small = atlas.allocate(32);
    REQUIRE(big.size == 128);
    REQUIRE(small.size == 32);
    REQUIRE(atlas.freeTexels() == 256 * 256 - 128 * 128 - 32 * 32);
    REQUIRE((small.x >= big.x + big.size || small.y >= big.y + big.size || small.x + small.size <= big.x ||
             small.y + small.size <= big.y));
    REQUIRE(atlas.allocate(48).size == 0);   // not a power of two
    REQUIRE(atlas.allocate(16).size == 0);   // below the smallest tile
    REQUIRE(atlas.allocate(512).size == 0);  // bigger than the atlas
    atlas.release(big);
    atlas.release(small);
    REQUIRE(atlas.allocate(256).size == 256);
}

TEST_CASE("AT3 releasing a tile twice, or one from before a reset, changes nothing", "[shadow]") {
    ShadowAtlasAllocator atlas;
    atlas.reset(512, 64);
    const AtlasTile t = atlas.allocate(64);
    REQUIRE(t.size == 64);
    atlas.release(t);
    REQUIRE(atlas.freeTexels() == 512 * 512);
    atlas.release(t);
    REQUIRE(atlas.freeTexels() == 512 * 512);
    const AtlasTile big = atlas.allocate(512);
    REQUIRE(big.size == 512);
    REQUIRE(atlas.allocate(64).size == 0);

    atlas.reset(512, 64);
    const AtlasTile stale = atlas.allocate(64);
    REQUIRE(stale.size == 64);
    atlas.reset(512, 64);
    atlas.release(stale);
    REQUIRE(atlas.freeTexels() == 512 * 512);
    const AtlasTile a = atlas.allocate(512);
    REQUIRE(a.size == 512);
    REQUIRE(atlas.allocate(64).size == 0);
}

TEST_CASE("AT4 pages hand out tiles from page 0 until it is full, then from page 1", "[shadow]") {
    runner::ShadowAtlasPages atlas;
    atlas.reset(256, 2, 64);
    REQUIRE(atlas.pageSize() == 256);
    REQUIRE(atlas.pageCount() == 2);
    REQUIRE(atlas.freeTexels() == 2 * 256 * 256);
    std::vector<AtlasTile> tiles;
    for (int i = 0; i < 16; ++i) {
        const AtlasTile tile = atlas.allocate(64);
        REQUIRE(tile.size == 64);
        REQUIRE(tile.page == 0);
        tiles.push_back(tile);
    }
    const AtlasTile next = atlas.allocate(128);
    REQUIRE(next.size == 128);
    REQUIRE(next.page == 1);
    REQUIRE(atlas.allocate(256).size == 0);  // neither page has that much left
    REQUIRE(atlas.freeTexels() == 256 * 256 - 128 * 128);

    // A tile goes back to its own page: page 0 can give a whole page again, page 1 still cannot.
    for (const AtlasTile& tile : tiles) {
        atlas.release(tile);
    }
    const AtlasTile whole = atlas.allocate(256);
    REQUIRE(whole.size == 256);
    REQUIRE(whole.page == 0);
    REQUIRE(atlas.allocate(256).size == 0);
    atlas.release(next);
    REQUIRE(atlas.allocate(256).page == 1);
}
