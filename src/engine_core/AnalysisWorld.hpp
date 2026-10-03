#pragma once

#include "LuaApi.hpp"
#include "types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine_core {

class DataModel;

namespace analysis {

// One instance as script analysis copies it from the tree.
struct NodeSnap {
    InstanceId id = 0;
    InstanceId parent = 0xffffffffu;
    std::string name;
    std::string class_name;
    std::string source;
    bool lua = false;
    bool module = false;
    // Direct children in the same order FindFirstChild walks them.
    std::vector<InstanceId> children;
};

// The tree a check reads, copied on the gameplay thread so the checkers never
// touch the DataModel. The type hooks look instances up constantly, so lookups
// go through `index`, which reindex() rebuilds after nodes change.
struct WorldSnap {
    InstanceId root = 0;
    // Captured while the simulation ran: the play tree, not the authored one.
    bool play = false;
    std::vector<NodeSnap> nodes;
    std::unordered_map<InstanceId, std::size_t> index;

    void reindex();
    const NodeSnap* find(InstanceId id) const;
    std::optional<InstanceId> child_named(InstanceId parent, std::string_view name) const;
    // The ModuleScript with this name, preferring one under prefer_parent.
    std::optional<InstanceId> module_named(std::string_view name, std::optional<InstanceId> prefer_parent) const;
    // workspace: the root's Workspace child.
    std::optional<InstanceId> workspace() const;
};

// The place now: the root and everything under it. An instance outside it,
// such as the children a destroyed folder leaves parentless, is not in the
// place, and neither are its scripts. Gameplay thread, or a thread holding the
// DataModel lock.
std::shared_ptr<WorldSnap> capture_world(DataModel& game);

// Drops every node the root does not reach through children, keeping the rest
// in order. capture_world does this; completion's snapshots keep detached
// instances, so a buffer outside the tree still knows what it is.
void keep_place_only(WorldSnap& world);

// The place as completion sees it. The root is the parentless Game or DataModel,
// and children follow the order of `nodes`.
std::shared_ptr<WorldSnap> world_from_nodes(const std::vector<LuaNode>& nodes);

// Two snapshots with the same instances, parents, names, classes, and child
// order. Sources may differ.
bool same_tree(const WorldSnap& a, const WorldSnap& b);

// What changed between two snapshots of one place, as far as a script's types can tell.
struct TreeDiff {
    // Instances whose child list changed: a child added, removed, renamed, or
    // moved among its siblings. A lookup on one may now find something else.
    std::unordered_set<InstanceId> parents;
    // Instances renamed, reparented, or destroyed.
    std::unordered_set<InstanceId> moved;
    std::vector<InstanceId> added_scripts;
    std::vector<InstanceId> removed_scripts;
    // Scripts in both whose source differs.
    std::vector<InstanceId> edited_scripts;
};

TreeDiff diff_worlds(const WorldSnap& before, const WorldSnap& after);

}  // namespace analysis
}  // namespace engine_core
