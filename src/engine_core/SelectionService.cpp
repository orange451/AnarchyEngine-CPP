#include "SelectionService.hpp"

#include <algorithm>
#include <utility>

namespace engine_core {

std::vector<InstanceId> SelectionService::get() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ids_;
}

std::vector<InstanceId> SelectionService::get(std::uint64_t& revision) const {
    std::lock_guard<std::mutex> lock(mu_);
    revision = revision_.load(std::memory_order_relaxed);
    return ids_;
}

bool SelectionService::set(std::vector<InstanceId> ids) {
    std::vector<InstanceId> kept;
    kept.reserve(ids.size());
    for (InstanceId id : ids) {
        if (id != 0 && std::find(kept.begin(), kept.end(), id) == kept.end()) {
            kept.push_back(id);
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (kept == ids_) {
        return false;
    }
    ids_ = std::move(kept);
    revision_.fetch_add(1, std::memory_order_release);
    return true;
}

}  // namespace engine_core
