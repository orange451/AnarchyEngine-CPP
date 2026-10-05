#pragma once

#include "DraggerMath.hpp"
#include "PVInstance.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Translate handles at its own Transform: three arrows and three plane
// squares. Dragging them moves nothing. DraggerWorld turns the drag into
// events, and whoever listens moves what it likes, the Dragger included.
// Active anywhere under game; the studio's Move tool keeps one in Core, and
// a game may make its own.
//
// Transform  Matrix4             identity. Saved. Where the handles sit.
// Space      Enum.TransformSpace   World. Saved. Local follows the Transform's rotation.
// Increment  number              0. Saved. Snap step in studs; 0 is none.
// Dragging   boolean             read-only.
//
// DragBegan(handle), Dragged(handle, offset), DragEnded(handle): handle an
// Enum.DraggerHandle, offset the total world move since the drag began,
// after snapping, measured from the Transform the Dragger had at the press.
class Dragger : public PVInstance {
public:
    Dragger(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PVInstance(tag, state, id) {}

    const char* class_name() const override { return "Dragger"; }
    bool dragger() const override { return true; }

    Matrix4 transform() const override { return transform_; }
    bool local_space() const { return local_; }
    double increment() const { return increment_; }
    bool dragging() const { return dragging_; }
    DraggerHandle hovered() const { return hovered_; }
    DraggerHandle active_handle() const { return active_; }

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_transform(const Matrix4& transform);
    std::optional<std::string> set_pv_transform(const Matrix4& transform) override { return set_transform(transform); }
    std::optional<std::string> set_space(int space);
    std::optional<std::string> set_increment(double increment);

    // DraggerWorld's own writes: not edits, so no history and no saving.
    void set_hovered(DraggerHandle handle) { hovered_ = handle; }
    void set_drag(bool dragging, DraggerHandle handle);

protected:
    void on_reuse() override;

private:
    Matrix4 transform_ = matrix4_identity();
    bool local_ = false;
    double increment_ = 0.0;
    bool dragging_ = false;
    DraggerHandle hovered_ = DraggerHandle::None;
    DraggerHandle active_ = DraggerHandle::None;
};

}  // namespace engine_core
