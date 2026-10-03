#pragma once

#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace engine_core {

// The instances the studio has selected, in the order they were picked.
// Scripts reach it as game:GetService("Selection"). The explorers show it and
// write it on a click, so either side sees what the other chose.
//
// Any thread may call it. The list has its own lock, never the DataModel lock,
// and nothing here reads the world. DataModel::destroy takes a destroyed id out
// of the list, so SelectionChanged fires when a selected instance is deleted.
class SelectionService {
public:
    std::vector<InstanceId> get() const;
    // The list and the revision it belongs to, read together.
    std::vector<InstanceId> get(std::uint64_t& revision) const;

    // Replaces the list. A repeated id keeps its first place, and the root
    // (id 0) is dropped. A list equal to the current one changes nothing.
    // Returns true when the list changed.
    bool set(std::vector<InstanceId> ids);
    // Takes id out of the list, if it is there. Returns true when the list changed.
    bool remove(InstanceId id);

    // Bumps on every change and never resets. A reader that saw this value
    // has already seen the list.
    std::uint64_t revision() const { return revision_.load(std::memory_order_acquire); }

private:
    mutable std::mutex mu_;
    std::vector<InstanceId> ids_;
    std::atomic<std::uint64_t> revision_{0};
};

}  // namespace engine_core
