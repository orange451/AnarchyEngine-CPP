#pragma once

#include "types.hpp"

#include <atomic>
#include <string>

namespace engine_core {

class DataModel;

// A saved reference to another instance, held by the target's GUID, so a
// project load, Stop, or undo can set it before the target exists, and a GUID
// no instance holds is kept as it was read. resolve caches the id it found.
class InstanceRef {
public:
    const std::string& guid() const { return guid_; }
    void set_guid(std::string guid);
    // The live instance holding the GUID, or 0. Never the root.
    InstanceId resolve(const DataModel& world) const;

private:
    std::string guid_;
    mutable std::atomic<InstanceId> cached_{0};
};

}  // namespace engine_core
