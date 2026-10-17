#pragma once

#include "Events.hpp"
#include "types.hpp"

#include <cstdint>
#include <functional>

namespace engine_core {

// One change to the tree, as replication reads it. There is no "created":
// create<T>() makes an instance unparented, and its first set_parent reports
// Parented from nowhere. Quiet writes (physics, the pose blend) are never
// reported. The sink reads current values from the tree itself.
struct TreeChange {
    enum class Kind : std::uint8_t { Parented, Destroyed, Renamed, Property };
    Kind kind = Kind::Property;
    InstanceId id = 0;
    // Parented: where it was, DataModel::kNoParent when it was nowhere.
    InstanceId previous_parent = 0;
    // Property: the field, and for Field::Reflected the lua_property_id.
    Field field = Field::Name;
    std::uint64_t payload = 0;
};

using TreeChangeSink = std::function<void(const TreeChange&)>;

}  // namespace engine_core
