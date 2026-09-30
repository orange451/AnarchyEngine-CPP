#pragma once

#include "DataModel.hpp"
#include "DenseIdSet.hpp"
#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace engine_core {

struct VisualInstance {
    InstanceId id = 0;
    Transform world = transform_identity();
    ColorRgb color{};
    float size[3] = {1.f, 1.f, 1.f};
    bool alive = true;
    WriteOrigin transform_origin = WriteOrigin::Simulation;
    WriteOrigin color_origin = WriteOrigin::Simulation;
    WriteOrigin size_origin = WriteOrigin::Simulation;
};

// Path C. Applied after the DataModel copy. Gone on the next Prepare
// unless the job submits it again. Physics never sees it.
struct SnapshotOverride {
    InstanceId id = 0;
    VisualField field = VisualField::Transform;
    Transform transform = transform_identity();
    ColorRgb color{};
};

struct VisualSnapshot {
    std::uint64_t frame = 0;
    Transform camera = transform_identity();
    std::vector<VisualInstance> instances;
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
    void set_camera(const Transform& camera);

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
    VisualInstance* base_find(InstanceId id);
    void erase_base(InstanceId id);
    void apply_live(DataModel& game, const Invalidation& change);
    void resync(DataModel& game);
    void blit(VisualSnapshot& dst) const;
    void apply_overrides(VisualSnapshot& dst);

    VisualSnapshot base_{};
    VisualSnapshot buffers_[2]{};
    int front_ = 0;
    std::uint64_t next_frame_ = 1;
    std::atomic<std::uint64_t> published_frame_{0};
    std::vector<SnapshotOverride> overrides_;
    bool camera_pending_ = false;
    Transform pending_camera_ = transform_identity();
    bool window_open_ = false;
    // The ids with a row in base_.instances, position for position.
    DenseIdSet base_ids_;
};

}  // namespace engine_core
