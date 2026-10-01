#pragma once

#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <string>
#include <string_view>

namespace engine_core {

// A saved property's value as the JSON a project file holds, by the type its
// class registered: number, boolean, string, Color3 as [r, g, b], Vector3 as
// [x, y, z], Matrix4 as 16 numbers, column-major, a reference as the
// target's GUID string, or null, or an enum
// (an Enum slot) as its item's name. False for any other type.
bool slot_to_json(const LuaSlot& slot, std::string_view type, JsonValue& out);
// The reverse. False, with error naming the property, when value is not one.
// enum_type is the property's (LuaField::enum_type), or null when it has none.
bool slot_from_json(const JsonValue& value, std::string_view type, const char* name, LuaSlot& out,
                    std::string& error, const EnumType* enum_type = nullptr);
// The same value, compared by kind.
bool same_slot(const LuaSlot& a, const LuaSlot& b);

// The class a reference property holds, as its type names it: "Texture?" gives
// "Texture". Empty for any other type. A reference's slot carries the target's
// GUID in text, and the live target, if any, in id.
std::string reference_class(std::string_view type);

}  // namespace engine_core
