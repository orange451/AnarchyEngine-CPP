#pragma once

#include "PVInstance.hpp"
#include "InstanceRef.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace engine_core {

// What every rigid body shares: PhysicsObject and PlayerController. While it
// is in Workspace, at any depth, and the place is playing, the physics world
// (PhysicsWorld) simulates it; anywhere else it is only data. It is not a
// GameObject: it has a Transform but draws nothing. When GameObject names
// one, or else when its parent is a GameObject, the body starts at that
// GameObject's Transform and moves it; stopped, its Transform follows that
// GameObject's position and rotation (PhysicsWorld::follow_game_objects). Of
// several bodies moving one GameObject, only the first in tree order gets
// one. Abstract: the Lua class PhysicsBase cannot be made, and
// IsA("PhysicsBase") is true of both.
//
// Transform        Matrix4   where the body is. Identity.
// Velocity         Vector3   world units per second. (0, 0, 0).
// Anchored         boolean   a static body, which nothing moves. false.
// Mass             number    1. Below kMinMass, as 0 is, is taken as kMinMass.
// LinearDamping    number    0, not below 0.
// GameObject       GameObject?  what the body moves. Nil: the parent, if it
//                               is a GameObject.
//
// Each is a saved registry property, so DataModel saves, loads, undoes, and
// restores it at Stop. A value that is not finite is refused. Writes made by
// the physics world itself go through store_simulated and are none of those.
class PhysicsBase : public PVInstance {
public:
    // What a write changed, for the physics world to push into the body.
    enum Dirty : std::uint32_t {
        kDirtyPose = 1u << 0,
        kDirtyVelocity = 1u << 1,
        kDirtyMaterial = 1u << 2,
        kDirtyShape = 1u << 3,
        kDirtyMass = 1u << 4,
        kDirtyType = 1u << 5,
        kDirtyDamping = 1u << 6,
        kDirtyAll = 0x7fu,
    };

    static constexpr double kDefaultMass = 1.0;
    static constexpr double kMinMass = 0.001;
    static constexpr float kMinSize = 0.01f;

    PhysicsBase(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PVInstance(tag, state, id) {}

    bool physics_body() const override { return true; }

    Matrix4 transform() const override { return transform_; }
    Vec3 velocity() const { return velocity_; }
    bool anchored() const { return anchored_; }
    double mass() const { return mass_; }
    double linear_damping() const { return linear_damping_; }

    LuaSlot game_object() const;
    // The live target, or 0.
    InstanceId game_object_id() const;
    // The GameObject the body moves: GameObject when it names one, else the
    // parent when that is a GameObject, else 0. The parent link is never
    // written to GameObject, so a body moved out from under it stops moving it.
    InstanceId driven_game_object() const;

    // SimulationThread. Each returns why it refused the value, changing nothing.
    virtual std::optional<std::string> set_transform(const Matrix4& transform);
    std::optional<std::string> set_pv_transform(const Matrix4& transform) override { return set_transform(transform); }
    std::optional<std::string> set_velocity(Vec3 velocity);
    void set_anchored(bool anchored);
    std::optional<std::string> set_mass(double mass);
    std::optional<std::string> set_linear_damping(double damping);
    std::optional<std::string> set_game_object(const LuaSlot& value);

    // The physics world's side. take_dirty returns what writes changed since
    // the last call and clears it. store_simulated keeps what the body did,
    // with no Changed, no history, and no dirty mark.
    std::uint32_t take_dirty() {
        const std::uint32_t out = dirty_;
        dirty_ = 0;
        return out;
    }
    void store_simulated(const Matrix4& transform, Vec3 velocity);

    // One warning, until the condition clears: this one lost its GameObject
    // to an earlier body.
    bool warned_shared = false;

protected:
    void on_reuse() override;

    std::optional<std::string> set_number(const char* property, double& slot, double value, std::uint32_t dirty);
    std::optional<std::string> set_vec(const char* property, Vec3& slot, Vec3 value, std::uint32_t dirty);
    void mark_dirty(std::uint32_t dirty) { dirty_ |= dirty; }

private:
    Matrix4 transform_ = matrix4_identity();
    Vec3 velocity_{};
    bool anchored_ = false;
    double mass_ = kDefaultMass;
    double linear_damping_ = 0.0;
    InstanceRef game_object_ref_;
    std::uint32_t dirty_ = kDirtyAll;
};

// The Lua slot helpers PhysicsBase, PhysicsObject, and PlayerController
// register their fields with.
namespace physics_detail {

LuaSlot number_slot(double value);
LuaSlot vec3_slot(Vec3 value);
LuaSlot bool_slot(bool value);
LuaSlot matrix_slot(const Matrix4& value);
inline bool finite(Vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
// Contract-fails off SimulationThread.
void require_gameplay_thread(const DataModel& object);
// Puts error into in and returns false, or returns true when there is none.
bool refuse(LuaSlot& in, std::optional<std::string> error);

template <class T, double (T::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const T* body = dynamic_cast<const T*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = number_slot((body->*Get)());
    return true;
}

template <class T, std::optional<std::string> (T::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    T* body = dynamic_cast<T*>(&object);
    return body != nullptr && refuse(in, (body->*Set)(in.number));
}

template <class T, Vec3 (T::*Get)() const>
bool read_vec(DataModel&, DataModel& object, LuaSlot& out) {
    const T* body = dynamic_cast<const T*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = vec3_slot((body->*Get)());
    return true;
}

template <class T, std::optional<std::string> (T::*Set)(Vec3)>
bool write_vec(DataModel&, DataModel& object, LuaSlot& in) {
    T* body = dynamic_cast<T*>(&object);
    return body != nullptr && refuse(in, (body->*Set)(in.vec));
}

}  // namespace physics_detail

}  // namespace engine_core
