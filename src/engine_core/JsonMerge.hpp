#pragma once

#include "PropertyBag.hpp"

#include <string>
#include <vector>

namespace engine_core {

// How one top-level key of an instance's file changed, across three versions:
// the last load or save (base), the disk now, and what the studio would write now.
enum class KeyChange {
    Unchanged,   // the disk and the studio both match the base
    DiskOnly,    // the disk changed it; the studio did not
    StudioOnly,  // the studio changed it; the disk did not
    Agreed,      // both changed it, to the same value
    Conflict,    // both changed it, to different values
};

struct KeyMerge {
    std::string key;
    KeyChange change = KeyChange::Unchanged;
};

// Every key of the three objects, byte-sorted, with how it changed. A key an
// object lacks has the value "absent", which for an instance file is the class
// default. "id" names the instance and is left out.
std::vector<KeyMerge> merge_keys(const JsonValue& base, const JsonValue& disk, const JsonValue& studio);

// Equal as write_json writes them. Null is absent: equal only to null.
bool same_value(const JsonValue* a, const JsonValue* b);

// A value on one line for a person: numbers as the file writes them, an array
// of numbers as "1, 0, 0", a string as its text, null (absent) as "(default)",
// and anything else as its JSON on one line.
std::string display_value(const JsonValue* value);

}  // namespace engine_core
