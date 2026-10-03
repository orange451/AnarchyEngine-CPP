#include "Dragger.hpp"

#include "Containment.hpp"
#include "Contract.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "PVInstance.hpp"
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

LuaSlot space_slot(bool local) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &dragger_space_enum();
    slot.number = local ? 1 : 0;
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("Dragger setters run on SimulationThread");
    }
}

}  // namespace

LuaSlot Dragger::adornee() const { return instance_reference_slot(adornee_, "PVInstance"); }

InstanceId Dragger::target() const {
    const LuaSlot bound = adornee();
    const InstanceId id = bound.kind == LuaSlot::Kind::Instance ? bound.id : parent(this->id());
    if (id == 0 || id == kNoParent || !alive(id) || !in_game(id)) {
        return 0;
    }
    return dynamic_cast<const PVInstance*>(instance(id)) != nullptr ? id : 0;
}

std::optional<std::string> Dragger::set_adornee(const LuaSlot& value) {
    require_thread(*this);
    return set_instance_reference("Adornee", "PVInstance", adornee_, value);
}

std::optional<std::string> Dragger::set_space(int space) {
    require_thread(*this);
    if (enum_item_name(dragger_space_enum(), space) == nullptr) {
        return std::string("Space must be an Enum.DraggerSpace");
    }
    const bool next = space == 1;
    if (next == local_) {
        return std::nullopt;
    }
    const bool previous = local_;
    local_ = next;
    note_property_change("Space", space_slot(previous), space_slot(next));
    return std::nullopt;
}

std::optional<std::string> Dragger::set_increment(double increment) {
    require_thread(*this);
    if (!std::isfinite(increment) || increment < 0.0) {
        return std::string("Increment must be a finite number, 0 or more");
    }
    if (increment == increment_) {
        return std::nullopt;
    }
    const double previous = increment_;
    increment_ = increment;
    note_property_change("Increment", number_slot(previous), number_slot(increment));
    return std::nullopt;
}

void Dragger::set_drag(bool dragging, DraggerHandle handle) {
    active_ = dragging ? handle : DraggerHandle::None;
    if (dragging_ == dragging) {
        return;
    }
    dragging_ = dragging;
    emit_property("Dragging");
}

void Dragger::on_reuse() {
    adornee_.set_guid(std::string());
    local_ = false;
    increment_ = 0.0;
    dragging_ = false;
    hovered_ = DraggerHandle::None;
    active_ = DraggerHandle::None;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

Dragger* dragger_of(DataModel& object) { return dynamic_cast<Dragger*>(&object); }

bool read_adornee(DataModel&, DataModel& object, LuaSlot& out) {
    const Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    out = dragger->adornee();
    return true;
}

bool write_adornee(DataModel&, DataModel& object, LuaSlot& in) {
    Dragger* dragger = dragger_of(object);
    return dragger != nullptr && refuse(in, dragger->set_adornee(in));
}

bool read_space(DataModel&, DataModel& object, LuaSlot& out) {
    const Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    out = space_slot(dragger->local_space());
    return true;
}

bool write_space(DataModel&, DataModel& object, LuaSlot& in) {
    Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &dragger_space_enum()) {
        in.error = "Space must be an Enum.DraggerSpace";
        return false;
    }
    return refuse(in, dragger->set_space(static_cast<int>(in.number)));
}

bool read_increment(DataModel&, DataModel& object, LuaSlot& out) {
    const Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    out = number_slot(dragger->increment());
    return true;
}

bool write_increment(DataModel&, DataModel& object, LuaSlot& in) {
    Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Number) {
        in.error = "Increment must be a number";
        return false;
    }
    return refuse(in, dragger->set_increment(in.number));
}

bool read_dragging(DataModel&, DataModel& object, LuaSlot& out) {
    const Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Bool;
    out.flag = dragger->dragging();
    return true;
}

ANARCHY_LUA_REGISTER(register_dragger_lua) {
    static const LuaParam kHandleArgs[] = {{"handle", "EnumItem"}};
    static const LuaParam kDraggedArgs[] = {{"handle", "EnumItem"}, {"offset", "Vector3"}};
    const LuaField fields[] = {
        lua_saved_property("Adornee", "PVInstance?", read_adornee, write_adornee, "null"),
        lua_saved_enum("Space", dragger_space_enum(), read_space, write_space, "\"World\""),
        lua_saved_property("Increment", "number", read_increment, write_increment, "0"),
        lua_property("Dragging", "boolean", false, read_dragging, nullptr),
        lua_event("DragBegan", kHandleArgs, 1),
        lua_event("Dragged", kDraggedArgs, 2),
        lua_event("DragEnded", kHandleArgs, 1),
    };
    register_lua_class("Dragger", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Dragger", {"PVInstance"});
}

}  // namespace

}  // namespace engine_core
