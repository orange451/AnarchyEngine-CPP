#include "Light.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

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

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

}  // namespace

std::optional<std::string> Light::set_number(const char* property, double& slot, double value) {
    if (!on_gameplay_thread()) {
        contract_fail("Light setters run on SimulationThread");
    }
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    note_visual(VisualField::Light);
    return std::nullopt;
}

std::optional<std::string> Light::set_color(ColorRgb color) {
    if (!on_gameplay_thread()) {
        contract_fail("Light setters run on SimulationThread");
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
    note_visual(VisualField::Light);
    return std::nullopt;
}

std::optional<std::string> Light::set_intensity(double value) {
    return set_number("Intensity", intensity_, std::isfinite(value) ? std::max(value, 0.0) : value);
}

std::optional<std::string> Light::set_radius(double value) {
    return set_number("Radius", radius_, std::isfinite(value) ? std::max(value, 0.0) : value);
}

void Light::set_enabled(bool enabled) {
    if (!on_gameplay_thread()) {
        contract_fail("Light setters run on SimulationThread");
    }
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    note_property_change("Enabled", bool_slot(!enabled), bool_slot(enabled));
    note_visual(VisualField::Light);
}

void Light::on_reuse() {
    GameObject::on_reuse();
    color_ = kDefaultColor;
    intensity_ = kDefaultIntensity;
    radius_ = kDefaultRadius;
    enabled_ = true;
}

std::optional<std::string> SpotLight::set_outer_fov(double degrees) {
    return set_number("OuterFOV", outer_fov_,
                      std::isfinite(degrees) ? std::clamp(degrees, kMinOuterFov, kMaxOuterFov) : degrees);
}

std::optional<std::string> SpotLight::set_inner_fov_scale(double scale) {
    return set_number("InnerFOVScale", inner_fov_scale_, std::isfinite(scale) ? std::clamp(scale, 0.0, 1.0) : scale);
}

void SpotLight::on_reuse() {
    Light::on_reuse();
    outer_fov_ = kDefaultOuterFov;
    inner_fov_scale_ = kDefaultInnerFovScale;
}

namespace {

LuaSlot vec3_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

}  // namespace

std::optional<std::string> DirectionalLight::set_direction(Vec3 direction) {
    if (!on_gameplay_thread()) {
        contract_fail("DirectionalLight setters run on SimulationThread");
    }
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z)) {
        return std::string("Direction must be finite");
    }
    if (direction.x == direction_.x && direction.y == direction_.y && direction.z == direction_.z) {
        return std::nullopt;
    }
    const Vec3 previous = direction_;
    direction_ = direction;
    note_property_change("Direction", vec3_slot(previous), vec3_slot(direction));
    note_visual_row(VisualField::Light);
    return std::nullopt;
}

std::optional<std::string> DirectionalLight::set_color(ColorRgb color) {
    if (!on_gameplay_thread()) {
        contract_fail("DirectionalLight setters run on SimulationThread");
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
    note_visual_row(VisualField::Light);
    return std::nullopt;
}

std::optional<std::string> DirectionalLight::set_intensity(double value) {
    if (!on_gameplay_thread()) {
        contract_fail("DirectionalLight setters run on SimulationThread");
    }
    if (!std::isfinite(value)) {
        return std::string("Intensity must be a finite number");
    }
    value = std::max(value, 0.0);
    if (intensity_ == value) {
        return std::nullopt;
    }
    const double previous = intensity_;
    intensity_ = value;
    note_property_change("Intensity", number_slot(previous), number_slot(value));
    note_visual_row(VisualField::Light);
    return std::nullopt;
}

void DirectionalLight::set_enabled(bool enabled) {
    if (!on_gameplay_thread()) {
        contract_fail("DirectionalLight setters run on SimulationThread");
    }
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    note_property_change("Enabled", bool_slot(!enabled), bool_slot(enabled));
    note_visual_row(VisualField::Light);
}

void DirectionalLight::on_reuse() {
    direction_ = kDefaultDirection;
    color_ = Light::kDefaultColor;
    intensity_ = Light::kDefaultIntensity;
    enabled_ = true;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

template <typename T, double (T::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* light = dynamic_cast<const T*>(&object);
    if (light == nullptr) {
        return false;
    }
    out = number_slot((light->*Get)());
    return true;
}

template <typename T, std::optional<std::string> (T::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    auto* light = dynamic_cast<T*>(&object);
    return light != nullptr && refuse(in, (light->*Set)(in.number));
}

// T is Light or DirectionalLight, which share these properties but not a base.
template <typename T>
bool read_color(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* light = dynamic_cast<const T*>(&object);
    if (light == nullptr) {
        return false;
    }
    out = color_slot(light->color());
    return true;
}

template <typename T>
bool write_color(DataModel&, DataModel& object, LuaSlot& in) {
    auto* light = dynamic_cast<T*>(&object);
    return light != nullptr && refuse(in, light->set_color(in.color));
}

template <typename T>
bool read_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* light = dynamic_cast<const T*>(&object);
    if (light == nullptr) {
        return false;
    }
    out = bool_slot(light->enabled());
    return true;
}

template <typename T>
bool write_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    auto* light = dynamic_cast<T*>(&object);
    if (light == nullptr) {
        return false;
    }
    light->set_enabled(in.flag);
    return true;
}

bool read_direction(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* sun = dynamic_cast<const DirectionalLight*>(&object);
    if (sun == nullptr) {
        return false;
    }
    out = vec3_slot(sun->direction());
    return true;
}

bool write_direction(DataModel&, DataModel& object, LuaSlot& in) {
    auto* sun = dynamic_cast<DirectionalLight*>(&object);
    return sun != nullptr && refuse(in, sun->set_direction(in.vec));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_light_lua) {
    // The defaults, as a file would hold them, from the classes' own constants.
    static const std::string color = [] {
        const float channels[3] = {Light::kDefaultColor.r, Light::kDefaultColor.g, Light::kDefaultColor.b};
        return write_json(json_floats(channels, 3));
    }();
    static const std::string intensity = number_json(Light::kDefaultIntensity);
    static const std::string radius = number_json(Light::kDefaultRadius);
    static const std::string outer_fov = number_json(SpotLight::kDefaultOuterFov);
    static const std::string inner_fov_scale = number_json(SpotLight::kDefaultInnerFovScale);
    // Each class that can be made lists the fields, as FileAsset's subclasses
    // list Path: Light itself is only for IsA, and is never made.
    const LuaField point_fields[] = {
        lua_saved_property("Color", "Color3", read_color<Light>, write_color<Light>, color.c_str()),
        lua_slider(lua_saved_property("Intensity", "number", read_number<Light, &Light::intensity>,
                                      write_number<Light, &Light::set_intensity>, intensity.c_str()),
                   0.0, Light::kMaxIntensitySlider),
        lua_slider(lua_saved_property("Radius", "number", read_number<Light, &Light::radius>,
                                      write_number<Light, &Light::set_radius>, radius.c_str()),
                   0.0, Light::kMaxRadiusSlider),
        lua_saved_property("Enabled", "boolean", read_enabled<Light>, write_enabled<Light>, "true"),
    };
    register_lua_class("Light", "GameObject", nullptr, 0);
    register_lua_class("PointLight", "Light", point_fields, static_cast<int>(std::size(point_fields)));
    // Not a Light: it has no Transform, so it is a plain Instance.
    static const std::string direction = [] {
        const float axes[3] = {DirectionalLight::kDefaultDirection.x, DirectionalLight::kDefaultDirection.y,
                               DirectionalLight::kDefaultDirection.z};
        return write_json(json_floats(axes, 3));
    }();
    const LuaField directional_fields[] = {
        lua_saved_property("Direction", "Vector3", read_direction, write_direction, direction.c_str()),
        lua_saved_property("Color", "Color3", read_color<DirectionalLight>, write_color<DirectionalLight>,
                           color.c_str()),
        lua_slider(lua_saved_property("Intensity", "number", read_number<DirectionalLight, &DirectionalLight::intensity>,
                                      write_number<DirectionalLight, &DirectionalLight::set_intensity>,
                                      intensity.c_str()),
                   0.0, Light::kMaxIntensitySlider),
        lua_saved_property("Enabled", "boolean", read_enabled<DirectionalLight>, write_enabled<DirectionalLight>,
                           "true"),
    };
    register_lua_class("DirectionalLight", "Instance", directional_fields,
                       static_cast<int>(std::size(directional_fields)));
    const LuaField spot_fields[] = {
        point_fields[0],
        point_fields[1],
        point_fields[2],
        point_fields[3],
        lua_slider(lua_saved_property("OuterFOV", "number", read_number<SpotLight, &SpotLight::outer_fov>,
                                      write_number<SpotLight, &SpotLight::set_outer_fov>, outer_fov.c_str()),
                   SpotLight::kMinOuterFov, SpotLight::kMaxOuterFov),
        lua_slider(lua_saved_property("InnerFOVScale", "number", read_number<SpotLight, &SpotLight::inner_fov_scale>,
                                      write_number<SpotLight, &SpotLight::set_inner_fov_scale>,
                                      inner_fov_scale.c_str()),
                   0.0, 1.0),
    };
    register_lua_class("SpotLight", "Light", spot_fields, static_cast<int>(std::size(spot_fields)));
    // Light adds Lighting to what it inherits from GameObject.
    register_suited_parents("Light", {"Lighting"});
    register_suited_parents("DirectionalLight", {"Lighting", "Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
