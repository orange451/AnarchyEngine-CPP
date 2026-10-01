#pragma once

#include "DataModel.hpp"
#include "DenseIdSet.hpp"
#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace anarchy::amesh {
struct Data;
}

namespace engine_core {

struct VisualInstance {
    InstanceId id = 0;
    Matrix4 world = matrix4_identity();
    bool alive = true;
    WriteOrigin transform_origin = WriteOrigin::Simulation;
    // What the row draws: its entry in VisualSnapshot::prefabs. 0 is no
    // Prefab, and a GameObject without one draws nothing.
    std::uint32_t prefab = 0;
    // A Camera's FieldOfView, in degrees. 0 when the row is not a Camera.
    float field_of_view = 0.f;
};

// One Model's Mesh, as the renderer loads it: a file, or the geometry this
// play session's edits made (Mesh::session_geometry), which wins while it lasts.
struct VisualMesh {
    // Relative to the project's resources folder. Empty when session is set.
    std::string path;
    // Immutable once published. revision is unique across every Mesh, so a
    // renderer uploads again only when it changes.
    std::shared_ptr<const anarchy::amesh::Data> session;
    std::uint64_t revision = 0;
    // The Mesh, which a renderer can key its upload of session by.
    InstanceId mesh = 0;
    // The Model's Material: its DiffuseTexture's Path, relative to the
    // resources folder, and its Color. An empty path, as with no Material or
    // no DiffuseTexture, draws the Color alone; no Material is white.
    std::string diffuse_texture;
    ColorRgb color{};
};

// What one Prefab draws, found again at every Prepare, so an edit to its
// Models or their Meshes shows on the next frame.
struct VisualPrefab {
    // One per Model, in child order. A Model with no Mesh, or a Mesh with
    // neither a Path nor session geometry, adds nothing, and so does a Prefab
    // no live instance holds.
    std::vector<VisualMesh> meshes;
};

// Path C. Applied after the DataModel copy. Gone on the next Prepare
// unless the job submits it again. Physics never sees it.
struct SnapshotOverride {
    InstanceId id = 0;
    VisualField field = VisualField::Transform;
    Matrix4 transform = matrix4_identity();
};

struct VisualSnapshot {
    std::uint64_t frame = 0;
    Matrix4 camera = matrix4_identity();
    std::vector<VisualInstance> instances;
    // Indexed by VisualInstance::prefab. Entry 0 is always empty.
    std::vector<VisualPrefab> prefabs;
};

// Double buffer plus the one-frame override list.
// Only RenderThread calls these methods. front() is the buffer Perform reads.
// find() returns a pointer into that buffer; it dies at the next publish.
class SnapshotPump {
public:
    void reserve(std::size_t instances);

    void begin_prerender_window(DataModel& game);
    void end_prerender_window(DataModel& game);

    // Path C. No DataModel write.
    void override_visual(const SnapshotOverride& override);
    void set_camera(const Matrix4& camera);

    // Copies dirty DataModel fields into the base snapshot. Needs the DataModel
    // lock, and is the only step here that does.
    void take_changes(DataModel& game);
    // Clones the base snapshot into the back buffer and applies overrides onto
    // the back buffer only. Touches no DataModel state, so it runs after the
    // lock is released. Does not swap.
    void finish_copy();
    // take_changes, then finish_copy. Call publish() after.
    void prepare_copy(DataModel& game);

    void publish();

    const VisualSnapshot& front() const;
    const VisualInstance* find(InstanceId id) const;
    std::uint64_t published_frame() const { return published_frame_.load(); }

private:
    // A Prefab GUID some row names. Rows share an entry, so the Prefab's
    // Models are found once per Prepare however many rows draw it.
    struct PrefabEntry {
        std::string guid;
        InstanceId cached = 0;
        std::uint32_t rows = 0;
    };

    VisualInstance* base_find(InstanceId id);
    void erase_base(InstanceId id);
    void apply_live(DataModel& game, const Invalidation& change);
    void resync(DataModel& game);
    void blit(VisualSnapshot& dst) const;
    void apply_overrides(VisualSnapshot& dst);
    // The row's entry for guid, counting the row; 0 for an empty guid.
    std::uint32_t acquire_prefab(const std::string& guid);
    void release_prefab(std::uint32_t entry);
    // Points inst at guid's entry, when it names another.
    void set_row_prefab(VisualInstance& inst, const std::string& guid);
    // Fills base_.prefabs from each entry's Prefab, as the DataModel is now.
    void resolve_prefabs(DataModel& game);

    VisualSnapshot base_{};
    VisualSnapshot buffers_[2]{};
    int front_ = 0;
    std::uint64_t next_frame_ = 1;
    std::atomic<std::uint64_t> published_frame_{0};
    std::vector<SnapshotOverride> overrides_;
    bool camera_pending_ = false;
    Matrix4 pending_camera_ = matrix4_identity();
    bool window_open_ = false;
    // The ids with a row in base_.instances, position for position.
    DenseIdSet base_ids_;
    // Indexed like VisualSnapshot::prefabs; entry 0 is unused. A GUID's entry
    // is freed when its last row leaves it, and reused. These allocate only
    // when a row names a Prefab no other row does.
    std::vector<PrefabEntry> prefab_entries_;
    std::vector<std::uint32_t> free_prefab_entries_;
    std::unordered_map<std::string, std::uint32_t> prefab_by_guid_;
};

}  // namespace engine_core
