#include "PhysicsObject.hpp"

#include "Contract.hpp"
#include "Enum.hpp"
#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace engine_core {

using namespace physics_detail;

namespace {

LuaSlot shape_slot(PhysicsObject::Shape shape) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &physics_shape_enum();
    slot.number = static_cast<int>(shape);
    return slot;
}

}  // namespace

LuaSlot PhysicsObject::mesh() const { return instance_reference_slot(mesh_ref_, "Mesh"); }

InstanceId PhysicsObject::mesh_id() const {
    const LuaSlot slot = mesh();
    return slot.kind == LuaSlot::Kind::Instance ? slot.id : 0;
}

std::optional<std::string> PhysicsObject::set_angular_velocity(Vec3 velocity) {
    return set_vec("AngularVelocity", angular_velocity_, velocity, kDirtyVelocity);
}

std::optional<std::string> PhysicsObject::set_size(Vec3 size) {
    if (finite(size)) {
        size = {std::max(size.x, kMinSize), std::max(size.y, kMinSize), std::max(size.z, kMinSize)};
    }
    return set_vec("Size", size_, size, kDirtyShape);
}

std::optional<std::string> PhysicsObject::set_friction(double friction) {
    return set_number("Friction", friction_, std::isfinite(friction) ? std::max(friction, 0.0) : friction,
                      kDirtyMaterial);
}

std::optional<std::string> PhysicsObject::set_bounciness(double bounciness) {
    return set_number("Bounciness", bounciness_, std::isfinite(bounciness) ? std::max(bounciness, 0.0) : bounciness,
                      kDirtyMaterial);
}

std::optional<std::string> PhysicsObject::set_angular_damping(double damping) {
    return set_number("AngularDamping", angular_damping_, std::isfinite(damping) ? std::max(damping, 0.0) : damping,
                      kDirtyDamping);
}

std::optional<std::string> PhysicsObject::set_shape(int shape) {
    require_gameplay_thread(*this);
    if (enum_item_name(physics_shape_enum(), shape) == nullptr) {
        return std::string("Shape must be an Enum.PhysicsShape");
    }
    const Shape next = static_cast<Shape>(shape);
    if (next == shape_) {
        return std::nullopt;
    }
    const Shape previous = shape_;
    shape_ = next;
    mark_dirty(kDirtyShape);
    note_property_change("Shape", shape_slot(previous), shape_slot(next));
    // A Hull or a Custom is its Mesh at the Mesh's own size: Size, which it
    // does not show, goes back to 1.
    const bool meshed = next == Shape::Hull || next == Shape::Custom;
    if (meshed && (size_.x != 1.f || size_.y != 1.f || size_.z != 1.f)) {
        const Vec3 previous_size = size_;
        size_ = {1.f, 1.f, 1.f};
        note_property_change("Size", vec3_slot(previous_size), vec3_slot(size_));
    }
    return std::nullopt;
}

std::optional<std::string> PhysicsObject::set_mesh(const LuaSlot& value) {
    require_gameplay_thread(*this);
    std::optional<std::string> error = set_instance_reference("Mesh", "Mesh", mesh_ref_, value);
    if (!error) {
        mark_dirty(kDirtyShape);
        warned_hull = false;
    }
    return error;
}

void PhysicsObject::on_reuse() {
    PhysicsBase::on_reuse();
    angular_velocity_ = {};
    friction_ = kDefaultFriction;
    bounciness_ = 0.0;
    angular_damping_ = 0.0;
    shape_ = Shape::Box;
    size_ = {1.f, 1.f, 1.f};
    mesh_ref_.set_guid(std::string());
    warned_hull = false;
    warned_custom = false;
}

namespace {

bool read_mesh(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = dynamic_cast<const PhysicsObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = body->mesh();
    return true;
}

bool write_mesh(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = dynamic_cast<PhysicsObject*>(&object);
    return body != nullptr && refuse(in, body->set_mesh(in));
}

bool read_shape(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = dynamic_cast<const PhysicsObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = shape_slot(body->shape());
    return true;
}

bool write_shape(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = dynamic_cast<PhysicsObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &physics_shape_enum()) {
        in.error = "Shape must be an Enum.PhysicsShape";
        return false;
    }
    return refuse(in, body->set_shape(static_cast<int>(in.number)));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_physics_object_lua) {
    static const std::string friction = number_json(PhysicsObject::kDefaultFriction);
    const LuaField fields[] = {
        lua_saved_property("AngularVelocity", "Vector3", read_vec<PhysicsObject, &PhysicsObject::angular_velocity>,
                           write_vec<PhysicsObject, &PhysicsObject::set_angular_velocity>, "[0,0,0]"),
        lua_saved_property("Friction", "number", read_number<PhysicsObject, &PhysicsObject::friction>,
                           write_number<PhysicsObject, &PhysicsObject::set_friction>, friction.c_str()),
        lua_slider(lua_saved_property("Bounciness", "number", read_number<PhysicsObject, &PhysicsObject::bounciness>,
                                      write_number<PhysicsObject, &PhysicsObject::set_bounciness>, "0"),
                   0.0, 1.0),
        lua_saved_property("AngularDamping", "number", read_number<PhysicsObject, &PhysicsObject::angular_damping>,
                           write_number<PhysicsObject, &PhysicsObject::set_angular_damping>, "0"),
        lua_saved_enum("Shape", physics_shape_enum(), read_shape, write_shape, "\"Box\""),
        lua_shown_when(lua_saved_property("Size", "Vector3", read_vec<PhysicsObject, &PhysicsObject::size>,
                                          write_vec<PhysicsObject, &PhysicsObject::set_size>, "[1,1,1]"),
                       "Shape",
                       {static_cast<int>(PhysicsObject::Shape::Box), static_cast<int>(PhysicsObject::Shape::Sphere),
                        static_cast<int>(PhysicsObject::Shape::Capsule),
                        static_cast<int>(PhysicsObject::Shape::Cylinder), static_cast<int>(PhysicsObject::Shape::Cone),
                        static_cast<int>(PhysicsObject::Shape::Wedge)}),
        lua_shown_when(lua_saved_property("Mesh", "Mesh?", read_mesh, write_mesh, "null"), "Shape",
                       {static_cast<int>(PhysicsObject::Shape::Hull), static_cast<int>(PhysicsObject::Shape::Custom)}),
    };
    register_lua_class("PhysicsObject", "PhysicsBase", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("PhysicsObject", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
