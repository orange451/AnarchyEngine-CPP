#include "Lighting.hpp"

#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <cmath>
#include <string>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot mode_slot(AntialiasingMode mode) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &antialiasing_mode_enum();
    slot.number = static_cast<int>(mode);
    return slot;
}

LuaSlot color_slot(ColorRgb color) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Color;
    slot.color = color;
    return slot;
}

}  // namespace

const char* Lighting::class_name() const { return "Lighting"; }

std::optional<std::string> Lighting::set_number(const char* property, double& slot, double value) {
    if (!on_gameplay_thread()) {
        contract_fail("Lighting setters run on SimulationThread");
    }
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    if (value < 0) {
        value = 0;
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> Lighting::set_color(const char* property, ColorRgb& slot, ColorRgb color) {
    if (!on_gameplay_thread()) {
        contract_fail("Lighting setters run on SimulationThread");
    }
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) {
        return std::string(property) + " must be finite";
    }
    // A Color3 has no alpha.
    color.a = 1.f;
    if (same_color(slot, color)) {
        return std::nullopt;
    }
    const ColorRgb previous = slot;
    slot = color;
    note_property_change(property, color_slot(previous), color_slot(color));
    return std::nullopt;
}

std::optional<std::string> Lighting::set_ambient(ColorRgb color) { return set_color("Ambient", ambient_, color); }

std::optional<std::string> Lighting::set_brightness(double value) {
    return set_number("Brightness", brightness_, value);
}

std::optional<std::string> Lighting::set_exposure(double value) { return set_number("Exposure", exposure_, value); }

std::optional<std::string> Lighting::set_saturation(double value) {
    return set_number("Saturation", saturation_, value);
}

std::optional<std::string> Lighting::set_gamma(double value) { return set_number("Gamma", gamma_, value); }

std::optional<std::string> Lighting::set_antialiasing(int mode) {
    if (!on_gameplay_thread()) {
        contract_fail("Lighting setters run on SimulationThread");
    }
    if (enum_item_name(antialiasing_mode_enum(), mode) == nullptr) {
        return std::string("Antialiasing must be an Enum.AntialiasingMode");
    }
    const AntialiasingMode next = static_cast<AntialiasingMode>(mode);
    if (next == antialiasing_) {
        return std::nullopt;
    }
    const AntialiasingMode previous = antialiasing_;
    antialiasing_ = next;
    note_property_change("Antialiasing", mode_slot(previous), mode_slot(next));
    return std::nullopt;
}

namespace {

Lighting* lighting_of(DataModel& object) { return dynamic_cast<Lighting*>(&object); }

bool refuse(LuaSlot& in, const std::optional<std::string>& error) {
    if (error) {
        in.error = *error;
        return false;
    }
    return true;
}

template <ColorRgb (Lighting::*Get)() const>
bool read_color(DataModel&, DataModel& object, LuaSlot& out) {
    Lighting* lighting = lighting_of(object);
    if (lighting == nullptr) {
        return false;
    }
    out = color_slot((lighting->*Get)());
    return true;
}

template <std::optional<std::string> (Lighting::*Set)(ColorRgb)>
bool write_color(DataModel&, DataModel& object, LuaSlot& in) {
    Lighting* lighting = lighting_of(object);
    return lighting != nullptr && refuse(in, (lighting->*Set)(in.color));
}

template <double (Lighting::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    Lighting* lighting = lighting_of(object);
    if (lighting == nullptr) {
        return false;
    }
    out = number_slot((lighting->*Get)());
    return true;
}

template <std::optional<std::string> (Lighting::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    Lighting* lighting = lighting_of(object);
    return lighting != nullptr && refuse(in, (lighting->*Set)(in.number));
}

bool read_antialiasing(DataModel&, DataModel& object, LuaSlot& out) {
    Lighting* lighting = lighting_of(object);
    if (lighting == nullptr) {
        return false;
    }
    out = mode_slot(lighting->antialiasing());
    return true;
}

bool write_antialiasing(DataModel&, DataModel& object, LuaSlot& in) {
    Lighting* lighting = lighting_of(object);
    if (lighting == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &antialiasing_mode_enum()) {
        in.error = "Antialiasing must be an Enum.AntialiasingMode";
        return false;
    }
    return refuse(in, lighting->set_antialiasing(static_cast<int>(in.number)));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

std::string color_json(ColorRgb color) {
    const float channels[3] = {color.r, color.g, color.b};
    return write_json(json_floats(channels, 3));
}

ANARCHY_LUA_REGISTER(register_lighting_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string ambient = color_json(Lighting::kDefaultAmbient);
    static const std::string brightness = number_json(Lighting::kDefaultBrightness);
    static const std::string exposure = number_json(Lighting::kDefaultExposure);
    static const std::string saturation = number_json(Lighting::kDefaultSaturation);
    static const std::string gamma = number_json(Lighting::kDefaultGamma);
    const LuaField fields[] = {
        lua_saved_property("Ambient", "Color3", read_color<&Lighting::ambient>, write_color<&Lighting::set_ambient>,
                           ambient.c_str()),
        lua_saved_property("Brightness", "number", read_number<&Lighting::brightness>,
                           write_number<&Lighting::set_brightness>, brightness.c_str()),
        lua_slider(lua_saved_property("Exposure", "number", read_number<&Lighting::exposure>,
                                      write_number<&Lighting::set_exposure>, exposure.c_str()),
                   0.0, 2.0),
        lua_slider(lua_saved_property("Saturation", "number", read_number<&Lighting::saturation>,
                                      write_number<&Lighting::set_saturation>, saturation.c_str()),
                   0.0, 2.0),
        lua_slider(lua_saved_property("Gamma", "number", read_number<&Lighting::gamma>,
                                      write_number<&Lighting::set_gamma>, gamma.c_str()),
                   0.0, 4.0),
        lua_saved_enum("Antialiasing", antialiasing_mode_enum(), read_antialiasing, write_antialiasing, "\"FXAA\""),
    };
    register_lua_class("Lighting", "SceneService", fields, static_cast<int>(sizeof(fields) / sizeof(fields[0])));
}

}  // namespace

}  // namespace engine_core
