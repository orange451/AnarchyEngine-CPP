#pragma once

#include "DataModel.hpp"

#include <optional>
#include <string>

namespace engine_core {

// An instance with a place in the world: every class with a Transform
// property is one (GameObject and its subclasses, PhysicsObject, Attachment). Each keeps
// its Transform in its own way; this gives the rest of the engine one way to
// read it, such as a SoundEmitter parented to one. It adds no properties.
// GameObject.cpp registers its Lua class.
class PVInstance : public DataModel {
public:
    PVInstance(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    // The Transform. A dead instance's is a zero matrix.
    virtual Matrix4 transform() const = 0;
    // Writes the Transform through the class's own setter, so it is checked,
    // recorded, and fires Changed like any edit. Returns why it refused.
    // SimulationThread.
    virtual std::optional<std::string> set_pv_transform(const Matrix4& transform) = 0;
};

}  // namespace engine_core
