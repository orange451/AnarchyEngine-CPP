#pragma once

#include "DataModel.hpp"

namespace engine_core {

// An instance with a place in the world: every class with a Transform
// property is one (GameObject and its subclasses, PhysicsObject). Each keeps
// its Transform in its own way; this gives the rest of the engine one way to
// read it, such as a SoundEmitter parented to one. It adds no properties.
// GameObject.cpp registers its Lua class.
class PVInstance : public DataModel {
public:
    PVInstance(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    // The Transform. A dead instance's is a zero matrix.
    virtual Matrix4 transform() const = 0;
};

}  // namespace engine_core
