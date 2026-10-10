#include "ScreenSpaceReflections.hpp"

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

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("ScreenSpaceReflections setters run on SimulationThread");
    }
}

}  // namespace

std::optional<std::string> ScreenSpaceReflections::set_enabled(bool value) {
    require_thread(*this);
    if (enabled_ == value) {
        return std::nullopt;
    }
    enabled_ = value;
    note_property_change("Enabled", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> ScreenSpaceReflections::set_number(const char* property, double& slot, double value,
                                                              double max) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    value = std::clamp(value, 0.0, max);
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> ScreenSpaceReflections::set_intensity(double value) {
    return set_number("Intensity", intensity_, value, kMaxIntensity);
}

std::optional<std::string> ScreenSpaceReflections::set_max_distance(double value) {
    return set_number("MaxDistance", max_distance_, value, kMaxMaxDistance);
}

std::optional<std::string> ScreenSpaceReflections::set_max_roughness(double value) {
    return set_number("MaxRoughness", max_roughness_, value, kMaxMaxRoughness);
}

void ScreenSpaceReflections::on_reuse() {
    enabled_ = kDefaultEnabled;
    intensity_ = kDefaultIntensity;
    max_distance_ = kDefaultMaxDistance;
    max_roughness_ = kDefaultMaxRoughness;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

ScreenSpaceReflections* reflections_of(DataModel& object) { return dynamic_cast<ScreenSpaceReflections*>(&object); }

bool read_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    const ScreenSpaceReflections* ssr = reflections_of(object);
    if (ssr == nullptr) {
        return false;
    }
    out = bool_slot(ssr->enabled());
    return true;
}

bool write_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    ScreenSpaceReflections* ssr = reflections_of(object);
    return ssr != nullptr && refuse(in, ssr->set_enabled(in.flag));
}

template <double (ScreenSpaceReflections::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const ScreenSpaceReflections* ssr = reflections_of(object);
    if (ssr == nullptr) {
        return false;
    }
    out = number_slot((ssr->*Get)());
    return true;
}

template <std::optional<std::string> (ScreenSpaceReflections::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    ScreenSpaceReflections* ssr = reflections_of(object);
    return ssr != nullptr && refuse(in, (ssr->*Set)(in.number));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_screen_space_reflections_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string intensity = number_json(ScreenSpaceReflections::kDefaultIntensity);
    static const std::string max_distance = number_json(ScreenSpaceReflections::kDefaultMaxDistance);
    static const std::string max_roughness = number_json(ScreenSpaceReflections::kDefaultMaxRoughness);
    using SSR = ScreenSpaceReflections;
    const LuaField fields[] = {
        lua_saved_property("Enabled", "boolean", read_enabled, write_enabled, SSR::kDefaultEnabled ? "true" : "false"),
        lua_slider(lua_saved_property("Intensity", "number", read_number<&SSR::intensity>,
                                      write_number<&SSR::set_intensity>, intensity.c_str()),
                   0.0, SSR::kMaxIntensity),
        lua_slider(lua_saved_property("MaxDistance", "number", read_number<&SSR::max_distance>,
                                      write_number<&SSR::set_max_distance>, max_distance.c_str()),
                   0.0, SSR::kMaxMaxDistance),
        lua_slider(lua_saved_property("MaxRoughness", "number", read_number<&SSR::max_roughness>,
                                      write_number<&SSR::set_max_roughness>, max_roughness.c_str()),
                   0.0, SSR::kMaxMaxRoughness),
    };
    register_lua_class("ScreenSpaceReflections", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("ScreenSpaceReflections", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
