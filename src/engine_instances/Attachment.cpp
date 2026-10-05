#include "Attachment.hpp"

#include "Containment.hpp"
#include "Contract.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <cmath>
#include <iterator>

namespace engine_core {
namespace {

LuaSlot matrix_slot(const Matrix4& value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Matrix4;
    slot.transform = value;
    return slot;
}

LuaSlot space_slot(TransformSpace space) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &transform_space_enum();
    slot.number = static_cast<int>(space);
    return slot;
}

bool finite(const Matrix4& value) {
    for (float component : value.m) {
        if (!std::isfinite(component)) {
            return false;
        }
    }
    return true;
}

// The frame an Offset in space is measured from under the instance at id: its
// Transform in Local, only its position in World, and identity when it is not
// a PVInstance.
Matrix4 frame_of(const DataModel& world, InstanceId id, TransformSpace space) {
    if (id == DataModel::kNoParent) {
        return matrix4_identity();
    }
    const auto* holder = dynamic_cast<const PVInstance*>(world.instance(id));
    if (holder == nullptr) {
        return matrix4_identity();
    }
    const Matrix4 transform = holder->transform();
    if (space == TransformSpace::World) {
        const Vec3 position = matrix4_position(transform);
        return matrix4_translation(position.x, position.y, position.z);
    }
    return transform;
}

}  // namespace

Matrix4 Attachment::parent_frame() const { return frame_of(*this, parent(id()), offset_space_); }

Matrix4 Attachment::transform() const {
    if (!alive(id())) {
        return Matrix4{};
    }
    return matrix4_multiply(parent_frame(), offset_);
}

std::optional<std::string> Attachment::set_offset(const Matrix4& offset) {
    if (!on_gameplay_thread()) {
        contract_fail("Attachment setters run on SimulationThread");
    }
    if (!finite(offset)) {
        return std::string("Offset must be finite");
    }
    if (same_matrix4(offset_, offset)) {
        return std::nullopt;
    }
    const Matrix4 previous = offset_;
    offset_ = offset;
    note_property_change("Offset", matrix_slot(previous), matrix_slot(offset));
    emit_property("Transform");
    return std::nullopt;
}

std::optional<std::string> Attachment::set_transform(const Matrix4& transform) {
    if (!finite(transform)) {
        return std::string("Transform must be finite");
    }
    const Matrix4 offset = matrix4_multiply(matrix4_inverse(parent_frame()), transform);
    if (!finite(offset)) {
        return std::string("Transform cannot be set while the parent's Transform has no inverse");
    }
    return set_offset(offset);
}

std::optional<std::string> Attachment::set_offset_space(int space) {
    if (!on_gameplay_thread()) {
        contract_fail("Attachment setters run on SimulationThread");
    }
    if (enum_item_name(transform_space_enum(), space) == nullptr) {
        return std::string("OffsetSpace must be an Enum.TransformSpace");
    }
    const TransformSpace next = static_cast<TransformSpace>(space);
    if (next == offset_space_) {
        return std::nullopt;
    }
    // Offset stays as it is, so the Transform moves to the new frame.
    const TransformSpace previous = offset_space_;
    offset_space_ = next;
    note_property_change("OffsetSpace", space_slot(previous), space_slot(next));
    emit_property("Transform");
    return std::nullopt;
}

void Attachment::on_reuse() {
    offset_ = matrix4_identity();
    offset_space_ = TransformSpace::Local;
}

void Attachment::on_parent_changed(InstanceId previous, InstanceId next) {
    if (!alive(id())) {
        return;
    }
    if (!same_matrix4(frame_of(*this, previous, offset_space_), frame_of(*this, next, offset_space_))) {
        emit_property("Transform");
    }
}

namespace {

Attachment* attachment_of(DataModel& object) { return dynamic_cast<Attachment*>(&object); }

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

bool read_offset(DataModel&, DataModel& object, LuaSlot& out) {
    const Attachment* attachment = attachment_of(object);
    if (attachment == nullptr) {
        return false;
    }
    out = matrix_slot(attachment->offset());
    return true;
}

bool write_offset(DataModel&, DataModel& object, LuaSlot& in) {
    Attachment* attachment = attachment_of(object);
    return attachment != nullptr && refuse(in, attachment->set_offset(in.transform));
}

bool read_offset_space(DataModel&, DataModel& object, LuaSlot& out) {
    const Attachment* attachment = attachment_of(object);
    if (attachment == nullptr) {
        return false;
    }
    out = space_slot(attachment->offset_space());
    return true;
}

bool write_offset_space(DataModel&, DataModel& object, LuaSlot& in) {
    Attachment* attachment = attachment_of(object);
    if (attachment == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &transform_space_enum()) {
        in.error = "OffsetSpace must be an Enum.TransformSpace";
        return false;
    }
    return refuse(in, attachment->set_offset_space(static_cast<int>(in.number)));
}

bool read_transform(DataModel&, DataModel& object, LuaSlot& out) {
    const Attachment* attachment = attachment_of(object);
    if (attachment == nullptr) {
        return false;
    }
    out = matrix_slot(attachment->transform());
    return true;
}

bool write_transform(DataModel&, DataModel& object, LuaSlot& in) {
    Attachment* attachment = attachment_of(object);
    return attachment != nullptr && refuse(in, attachment->set_transform(in.transform));
}

ANARCHY_LUA_REGISTER(register_attachment_lua) {
    static const std::string identity = [] {
        const Matrix4 value = matrix4_identity();
        return write_json(json_floats(value.m, 16));
    }();
    const LuaField fields[] = {
        // Transform is worked out from Offset, so only Offset is saved.
        lua_property("Transform", "Matrix4", true, read_transform, write_transform),
        lua_saved_property("Offset", "Matrix4", read_offset, write_offset, identity.c_str()),
        lua_saved_enum("OffsetSpace", transform_space_enum(), read_offset_space, write_offset_space, "\"Local\""),
    };
    register_lua_class("Attachment", "PVInstance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Attachment", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
