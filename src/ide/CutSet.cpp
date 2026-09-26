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

}  // namespace ide
