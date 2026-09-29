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
    if (&slot == &clock_time_) {
        value = std::fmod(value, 24.0);
        if (value < 0) {
            value += 24.0;
        }
    } else if (value < 0) {
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

std::optional<std::string> Lighting::set_clock_time(double value) {
    return set_number("ClockTime", clock_time_, value);
}

std::optional<std::string> Lighting::set_fog_color(ColorRgb color) { return set_color("FogColor", fog_color_, color); }

std::optional<std::string> Lighting::set_fog_start(double value) { return set_number("FogStart", fog_start_, value); }

std::optional<std::string> Lighting::set_fog_end(double value) { return set_number("FogEnd", fog_end_, value); }

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

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

std::string color_json(ColorRgb color) {
    const float channels[3] = {color.r, color.g, color.b};
    return write_json(json_floats(channels, 3));
}

ANARCHY_LUA_REGISTER(register_lighting_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string ambient = color_json(Lighting::kDefaultAmbient);
    static const std::string brightness = number_json(Lighting::kDefaultBrightness);
    static const std::string clock_time = number_json(Lighting::kDefaultClockTime);
    static const std::string fog_color = color_json(Lighting::kDefaultFogColor);
    static const std::string fog_start = number_json(Lighting::kDefaultFogStart);
    static const std::string fog_end = number_json(Lighting::kDefaultFogEnd);
    const LuaField fields[] = {
        lua_saved_property("Ambient", "Color3", read_color<&Lighting::ambient>, write_color<&Lighting::set_ambient>,
                           ambient.c_str()),
        lua_saved_property("Brightness", "number", read_number<&Lighting::brightness>,
                           write_number<&Lighting::set_brightness>, brightness.c_str()),
        lua_saved_property("ClockTime", "number", read_number<&Lighting::clock_time>,
                           write_number<&Lighting::set_clock_time>, clock_time.c_str()),
        lua_saved_property("FogColor", "Color3", read_color<&Lighting::fog_color>,
                           write_color<&Lighting::set_fog_color>, fog_color.c_str()),
        lua_saved_property("FogStart", "number", read_number<&Lighting::fog_start>,
                           write_number<&Lighting::set_fog_start>, fog_start.c_str()),
        lua_saved_property("FogEnd", "number", read_number<&Lighting::fog_end>, write_number<&Lighting::set_fog_end>,
                           fog_end.c_str()),
    };
    register_lua_class("Lighting", "SceneService", fields, 6);
}

}  // namespace

}  // namespace engine_core
