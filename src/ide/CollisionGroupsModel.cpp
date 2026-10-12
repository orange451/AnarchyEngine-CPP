#include "CollisionGroupsModel.hpp"

#include "CollisionGroupEdits.hpp"
#include "PhysicsBase.hpp"
#include "ScopedRecording.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace ide {

namespace {

using engine_core::CollisionGroups;
using engine_core::InstanceId;
using engine_core::PhysicsBase;

// Every PhysicsBase in the place, under the root, in tree order. Core, the
// plugins' service, is not the place: Stop never puts it back.
template <typename Visit>
void each_body(const engine_core::DataModel& world, InstanceId from, Visit&& visit) {
    for (const InstanceId child : world.get_children(from)) {
        if (from == 0 && child == world.core()) {
            continue;
        }
        if (const auto* body = dynamic_cast<const PhysicsBase*>(world.instance(child))) {
            visit(child, *body);
        }
        each_body(world, child, visit);
    }
}

std::string trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

}  // namespace

int CollisionGroupsView::index_of(const std::string& name) const {
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

CollisionGroupsView read_collision_groups(const engine_core::DataModel& world) {
    CollisionGroupsView view;
    const CollisionGroups& groups = world.collision_groups();
    view.playing = world.simulation_running();
    std::unordered_map<std::string, std::size_t> counts;
    each_body(world, 0, [&counts](InstanceId, const PhysicsBase& body) { ++counts[body.collision_group()]; });
    for (const CollisionGroups::Info& info : groups.groups()) {
        CollisionGroupRow row;
        row.name = info.name;
        row.filter_type = info.filter_type;
        const auto found = counts.find(info.name);
        row.bodies = found != counts.end() ? found->second : 0;
        view.groups.push_back(std::move(row));
    }
    for (std::size_t i = 0; i < view.groups.size(); ++i) {
        std::vector<bool> line;
        for (std::size_t j = 0; j <= i; ++j) {
            line.push_back(groups.collidable(view.groups[i].name, view.groups[j].name));
        }
        view.collide.push_back(std::move(line));
    }
    std::map<std::string, std::size_t> unknown;
    for (const auto& [name, count] : counts) {
        // An empty name is Default's, as any unregistered one is, but no group could take it.
        if (!name.empty() && !groups.registered(name)) {
            unknown[name] = count;
        }
    }
    for (const auto& [name, count] : unknown) {
        view.unregistered.push_back(UnregisteredGroup{name, count});
    }
    return view;
}

std::vector<InstanceId> collision_group_bodies(const engine_core::DataModel& world, const std::string& name) {
    std::vector<InstanceId> out;
    each_body(world, 0, [&](InstanceId id, const PhysicsBase& body) {
        if (body.collision_group() == name) {
            out.push_back(id);
        }
    });
    return out;
}

std::string fresh_group_name(const CollisionGroups& groups) {
    const std::string base = "New Group";
    if (!groups.registered(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        std::string name = base + " " + std::to_string(n);
        if (!groups.registered(name)) {
            return name;
        }
    }
}

std::optional<std::string> run_new_group(engine_core::DataModel& world, std::string& made) {
    ScopedRecording step(world, "New Collision Group");
    std::string name = fresh_group_name(world.collision_groups());
    if (std::optional<std::string> refused = engine_core::edit_collision_groups(
            world, [&name](CollisionGroups& groups) {
                return groups.register_group(name, CollisionGroups::FilterType::Exclude);
            })) {
        return refused;
    }
    made = std::move(name);
    return std::nullopt;
}

std::optional<std::string> run_rename_group(engine_core::DataModel& world, const std::string& from,
                                            const std::string& to) {
    const std::string name = trimmed(to);
    if (name.empty()) {
        return std::string("A collision group needs a name");
    }
    ScopedRecording step(world, "Rename Collision Group");
    return engine_core::rename_collision_group(world, from, name);
}

std::optional<std::string> run_delete_group(engine_core::DataModel& world, const std::string& name) {
    if (name == CollisionGroups::kDefault) {
        return std::string("Default cannot be unregistered");
    }
    if (!world.collision_groups().registered(name)) {
        return std::nullopt;
    }
    ScopedRecording step(world, "Delete Collision Group");
    for (const InstanceId id : collision_group_bodies(world, name)) {
        if (auto* body = dynamic_cast<PhysicsBase*>(world.instance(id))) {
            body->set_collision_group(CollisionGroups::kDefault);
        }
    }
    return engine_core::edit_collision_groups(
        world, [&name](CollisionGroups& groups) { return groups.unregister_group(name); });
}

std::optional<std::string> run_set_filter_type(engine_core::DataModel& world, const std::string& name,
                                               CollisionGroups::FilterType filter_type) {
    ScopedRecording step(world, "Set Collision Group Filter");
    return engine_core::edit_collision_groups(
        world, [&](CollisionGroups& groups) { return groups.set_filter_type(name, filter_type); });
}

std::optional<std::string> run_set_collidable(engine_core::DataModel& world, const std::string& a,
                                              const std::string& b, bool collidable) {
    ScopedRecording step(world, "Set Groups Collidable");
    return engine_core::edit_collision_groups(
        world, [&](CollisionGroups& groups) { return groups.set_collidable(a, b, collidable); });
}

std::optional<std::string> run_assign_group(engine_core::DataModel& world, const std::vector<InstanceId>& ids,
                                            const std::string& name, std::size_t& moved) {
    moved = 0;
    if (!world.collision_groups().registered(name)) {
        return "Unknown collision group \"" + name + "\"";
    }
    // Each body once, though a selection may hold it and its Folder both.
    std::vector<InstanceId> bodies;
    for (const InstanceId id : ids) {
        if (!world.alive(id)) {
            continue;
        }
        if (dynamic_cast<const PhysicsBase*>(world.instance(id)) != nullptr) {
            bodies.push_back(id);
        }
        each_body(world, id, [&bodies](InstanceId body, const PhysicsBase&) { bodies.push_back(body); });
    }
    std::sort(bodies.begin(), bodies.end());
    bodies.erase(std::unique(bodies.begin(), bodies.end()), bodies.end());
    ScopedRecording step(world, "Assign Collision Group");
    for (const InstanceId id : bodies) {
        auto* body = dynamic_cast<PhysicsBase*>(world.instance(id));
        if (body != nullptr && body->collision_group() != name) {
            body->set_collision_group(name);
            ++moved;
        }
    }
    return std::nullopt;
}

std::optional<std::string> run_register_groups(engine_core::DataModel& world, const std::vector<std::string>& names) {
    ScopedRecording step(world, "Register Collision Groups");
    return engine_core::edit_collision_groups(world, [&names](CollisionGroups& groups) -> std::optional<std::string> {
        for (const std::string& name : names) {
            if (std::optional<std::string> refused =
                    groups.register_group(name, CollisionGroups::FilterType::Exclude)) {
                return refused;
            }
        }
        return std::nullopt;
    });
}

}  // namespace ide
