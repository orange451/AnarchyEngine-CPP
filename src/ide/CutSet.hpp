#pragma once

#include "DataModel.hpp"

#include <vector>

namespace ide {

// What a Cut of ids takes: the live ones, less any whose ancestor is also
// taken, since it moves with that ancestor. In the order the tree shows them.
// The caller holds the DataModel lock.
std::vector<engine_core::InstanceId> cut_set(const engine_core::DataModel& model,
                                             const std::vector<engine_core::InstanceId>& ids);

// Puts each of ids under parent, in the order given, each last among its
// children. One already under parent stays where it is. A dead id, the root,
// or one that would go inside itself stays put. False when none could move.
// Runs on the simulation thread.
bool move_set(engine_core::DataModel& world, const std::vector<engine_core::InstanceId>& ids,
              engine_core::InstanceId parent);

}  // namespace ide
