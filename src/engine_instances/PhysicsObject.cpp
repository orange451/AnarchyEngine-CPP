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
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot vec3_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot matrix_slot(const Matrix4& value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Matrix4;
    slot.transform = value;
    return slot;
}

LuaSlot shape_slot(PhysicsObject::Shape shape) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &physics_shape_enum();
    slot.number = static_cast<int>(shape);
    return slot;
}

bool finite(Vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }

bool same_vec(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("PhysicsObject setters run on SimulationThread");
    }
}

}  // namespace

LuaSlot PhysicsObject::mesh() const { return instance_reference_slot(mesh_ref_, "Mesh"); }

LuaSlot PhysicsObject::game_object() const { return instance_reference_slot(game_object_ref_, "GameObject"); }

InstanceId PhysicsObject::mesh_id() const {
    const LuaSlot slot = mesh();
    return slot.kind == LuaSlot::Kind::Instance ? slot.id : 0;
}

InstanceId PhysicsObject::game_object_id() const {
    const LuaSlot slot = game_object();
    return slot.kind == LuaSlot::Kind::Instance ? slot.id : 0;
}

InstanceId PhysicsObject::driven_game_object() const {
    if (const InstanceId linked = game_object_id(); linked != 0) {
        return linked;
    }
    const InstanceId above = parent(id());
    return above != kNoParent && above != 0 && DataModel::game_object(above) != nullptr ? above : 0;
}

std::optional<std::string> PhysicsObject::set_transform(const Matrix4& transform) {
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
    dirty_ |= kDirtyPose;
    note_property_change("Transform", matrix_slot(previous), matrix_slot(transform));
    return std::nullopt;
}

std::optional<std::string> PhysicsObject::set_vec(const char* property, Vec3& slot, Vec3 value, std::uint32_t dirty) {
    require_thread(*this);
    if (!finite(value)) {
        return std::string(property) + " must be finite";
    }
    if (same_vec(slot, value)) {
        return std::nullopt;
    }
    const Vec3 previous = slot;
    slot = value;
    dirty_ |= dirty;
    note_property_change(property, vec3_slot(previous), vec3_slot(value));
    return std::nullopt;
}

std::optional<std::string> PhysicsObject::set_velocity(Vec3 velocity) {
    return set_vec("Velocity", velocity_, velocity, kDirtyVelocity);
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

void PhysicsObject::set_anchored(bool anchored) {
    require_thread(*this);
    if (anchored_ == anchored) {
        return;
    }
    anchored_ = anchored;
    dirty_ |= kDirtyType;
    note_property_change("Anchored", bool_slot(!anchored), bool_slot(anchored));
}

std::optional<std::string> PhysicsObject::set_number(const char* property, double& slot, double value,
                                                     std::uint32_t dirty) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    dirty_ |= dirty;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> PhysicsObject::set_mass(double mass) {
    return set_number("Mass", mass_, std::isfinite(mass) ? std::max(mass, kMinMass) : mass, kDirtyMass);
}

std::optional<std::string> PhysicsObject::set_friction(double friction) {
    return set_number("Friction", friction_, std::isfinite(friction) ? std::max(friction, 0.0) : friction,
                      kDirtyMaterial);
}

std::optional<std::string> PhysicsObject::set_bounciness(double bounciness) {
    return set_number("Bounciness", bounciness_, std::isfinite(bounciness) ? std::max(bounciness, 0.0) : bounciness,
                      kDirtyMaterial);
}

std::optional<std::string> PhysicsObject::set_linear_damping(double damping) {
    return set_number("LinearDamping", linear_damping_, std::isfinite(damping) ? std::max(damping, 0.0) : damping,
                      kDirtyDamping);
}

std::optional<std::string> PhysicsObject::set_angular_damping(double damping) {
    return set_number("AngularDamping", angular_damping_, std::isfinite(damping) ? std::max(damping, 0.0) : damping,
                      kDirtyDamping);
}

std::optional<std::string> PhysicsObject::set_shape(int shape) {
    require_thread(*this);
    if (enum_item_name(physics_shape_enum(), shape) == nullptr) {
        return std::string("Shape must be an Enum.PhysicsShape");
    }
    const Shape next = static_cast<Shape>(shape);
    if (next == shape_) {
        return std::nullopt;
    }
    const Shape previous = shape_;
    shape_ = next;
    dirty_ |= kDirtyShape;
    note_property_change("Shape", shape_slot(previous), shape_slot(next));
    return std::nullopt;
}

std::optional<std::string> PhysicsObject::set_mesh(const LuaSlot& value) {
    require_thread(*this);
    std::optional<std::string> error = set_instance_reference("Mesh", "Mesh", mesh_ref_, value);
    if (!error) {
        dirty_ |= kDirtyShape;
        warned_hull = false;
    }
    return error;
}

std::optional<std::string> PhysicsObject::set_game_object(const LuaSlot& value) {
    require_thread(*this);
    // The physics world sees a new target by comparing it with the one its body drives.
    return set_instance_reference("GameObject", "GameObject", game_object_ref_, value);
}

void PhysicsObject::store_simulated(const Matrix4& transform, Vec3 velocity, Vec3 angular_velocity) {
    transform_ = transform;
    velocity_ = velocity;
    angular_velocity_ = angular_velocity;
}

void PhysicsObject::on_reuse() {
    transform_ = matrix4_identity();
    velocity_ = {};
    angular_velocity_ = {};
    anchored_ = false;
    mass_ = kDefaultMass;
    friction_ = kDefaultFriction;
    bounciness_ = 0.0;
    linear_damping_ = 0.0;
    angular_damping_ = 0.0;
    shape_ = Shape::Box;
    size_ = {1.f, 1.f, 1.f};
    mesh_ref_.set_guid(std::string());
    game_object_ref_.set_guid(std::string());
    dirty_ = kDirtyAll;
    warned_shared = false;
    warned_hull = false;
    warned_custom = false;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

PhysicsObject* body_of(DataModel& object) { return dynamic_cast<PhysicsObject*>(&object); }

template <double (PhysicsObject::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = body_of(object);
    if (body == nullptr) {
        return false;
    }
    out = number_slot((body->*Get)());
    return true;
}

template <std::optional<std::string> (PhysicsObject::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = body_of(object);
    return body != nullptr && refuse(in, (body->*Set)(in.number));
}

template <Vec3 (PhysicsObject::*Get)() const>
bool read_vec(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = body_of(object);
    if (body == nullptr) {
        return false;
    }
    out = vec3_slot((body->*Get)());
    return true;
}

template <std::optional<std::string> (PhysicsObject::*Set)(Vec3)>
bool write_vec(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = body_of(object);
    return body != nullptr && refuse(in, (body->*Set)(in.vec));
}

template <LuaSlot (PhysicsObject::*Get)() const>
bool read_reference(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = body_of(object);
    if (body == nullptr) {
        return false;
    }
    out = (body->*Get)();
    return true;
}

template <std::optional<std::string> (PhysicsObject::*Set)(const LuaSlot&)>
bool write_reference(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = body_of(object);
    return body != nullptr && refuse(in, (body->*Set)(in));
}

bool read_transform(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = body_of(object);
    if (body == nullptr) {
        return false;
    }
    out = matrix_slot(body->transform());
    return true;
}

bool write_transform(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = body_of(object);
    return body != nullptr && refuse(in, body->set_transform(in.transform));
}

bool read_anchored(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = body_of(object);
    if (body == nullptr) {
        return false;
    }
    out = bool_slot(body->anchored());
    return true;
}

bool write_anchored(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = body_of(object);
    if (body == nullptr) {
        return false;
    }
    body->set_anchored(in.flag);
    return true;
}

bool read_shape(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = body_of(object);
    if (body == nullptr) {
        return false;
    }
    out = shape_slot(body->shape());
    return true;
}

bool write_shape(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = body_of(object);
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
    static const std::string mass = number_json(PhysicsObject::kDefaultMass);
    static const std::string friction = number_json(PhysicsObject::kDefaultFriction);
    static const std::string identity = [] {
        const Matrix4 value = matrix4_identity();
        return write_json(json_floats(value.m, 16));
    }();
    const LuaField fields[] = {
        lua_saved_property("Transform", "Matrix4", read_transform, write_transform, identity.c_str()),
        lua_saved_property("Velocity", "Vector3", read_vec<&PhysicsObject::velocity>,
                           write_vec<&PhysicsObject::set_velocity>, "[0,0,0]"),
        lua_saved_property("AngularVelocity", "Vector3", read_vec<&PhysicsObject::angular_velocity>,
                           write_vec<&PhysicsObject::set_angular_velocity>, "[0,0,0]"),
        lua_saved_property("Anchored", "boolean", read_anchored, write_anchored, "false"),
        lua_saved_property("Mass", "number", read_number<&PhysicsObject::mass>, write_number<&PhysicsObject::set_mass>,
                           mass.c_str()),
        lua_saved_property("Friction", "number", read_number<&PhysicsObject::friction>,
                           write_number<&PhysicsObject::set_friction>, friction.c_str()),
        lua_slider(lua_saved_property("Bounciness", "number", read_number<&PhysicsObject::bounciness>,
                                      write_number<&PhysicsObject::set_bounciness>, "0"),
                   0.0, 1.0),
        lua_saved_property("LinearDamping", "number", read_number<&PhysicsObject::linear_damping>,
                           write_number<&PhysicsObject::set_linear_damping>, "0"),
        lua_saved_property("AngularDamping", "number", read_number<&PhysicsObject::angular_damping>,
                           write_number<&PhysicsObject::set_angular_damping>, "0"),
        lua_saved_enum("Shape", physics_shape_enum(), read_shape, write_shape, "\"Box\""),
        lua_saved_property("Size", "Vector3", read_vec<&PhysicsObject::size>, write_vec<&PhysicsObject::set_size>,
                           "[1,1,1]"),
        lua_shown_when(lua_saved_property("Mesh", "Mesh?", read_reference<&PhysicsObject::mesh>,
                                          write_reference<&PhysicsObject::set_mesh>, "null"),
                       "Shape",
                       {static_cast<int>(PhysicsObject::Shape::Hull), static_cast<int>(PhysicsObject::Shape::Custom)}),
        lua_saved_property("GameObject", "GameObject?", read_reference<&PhysicsObject::game_object>,
                           write_reference<&PhysicsObject::set_game_object>, "null"),
    };
    register_lua_class("PhysicsObject", "PVInstance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("PhysicsObject", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
