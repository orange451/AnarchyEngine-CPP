#include "Attributes.hpp"

#include "PropertyBag.hpp"
#include "PropertyReflection.hpp"

namespace engine_core {
namespace {

constexpr LuaSlot::Kind kAttributeKinds[] = {LuaSlot::Kind::Number, LuaSlot::Kind::Bool,    LuaSlot::Kind::String,
                                             LuaSlot::Kind::Vec2,   LuaSlot::Kind::Vec3,    LuaSlot::Kind::Color,
                                             LuaSlot::Kind::Matrix4, LuaSlot::Kind::Instance};

bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void add_error(std::string& error, const std::string& name, const std::string& why) {
    if (!error.empty()) {
        error += '\n';
    }
    error += "attribute " + name + ": " + why;
}

}  // namespace

std::optional<std::string> attribute_name_error(std::string_view name) {
    bool valid = !name.empty() && name.size() <= kMaxAttributeName;
    for (const char c : name) {
        valid = valid && name_char(c);
    }
    if (valid) {
        return std::nullopt;
    }
    return std::string("an attribute name is letters, digits, and underscores, up to 100");
}

std::optional<std::string> attribute_value_error(const LuaSlot& value) {
    if (value.kind == LuaSlot::Kind::Nil || attribute_type_name(value.kind) != nullptr) {
        return std::nullopt;
    }
    return std::string("an attribute holds a number, boolean, string, Vector2, Vector3, Color3, Matrix4, or Instance");
}

const char* attribute_type_name(LuaSlot::Kind kind) {
    switch (kind) {
    case LuaSlot::Kind::Number:
        return "number";
    case LuaSlot::Kind::Bool:
        return "boolean";
    case LuaSlot::Kind::String:
        return "string";
    case LuaSlot::Kind::Vec2:
        return "Vector2";
    case LuaSlot::Kind::Vec3:
        return "Vector3";
    case LuaSlot::Kind::Color:
        return "Color3";
    case LuaSlot::Kind::Matrix4:
        return "Matrix4";
    case LuaSlot::Kind::Instance:
        return "Instance?";
    default:
        return nullptr;
    }
}

std::string attributes_to_json(const AttributeMap& map) {
    if (map.empty()) {
        return {};
    }
    JsonValue object = JsonValue::object();
    for (const auto& [name, slot] : map) {
        const char* type = attribute_type_name(slot.kind);
        JsonValue value;
        if (type == nullptr || !slot_to_json(slot, type, value)) {
            continue;
        }
        JsonValue entry = JsonValue::object();
        entry.set("type", JsonValue::string(type));
        entry.set("value", std::move(value));
        object.set(name, std::move(entry));
    }
    return compact_json(object);
}

bool attributes_from_json(std::string_view text, AttributeMap& out, std::string& error) {
    out.clear();
    if (text.empty()) {
        return true;
    }
    JsonValue root;
    std::string why;
    if (!parse_json(text, root, why)) {
        error = "Attributes is not JSON: " + why;
        return false;
    }
    if (root.is_null()) {
        return true;
    }
    if (!root.is_object()) {
        error = "Attributes must be a JSON object";
        return false;
    }
    for (const auto& [name, entry] : root.members()) {
        if (const auto bad = attribute_name_error(name)) {
            add_error(error, name, *bad);
            continue;
        }
        const JsonValue* type = entry.is_object() ? entry.find("type") : nullptr;
        const JsonValue* value = entry.is_object() ? entry.find("value") : nullptr;
        if (type == nullptr || value == nullptr || !type->is_string()) {
            add_error(error, name, "must be {\"type\": ..., \"value\": ...}");
            continue;
        }
        bool known = false;
        for (const LuaSlot::Kind kind : kAttributeKinds) {
            known = known || type->as_string() == attribute_type_name(kind);
        }
        if (!known) {
            add_error(error, name, "an attribute cannot hold a " + type->as_string());
            continue;
        }
        LuaSlot slot;
        std::string refused;
        if (!slot_from_json(*value, type->as_string(), name.c_str(), slot, refused)) {
            add_error(error, name, refused);
            continue;
        }
        // An Instance saved as null names nothing: there is nothing to keep.
        if (slot.kind == LuaSlot::Kind::Nil) {
            continue;
        }
        out[name] = std::move(slot);
    }
    return true;
}

}  // namespace engine_core
