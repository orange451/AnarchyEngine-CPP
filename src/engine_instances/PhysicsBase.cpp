#include "PhysicsBase.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace engine_core {

namespace physics_detail {

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

void require_gameplay_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("Physics setters run on SimulationThread");
    }
}

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

}  // namespace physics_detail

using namespace physics_detail;

namespace {

bool same_vec(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

}  // namespace

LuaSlot PhysicsBase::game_object() const { return instance_reference_slot(game_object_ref_, "GameObject"); }

InstanceId PhysicsBase::game_object_id() const {
    const LuaSlot slot = game_object();
    return slot.kind == LuaSlot::Kind::Instance ? slot.id : 0;
}

InstanceId PhysicsBase::driven_game_object() const {
    if (const InstanceId linked = game_object_id(); linked != 0) {
        return linked;
    }
    const InstanceId above = parent(id());
    return above != kNoParent && above != 0 && DataModel::game_object(above) != nullptr ? above : 0;
}

std::optional<std::string> PhysicsBase::set_transform(const Matrix4& transform) {
    require_gameplay_thread(*this);
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

std::optional<std::string> PhysicsBase::set_vec(const char* property, Vec3& slot, Vec3 value, std::uint32_t dirty) {
    require_gameplay_thread(*this);
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

std::optional<std::string> PhysicsBase::set_velocity(Vec3 velocity) {
    return set_vec("Velocity", velocity_, velocity, kDirtyVelocity);
}

void PhysicsBase::set_anchored(bool anchored) {
    require_gameplay_thread(*this);
    if (anchored_ == anchored) {
        return;
    }
    anchored_ = anchored;
    dirty_ |= kDirtyType;
    note_property_change("Anchored", bool_slot(!anchored), bool_slot(anchored));
}

std::optional<std::string> PhysicsBase::set_number(const char* property, double& slot, double value,
                                                   std::uint32_t dirty) {
    require_gameplay_thread(*this);
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

std::optional<std::string> PhysicsBase::set_mass(double mass) {
    return set_number("Mass", mass_, std::isfinite(mass) ? std::max(mass, kMinMass) : mass, kDirtyMass);
}

std::optional<std::string> PhysicsBase::set_linear_damping(double damping) {
    return set_number("LinearDamping", linear_damping_, std::isfinite(damping) ? std::max(damping, 0.0) : damping,
                      kDirtyDamping);
}

std::optional<std::string> PhysicsBase::set_game_object(const LuaSlot& value) {
    require_gameplay_thread(*this);
    // The physics world sees a new target by comparing it with the one its body drives.
    return set_instance_reference("GameObject", "GameObject", game_object_ref_, value);
}

void PhysicsBase::store_simulated(const Matrix4& transform, Vec3 velocity) {
    transform_ = transform;
    velocity_ = velocity;
}

void PhysicsBase::on_reuse() {
    transform_ = matrix4_identity();
    velocity_ = {};
    anchored_ = false;
    mass_ = kDefaultMass;
    linear_damping_ = 0.0;
    game_object_ref_.set_guid(std::string());
    dirty_ = kDirtyAll;
    warned_shared = false;
}

namespace {

PhysicsBase* base_of(DataModel& object) { return dynamic_cast<PhysicsBase*>(&object); }

bool read_transform(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    out = matrix_slot(body->transform());
    return true;
}

bool write_transform(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsBase* body = base_of(object);
    return body != nullptr && refuse(in, body->set_transform(in.transform));
}

bool read_anchored(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    out = bool_slot(body->anchored());
    return true;
}

bool write_anchored(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    body->set_anchored(in.flag);
    return true;
}

bool read_game_object(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    out = body->game_object();
    return true;
}

bool write_game_object(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsBase* body = base_of(object);
    return body != nullptr && refuse(in, body->set_game_object(in));
}

ANARCHY_LUA_REGISTER(register_physics_base_lua) {
    static const std::string mass = write_json(JsonValue::number(PhysicsBase::kDefaultMass));
    static const std::string identity = [] {
        const Matrix4 value = matrix4_identity();
        return write_json(json_floats(value.m, 16));
    }();
    const LuaField fields[] = {
        lua_saved_property("Transform", "Matrix4", read_transform, write_transform, identity.c_str()),
        lua_saved_property("Velocity", "Vector3", read_vec<PhysicsBase, &PhysicsBase::velocity>,
                           write_vec<PhysicsBase, &PhysicsBase::set_velocity>, "[0,0,0]"),
        lua_saved_property("Anchored", "boolean", read_anchored, write_anchored, "false"),
        lua_slider(lua_saved_property("Mass", "number", read_number<PhysicsBase, &PhysicsBase::mass>,
                                      write_number<PhysicsBase, &PhysicsBase::set_mass>, mass.c_str()),
                   0.0, 100.0),
        lua_slider(lua_saved_property("LinearDamping", "number", read_number<PhysicsBase, &PhysicsBase::linear_damping>,
                                      write_number<PhysicsBase, &PhysicsBase::set_linear_damping>, "0"),
                   0.0, 1.0),
        lua_saved_property("GameObject", "GameObject?", read_game_object, write_game_object, "null"),
    };
    // Abstract: no factory registers it.
    register_lua_class("PhysicsBase", "PVInstance", fields, static_cast<int>(std::size(fields)));
}

}  // namespace

}  // namespace engine_core
