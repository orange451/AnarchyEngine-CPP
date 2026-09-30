#pragma once

#include "DataModel.hpp"

#include <string>
#include <vector>

namespace ide {

// What a Cut of ids takes: the live ones, less any whose ancestor is also
// taken, since it moves with that ancestor, and less game and the scene
// services, which cannot leave the tree. In the order the tree shows them.
// The caller holds the DataModel lock.
std::vector<engine_core::InstanceId> cut_set(const engine_core::DataModel& game,
                                             const std::vector<engine_core::InstanceId>& ids);

// Puts each of ids under parent, in the order given, each last among its
// children. One already under parent stays where it is. A dead id, and one
// DataModel::parent_error refuses, stays put; refused, when given, gets the
// first reason. False when none could move. Runs on the simulation thread.
bool move_set(engine_core::DataModel& world, const std::vector<engine_core::InstanceId>& ids,
              engine_core::InstanceId parent, std::string* refused = nullptr);

// Makes class_name for the explorer's or the Assets pane's insert, under asked,
// or under Workspace when asked is the root. 0, with error set, when the place
// is full or refuses the class there; nothing is left behind then.
engine_core::InstanceId insert_instance(engine_core::DataModel& world, const std::string& class_name,
                                        engine_core::InstanceId asked, std::string& error);

}  // namespace ide
