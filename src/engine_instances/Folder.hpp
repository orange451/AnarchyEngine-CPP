#pragma once

#include "DataModel.hpp"

namespace engine_core {

// A container. It holds other instances and has no spatial fields.
// class_name is defined in Folder.cpp so that file, and its Lua registration, stays linked.
class Folder : public DataModel {
public:
    Folder(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const char* class_name() const override;
};

}  // namespace engine_core
