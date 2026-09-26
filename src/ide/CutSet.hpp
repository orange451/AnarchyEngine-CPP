#pragma once

#include "DataModel.hpp"

#include <vector>

namespace ide {

// What a Cut of ids takes: the live ones, less any whose ancestor is also
// taken, since it moves with that ancestor. In the order the tree shows them.
// The caller holds the DataModel lock.
std::vector<engine_core::InstanceId> cut_set(const engine_core::DataModel& model,
                                             const std::vector<engine_core::InstanceId>& ids);

}  // namespace ide
