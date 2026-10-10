#pragma once

#include "Skeleton.hpp"

#include <memory>
#include <string>

namespace engine_core {

class DataModel;
class Mesh;

// The skeleton the GameObjects drawing the Prefab with this GUID pose: that of
// the first Model, in child order, whose Mesh has bones. Null when there is
// none, or no live Prefab holds the GUID. Needs the DataModel lock; a read
// lock is enough.
std::shared_ptr<const Skeleton> prefab_skeleton(const DataModel& game, const std::string& prefab_guid);

// Whether mesh's bones are skeleton's, so one pose moves both: the same table,
// by its signature. A Prefab's other Models draw unposed, in their bind pose.
bool mesh_poses_with(const Mesh& mesh, const Skeleton& skeleton);

}  // namespace engine_core
