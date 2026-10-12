#include "CollisionGroups.hpp"

#include "PropertyBag.hpp"

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

std::optional<std::string> CollisionGroups::rename_group(const std::string& from, const std::string& to) {
    if (from == kDefault) {
        return std::string("Default cannot be renamed");
    }
    const int found = bit_of(from);
    if (found < 0) {
        return unknown(from);
    }
    if (to.empty()) {
        return std::string("Collision group name must not be empty");
    }
    if (to == from) {
        return std::nullopt;
    }
    if (bit_of(to) >= 0) {
        return "A collision group named \"" + to + "\" already exists";
    }
    slots_[static_cast<std::size_t>(found)].name = to;
    changed();
    return std::nullopt;
}

std::string CollisionGroups::save_text() const {
    const std::vector<int> order = ordered();
    const bool plain = order.size() == 1 && slots_[0].filter_type == FilterType::Exclude && explicit_[0] == 0;
    if (plain) {
        return {};
    }
    auto type_name = [](FilterType type) { return std::string(type == FilterType::Include ? "Include" : "Exclude"); };
    JsonValue groups = JsonValue::array();
    JsonValue pairs = JsonValue::array();
    for (std::size_t at = 0; at < order.size(); ++at) {
        const Slot& slot = slots_[static_cast<std::size_t>(order[at])];
        groups.items().push_back(
            JsonValue::array({JsonValue::string(slot.name), JsonValue::string(type_name(slot.filter_type))}));
        for (std::size_t other = at; other < order.size(); ++other) {
            const std::size_t i = static_cast<std::size_t>(order[at]);
            if ((explicit_[i] & bit(order[other])) == 0) {
                continue;
            }
            pairs.items().push_back(JsonValue::array({JsonValue::string(slot.name),
                                                      JsonValue::string(slots_[static_cast<std::size_t>(order[other])].name),
                                                      JsonValue::boolean((value_[i] & bit(order[other])) != 0)}));
        }
    }
    // In written order: "groups" before "pairs".
    JsonValue table = JsonValue::object();
    table.set("groups", std::move(groups));
    table.set("pairs", std::move(pairs));
    return compact_json(table);
}

std::optional<std::string> CollisionGroups::load_text(const std::string& text) {
    CollisionGroups built;
    built.revision_ = revision_;
    if (!text.empty()) {
        const std::string refused = "CollisionGroups is not a collision group table";
        JsonValue table;
        std::string error;
        if (!parse_json(text, table, error) || !table.is_object()) {
            return refused;
        }
        const JsonValue* groups = table.find("groups");
        const JsonValue* pairs = table.find("pairs");
        if (groups == nullptr || !groups->is_array() || groups->items().empty() || pairs == nullptr ||
            !pairs->is_array()) {
            return refused;
        }
        for (std::size_t at = 0; at < groups->items().size(); ++at) {
            const JsonValue& entry = groups->items()[at];
            if (!entry.is_array() || entry.items().size() != 2 || !entry.items()[0].is_string() ||
                !entry.items()[1].is_string()) {
                return refused;
            }
            const std::string& name = entry.items()[0].as_string();
            const std::string& type = entry.items()[1].as_string();
            if (type != "Exclude" && type != "Include") {
                return "Collision group \"" + name + "\" has no filter type \"" + type + "\"";
            }
            const FilterType filter_type = type == "Include" ? FilterType::Include : FilterType::Exclude;
            if (at == 0) {
                if (name != kDefault) {
                    return std::string("A collision group table starts with Default");
                }
                built.slots_[0].filter_type = filter_type;
                continue;
            }
            if (name.empty()) {
                return std::string("Collision group name must not be empty");
            }
            if (built.bit_of(name) >= 0) {
                return "Collision group \"" + name + "\" is listed twice";
            }
            if (std::optional<std::string> refusal = built.register_group(name, filter_type)) {
                return refusal;
            }
        }
        for (const JsonValue& entry : pairs->items()) {
            if (!entry.is_array() || entry.items().size() != 3 || !entry.items()[0].is_string() ||
                !entry.items()[1].is_string() || !entry.items()[2].is_bool()) {
                return refused;
            }
            if (std::optional<std::string> refusal = built.set_collidable(
                    entry.items()[0].as_string(), entry.items()[1].as_string(), entry.items()[2].as_bool())) {
                return refusal;
            }
        }
    }
    built.changed();
    *this = built;
    return std::nullopt;
}

bool CollisionGroups::registered(std::string_view name) const { return bit_of(name) >= 0; }

bool CollisionGroups::collidable(std::string_view a, std::string_view b) const {
    return pair(resolved(a), resolved(b));
}

std::vector<int> CollisionGroups::ordered() const {
    std::vector<int> order;
    for (int i = 0; i < kMaxGroups; ++i) {
        if (!slots_[static_cast<std::size_t>(i)].name.empty()) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return slots_[static_cast<std::size_t>(a)].order < slots_[static_cast<std::size_t>(b)].order;
    });
    return order;
}

std::vector<CollisionGroups::Info> CollisionGroups::groups() const {
    const std::vector<int> order = ordered();
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
