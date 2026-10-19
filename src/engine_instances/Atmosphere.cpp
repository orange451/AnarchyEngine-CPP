#include "Atmosphere.hpp"

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

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot color_slot(ColorRgb color) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Color;
    slot.color = color;
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("Atmosphere setters run on SimulationThread");
    }
}

}  // namespace

std::optional<std::string> Atmosphere::set_enabled(bool value) {
    require_thread(*this);
    if (enabled_ == value) {
        return std::nullopt;
    }
    enabled_ = value;
    note_property_change("Enabled", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> Atmosphere::set_number(const char* property, double& slot, double value, double max) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    if (max >= 0.0) {
        value = std::clamp(value, 0.0, max);
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> Atmosphere::set_density(double value) {
    return set_number("Density", density_, value, kMaxDensity);
}

std::optional<std::string> Atmosphere::set_height_falloff(double value) {
    return set_number("HeightFalloff", height_falloff_, value, kMaxHeightFalloff);
}

std::optional<std::string> Atmosphere::set_base_height(double value) {
    return set_number("BaseHeight", base_height_, value, -1.0);
}

std::optional<std::string> Atmosphere::set_aerial_perspective(double value) {
    return set_number("AerialPerspective", aerial_perspective_, value, 1.0);
}

std::optional<std::string> Atmosphere::set_sun_scatter(double value) {
    return set_number("SunScatter", sun_scatter_, value, 1.0);
}

std::optional<std::string> Atmosphere::set_sky_haze(double value) {
    return set_number("SkyHaze", sky_haze_, value, 1.0);
}

std::optional<std::string> Atmosphere::set_color(ColorRgb color) {
    require_thread(*this);
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
    return std::nullopt;
}

void Atmosphere::on_reuse() {
    enabled_ = kDefaultEnabled;
    density_ = kDefaultDensity;
    height_falloff_ = kDefaultHeightFalloff;
    base_height_ = kDefaultBaseHeight;
    color_ = kDefaultColor;
    aerial_perspective_ = kDefaultAerialPerspective;
    sun_scatter_ = kDefaultSunScatter;
    sky_haze_ = kDefaultSkyHaze;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

Atmosphere* atmosphere_of(DataModel& object) { return dynamic_cast<Atmosphere*>(&object); }

bool read_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    const Atmosphere* air = atmosphere_of(object);
    if (air == nullptr) {
        return false;
    }
    out = bool_slot(air->enabled());
    return true;
}

bool write_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    Atmosphere* air = atmosphere_of(object);
    return air != nullptr && refuse(in, air->set_enabled(in.flag));
}

template <double (Atmosphere::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const Atmosphere* air = atmosphere_of(object);
    if (air == nullptr) {
        return false;
    }
    out = number_slot((air->*Get)());
    return true;
}

template <std::optional<std::string> (Atmosphere::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    Atmosphere* air = atmosphere_of(object);
    return air != nullptr && refuse(in, (air->*Set)(in.number));
}

bool read_color(DataModel&, DataModel& object, LuaSlot& out) {
    const Atmosphere* air = atmosphere_of(object);
    if (air == nullptr) {
        return false;
    }
    out = color_slot(air->color());
    return true;
}

bool write_color(DataModel&, DataModel& object, LuaSlot& in) {
    Atmosphere* air = atmosphere_of(object);
    return air != nullptr && refuse(in, air->set_color(in.color));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_atmosphere_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string density = number_json(Atmosphere::kDefaultDensity);
    static const std::string falloff = number_json(Atmosphere::kDefaultHeightFalloff);
    static const std::string base = number_json(Atmosphere::kDefaultBaseHeight);
    static const std::string color = [] {
        const float channels[3] = {Atmosphere::kDefaultColor.r, Atmosphere::kDefaultColor.g,
                                   Atmosphere::kDefaultColor.b};
        return write_json(json_floats(channels, 3));
    }();
    static const std::string aerial = number_json(Atmosphere::kDefaultAerialPerspective);
    static const std::string scatter = number_json(Atmosphere::kDefaultSunScatter);
    static const std::string haze = number_json(Atmosphere::kDefaultSkyHaze);
    const LuaField fields[] = {
        lua_saved_property("Enabled", "boolean", read_enabled, write_enabled,
                           Atmosphere::kDefaultEnabled ? "true" : "false"),
        lua_slider(lua_saved_property("Density", "number", read_number<&Atmosphere::density>,
                                      write_number<&Atmosphere::set_density>, density.c_str()),
                   0.0, Atmosphere::kMaxDensity),
        lua_slider(lua_saved_property("HeightFalloff", "number", read_number<&Atmosphere::height_falloff>,
                                      write_number<&Atmosphere::set_height_falloff>, falloff.c_str()),
                   0.0, Atmosphere::kMaxHeightFalloff),
        lua_saved_property("BaseHeight", "number", read_number<&Atmosphere::base_height>,
                           write_number<&Atmosphere::set_base_height>, base.c_str()),
        lua_saved_property("Color", "Color3", read_color, write_color, color.c_str()),
        lua_slider(lua_saved_property("AerialPerspective", "number", read_number<&Atmosphere::aerial_perspective>,
                                      write_number<&Atmosphere::set_aerial_perspective>, aerial.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("SunScatter", "number", read_number<&Atmosphere::sun_scatter>,
                                      write_number<&Atmosphere::set_sun_scatter>, scatter.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("SkyHaze", "number", read_number<&Atmosphere::sky_haze>,
                                      write_number<&Atmosphere::set_sky_haze>, haze.c_str()),
                   0.0, 1.0),
    };
    register_lua_class("Atmosphere", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Atmosphere", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
