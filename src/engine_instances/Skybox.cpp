#include "Skybox.hpp"

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

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("Skybox setters run on SimulationThread");
    }
}

}  // namespace

LuaSlot Skybox::image() const { return instance_reference_slot(image_ref_, "Texture"); }

LuaSlot Skybox::reflections() const { return instance_reference_slot(reflections_ref_, "Texture"); }

std::optional<std::string> Skybox::set_image(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("Image", "Texture", image_ref_, value);
}

std::optional<std::string> Skybox::set_reflections(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("Reflections", "Texture", reflections_ref_, value);
}

std::optional<std::string> Skybox::set_number(const char* property, double& slot, double value) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    if (&slot == &rotation_) {
        value = std::fmod(value, 360.0);
        if (value < 0) {
            value += 360.0;
        }
        // fmod of a tiny negative number, plus 360, can round to 360 itself.
        if (value >= 360.0) {
            value = 0.0;
        }
    } else {
        value = std::clamp(value, 0.0, kMaxExposure);
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> Skybox::set_exposure(double value) { return set_number("Exposure", exposure_, value); }

std::optional<std::string> Skybox::set_rotation(double degrees) {
    return set_number("Rotation", rotation_, degrees);
}

std::optional<std::string> Skybox::set_tint(ColorRgb color) {
    require_thread(*this);
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) {
        return std::string("Tint must be finite");
    }
    // A Color3 has no alpha.
    color.a = 1.f;
    if (same_color(tint_, color)) {
        return std::nullopt;
    }
    const ColorRgb previous = tint_;
    tint_ = color;
    note_property_change("Tint", color_slot(previous), color_slot(color));
    return std::nullopt;
}

void Skybox::on_reuse() {
    image_ref_.set_guid(std::string());
    reflections_ref_.set_guid(std::string());
    exposure_ = kDefaultExposure;
    rotation_ = kDefaultRotation;
    tint_ = kDefaultTint;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

Skybox* skybox_of(DataModel& object) { return dynamic_cast<Skybox*>(&object); }

template <LuaSlot (Skybox::*Get)() const>
bool read_texture(DataModel&, DataModel& object, LuaSlot& out) {
    const Skybox* sky = skybox_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = (sky->*Get)();
    return true;
}

template <std::optional<std::string> (Skybox::*Set)(const LuaSlot&)>
bool write_texture(DataModel&, DataModel& object, LuaSlot& in) {
    Skybox* sky = skybox_of(object);
    return sky != nullptr && refuse(in, (sky->*Set)(in));
}

template <double (Skybox::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const Skybox* sky = skybox_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = number_slot((sky->*Get)());
    return true;
}

template <std::optional<std::string> (Skybox::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    Skybox* sky = skybox_of(object);
    return sky != nullptr && refuse(in, (sky->*Set)(in.number));
}

bool read_tint(DataModel&, DataModel& object, LuaSlot& out) {
    const Skybox* sky = skybox_of(object);
    if (sky == nullptr) {
        return false;
    }
    out = color_slot(sky->tint());
    return true;
}

bool write_tint(DataModel&, DataModel& object, LuaSlot& in) {
    Skybox* sky = skybox_of(object);
    return sky != nullptr && refuse(in, sky->set_tint(in.color));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_skybox_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string exposure = number_json(Skybox::kDefaultExposure);
    static const std::string rotation = number_json(Skybox::kDefaultRotation);
    static const std::string tint = [] {
        const float channels[3] = {Skybox::kDefaultTint.r, Skybox::kDefaultTint.g, Skybox::kDefaultTint.b};
        return write_json(json_floats(channels, 3));
    }();
    const LuaField fields[] = {
        lua_saved_property("Image", "Texture?", read_texture<&Skybox::image>, write_texture<&Skybox::set_image>,
                           "null"),
        lua_slider(lua_saved_property("Exposure", "number", read_number<&Skybox::exposure>,
                                      write_number<&Skybox::set_exposure>, exposure.c_str()),
                   0.0, Skybox::kMaxExposure),
        lua_slider(lua_saved_property("Rotation", "number", read_number<&Skybox::rotation>,
                                      write_number<&Skybox::set_rotation>, rotation.c_str()),
                   0.0, 360.0),
        lua_saved_property("Tint", "Color3", read_tint, write_tint, tint.c_str()),
        lua_saved_property("Reflections", "Texture?", read_texture<&Skybox::reflections>,
                           write_texture<&Skybox::set_reflections>, "null"),
    };
    register_lua_class("Skybox", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Skybox", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
