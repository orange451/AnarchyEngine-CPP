#include "Attachment.hpp"

#include "Containment.hpp"
#include "Contract.hpp"
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

bool finite(const Matrix4& value) {
    for (float component : value.m) {
        if (!std::isfinite(component)) {
            return false;
        }
    }
    return true;
}

// The Transform of the instance at id, or identity when it is not a PVInstance.
Matrix4 pv_transform_of(const DataModel& world, InstanceId id) {
    if (id == DataModel::kNoParent) {
        return matrix4_identity();
    }
    const auto* holder = dynamic_cast<const PVInstance*>(world.instance(id));
    return holder != nullptr ? holder->transform() : matrix4_identity();
}

}  // namespace

Matrix4 Attachment::parent_transform() const { return pv_transform_of(*this, parent(id())); }

Matrix4 Attachment::transform() const {
    if (!alive(id())) {
        return Matrix4{};
    }
    return matrix4_multiply(parent_transform(), offset_);
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
    const Matrix4 offset = matrix4_multiply(matrix4_inverse(parent_transform()), transform);
    if (!finite(offset)) {
        return std::string("Transform cannot be set while the parent's Transform has no inverse");
    }
    return set_offset(offset);
}

void Attachment::on_reuse() { offset_ = matrix4_identity(); }

void Attachment::on_parent_changed(InstanceId previous, InstanceId next) {
    if (!alive(id())) {
        return;
    }
    if (!same_matrix4(pv_transform_of(*this, previous), pv_transform_of(*this, next))) {
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
    };
    register_lua_class("Attachment", "PVInstance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Attachment", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
