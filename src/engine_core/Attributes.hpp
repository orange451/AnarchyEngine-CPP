#pragma once

#include "LuaApi.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace engine_core {

// An instance's attributes: named values it carries beside its class's
// properties, which scripts set with SetAttribute and read with GetAttribute.
// Ordered by name, so the saved JSON and the Properties rows are stable.
// An Instance value keeps its target's GUID in text, and id 0: the reader
// resolves it, so a target that dies leaves the attribute naming it.
using AttributeMap = std::map<std::string, LuaSlot, std::less<>>;

// The longest attribute name.
inline constexpr std::size_t kMaxAttributeName = 100;

// Why name cannot name an attribute, or nothing when it can: 1 to 100
// letters, digits, and underscores.
std::optional<std::string> attribute_name_error(std::string_view name);

// Why value cannot be an attribute's, or nothing when it can: a number,
// boolean, string, Vector2, Vector3, Color3, Matrix4, or Instance. Nil is
// allowed, as it means remove.
std::optional<std::string> attribute_value_error(const LuaSlot& value);

// The class registry's type name for a kind attributes hold, as slot_to_json
// takes it: "number", "boolean", "string", "Vector2", "Vector3", "Color3",
// "Matrix4", or "Instance?". Null for any other kind.
const char* attribute_type_name(LuaSlot::Kind kind);

// The instance event GetAttributeChangedSignal(name) connects to:
// "AttributeChanged:<name>", which no declared event can be named.
std::string attribute_event_name(std::string_view name);

// map as the hidden Attributes property holds it: a JSON object of
// {"<name>": {"type": "<type name>", "value": <value>}}, compact. Empty for
// an empty map.
std::string attributes_to_json(const AttributeMap& map);

// text read back into out, which it replaces. Empty text and null are an
// empty map. An entry with a bad name, a type attributes cannot hold, or a
// value its type refuses is left out, and named in error as "attribute X:
// why", one per line; the rest load. False, with out empty, only when text is
// not a JSON object.
bool attributes_from_json(std::string_view text, AttributeMap& out, std::string& error);

}  // namespace engine_core
