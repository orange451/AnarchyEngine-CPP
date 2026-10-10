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

LuaSlot matrix_slot(const Matrix4& value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Matrix4;
    slot.transform = value;
    return slot;
}

LuaSlot space_slot(bool local) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &transform_space_enum();
    slot.number = local ? 1 : 0;
    return slot;
}

LuaSlot mode_slot(DraggerMode mode) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &transform_mode_enum();
    slot.number = static_cast<int>(mode);
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("Dragger setters run on SimulationThread");
    }
}

}  // namespace

std::optional<std::string> Dragger::set_transform(const Matrix4& transform) {
    require_thread(*this);
    for (float value : transform.m) {
        if (!std::isfinite(value)) {
            return std::string("Transform must be finite");
        }
    }
    if (same_matrix4(transform_, transform)) {
        return std::nullopt;
    }
    const Matrix4 previous = transform_;
    transform_ = transform;
    note_property_change("Transform", matrix_slot(previous), matrix_slot(transform));
    return std::nullopt;
}

std::optional<std::string> Dragger::set_space(int space) {
    require_thread(*this);
    if (enum_item_name(transform_space_enum(), space) == nullptr) {
        return std::string("Space must be an Enum.TransformSpace");
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

std::optional<std::string> Dragger::set_transform_mode(int mode) {
    require_thread(*this);
    if (enum_item_name(transform_mode_enum(), mode) == nullptr) {
        return std::string("TransformMode must be an Enum.TransformMode");
    }
    const auto next = static_cast<DraggerMode>(mode);
    if (next == mode_) {
        return std::nullopt;
    }
    const DraggerMode previous = mode_;
    mode_ = next;
    note_property_change("TransformMode", mode_slot(previous), mode_slot(next));
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
    transform_ = matrix4_identity();
    local_ = false;
    mode_ = DraggerMode::Translation;
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

bool read_transform(DataModel&, DataModel& object, LuaSlot& out) {
    const Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    out = matrix_slot(dragger->transform());
    return true;
}

bool write_transform(DataModel&, DataModel& object, LuaSlot& in) {
    Dragger* dragger = dragger_of(object);
    return dragger != nullptr && refuse(in, dragger->set_transform(in.transform));
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
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &transform_space_enum()) {
        in.error = "Space must be an Enum.TransformSpace";
        return false;
    }
    return refuse(in, dragger->set_space(static_cast<int>(in.number)));
}

bool read_transform_mode(DataModel&, DataModel& object, LuaSlot& out) {
    const Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    out = mode_slot(dragger->transform_mode());
    return true;
}

bool write_transform_mode(DataModel&, DataModel& object, LuaSlot& in) {
    Dragger* dragger = dragger_of(object);
    if (dragger == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &transform_mode_enum()) {
        in.error = "TransformMode must be an Enum.TransformMode";
        return false;
    }
    return refuse(in, dragger->set_transform_mode(static_cast<int>(in.number)));
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
    // offset is a Vector3 in Translation and a Matrix4 in Rotation.
    static const LuaParam kDraggedArgs[] = {{"handle", "EnumItem"}, {"offset", "Vector3 | Matrix4"}};
    static const std::string identity = [] {
        const Matrix4 value = matrix4_identity();
        return write_json(json_floats(value.m, 16));
    }();
    const LuaField fields[] = {
        lua_saved_property("Transform", "Matrix4", read_transform, write_transform, identity.c_str()),
        lua_saved_enum("Space", transform_space_enum(), read_space, write_space, "\"World\""),
        lua_saved_enum("TransformMode", transform_mode_enum(), read_transform_mode, write_transform_mode,
                       "\"Translation\""),
        lua_saved_property("Increment", "number", read_increment, write_increment, "0"),
        lua_property("Dragging", "boolean", false, read_dragging, nullptr),
        lua_event("DragBegan", kHandleArgs, 1),
        lua_event("Dragged", kDraggedArgs, 2),
        lua_event("DragEnded", kHandleArgs, 1),
    };
    register_lua_class("Dragger", "PVInstance", fields, static_cast<int>(std::size(fields)));
}

}  // namespace

}  // namespace engine_core
