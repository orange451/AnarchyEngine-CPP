#include "WireframeAdornment.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

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

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("WireframeAdornment setters run on SimulationThread");
    }
}

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

WireframeAdornment::WireframeAdornment(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
    : DataModel(tag, state, id) {}

const char* WireframeAdornment::class_name() const { return "WireframeAdornment"; }

LuaSlot WireframeAdornment::adornee() const { return instance_reference_slot(adornee_ref_, "PVInstance"); }

InstanceId WireframeAdornment::adornee_id() const {
    const InstanceId target = adornee_ref_.resolve(*this);
    if (target == 0) {
        return 0;
    }
    const DataModel* object = instance(target);
    return object != nullptr && lua_class_inherits(object->class_name(), "PVInstance") ? target : 0;
}

bool WireframeAdornment::drawn() const { return visible_ && (in_workspace(id()) || in_core(id())); }

std::optional<std::string> WireframeAdornment::set_adornee(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("Adornee", "PVInstance", adornee_ref_, value);
}

std::optional<std::string> WireframeAdornment::set_color(ColorRgb color) {
    require_thread(*this);
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) {
        return std::string("Color3 must be finite");
    }
    color.a = 1.f;
    if (color.r == color_.r && color.g == color_.g && color.b == color_.b) {
        return std::nullopt;
    }
    const ColorRgb previous = color_;
    color_ = color;
    note_property_change("Color3", color_slot(previous), color_slot(color));
    return std::nullopt;
}

std::optional<std::string> WireframeAdornment::set_transparency(double value) {
    require_thread(*this);
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        return std::string("Transparency must be from 0 to 1");
    }
    if (value == transparency_) {
        return std::nullopt;
    }
    const double previous = transparency_;
    transparency_ = value;
    note_property_change("Transparency", number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> WireframeAdornment::set_visible(bool visible) {
    require_thread(*this);
    if (visible == visible_) {
        return std::nullopt;
    }
    visible_ = visible;
    note_property_change("Visible", bool_slot(!visible), bool_slot(visible));
    return std::nullopt;
}

std::optional<std::string> WireframeAdornment::add_line(const Line& line) {
    require_thread(*this);
    if (!finite(line.from) || !finite(line.to)) {
        return std::string("points must be finite");
    }
    if (lines_.size() >= kMaxLines) {
        return std::string("a WireframeAdornment holds at most 65536 lines");
    }
    lines_.push_back(line);
    return std::nullopt;
}

void WireframeAdornment::on_reuse() {
    adornee_ref_.set_guid(std::string());
    color_ = ColorRgb{};
    transparency_ = 0.0;
    visible_ = true;
    lines_.clear();
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

WireframeAdornment* wire_of(DataModel& object) { return dynamic_cast<WireframeAdornment*>(&object); }

bool read_adornee(DataModel&, DataModel& object, LuaSlot& out) {
    const WireframeAdornment* wire = wire_of(object);
    if (wire == nullptr) {
        return false;
    }
    out = wire->adornee();
    return true;
}

bool write_adornee(DataModel&, DataModel& object, LuaSlot& in) {
    WireframeAdornment* wire = wire_of(object);
    return wire != nullptr && refuse(in, wire->set_adornee(in));
}

bool read_color(DataModel&, DataModel& object, LuaSlot& out) {
    const WireframeAdornment* wire = wire_of(object);
    if (wire == nullptr) {
        return false;
    }
    out = color_slot(wire->color());
    return true;
}

bool write_color(DataModel&, DataModel& object, LuaSlot& in) {
    WireframeAdornment* wire = wire_of(object);
    return wire != nullptr && refuse(in, wire->set_color(in.color));
}

bool read_transparency(DataModel&, DataModel& object, LuaSlot& out) {
    const WireframeAdornment* wire = wire_of(object);
    if (wire == nullptr) {
        return false;
    }
    out = number_slot(wire->transparency());
    return true;
}

bool write_transparency(DataModel&, DataModel& object, LuaSlot& in) {
    WireframeAdornment* wire = wire_of(object);
    return wire != nullptr && refuse(in, wire->set_transparency(in.number));
}

bool read_visible(DataModel&, DataModel& object, LuaSlot& out) {
    const WireframeAdornment* wire = wire_of(object);
    if (wire == nullptr) {
        return false;
    }
    out = bool_slot(wire->visible());
    return true;
}

bool write_visible(DataModel&, DataModel& object, LuaSlot& in) {
    WireframeAdornment* wire = wire_of(object);
    return wire != nullptr && refuse(in, wire->set_visible(in.flag));
}

ANARCHY_LUA_REGISTER(register_wireframe_lua) {
    const LuaField fields[] = {
        lua_saved_property("Adornee", "PVInstance?", read_adornee, write_adornee, "null"),
        lua_saved_property("Color3", "Color3", read_color, write_color, "[1,1,1]"),
        lua_slider(lua_saved_property("Transparency", "number", read_transparency, write_transparency, "0"), 0.0, 1.0),
        lua_saved_property("Visible", "boolean", read_visible, write_visible, "true"),
    };
    register_lua_class("WireframeAdornment", "Instance", fields, static_cast<int>(std::size(fields)));
}

}  // namespace

}  // namespace engine_core
