#include "CollisionGroupEdits.hpp"

#include "DataModel.hpp"
#include "PhysicsBase.hpp"
#include "SceneService.hpp"

#include <algorithm>
#include <vector>

namespace engine_core {

std::optional<std::string> edit_collision_groups(DataModel& world, const CollisionGroupsEdit& edit) {
    CollisionGroups& groups = world.collision_groups();
    CollisionGroups copy = groups;
    if (std::optional<std::string> refused = edit(copy)) {
        return refused;
    }
    std::string before = groups.save_text();
    std::string after = copy.save_text();
    if (before == after) {
        return std::nullopt;
    }
    // The copy's revision grew from groups', so the physics world sees a change.
    groups = copy;
    if (auto* workspace = dynamic_cast<Workspace*>(world.instance(world.scene_service("Workspace")))) {
        workspace->note_collision_groups(std::move(before), std::move(after));
    }
    return std::nullopt;
}

std::optional<std::string> rename_collision_group(DataModel& world, const std::string& from, const std::string& to) {
    if (std::optional<std::string> refused =
            edit_collision_groups(world, [&](CollisionGroups& groups) { return groups.rename_group(from, to); })) {
        return refused;
    }
    if (from == to) {
        return std::nullopt;
    }
    // The place's bodies: Core, the plugins' service, is not the place, and Stop never puts it back.
    std::vector<InstanceId> open = world.get_children(0);
    open.erase(std::remove(open.begin(), open.end(), world.core()), open.end());
    while (!open.empty()) {
        const InstanceId id = open.back();
        open.pop_back();
        if (auto* body = dynamic_cast<PhysicsBase*>(world.instance(id)); body != nullptr && body->collision_group() == from) {
            body->set_collision_group(to);
        }
        for (const InstanceId child : world.get_children(id)) {
            open.push_back(child);
        }
    }
    return std::nullopt;
}

}  // namespace engine_core
