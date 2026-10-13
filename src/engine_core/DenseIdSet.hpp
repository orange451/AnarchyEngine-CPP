#pragma once

#include "Contract.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace engine_core {

// Dense set of live instance ids. ids() is packed and unordered; contains,
// position, insert, and erase are O(1) through a per-slot position map keyed
// by id_slot. A stale generation misses because the mapped entry no longer
// holds the queried id. erase is swap-and-pop and reports the swap, so an
// owner keeping a parallel payload array mirrors it. Not thread-safe: the
// owner touches it under whatever already guards its writes.
class DenseIdSet {
public:
    // Sizes the position map and the dense array for this many slots up
    // front. Both grow past it as higher slots arrive, so this is a hint.
    void reserve(std::size_t slots) {
        index_.assign(slots, -1);
        dense_.reserve(slots);
    }

    std::size_t size() const { return dense_.size(); }
    const std::vector<InstanceId>& ids() const { return dense_; }
    bool contains(InstanceId id) const { return position(id) >= 0; }

    // Position of id in ids(), or -1 when absent.
    int position(InstanceId id) const {
        const std::uint32_t slot = id_slot(id);
        if (slot >= index_.size()) {
            return -1;
        }
        const int pos = index_[slot];
        if (pos < 0 || dense_[static_cast<std::size_t>(pos)] != id) {
            return -1;
        }
        return pos;
    }

    // False when id is already present. Grows to take any slot.
    bool insert(InstanceId id) {
        if (position(id) >= 0) {
            return false;
        }
        const std::uint32_t slot = id_slot(id);
        if (slot >= index_.size()) {
            index_.resize(std::max<std::size_t>(std::size_t{slot} + 1, index_.size() * 2), -1);
        }
        index_[slot] = static_cast<int>(dense_.size());
        dense_.push_back(id);
        return true;
    }

    // Removes id. Returns the position it vacated, or -1 when absent. When
    // another id was swapped into that position, *moved names it, else 0.
    int erase(InstanceId id, InstanceId* moved = nullptr) {
        if (moved != nullptr) {
            *moved = 0;
        }
        const int pos = position(id);
        if (pos < 0) {
            return -1;
        }
        const InstanceId last = dense_.back();
        dense_[static_cast<std::size_t>(pos)] = last;
        dense_.pop_back();
        index_[id_slot(id)] = -1;
        if (last != id) {
            index_[id_slot(last)] = pos;
            if (moved != nullptr) {
                *moved = last;
            }
        }
        return pos;
    }

    void clear() {
        for (const InstanceId id : dense_) {
            index_[id_slot(id)] = -1;
        }
        dense_.clear();
    }

private:
    std::vector<InstanceId> dense_;
    // id_slot -> position in dense_, -1 when absent.
    std::vector<int> index_;
};

}  // namespace engine_core
