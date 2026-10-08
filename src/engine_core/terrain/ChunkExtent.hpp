#pragma once

// The bounding box of a changing set of chunk coordinates, kept up to date
// as coordinates come and go: a count per occupied value on each axis, so
// add and remove are O(log n) and the box is read straight off each axis's
// lowest and highest key -- never a scan of the whole set. LodTree keeps one
// for its level-0 nodes; VoxelVolume keeps one for its stored chunks.

#include "terrain/VoxelChunk.hpp"

#include <array>
#include <cstddef>
#include <map>

namespace engine_core::terrain {

class ChunkExtent {
public:
    // Call once per coordinate entering the set (never twice for one still in it).
    void add(const ChunkCoord& coord) {
        ++axes_[0][coord.x];
        ++axes_[1][coord.y];
        ++axes_[2][coord.z];
        ++count_;
    }
    // Call once per coordinate leaving the set (one add() made earlier).
    void remove(const ChunkCoord& coord) {
        drop(axes_[0], coord.x);
        drop(axes_[1], coord.y);
        drop(axes_[2], coord.z);
        --count_;
    }
    void clear() {
        for (auto& axis : axes_) axis.clear();
        count_ = 0;
    }
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }
    // Valid only when !empty().
    ChunkCoord lo() const { return ChunkCoord{axes_[0].begin()->first, axes_[1].begin()->first, axes_[2].begin()->first}; }
    ChunkCoord hi() const {
        return ChunkCoord{axes_[0].rbegin()->first, axes_[1].rbegin()->first, axes_[2].rbegin()->first};
    }

private:
    static void drop(std::map<int, std::size_t>& axis, int value) {
        const auto found = axis.find(value);
        if (found != axis.end() && --found->second == 0) {
            axis.erase(found);
        }
    }

    std::array<std::map<int, std::size_t>, 3> axes_;
    std::size_t count_ = 0;
};

}  // namespace engine_core::terrain
