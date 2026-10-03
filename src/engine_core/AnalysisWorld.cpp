#include "AnalysisWorld.hpp"

#include "DataModel.hpp"
#include "LuaSource.hpp"
#include "ModuleScript.hpp"

#include <algorithm>

namespace engine_core {
namespace analysis {

void WorldSnap::reindex() {
    index.clear();
    index.reserve(nodes.size());
    for (std::size_t at = 0; at < nodes.size(); ++at) {
        index.emplace(nodes[at].id, at);
    }
}

const NodeSnap* WorldSnap::find(InstanceId id) const {
    const auto found = index.find(id);
    return found == index.end() ? nullptr : &nodes[found->second];
}

std::optional<InstanceId> WorldSnap::child_named(InstanceId parent, std::string_view name) const {
    const NodeSnap* parent_node = find(parent);
    if (parent_node == nullptr) {
        return std::nullopt;
    }
    for (InstanceId child : parent_node->children) {
        const NodeSnap* node = find(child);
        if (node != nullptr && node->name == name) {
            return child;
        }
    }
    return std::nullopt;
}

std::optional<InstanceId> WorldSnap::module_named(std::string_view name, std::optional<InstanceId> prefer_parent) const {
    std::optional<InstanceId> fallback;
    for (const NodeSnap& node : nodes) {
        if (!node.module || node.name != name) {
            continue;
        }
        if (prefer_parent && node.parent == *prefer_parent) {
            return node.id;
        }
        if (!fallback) {
            fallback = node.id;
        }
    }
    return fallback;
}

std::optional<InstanceId> WorldSnap::workspace() const {
    const NodeSnap* root_node = find(root);
    if (root_node != nullptr) {
        for (InstanceId child : root_node->children) {
            const NodeSnap* node = find(child);
            if (node != nullptr && node->class_name == "Workspace") {
                return child;
            }
        }
    }
    return std::nullopt;
}

std::shared_ptr<WorldSnap> capture_world(DataModel& game) {
    auto world = std::make_shared<WorldSnap>();
    world->root = game.id();
    NodeSnap root;
    root.id = world->root;
    root.parent = DataModel::kNoParent;
    root.name = game.name(world->root);
    root.class_name = game.class_name();
    world->nodes.push_back(std::move(root));
    game.for_each_instance([&](DataModel& object) {
        NodeSnap node;
        node.id = object.id();
        node.parent = game.parent(object.id());
        node.name = game.name(object.id());
        node.class_name = object.class_name() != nullptr ? object.class_name() : "";
        if (auto* source = dynamic_cast<LuaSource*>(&object)) {
            node.lua = true;
            node.module = dynamic_cast<ModuleScript*>(source) != nullptr;
            node.source = source->source();
        }
        world->nodes.push_back(std::move(node));
    });
    world->reindex();
    for (NodeSnap& node : world->nodes) {
        for (InstanceId child = game.first_child(node.id); child != 0; child = game.next_sibling(child)) {
            node.children.push_back(child);
        }
    }
    keep_place_only(*world);
    return world;
}

void keep_place_only(WorldSnap& world) {
    std::unordered_set<InstanceId> placed;
    std::vector<InstanceId> stack;
    if (world.find(world.root) != nullptr) {
        stack.push_back(world.root);
    }
    while (!stack.empty()) {
        const InstanceId id = stack.back();
        stack.pop_back();
        const NodeSnap* node = world.find(id);
        if (node == nullptr || !placed.insert(id).second) {
            continue;
        }
        stack.insert(stack.end(), node->children.begin(), node->children.end());
    }
    if (placed.size() == world.nodes.size()) {
        return;
    }
    world.nodes.erase(std::remove_if(world.nodes.begin(), world.nodes.end(),
                                     [&placed](const NodeSnap& node) { return placed.count(node.id) == 0; }),
                      world.nodes.end());
    world.reindex();
}

std::shared_ptr<WorldSnap> world_from_nodes(const std::vector<LuaNode>& nodes) {
    auto world = std::make_shared<WorldSnap>();
    bool rooted = false;
    for (const LuaNode& item : nodes) {
        NodeSnap node;
        node.id = item.id;
        node.parent = item.parent;
        node.name = item.name;
        node.class_name = item.class_name;
        node.source = item.source;
        node.module = item.class_name == "ModuleScript";
        node.lua = node.module || item.class_name == "Script";
        if (!rooted && item.parent == DataModel::kNoParent &&
            (item.class_name == "Game" || item.class_name == "DataModel")) {
            world->root = item.id;
            rooted = true;
        }
        world->nodes.push_back(std::move(node));
    }
    // Children in the order the nodes came, which completion_world makes the
    // tree's sibling order, as capture_world has it.
    world->reindex();
    for (std::size_t at = 0; at < world->nodes.size(); ++at) {
        const InstanceId id = world->nodes[at].id;
        const InstanceId parent = world->nodes[at].parent;
        const auto found = world->index.find(parent);
        if (found != world->index.end() && parent != id) {
            world->nodes[found->second].children.push_back(id);
        }
    }
    return world;
}

// Completion's snapshots and the analyzer's are made apart, so they are
// compared by what they hold, not by identity.
bool same_tree(const WorldSnap& a, const WorldSnap& b) {
    if (a.root != b.root || a.nodes.size() != b.nodes.size()) {
        return false;
    }
    for (const NodeSnap& node : b.nodes) {
        const NodeSnap* was = a.find(node.id);
        // Children in the same order too: FindFirstChild takes the first of two
        // siblings with one name, and the place's types follow that order.
        if (was == nullptr || was->parent != node.parent || was->name != node.name ||
            was->class_name != node.class_name || was->lua != node.lua ||
            was->module != node.module || was->children != node.children) {
            return false;
        }
    }
    return true;
}

TreeDiff diff_worlds(const WorldSnap& before, const WorldSnap& after) {
    TreeDiff diff;
    const auto mark_parent = [&diff](InstanceId parent) {
        if (parent != DataModel::kNoParent) {
            diff.parents.insert(parent);
        }
    };
    for (const NodeSnap& node : after.nodes) {
        const NodeSnap* was = before.find(node.id);
        if (was == nullptr) {
            mark_parent(node.parent);
            if (node.lua) {
                diff.added_scripts.push_back(node.id);
            }
            continue;
        }
        if (was->parent != node.parent) {
            diff.moved.insert(node.id);
            mark_parent(was->parent);
            mark_parent(node.parent);
        }
        if (was->name != node.name || was->class_name != node.class_name) {
            diff.moved.insert(node.id);
            mark_parent(node.parent);
        }
        if (was->children != node.children) {
            diff.parents.insert(node.id);
        }
        if (node.lua && was->lua && was->source != node.source) {
            diff.edited_scripts.push_back(node.id);
        }
    }
    for (const NodeSnap& node : before.nodes) {
        if (after.find(node.id) != nullptr) {
            continue;
        }
        diff.moved.insert(node.id);
        mark_parent(node.parent);
        if (node.lua) {
            diff.removed_scripts.push_back(node.id);
        }
    }
    return diff;
}

}  // namespace analysis
}  // namespace engine_core
