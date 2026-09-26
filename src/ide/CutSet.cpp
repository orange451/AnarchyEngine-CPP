#include "CutSet.hpp"

#include <algorithm>
#include <cstddef>
#include <unordered_set>

namespace ide {

std::vector<engine_core::InstanceId> cut_set(const engine_core::DataModel& game,
                                             const std::vector<engine_core::InstanceId>& ids) {
    std::unordered_set<engine_core::InstanceId> wanted;
    for (engine_core::InstanceId id : ids) {
        if (id != 0 && game.alive(id)) {
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
              engine_core::InstanceId parent) {
    constexpr engine_core::InstanceId kNone = engine_core::DataModel::kNoParent;
    if (parent == kNone || (parent != 0 && !world.alive(parent))) {
        return false;
    }
    bool any = false;
    for (engine_core::InstanceId id : ids) {
        if (id == 0 || !world.alive(id)) {
            continue;
        }
        bool cycle = false;
        for (engine_core::InstanceId cursor = parent; cursor != 0 && cursor != kNone; cursor = world.parent(cursor)) {
            if (cursor == id) {
                cycle = true;
                break;
            }
        }
        if (cycle) {
            continue;
        }
        if (world.parent(id) == parent) {
            continue;
        }
        world.set_parent(id, parent);
        any = true;
    }
    return any;
}

}  // namespace ide
