#pragma once

#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <string>
#include <string_view>

namespace engine_core {

// A saved property's value as the JSON a project file holds, by the type its
// class registered: number, boolean, string, Color3 as [r, g, b], or Vector3
// as [x, y, z]. False for any other type.
bool slot_to_json(const LuaSlot& slot, std::string_view type, JsonValue& out);
// The reverse. False, with error naming the property, when value is not one.
bool slot_from_json(const JsonValue& value, std::string_view type, const char* name, LuaSlot& out,
                    std::string& error);
// The same value, compared by kind.
bool same_slot(const LuaSlot& a, const LuaSlot& b);

}  // namespace engine_core
