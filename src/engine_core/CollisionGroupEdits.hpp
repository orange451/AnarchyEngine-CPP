#pragma once

#include "CollisionGroups.hpp"

#include <functional>
#include <optional>
#include <string>

namespace engine_core {

class DataModel;

// The writes that change a place's collision groups (DataModel::collision_groups)
// as an edit: PhysicsService's methods and the Studio's Collision Groups pane
// (docs/superpowers/specs/2026-10-12-collision-groups-editor-design.md §2).
// SimulationThread, under the write lock.

using CollisionGroupsEdit = std::function<std::optional<std::string>(CollisionGroups&)>;

// Applies edit to a copy of world's groups. When it succeeds and the saved
// form (CollisionGroups::save_text) changed, puts the copy in and notes the
// change on Workspace's hidden CollisionGroups: an undo step inside an open
// recording, Changed, and an unsaved place. Returns edit's refusal, changing
// nothing.
std::optional<std::string> edit_collision_groups(DataModel& world, const CollisionGroupsEdit& edit);

// PhysicsService:RenameCollisionGroup: renames the group, keeping its bit,
// type, and pair settings, then moves every PhysicsBase in the place whose
// CollisionGroup is from to to.
std::optional<std::string> rename_collision_group(DataModel& world, const std::string& from, const std::string& to);

}  // namespace engine_core
