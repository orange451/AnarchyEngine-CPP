#pragma once

#include "CollisionGroups.hpp"
#include "DataModel.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ide {

// What the Collision Groups pane shows and does, without widgets: the place's
// collision groups, how many bodies are in each, which pairs collide, and the
// writes that change them
// (docs/superpowers/specs/2026-10-12-collision-groups-editor-design.md §3).

// One registered group, as a row shows it.
struct CollisionGroupRow {
    std::string name;
    engine_core::CollisionGroups::FilterType filter_type = engine_core::CollisionGroups::FilterType::Exclude;
    // PhysicsBases in the place whose CollisionGroup is exactly name.
    std::size_t bodies = 0;

    bool operator==(const CollisionGroupRow& other) const {
        return name == other.name && filter_type == other.filter_type && bodies == other.bodies;
    }
    bool operator!=(const CollisionGroupRow& other) const { return !(*this == other); }
};

// A name bodies use that no group has: they collide as Default.
struct UnregisteredGroup {
    std::string name;
    std::size_t bodies = 0;

    bool operator==(const UnregisteredGroup& other) const { return name == other.name && bodies == other.bodies; }
};

struct CollisionGroupsView {
    // In registration order, Default first.
    std::vector<CollisionGroupRow> groups;
    // collide[i][j], j <= i: whether groups[i] and groups[j] collide. The
    // lower triangle the matrix draws, the diagonal included.
    std::vector<std::vector<bool>> collide;
    // By name.
    std::vector<UnregisteredGroup> unregistered;
    bool playing = false;

    bool operator==(const CollisionGroupsView& other) const {
        return groups == other.groups && collide == other.collide && unregistered == other.unregistered &&
               playing == other.playing;
    }
    bool operator!=(const CollisionGroupsView& other) const { return !(*this == other); }
    // The row named name, or -1.
    int index_of(const std::string& name) const;
};

// The caller holds a read lock.
CollisionGroupsView read_collision_groups(const engine_core::DataModel& world);
// The PhysicsBases in the place whose CollisionGroup is name, in tree order.
// The caller holds a read lock.
std::vector<engine_core::InstanceId> collision_group_bodies(const engine_core::DataModel& world,
                                                            const std::string& name);
// "New Group", or "New Group 2", 3, ...: the first no group has.
std::string fresh_group_name(const engine_core::CollisionGroups& groups);

// SimulationThread. Each is one undo step, named as the pane's action:
// "New Collision Group", "Rename Collision Group", "Delete Collision Group",
// "Set Collision Group Filter", "Set Groups Collidable", "Assign Collision
// Group", and "Register Collision Groups". Each returns why it refused, having
// changed nothing.

// Registers a fresh name (fresh_group_name), Exclude, into made.
std::optional<std::string> run_new_group(engine_core::DataModel& world, std::string& made);
// to is trimmed of spaces first; it must not be empty or another group's.
// The group's bodies move with it.
std::optional<std::string> run_rename_group(engine_core::DataModel& world, const std::string& from,
                                            const std::string& to);
// What deleting a group does to the bodies in it.
enum class GroupBodies {
    // Their CollisionGroup becomes "Default".
    MoveToDefault,
    // They keep naming it, and collide as Default while no group has the name.
    Keep,
};
// Unregisters the group, first moving its bodies to Default unless bodies is Keep.
std::optional<std::string> run_delete_group(engine_core::DataModel& world, const std::string& name,
                                            GroupBodies bodies = GroupBodies::MoveToDefault);
std::optional<std::string> run_set_filter_type(engine_core::DataModel& world, const std::string& name,
                                               engine_core::CollisionGroups::FilterType filter_type);
std::optional<std::string> run_set_collidable(engine_core::DataModel& world, const std::string& a,
                                              const std::string& b, bool collidable);
// Puts each PhysicsBase among ids, and under them, in group name; how many
// changed goes in moved.
std::optional<std::string> run_assign_group(engine_core::DataModel& world, const std::vector<engine_core::InstanceId>& ids,
                                            const std::string& name, std::size_t& moved);
// Registers each of names that is not registered, Exclude.
std::optional<std::string> run_register_groups(engine_core::DataModel& world, const std::vector<std::string>& names);

}  // namespace ide
