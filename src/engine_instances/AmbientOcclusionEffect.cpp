#include "AmbientOcclusionEffect.hpp"

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

LuaSlot quality_slot(EffectQuality quality) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &effect_quality_enum();
    slot.number = static_cast<int>(quality);
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("AmbientOcclusionEffect setters run on SimulationThread");
    }
}

}  // namespace

std::optional<std::string> AmbientOcclusionEffect::set_enabled(bool value) {
    require_thread(*this);
    if (enabled_ == value) {
        return std::nullopt;
    }
    enabled_ = value;
    note_property_change("Enabled", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> AmbientOcclusionEffect::set_number(const char* property, double& slot, double value,
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

std::optional<std::string> AmbientOcclusionEffect::set_intensity(double value) {
    return set_number("Intensity", intensity_, value, kMaxIntensity);
}

std::optional<std::string> AmbientOcclusionEffect::set_radius(double value) {
    return set_number("Radius", radius_, value, kMaxRadius);
}

std::optional<std::string> AmbientOcclusionEffect::set_quality(int value) {
    require_thread(*this);
    if (enum_item_name(effect_quality_enum(), value) == nullptr) {
        return std::string("Quality must be an Enum.EffectQuality");
    }
    const EffectQuality next = static_cast<EffectQuality>(value);
    if (next == quality_) {
        return std::nullopt;
    }
    const EffectQuality previous = quality_;
    quality_ = next;
    note_property_change("Quality", quality_slot(previous), quality_slot(next));
    return std::nullopt;
}

void AmbientOcclusionEffect::on_reuse() {
    enabled_ = kDefaultEnabled;
    intensity_ = kDefaultIntensity;
    radius_ = kDefaultRadius;
    quality_ = kDefaultQuality;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

AmbientOcclusionEffect* occlusion_of(DataModel& object) { return dynamic_cast<AmbientOcclusionEffect*>(&object); }

bool read_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    const AmbientOcclusionEffect* ao = occlusion_of(object);
    if (ao == nullptr) {
        return false;
    }
    out = bool_slot(ao->enabled());
    return true;
}

bool write_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    AmbientOcclusionEffect* ao = occlusion_of(object);
    return ao != nullptr && refuse(in, ao->set_enabled(in.flag));
}

template <double (AmbientOcclusionEffect::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const AmbientOcclusionEffect* ao = occlusion_of(object);
    if (ao == nullptr) {
        return false;
    }
    out = number_slot((ao->*Get)());
    return true;
}

template <std::optional<std::string> (AmbientOcclusionEffect::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    AmbientOcclusionEffect* ao = occlusion_of(object);
    return ao != nullptr && refuse(in, (ao->*Set)(in.number));
}

bool read_quality(DataModel&, DataModel& object, LuaSlot& out) {
    const AmbientOcclusionEffect* ao = occlusion_of(object);
    if (ao == nullptr) {
        return false;
    }
    out = quality_slot(ao->quality());
    return true;
}

bool write_quality(DataModel&, DataModel& object, LuaSlot& in) {
    AmbientOcclusionEffect* ao = occlusion_of(object);
    if (ao == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &effect_quality_enum()) {
        in.error = "Quality must be an Enum.EffectQuality";
        return false;
    }
    return refuse(in, ao->set_quality(static_cast<int>(in.number)));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_ambient_occlusion_effect_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string intensity = number_json(AmbientOcclusionEffect::kDefaultIntensity);
    static const std::string radius = number_json(AmbientOcclusionEffect::kDefaultRadius);
    using AO = AmbientOcclusionEffect;
    const LuaField fields[] = {
        lua_saved_property("Enabled", "boolean", read_enabled, write_enabled, AO::kDefaultEnabled ? "true" : "false"),
        lua_slider(lua_saved_property("Intensity", "number", read_number<&AO::intensity>,
                                      write_number<&AO::set_intensity>, intensity.c_str()),
                   0.0, AO::kMaxIntensity),
        lua_slider(lua_saved_property("Radius", "number", read_number<&AO::radius>, write_number<&AO::set_radius>,
                                      radius.c_str()),
                   0.0, AO::kMaxRadius),
        lua_saved_enum("Quality", effect_quality_enum(), read_quality, write_quality, "\"Medium\""),
    };
    register_lua_class("AmbientOcclusionEffect", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("AmbientOcclusionEffect", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
