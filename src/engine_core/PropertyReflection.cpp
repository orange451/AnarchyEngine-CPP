#include "PropertyReflection.hpp"

#include "DataModel.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"

#include <cstring>
#include <vector>

namespace engine_core {

std::string reference_class(std::string_view type) {
    if (type.size() < 2 || type.back() != '?') {
        return {};
    }
    const std::string base(type.substr(0, type.size() - 1));
    return lua_class_known(base.c_str()) ? base : std::string();
}

bool slot_to_json(const LuaSlot& slot, std::string_view type, JsonValue& out) {
    if (slot.kind == LuaSlot::Kind::Enum) {
        const char* item = slot.enum_type != nullptr
                               ? enum_item_name(*slot.enum_type, static_cast<int>(slot.number))
                               : nullptr;
        if (item == nullptr) {
            return false;
        }
        out = JsonValue::string(item);
    } else if (type == "number" && slot.kind == LuaSlot::Kind::Number) {
        out = JsonValue::number(slot.number);
    } else if (type == "boolean" && slot.kind == LuaSlot::Kind::Bool) {
        out = JsonValue::boolean(slot.flag);
    } else if (type == "string" && slot.kind == LuaSlot::Kind::String) {
        out = JsonValue::string(slot.text);
    } else if (type == "Color3" && slot.kind == LuaSlot::Kind::Color) {
        const float channels[3] = {slot.color.r, slot.color.g, slot.color.b};
        out = json_floats(channels, 3);
    } else if (type == "Vector3" && slot.kind == LuaSlot::Kind::Vec3) {
        const float axes[3] = {slot.vec.x, slot.vec.y, slot.vec.z};
        out = json_floats(axes, 3);
    } else if (type == "Vector2" && slot.kind == LuaSlot::Kind::Vec2) {
        const float axes[2] = {slot.vec.x, slot.vec.y};
        out = json_floats(axes, 2);
    } else if (type == "Matrix4" && slot.kind == LuaSlot::Kind::Matrix4) {
        out = json_floats(slot.transform.m, 16);
    } else if (!reference_class(type).empty() &&
               (slot.kind == LuaSlot::Kind::Instance || slot.kind == LuaSlot::Kind::Nil)) {
        out = slot.text.empty() ? JsonValue() : JsonValue::string(slot.text);
    } else {
        return false;
    }
    return true;
}

bool slot_from_json(const JsonValue& value, std::string_view type, const char* name, LuaSlot& out,
                    std::string& error, const EnumType* enum_type) {
    const std::string label = name != nullptr ? name : "";
    std::vector<float> floats;
    if (enum_type != nullptr) {
        const int item = value.is_string() ? enum_item_value(*enum_type, value.as_string()) : -1;
        if (item < 0) {
            error = label + " must be the name of an Enum." + enum_type->name + " item";
            return false;
        }
        out.kind = LuaSlot::Kind::Enum;
        out.enum_type = enum_type;
        out.number = item;
    } else if (type == "number") {
        if (!value.is_number()) {
            error = label + " must be a number";
            return false;
        }
        out.kind = LuaSlot::Kind::Number;
        out.number = value.as_number();
    } else if (type == "boolean") {
        if (!value.is_bool()) {
            error = label + " must be true or false";
            return false;
        }
        out.kind = LuaSlot::Kind::Bool;
        out.flag = value.as_bool();
    } else if (type == "string") {
        if (!value.is_string()) {
            error = label + " must be a string";
            return false;
        }
        out.kind = LuaSlot::Kind::String;
        out.text = value.as_string();
    } else if (type == "Color3") {
        if (!read_json_floats(value, 3, 3, floats)) {
            error = label + " must be 3 numbers";
            return false;
        }
        out.kind = LuaSlot::Kind::Color;
        out.color = ColorRgb{floats[0], floats[1], floats[2], 1.f};
    } else if (type == "Vector3") {
        if (!read_json_floats(value, 3, 3, floats)) {
            error = label + " must be 3 numbers";
            return false;
        }
        out.kind = LuaSlot::Kind::Vec3;
        out.vec = Vec3{floats[0], floats[1], floats[2]};
    } else if (type == "Vector2") {
        if (!read_json_floats(value, 2, 2, floats)) {
            error = label + " must be 2 numbers";
            return false;
        }
        out.kind = LuaSlot::Kind::Vec2;
        out.vec = Vec3{floats[0], floats[1], 0.f};
    } else if (type == "Matrix4") {
        if (!read_json_floats(value, 16, 16, floats)) {
            error = label + " must be 16 numbers, column-major";
            return false;
        }
        out.kind = LuaSlot::Kind::Matrix4;
        std::memcpy(out.transform.m, floats.data(), sizeof(out.transform.m));
    } else if (!reference_class(type).empty()) {
        if (value.is_null()) {
            out.kind = LuaSlot::Kind::Nil;
            out.text.clear();
        } else if (value.is_string() && valid_guid(value.as_string())) {
            // Named by GUID; the class resolves it when read.
            out.kind = LuaSlot::Kind::Instance;
            out.id = 0;
            out.text = value.as_string();
        } else {
            error = label + " must be a GUID or null";
            return false;
        }
    } else {
        error = label + " has a type a project cannot hold";
        return false;
    }
    return true;
}

bool same_slot(const LuaSlot& a, const LuaSlot& b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case LuaSlot::Kind::Nil:
        // A reference that resolves to nothing still names its GUID.
        return a.text == b.text;
    case LuaSlot::Kind::Signal:
        return true;
    case LuaSlot::Kind::Bool:
        return a.flag == b.flag;
    case LuaSlot::Kind::Number:
        return a.number == b.number;
    case LuaSlot::Kind::String:
        return a.text == b.text;
    case LuaSlot::Kind::Instance:
        return a.id == b.id && a.text == b.text;
    case LuaSlot::Kind::Vec3:
        return a.vec.x == b.vec.x && a.vec.y == b.vec.y && a.vec.z == b.vec.z;
    case LuaSlot::Kind::Vec2:
        return a.vec.x == b.vec.x && a.vec.y == b.vec.y;
    case LuaSlot::Kind::Color:
        return same_color(a.color, b.color);
    case LuaSlot::Kind::Matrix4:
        return std::memcmp(a.transform.m, b.transform.m, sizeof(a.transform.m)) == 0;
    case LuaSlot::Kind::Enum:
        return a.enum_type == b.enum_type && a.number == b.number;
    }
    return false;
}

}  // namespace engine_core
