#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"
#include "Matrix4.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace engine_core {

// One rigid body. While it is in Workspace, at any depth, and the place is
// playing, the physics world (PhysicsWorld) simulates it; anywhere else it
// is only data. It is not a GameObject: it has a Transform but draws nothing.
// When GameObject names one, or else when its parent is a GameObject, the
// body starts at that GameObject's Transform and moves it. Of several
// PhysicsObjects moving one GameObject, only the first in tree order gets a
// body.
//
// Transform        Matrix4   where the body is. Identity.
// Velocity         Vector3   world units per second. (0, 0, 0).
// AngularVelocity  Vector3   world space, radians per second. (0, 0, 0).
// Anchored         boolean   a static body, which nothing moves. false.
// Mass             number    1. Below kMinMass, as 0 is, is taken as kMinMass.
// Friction         number    0.6, not below 0.
// Bounciness       number    restitution: 0, not below 0; the slider runs to 1.
// LinearDamping    number    0, not below 0.
// AngularDamping   number    0, not below 0.
// Shape            Enum.PhysicsShape  Box.
// Size             Vector3   (1, 1, 1); each axis at least kMinSize. Box: its
//                            extents. Sphere: diameter X. Capsule: diameter X,
//                            height Y, along Y. Hull and Custom: the mesh
//                            fits it.
// Mesh             Mesh?     a Hull's points, or a Custom's triangles: the
//                            whole mesh, which collides only while Anchored
//                            (Box3D gives a mesh contacts only on a static
//                            body); unanchored, a Custom is a Hull of it.
//                            Shown only for a Hull or a Custom.
// GameObject       GameObject?  what the body moves. Nil: the parent, if it
//                               is a GameObject.
//
// Each is a saved registry property, so DataModel saves, loads, undoes, and
// restores it at Stop. A value that is not finite is refused. Writes made by
// the physics world itself go through store_simulated and are none of those.
class PhysicsObject : public DataModel {
public:
    enum class Shape { Box = 0, Sphere = 1, Capsule = 2, Hull = 3, Custom = 4 };

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
    static constexpr double kDefaultFriction = 0.6;
    static constexpr float kMinSize = 0.01f;

    PhysicsObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const char* class_name() const override { return "PhysicsObject"; }
    bool physics_body() const override { return true; }

    const Matrix4& transform() const { return transform_; }
    Vec3 velocity() const { return velocity_; }
    Vec3 angular_velocity() const { return angular_velocity_; }
    bool anchored() const { return anchored_; }
    double mass() const { return mass_; }
    double friction() const { return friction_; }
    double bounciness() const { return bounciness_; }
    double linear_damping() const { return linear_damping_; }
    double angular_damping() const { return angular_damping_; }
    Shape shape() const { return shape_; }
    Vec3 size() const { return size_; }

    LuaSlot mesh() const;
    LuaSlot game_object() const;
    // The live targets, or 0.
    InstanceId mesh_id() const;
    InstanceId game_object_id() const;
    // The GameObject the body moves: GameObject when it names one, else the
    // parent when that is a GameObject, else 0. The parent link is never
    // written to GameObject, so a PhysicsObject moved out from under it
    // stops moving it.
    InstanceId driven_game_object() const;

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_transform(const Matrix4& transform);
    std::optional<std::string> set_velocity(Vec3 velocity);
    std::optional<std::string> set_angular_velocity(Vec3 velocity);
    void set_anchored(bool anchored);
    std::optional<std::string> set_mass(double mass);
    std::optional<std::string> set_friction(double friction);
    std::optional<std::string> set_bounciness(double bounciness);
    std::optional<std::string> set_linear_damping(double damping);
    std::optional<std::string> set_angular_damping(double damping);
    std::optional<std::string> set_shape(int shape);
    std::optional<std::string> set_size(Vec3 size);
    std::optional<std::string> set_mesh(const LuaSlot& value);
    std::optional<std::string> set_game_object(const LuaSlot& value);

    // The physics world's side. take_dirty returns what writes changed since
    // the last call and clears it. store_simulated keeps what the body did,
    // with no Changed, no history, and no dirty mark.
    std::uint32_t take_dirty() {
        const std::uint32_t out = dirty_;
        dirty_ = 0;
        return out;
    }
    void store_simulated(const Matrix4& transform, Vec3 velocity, Vec3 angular_velocity);

    // One warning each, until the condition clears: this one lost its
    // GameObject to an earlier PhysicsObject, or its Hull fell back to a Box.
    bool warned_shared = false;
    bool warned_hull = false;
    // Once, until it is reused: this unanchored Custom collides as a Hull.
    bool warned_custom = false;

protected:
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value, std::uint32_t dirty);
    std::optional<std::string> set_vec(const char* property, Vec3& slot, Vec3 value, std::uint32_t dirty);

    Matrix4 transform_ = matrix4_identity();
    Vec3 velocity_{};
    Vec3 angular_velocity_{};
    bool anchored_ = false;
    double mass_ = kDefaultMass;
    double friction_ = kDefaultFriction;
    double bounciness_ = 0.0;
    double linear_damping_ = 0.0;
    double angular_damping_ = 0.0;
    Shape shape_ = Shape::Box;
    Vec3 size_{1.f, 1.f, 1.f};
    InstanceRef mesh_ref_;
    InstanceRef game_object_ref_;
    std::uint32_t dirty_ = kDirtyAll;
};

}  // namespace engine_core
