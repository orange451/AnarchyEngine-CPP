#pragma once

#include "BrushVisuals.hpp"
#include "SnapshotPump.hpp"
#include "types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {

class DataModel;
class ViewportPane;

// Each ViewportPane's VisualViewport, kept between Prepares and built again
// only when the pane may have changed. A pane is looked at again when one of
// these happens:
// - an invalidation names something inside it;
// - the tree changed;
// - its own revision moved;
// - its camera, its Skybox, one of its Prefabs, or one of its poses reads differently;
// - mark_all was called.
// A pane that is built again publishes a new VisualViewport, with the next
// revision, only when the result differs from the last one. The engine thread,
// under the Prepare lock, as SnapshotPump.
class ViewportScenes {
public:
    ViewportScenes();
    // An invalidation's id: marks the ViewportPane it is inside, if any.
    void note(const DataModel& game, InstanceId id);
    // Every pane is built again at the next update, as after a resync or Stop.
    void mark_all() { all_ = true; }
    // Every pane's scene, as the DataModel is now, into out, by pane id.
    void update(DataModel& game, std::vector<std::shared_ptr<const VisualViewport>>& out);

private:
    // A Prefab some pane draws, resolved each update and shared by every pane that names it.
    struct Prefab {
        std::string guid;
        InstanceId cached = 0;
        std::shared_ptr<const VisualPrefab> current;
        std::uint64_t revision = 0;
        std::uint32_t panes = 0;
    };
    struct Pane {
        std::shared_ptr<const VisualViewport> published;
        std::uint64_t pane_revision = 0;
        // What the last build read from outside the pane's own values.
        InstanceId first_camera = 0;
        InstanceId sky = 0;
        std::vector<std::uint32_t> prefabs;            // entries in prefabs_
        std::vector<std::uint64_t> prefab_revisions;   // as built
        std::vector<std::uint32_t> skinned_rows;       // indexes into published->rows
        std::vector<InstanceId> brushes;               // the Brushes drawn, in order
        bool seen = false;
    };

    // Each Prefab a pane draws, read again; one that reads differently gets a new revision.
    void refresh_prefabs(const DataModel& game);
    // guid's entry in prefabs_, made and resolved the first time.
    std::uint32_t find_prefab(const DataModel& game, const std::string& guid);
    void release_prefabs(Pane& pane);
    // Whether something the pane read from outside its subtree reads differently now.
    bool outside_changed(const DataModel& game, const ViewportPane& pane, const Pane& entry);
    // Builds pane's scene into scratch_ and records what it read into entry.
    void build(const DataModel& game, const ViewportPane& pane, Pane& entry);

    std::unordered_map<InstanceId, Pane> panes_;
    std::vector<Prefab> prefabs_;
    std::vector<std::uint32_t> free_prefabs_;
    std::unordered_map<std::string, std::uint32_t> prefab_by_guid_;
    std::vector<InstanceId> pane_ids_;
    std::vector<InstanceId> dirty_;
    std::vector<InstanceId> walk_;
    VisualViewport scratch_;
    VisualPrefab prefab_scratch_;
    VisualSky sky_scratch_;
    // Pane Brushes' meshes, each drawn on its own; brush_scratch_ is their
    // draws as read again to compare.
    BrushVisuals brush_visuals_;
    std::vector<VisualBrushDraw> brush_scratch_;
    std::uint64_t tree_seen_ = 0;
    std::uint64_t next_revision_ = 1;
    bool all_ = true;
};

}  // namespace engine_core
