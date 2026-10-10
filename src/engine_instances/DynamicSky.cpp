#include "DynamicSky.hpp"

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

LuaSlot vec3_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
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
        contract_fail("DynamicSky setters run on SimulationThread");
    }
}

}  // namespace

LuaSlot DynamicSky::sun_texture() const { return instance_reference_slot(sun_texture_ref_, "Texture"); }

LuaSlot DynamicSky::moon_texture() const { return instance_reference_slot(moon_texture_ref_, "Texture"); }

std::optional<std::string> DynamicSky::set_number(const char* property, double& slot, double value, double min,
                                                  double max) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    value = std::clamp(value, min, max);
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> DynamicSky::set_time_of_day(double hours) {
    if (!std::isfinite(hours)) {
        require_thread(*this);
        return std::string("TimeOfDay must be a finite number");
    }
    hours = std::fmod(hours, 24.0);
    if (hours < 0) {
        hours += 24.0;
    }
    // fmod of a tiny negative number, plus 24, can round to 24 itself.
    if (hours >= 24.0) {
        hours = 0.0;
    }
    return set_number("TimeOfDay", time_of_day_, hours, 0.0, 24.0);
}

std::optional<std::string> DynamicSky::set_latitude(double degrees) {
    return set_number("Latitude", latitude_, degrees, -kMaxLatitude, kMaxLatitude);
}

std::optional<std::string> DynamicSky::set_brightness(double value) {
    return set_number("Brightness", brightness_, value, 0.0, kMaxBrightness);
}

std::optional<std::string> DynamicSky::set_shadows(bool value) {
    require_thread(*this);
    if (shadows_ == value) {
        return std::nullopt;
    }
    shadows_ = value;
    note_property_change("Shadows", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> DynamicSky::set_cloud_cover(double value) {
    return set_number("CloudCover", cloud_cover_, value, 0.0, 1.0);
}

std::optional<std::string> DynamicSky::set_cloud_density(double value) {
    return set_number("CloudDensity", cloud_density_, value, 0.0, 1.0);
}

std::optional<std::string> DynamicSky::set_wind_direction(Vec3 value) {
    require_thread(*this);
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
        return std::string("WindDirection must be finite");
    }
    if (value.x == wind_direction_.x && value.y == wind_direction_.y && value.z == wind_direction_.z) {
        return std::nullopt;
    }
    const Vec3 previous = wind_direction_;
    wind_direction_ = value;
    note_property_change("WindDirection", vec3_slot(previous), vec3_slot(value));
    return std::nullopt;
}

std::optional<std::string> DynamicSky::set_sun_texture(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("SunTexture", "Texture", sun_texture_ref_, value);
}

std::optional<std::string> DynamicSky::set_moon_texture(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("MoonTexture", "Texture", moon_texture_ref_, value);
}

std::optional<std::string> DynamicSky::set_sun_size(double degrees) {
    return set_number("SunSize", sun_size_, degrees, kMinBodySize, kMaxBodySize);
}

std::optional<std::string> DynamicSky::set_moon_size(double degrees) {
    return set_number("MoonSize", moon_size_, degrees, kMinBodySize, kMaxBodySize);
}

std::optional<std::string> DynamicSky::set_reflection_quality(int value) {
    require_thread(*this);
    if (enum_item_name(effect_quality_enum(), value) == nullptr) {
        return std::string("ReflectionQuality must be an Enum.EffectQuality");
    }
    const EffectQuality next = static_cast<EffectQuality>(value);
    if (next == reflection_quality_) {
        return std::nullopt;
    }
    const EffectQuality previous = reflection_quality_;
    reflection_quality_ = next;
    note_property_change("ReflectionQuality", quality_slot(previous), quality_slot(next));
    return std::nullopt;
}

void DynamicSky::on_reuse() {
    time_of_day_ = kDefaultTimeOfDay;
    latitude_ = kDefaultLatitude;
    brightness_ = kDefaultBrightness;
    shadows_ = kDefaultShadows;
    cloud_cover_ = kDefaultCloudCover;
    cloud_density_ = kDefaultCloudDensity;
    wind_direction_ = kDefaultWindDirection;
    sun_texture_ref_.set_guid(std::string());
    moon_texture_ref_.set_guid(std::string());
    sun_size_ = kDefaultSunSize;
    moon_size_ = kDefaultMoonSize;
    reflection_quality_ = kDefaultReflectionQuality;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

DynamicSky* sky_of(DataModel& object) { return dynamic_cast<DynamicSky*>(&object); }

template <double (DynamicSky::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = number_slot((sky->*Get)());
    return true;
}

template <std::optional<std::string> (DynamicSky::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    return sky != nullptr && refuse(in, (sky->*Set)(in.number));
}

template <LuaSlot (DynamicSky::*Get)() const>
bool read_texture(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = (sky->*Get)();
    return true;
}

template <std::optional<std::string> (DynamicSky::*Set)(const LuaSlot&)>
bool write_texture(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    return sky != nullptr && refuse(in, (sky->*Set)(in));
}

bool read_shadows(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = bool_slot(sky->shadows());
    return true;
}

bool write_shadows(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    return sky != nullptr && refuse(in, sky->set_shadows(in.flag));
}

bool read_wind(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = vec3_slot(sky->wind_direction());
    return true;
}

bool write_wind(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    return sky != nullptr && refuse(in, sky->set_wind_direction(in.vec));
}

bool read_quality(DataModel&, DataModel& object, LuaSlot& out) {
    const DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = quality_slot(sky->reflection_quality());
    return true;
}

bool write_quality(DataModel&, DataModel& object, LuaSlot& in) {
    DynamicSky* sky = sky_of(object);
    if (sky == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &effect_quality_enum()) {
        in.error = "ReflectionQuality must be an Enum.EffectQuality";
        return false;
    }
    return refuse(in, sky->set_reflection_quality(static_cast<int>(in.number)));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_dynamic_sky_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    using S = DynamicSky;
    static const std::string time_of_day = number_json(S::kDefaultTimeOfDay);
    static const std::string latitude = number_json(S::kDefaultLatitude);
    static const std::string brightness = number_json(S::kDefaultBrightness);
    static const std::string cloud_cover = number_json(S::kDefaultCloudCover);
    static const std::string cloud_density = number_json(S::kDefaultCloudDensity);
    static const std::string sun_size = number_json(S::kDefaultSunSize);
    static const std::string moon_size = number_json(S::kDefaultMoonSize);
    static const std::string wind = [] {
        const float axes[3] = {S::kDefaultWindDirection.x, S::kDefaultWindDirection.y, S::kDefaultWindDirection.z};
        return write_json(json_floats(axes, 3));
    }();
    const LuaField fields[] = {
        lua_group("Time"),
        lua_slider(lua_saved_property("TimeOfDay", "number", read_number<&S::time_of_day>,
                                      write_number<&S::set_time_of_day>, time_of_day.c_str()),
                   0.0, 24.0),
        lua_slider(lua_saved_property("Latitude", "number", read_number<&S::latitude>,
                                      write_number<&S::set_latitude>, latitude.c_str()),
                   -S::kMaxLatitude, S::kMaxLatitude),
        lua_group("Sun & Moon"),
        lua_slider(lua_saved_property("Brightness", "number", read_number<&S::brightness>,
                                      write_number<&S::set_brightness>, brightness.c_str()),
                   0.0, S::kMaxBrightness),
        lua_saved_property("Shadows", "boolean", read_shadows, write_shadows, S::kDefaultShadows ? "true" : "false"),
        lua_saved_property("SunTexture", "Texture?", read_texture<&S::sun_texture>,
                           write_texture<&S::set_sun_texture>, "null"),
        lua_slider(lua_saved_property("SunSize", "number", read_number<&S::sun_size>, write_number<&S::set_sun_size>,
                                      sun_size.c_str()),
                   S::kMinBodySize, S::kMaxBodySize),
        lua_saved_property("MoonTexture", "Texture?", read_texture<&S::moon_texture>,
                           write_texture<&S::set_moon_texture>, "null"),
        lua_slider(lua_saved_property("MoonSize", "number", read_number<&S::moon_size>,
                                      write_number<&S::set_moon_size>, moon_size.c_str()),
                   S::kMinBodySize, S::kMaxBodySize),
        lua_group("Clouds"),
        lua_slider(lua_saved_property("CloudCover", "number", read_number<&S::cloud_cover>,
                                      write_number<&S::set_cloud_cover>, cloud_cover.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("CloudDensity", "number", read_number<&S::cloud_density>,
                                      write_number<&S::set_cloud_density>, cloud_density.c_str()),
                   0.0, 1.0),
        lua_saved_property("WindDirection", "Vector3", read_wind, write_wind, wind.c_str()),
        lua_group("Quality"),
        lua_saved_enum("ReflectionQuality", effect_quality_enum(), read_quality, write_quality, "\"Medium\""),
    };
    register_lua_class("DynamicSky", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("DynamicSky", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
