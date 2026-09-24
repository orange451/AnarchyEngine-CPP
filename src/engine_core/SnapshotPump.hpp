#pragma once

#include "DataModel.hpp"
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

    void begin_prerender_window(DataModel& model);
    void end_prerender_window(DataModel& model);

    // Path C. No DataModel write.
    void override_visual(const SnapshotOverride& override);
    void set_camera(const Transform& camera);

    // Copies dirty DataModel fields into the base snapshot, clones that into
    // the back buffer, then applies overrides onto the back buffer only.
    // Does not swap. Call publish() after the DataModel lock is released.
    void prepare_copy(DataModel& model);

    void publish();

    const VisualSnapshot& front() const;
    const VisualInstance* find(InstanceId id) const;
    std::uint64_t published_frame() const { return published_frame_.load(); }

private:
    VisualInstance* base_find(InstanceId id);
    void erase_base(InstanceId id);
    void remember(InstanceId id, int position);
    void apply_live(DataModel& model, const Invalidation& change);
    void resync(DataModel& model);
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
    // Slot index -> position in base_.instances. -1 if that slot is not in the snapshot.
    std::vector<int> base_index_;
};

}  // namespace engine_core
