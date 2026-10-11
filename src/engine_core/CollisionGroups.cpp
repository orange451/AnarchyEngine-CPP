#include "CollisionGroups.hpp"

#include <algorithm>

namespace engine_core {
namespace {

std::uint64_t bit(int index) { return std::uint64_t{1} << index; }

std::string unknown(const std::string& name) {
    return "Unknown collision group \"" + name + "\"; did you forget to call RegisterCollisionGroup?";
}

}  // namespace

CollisionGroups::CollisionGroups() { reset(); }

void CollisionGroups::reset() {
    slots_ = {};
    explicit_ = {};
    value_ = {};
    next_order_ = 0;
    slots_[0].name = kDefault;
    slots_[0].order = next_order_++;
    changed();
}

int CollisionGroups::bit_of(std::string_view name) const {
    if (name.empty()) {
        return -1;
    }
    for (int i = 0; i < kMaxGroups; ++i) {
        if (slots_[static_cast<std::size_t>(i)].name == name) {
            return i;
        }
    }
    return -1;
}

int CollisionGroups::resolved(std::string_view name) const {
    const int found = bit_of(name);
    return found >= 0 ? found : 0;
}

bool CollisionGroups::pair(int a, int b) const {
    const std::size_t ia = static_cast<std::size_t>(a);
    if ((explicit_[ia] & bit(b)) != 0) {
        return (value_[ia] & bit(b)) != 0;
    }
    return slots_[ia].filter_type == FilterType::Exclude &&
           slots_[static_cast<std::size_t>(b)].filter_type == FilterType::Exclude;
}

void CollisionGroups::changed() {
    for (int i = 0; i < kMaxGroups; ++i) {
        std::uint64_t mask = 0;
        if (!slots_[static_cast<std::size_t>(i)].name.empty()) {
            for (int j = 0; j < kMaxGroups; ++j) {
                if (!slots_[static_cast<std::size_t>(j)].name.empty() && pair(i, j)) {
                    mask |= bit(j);
                }
            }
        }
        mask_[static_cast<std::size_t>(i)] = mask;
    }
    ++revision_;
}

std::optional<std::string> CollisionGroups::register_group(const std::string& name, FilterType filter_type) {
    if (name.empty()) {
        return std::string("Collision group name must not be empty");
    }
    if (bit_of(name) >= 0) {
        return std::nullopt;
    }
    int free = -1;
    for (int i = 0; i < kMaxGroups && free < 0; ++i) {
        if (slots_[static_cast<std::size_t>(i)].name.empty()) {
            free = i;
        }
    }
    if (free < 0) {
        return std::string("Too many collision groups (the most is 62)");
    }
    const std::size_t slot = static_cast<std::size_t>(free);
    slots_[slot].name = name;
    slots_[slot].filter_type = filter_type;
    slots_[slot].order = next_order_++;
    explicit_[slot] = 0;
    value_[slot] = 0;
    for (int i = 0; i < kMaxGroups; ++i) {
        explicit_[static_cast<std::size_t>(i)] &= ~bit(free);
        value_[static_cast<std::size_t>(i)] &= ~bit(free);
    }
    changed();
    return std::nullopt;
}

std::optional<std::string> CollisionGroups::unregister_group(const std::string& name) {
    if (name == kDefault) {
        return std::string("Default cannot be unregistered");
    }
    const int found = bit_of(name);
    if (found < 0) {
        return std::nullopt;
    }
    slots_[static_cast<std::size_t>(found)] = Slot{};
    explicit_[static_cast<std::size_t>(found)] = 0;
    value_[static_cast<std::size_t>(found)] = 0;
    for (int i = 0; i < kMaxGroups; ++i) {
        explicit_[static_cast<std::size_t>(i)] &= ~bit(found);
        value_[static_cast<std::size_t>(i)] &= ~bit(found);
    }
    changed();
    return std::nullopt;
}

std::optional<std::string> CollisionGroups::set_filter_type(const std::string& name, FilterType filter_type) {
    const int found = bit_of(name);
    if (found < 0) {
        return unknown(name);
    }
    slots_[static_cast<std::size_t>(found)].filter_type = filter_type;
    changed();
    return std::nullopt;
}

std::optional<std::string> CollisionGroups::set_collidable(const std::string& a, const std::string& b,
                                                           bool collidable) {
    const int ia = bit_of(a);
    if (ia < 0) {
        return unknown(a);
    }
    const int ib = bit_of(b);
    if (ib < 0) {
        return unknown(b);
    }
    for (const auto& [from, to] : {std::pair<int, int>{ia, ib}, std::pair<int, int>{ib, ia}}) {
        explicit_[static_cast<std::size_t>(from)] |= bit(to);
        if (collidable) {
            value_[static_cast<std::size_t>(from)] |= bit(to);
        } else {
            value_[static_cast<std::size_t>(from)] &= ~bit(to);
        }
    }
    changed();
    return std::nullopt;
}

bool CollisionGroups::registered(std::string_view name) const { return bit_of(name) >= 0; }

bool CollisionGroups::collidable(std::string_view a, std::string_view b) const {
    return pair(resolved(a), resolved(b));
}

std::vector<CollisionGroups::Info> CollisionGroups::groups() const {
    std::vector<int> order;
    for (int i = 0; i < kMaxGroups; ++i) {
        if (!slots_[static_cast<std::size_t>(i)].name.empty()) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return slots_[static_cast<std::size_t>(a)].order < slots_[static_cast<std::size_t>(b)].order;
    });
    std::vector<Info> out;
    for (const int i : order) {
        const Slot& slot = slots_[static_cast<std::size_t>(i)];
        Info info;
        info.name = slot.name;
        info.filter_type = slot.filter_type;
        for (const int j : order) {
            const bool set = (explicit_[static_cast<std::size_t>(i)] & bit(j)) != 0;
            const bool value = (value_[static_cast<std::size_t>(i)] & bit(j)) != 0;
            if (set && value == (slot.filter_type == FilterType::Include)) {
                info.list.push_back(slots_[static_cast<std::size_t>(j)].name);
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::uint64_t CollisionGroups::category(std::string_view name) const { return bit(resolved(name)); }

std::uint64_t CollisionGroups::collides_with(std::string_view name) const {
    return mask_[static_cast<std::size_t>(resolved(name))];
}

}  // namespace engine_core
