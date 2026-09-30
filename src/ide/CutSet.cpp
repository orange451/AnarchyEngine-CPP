#include "CutSet.hpp"

#include "AssetInstances.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_set>

namespace ide {

std::vector<engine_core::InstanceId> cut_set(const engine_core::DataModel& game,
                                             const std::vector<engine_core::InstanceId>& ids) {
    std::unordered_set<engine_core::InstanceId> wanted;
    for (engine_core::InstanceId id : ids) {
        // Not game, and not a scene service: neither can leave the tree.
        if (!game.parent_error(id, engine_core::DataModel::kNoParent)) {
            wanted.insert(id);
        }
    }
    std::vector<engine_core::InstanceId> out;
    if (wanted.empty()) {
        return out;
    }
    // Preorder from the root. A taken instance's subtree is not visited.
    std::vector<engine_core::InstanceId> stack;
    const auto push_children = [&](engine_core::InstanceId parent) {
        const std::size_t mark = stack.size();
        for (engine_core::InstanceId child = game.first_child(parent); child != 0; child = game.next_sibling(child)) {
            stack.push_back(child);
        }
        std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(mark), stack.end());
    };
    push_children(0);
    while (!stack.empty() && out.size() < wanted.size()) {
        const engine_core::InstanceId id = stack.back();
        stack.pop_back();
        if (wanted.count(id) != 0) {
            out.push_back(id);
            continue;
        }
        push_children(id);
    }
    return out;
}

bool move_set(engine_core::DataModel& world, const std::vector<engine_core::InstanceId>& ids,
              engine_core::InstanceId parent, std::string* refused) {
    constexpr engine_core::InstanceId kNone = engine_core::DataModel::kNoParent;
    if (parent == kNone || (parent != 0 && !world.alive(parent))) {
        return false;
    }
    bool any = false;
    for (engine_core::InstanceId id : ids) {
        if (id != 0 && !world.alive(id)) {
            continue;
        }
        if (world.parent(id) == parent) {
            continue;
        }
        if (std::optional<std::string> error = world.parent_error(id, parent)) {
            if (refused != nullptr && refused->empty()) {
                *refused = std::move(*error);
            }
            continue;
        }
        world.set_parent(id, parent);
        any = true;
    }
    return any;
}

engine_core::InstanceId insert_instance(engine_core::DataModel& world, const std::string& class_name,
                                        engine_core::InstanceId asked, std::string& error) {
    // game holds only services, so an insert at the top goes into Workspace.
    const engine_core::InstanceId parent = asked == 0 ? world.scene_service("Workspace") : asked;
    if (parent == engine_core::DataModel::kNoParent || (parent != 0 && !world.alive(parent))) {
        error = "That instance no longer exists";
        return 0;
    }
    if (world.room_left() == 0) {
        error = engine_core::InstanceCapacityError().what();
        return 0;
    }
    if (!engine_core::lua_creatable_known(class_name.c_str())) {
        error = "Cannot make a " + class_name;
        return 0;
    }
    if (std::optional<std::string> refused = world.placement_error_for_class(parent, class_name)) {
        error = std::move(*refused);
        return 0;
    }
    engine_core::DataModel* created = engine_core::lua_create_instance(world, class_name.c_str());
    if (created == nullptr) {
        error = "Cannot make a " + class_name;
        return 0;
    }
    world.set_parent(created->id(), parent);
    return created->id();
}

engine_core::InstanceId add_prefab_instance(engine_core::DataModel& world, engine_core::InstanceId prefab,
                                            std::string& error) {
    if (!world.alive(prefab)) {
        error = "That instance no longer exists";
        return 0;
    }
    if (dynamic_cast<engine_core::Prefab*>(world.instance(prefab)) == nullptr) {
        error = "Only a Prefab can be added as a GameObject";
        return 0;
    }
    if (world.room_left() == 0) {
        error = engine_core::InstanceCapacityError().what();
        return 0;
    }
    engine_core::GameObject& object = world.create<engine_core::GameObject>();
    world.set_name(object.id(), world.name(prefab));
    world.set_parent(object.id(), world.scene_service("Workspace"));
    engine_core::LuaSlot value;
    value.kind = engine_core::LuaSlot::Kind::Instance;
    value.id = prefab;
    object.set_prefab(value);
    return object.id();
}

}  // namespace ide
