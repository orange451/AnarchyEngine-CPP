#include "CutSet.hpp"

#include <algorithm>
#include <cstddef>
#include <unordered_set>

namespace ide {

std::vector<engine_core::InstanceId> cut_set(const engine_core::DataModel& model,
                                             const std::vector<engine_core::InstanceId>& ids) {
    std::unordered_set<engine_core::InstanceId> wanted;
    for (engine_core::InstanceId id : ids) {
        if (id != 0 && model.alive(id)) {
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
        for (engine_core::InstanceId child = model.first_child(parent); child != 0; child = model.next_sibling(child)) {
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
              engine_core::InstanceId parent, engine_core::InstanceId before) {
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
        // Counted without id, so each one lands just before the anchor and
        // the ones moved ahead of it keep their order.
        std::vector<engine_core::InstanceId> siblings = world.get_children(parent);
        siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
        const auto anchor = before == 0 ? siblings.end() : std::find(siblings.begin(), siblings.end(), before);
        const int index = anchor == siblings.end() ? -1 : static_cast<int>(anchor - siblings.begin());
        world.set_parent_at(id, parent, index);
        any = true;
    }
    return any;
}

}  // namespace ide
