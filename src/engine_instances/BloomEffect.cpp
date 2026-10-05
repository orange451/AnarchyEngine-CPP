#include "BloomEffect.hpp"

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
        contract_fail("BloomEffect setters run on SimulationThread");
    }
}

}  // namespace

std::optional<std::string> BloomEffect::set_enabled(bool value) {
    require_thread(*this);
    if (enabled_ == value) {
        return std::nullopt;
    }
    enabled_ = value;
    note_property_change("Enabled", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> BloomEffect::set_number(const char* property, double& slot, double value, double max) {
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

std::optional<std::string> BloomEffect::set_intensity(double value) {
    return set_number("Intensity", intensity_, value, kMaxIntensity);
}

std::optional<std::string> BloomEffect::set_size(double value) { return set_number("Size", size_, value, kMaxSize); }

std::optional<std::string> BloomEffect::set_threshold(double value) {
    return set_number("Threshold", threshold_, value, kMaxThreshold);
}

void BloomEffect::on_reuse() {
    enabled_ = kDefaultEnabled;
    intensity_ = kDefaultIntensity;
    size_ = kDefaultSize;
    threshold_ = kDefaultThreshold;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

BloomEffect* bloom_of(DataModel& object) { return dynamic_cast<BloomEffect*>(&object); }

bool read_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    const BloomEffect* bloom = bloom_of(object);
    if (bloom == nullptr) {
        return false;
    }
    out = bool_slot(bloom->enabled());
    return true;
}

bool write_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    BloomEffect* bloom = bloom_of(object);
    return bloom != nullptr && refuse(in, bloom->set_enabled(in.flag));
}

template <double (BloomEffect::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const BloomEffect* bloom = bloom_of(object);
    if (bloom == nullptr) {
        return false;
    }
    out = number_slot((bloom->*Get)());
    return true;
}

template <std::optional<std::string> (BloomEffect::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    BloomEffect* bloom = bloom_of(object);
    return bloom != nullptr && refuse(in, (bloom->*Set)(in.number));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_bloom_effect_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string intensity = number_json(BloomEffect::kDefaultIntensity);
    static const std::string size = number_json(BloomEffect::kDefaultSize);
    static const std::string threshold = number_json(BloomEffect::kDefaultThreshold);
    const LuaField fields[] = {
        lua_saved_property("Enabled", "boolean", read_enabled, write_enabled,
                           BloomEffect::kDefaultEnabled ? "true" : "false"),
        lua_slider(lua_saved_property("Intensity", "number", read_number<&BloomEffect::intensity>,
                                      write_number<&BloomEffect::set_intensity>, intensity.c_str()),
                   0.0, BloomEffect::kMaxIntensity),
        lua_slider(lua_saved_property("Size", "number", read_number<&BloomEffect::size>,
                                      write_number<&BloomEffect::set_size>, size.c_str()),
                   0.0, BloomEffect::kMaxSize),
        lua_slider(lua_saved_property("Threshold", "number", read_number<&BloomEffect::threshold>,
                                      write_number<&BloomEffect::set_threshold>, threshold.c_str()),
                   0.0, BloomEffect::kMaxThreshold),
    };
    register_lua_class("BloomEffect", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("BloomEffect", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
