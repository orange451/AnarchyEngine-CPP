#include "InstanceRef.hpp"

#include "DataModel.hpp"

namespace engine_core {

void InstanceRef::set_guid(std::string guid) {
    guid_ = std::move(guid);
    cached_.store(0, std::memory_order_relaxed);
}

InstanceId InstanceRef::resolve(const DataModel& world) const {
    if (guid_.empty()) {
        return 0;
    }
    const InstanceId cached = cached_.load(std::memory_order_relaxed);
    if (cached != 0 && world.alive(cached) && world.guid(cached) == guid_) {
        return cached;
    }
    const std::optional<InstanceId> found = world.find_guid(guid_);
    const InstanceId id = found && *found != 0 ? *found : 0;
    cached_.store(id, std::memory_order_relaxed);
    return id;
}

}  // namespace engine_core
