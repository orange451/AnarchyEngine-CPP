#pragma once

#include "DataModel.hpp"
#include "DraggerMath.hpp"
#include "InstanceRef.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Translate handles on a PVInstance: three arrows and three plane squares,
// which DraggerWorld turns mouse drags on into moves of that PVInstance. It
// binds to Adornee when that is set, else to its parent, and only when that
// is a live PVInstance under game; otherwise it draws nothing and takes no
// input. The studio's Move tool keeps one in Core; a game may make its own.
//
// Adornee    PVInstance?          nil. Saved.
// Space      Enum.DraggerSpace    World. Saved. Local follows the target's rotation.
// Increment  number               0. Saved. Snap step in studs; 0 is none.
// Dragging   boolean              read-only.
//
// DragBegan(handle), Dragged(handle, offset), DragEnded(handle): handle an
// Enum.DraggerHandle, offset the total world move since the drag began,
// after snapping. Dragged fires after the target moved.
class Dragger : public DataModel {
public:
    Dragger(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const char* class_name() const override { return "Dragger"; }
    bool dragger() const override { return true; }

    LuaSlot adornee() const;
    bool local_space() const { return local_; }
    double increment() const { return increment_; }
    bool dragging() const { return dragging_; }
    DraggerHandle hovered() const { return hovered_; }
    DraggerHandle active_handle() const { return active_; }

    // The PVInstance it moves: Adornee if set, else the parent, when that is
    // a live PVInstance under game. 0 otherwise.
    InstanceId target() const;

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_adornee(const LuaSlot& value);
    std::optional<std::string> set_space(int space);
    std::optional<std::string> set_increment(double increment);

    // DraggerWorld's own writes: not edits, so no history and no saving.
    void set_hovered(DraggerHandle handle) { hovered_ = handle; }
    void set_drag(bool dragging, DraggerHandle handle);

protected:
    void on_reuse() override;

private:
    InstanceRef adornee_;
    bool local_ = false;
    double increment_ = 0.0;
    bool dragging_ = false;
    DraggerHandle hovered_ = DraggerHandle::None;
    DraggerHandle active_ = DraggerHandle::None;
};

}  // namespace engine_core
