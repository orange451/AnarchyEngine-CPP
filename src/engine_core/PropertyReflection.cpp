#include "PropertyReflection.hpp"

#include <cstring>
#include <vector>

namespace engine_core {

bool slot_to_json(const LuaSlot& slot, std::string_view type, JsonValue& out) {
    if (type == "number" && slot.kind == LuaSlot::Kind::Number) {
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
    } else {
        return false;
    }
    return true;
}

bool slot_from_json(const JsonValue& value, std::string_view type, const char* name, LuaSlot& out,
                    std::string& error) {
    const std::string label = name != nullptr ? name : "";
    std::vector<float> floats;
    if (type == "number") {
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
    case LuaSlot::Kind::Signal:
        return true;
    case LuaSlot::Kind::Bool:
        return a.flag == b.flag;
    case LuaSlot::Kind::Number:
        return a.number == b.number;
    case LuaSlot::Kind::String:
        return a.text == b.text;
    case LuaSlot::Kind::Instance:
        return a.id == b.id;
    case LuaSlot::Kind::Vec3:
        return a.vec.x == b.vec.x && a.vec.y == b.vec.y && a.vec.z == b.vec.z;
    case LuaSlot::Kind::Color:
        return same_color(a.color, b.color);
    case LuaSlot::Kind::Transform:
        return std::memcmp(a.transform.m, b.transform.m, sizeof(a.transform.m)) == 0;
    }
    return false;
}

}  // namespace engine_core
