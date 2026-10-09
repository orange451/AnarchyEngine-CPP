#pragma once

#include "amesh.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {

class DataModel;
class Material;
struct VisualBrushDraw;

// Turns every Brush in Workspace into draws for the snapshot. Anchored,
// opaque Brushes are baked, in world space, into 64-unit cells: one mesh per
// cell, one draw per Material in it, so thousands of brushes cost a few hundred
// draws. A cell is baked again only when one of its brushes changes, moves,
// arrives, or leaves, or a Material it uses changes its TextureScale. Every
// other Brush (moving, transparent, or with a transparent face) draws on its
// own, in its own space, its mesh rebuilt only when it changes.
//
// RenderThread, inside take_changes, under the DataModel write lock.
class BrushVisuals {
public:
    static constexpr double kCellSize = 64.0;

    void update(DataModel& game, std::vector<VisualBrushDraw>& out);

    // For tests: how many cells exist, and how many bakes have run.
    std::size_t cell_count() const { return cells_.size(); }
    std::uint64_t bakes() const { return bakes_; }

private:
    using CellKey = std::array<std::int64_t, 3>;

    struct Baked {
        std::uint64_t signature = 0;
        std::uint64_t revision = 0;
        std::shared_ptr<const anarchy::amesh::Data> mesh;
        // The Material GUID of LOD i + 1.
        std::vector<std::string> materials;
        bool seen = false;
    };

    const Material* material(const DataModel& game, const std::string& guid);

    std::map<CellKey, Baked> cells_;
    std::unordered_map<InstanceId, Baked> singles_;
    std::unordered_map<std::string, InstanceId> materials_;
    std::vector<InstanceId> ids_;
    std::map<CellKey, std::vector<InstanceId>> members_;
    std::uint64_t bakes_ = 0;
};

}  // namespace engine_core
