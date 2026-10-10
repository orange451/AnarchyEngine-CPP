#include "GameObject.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <cmath>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot color_slot(ColorRgb color) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Color;
    slot.color = color;
    return slot;
}

}  // namespace

LuaSlot GameObject::prefab() const {
    if (!alive(id_)) {
        return LuaSlot();
    }
    return instance_reference_slot(prefab_ref_, "Prefab");
}

std::optional<std::string> GameObject::set_prefab(const LuaSlot& value) {
    require_simulation_thread("set_prefab runs on SimulationThread");
    std::optional<std::string> error = set_instance_reference("Prefab", "Prefab", prefab_ref_, value);
    if (!error) {
        // The snapshot row keeps the GUID, and finds the Prefab's Models at each Prepare.
        note(id_, VisualField::Prefab, current_origin());
    }
    return error;
}

std::optional<std::string> GameObject::set_color(ColorRgb color) {
    if (!on_gameplay_thread()) {
        contract_fail("set_color runs on SimulationThread");
    }
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) {
        return std::string("Color must be finite");
    }
    // A Color3 has no alpha.
    color.a = 1.f;
    if (same_color(color_, color)) {
        return std::nullopt;
    }
    const ColorRgb previous = color_;
    color_ = color;
    note_property_change("Color", color_slot(previous), color_slot(color));
    note_visual(VisualField::Appearance);
    return std::nullopt;
}

std::optional<std::string> GameObject::set_transparency(double value) {
    if (!on_gameplay_thread()) {
        contract_fail("set_transparency runs on SimulationThread");
    }
    if (!std::isfinite(value)) {
        return std::string("Transparency must be a finite number");
    }
    if (value == transparency_) {
        return std::nullopt;
    }
    const double previous = transparency_;
    transparency_ = value;
    note_property_change("Transparency", number_slot(previous), number_slot(value));
    note_visual(VisualField::Appearance);
    return std::nullopt;
}

std::optional<std::string> GameObject::set_scale(double value) {
    if (!on_gameplay_thread()) {
        contract_fail("set_scale runs on SimulationThread");
    }
    if (!std::isfinite(value) || !(value > 0.0)) {
        return std::string("Scale must be a finite number above 0");
    }
    if (value == scale_) {
        return std::nullopt;
    }
    const double previous = scale_;
    scale_ = value;
    note_property_change("Scale", number_slot(previous), number_slot(value));
    note_visual(VisualField::Scale);
    return std::nullopt;
}

void GameObject::on_reuse() {
    SpatialObject::on_reuse();
    prefab_ref_.set_guid(std::string());
    color_ = kDefaultColor;
    transparency_ = kDefaultTransparency;
    scale_ = kDefaultScale;
}

namespace {

bool read_prefab(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = body->prefab();
    return true;
}

bool write_prefab(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = body->set_prefab(in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

bool read_color(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = color_slot(body->color());
    return true;
}

bool write_color(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = body->set_color(in.color)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

bool read_transparency(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = number_slot(body->transparency());
    return true;
}

bool write_transparency(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = body->set_transparency(in.number)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

bool read_scale(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = number_slot(body->scale());
    return true;
}

bool write_scale(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = body->set_scale(in.number)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

ANARCHY_LUA_REGISTER(register_game_object_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string color = [] {
        const float channels[3] = {GameObject::kDefaultColor.r, GameObject::kDefaultColor.g,
                                   GameObject::kDefaultColor.b};
        return write_json(json_floats(channels, 3));
    }();
    static const std::string transparency = write_json(JsonValue::number(GameObject::kDefaultTransparency));
    static const std::string scale = write_json(JsonValue::number(GameObject::kDefaultScale));
    // PVInstance has no source file of its own, which would not stay linked.
    // It is abstract, and adds no members: IsA("PVInstance") is true of every
    // class with a Transform.
    register_lua_class("PVInstance", "Instance", nullptr, 0);
    const LuaField fields[] = {
        lua_group("Behavior"),
        lua_saved_property("Prefab", "Prefab?", read_prefab, write_prefab, "null"),
        lua_group("Appearance"),
        lua_saved_property("Color", "Color3", read_color, write_color, color.c_str()),
        lua_slider(lua_saved_property("Transparency", "number", read_transparency, write_transparency,
                                      transparency.c_str()),
                   0.0, 1.0),
        lua_group("Transform"),
        lua_spatial_transform(),
        lua_saved_property("Scale", "number", read_scale, write_scale, scale.c_str()),
    };
    register_lua_class("GameObject", "PVInstance", fields, static_cast<int>(sizeof(fields) / sizeof(fields[0])));
    register_suited_parents("GameObject", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
