#include "CutSet.hpp"

#include "AssetInstances.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"

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

namespace {

CopiedNode copy_node(const engine_core::DataModel& game, engine_core::InstanceId id) {
    CopiedNode node;
    const engine_core::DataModel* object = game.instance(id);
    if (object == nullptr) {
        return node;
    }
    node.class_name = object->class_name();
    node.name = game.name(id);
    object->save_properties(node.properties);
    for (const engine_core::JsonValue::Member& member : game.extra_properties(id)) {
        engine_core::bag_set(node.properties, member.first, member.second);
    }
    if (const auto* lua = dynamic_cast<const engine_core::LuaSource*>(object)) {
        node.has_source = true;
        node.source = lua->source();
    }
    for (engine_core::InstanceId child = game.first_child(id); child != 0; child = game.next_sibling(child)) {
        node.children.push_back(copy_node(game, child));
    }
    return node;
}

engine_core::InstanceId build_copy(engine_core::DataModel& world, const CopiedNode& node, engine_core::InstanceId parent,
                                   std::string* refused) {
    const auto refuse = [refused](std::string reason) {
        if (refused != nullptr && refused->empty()) {
            *refused = std::move(reason);
        }
    };
    if (world.room_left() == 0) {
        refuse(engine_core::InstanceCapacityError().what());
        return 0;
    }
    engine_core::DataModel* made = engine_core::lua_create_instance(world, node.class_name.c_str());
    if (made == nullptr) {
        refuse(node.class_name + " can't be copied.");
        return 0;
    }
    const engine_core::InstanceId id = made->id();
    for (const engine_core::JsonValue::Member& member : node.properties) {
        std::string error;
        if (!made->load_property(member.first, member.second, error) && error.empty()) {
            world.set_extra_property(id, member.first, member.second);
        }
    }
    if (node.has_source) {
        if (auto* lua = dynamic_cast<engine_core::LuaSource*>(made)) {
            lua->set_source(node.source);
        }
    }
    if (!world.rename_error(id, node.name)) {
        world.set_name(id, node.name);
    }
    if (std::optional<std::string> error = world.parent_error(id, parent)) {
        refuse(std::move(*error));
        world.destroy_tree(id);
        return 0;
    }
    world.set_parent(id, parent);
    for (const CopiedNode& child : node.children) {
        build_copy(world, child, id, refused);
    }
    return id;
}

}  // namespace

std::vector<CopiedNode> copy_set(const engine_core::DataModel& game, const std::vector<engine_core::InstanceId>& ids) {
    std::vector<CopiedNode> out;
    for (engine_core::InstanceId id : cut_set(game, ids)) {
        out.push_back(copy_node(game, id));
    }
    return out;
}

bool paste_copies(engine_core::DataModel& world, const std::vector<CopiedNode>& roots, engine_core::InstanceId parent,
                  std::vector<engine_core::InstanceId>* made, std::string* refused) {
    if (parent == engine_core::DataModel::kNoParent || (parent != 0 && !world.alive(parent))) {
        return false;
    }
    bool any = false;
    for (const CopiedNode& root : roots) {
        const engine_core::InstanceId id = build_copy(world, root, parent, refused);
        if (id != 0) {
            any = true;
            if (made != nullptr) {
                made->push_back(id);
            }
        }
    }
    return any;
}

}  // namespace ide
