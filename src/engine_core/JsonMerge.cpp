#include "JsonMerge.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace engine_core {

namespace {

// As the file writes it. A script can make a number NaN or infinite, which no
// file holds; those read as Luau prints them.
std::string display_number(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0 ? "inf" : "-inf";
    }
    return format_json_number(value);
}

}  // namespace

bool same_value(const JsonValue* a, const JsonValue* b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return *a == *b;
}

std::vector<KeyMerge> merge_keys(const JsonValue& base, const JsonValue& disk, const JsonValue& studio) {
    std::set<std::string> keys;
    for (const JsonValue* object : {&base, &disk, &studio}) {
        for (const JsonValue::Member& member : object->members()) {
            if (member.first != "id") {
                keys.insert(member.first);
            }
        }
    }
    std::vector<KeyMerge> out;
    for (const std::string& key : keys) {
        const JsonValue* was = base.find(key);
        const JsonValue* theirs = disk.find(key);
        const JsonValue* mine = studio.find(key);
        KeyMerge merged;
        merged.key = key;
        if (same_value(theirs, was)) {
            merged.change = same_value(mine, was) ? KeyChange::Unchanged : KeyChange::StudioOnly;
        } else if (same_value(mine, was)) {
            merged.change = KeyChange::DiskOnly;
        } else {
            merged.change = same_value(mine, theirs) ? KeyChange::Agreed : KeyChange::Conflict;
        }
        out.push_back(std::move(merged));
    }
    return out;
}

std::string display_value(const JsonValue* value) {
    if (value == nullptr) {
        return "(default)";
    }
    if (value->is_string()) {
        return value->as_string();
    }
    if (value->is_number()) {
        return display_number(value->as_number());
    }
    if (value->is_bool()) {
        return value->as_bool() ? "true" : "false";
    }
    if (value->is_array() && !value->items().empty() &&
        std::all_of(value->items().begin(), value->items().end(), [](const JsonValue& item) { return item.is_number(); })) {
        std::string out;
        for (const JsonValue& item : value->items()) {
            out += (out.empty() ? "" : ", ") + display_number(item.as_number());
        }
        return out;
    }
    // Anything else: its JSON, each line break and its indent folded to one space.
    std::string text;
    try {
        text = write_json(*value);
    } catch (const std::invalid_argument&) {
        return "(holds a number that is not finite)";
    }
    std::string out;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] != '\n') {
            out += text[at];
            continue;
        }
        while (at + 1 < text.size() && text[at + 1] == ' ') {
            ++at;
        }
        out += ' ';
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace engine_core
