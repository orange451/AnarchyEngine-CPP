#pragma once

#include "PhysicsBase.hpp"

namespace engine_core {

// A rigid body of any Shape: a PhysicsBase (which see for Transform,
// Velocity, Anchored, Mass, LinearDamping, and GameObject) with these too.
//
// AngularVelocity  Vector3   world space, radians per second. (0, 0, 0).
// Friction         number    0.6, not below 0.
// Bounciness       number    restitution: 0, not below 0; the slider runs to 1.
// AngularDamping   number    0, not below 0.
// Shape            Enum.PhysicsShape  Box.
// Size             Vector3   (1, 1, 1); each axis at least kMinSize. The body
//                            is made at Size times, per axis, the scale of the
//                            GameObject it moves: its Transform's axes and its
//                            Scale (PhysicsWorld::shape_scale). Size itself
//                            stays as written. Box: its
//                            extents. Sphere: diameter X. Capsule: diameter X,
//                            height Y, along Y. Cylinder: diameter X,
//                            height Y, along Y. Cone: base diameter X at
//                            the bottom, height Y to its tip. Wedge: Size's
//                            box halved by a slope from its bottom front
//                            (-Z) edge up to its top back (+Z) edge. Hull
//                            and Custom: unused and not shown; the Mesh is
//                            taken at its own size. Shape becoming one of
//                            them puts Size back to (1, 1, 1), the Box a Hull
//                            with no Mesh falls back to.
// Mesh             Mesh?     a Hull's points, or a Custom's triangles: the
//                            whole mesh, which collides only while Anchored
//                            (Box3D gives a mesh contacts only on a static
//                            body); unanchored, a Custom is convex pieces of it.
//                            Shown only for a Hull or a Custom.
class PhysicsObject : public PhysicsBase {
public:
    enum class Shape { Box = 0, Sphere = 1, Capsule = 2, Hull = 3, Custom = 4, Cylinder = 5, Cone = 6, Wedge = 7 };

    static constexpr double kDefaultFriction = 0.6;

    PhysicsObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PhysicsBase(tag, state, id) {}

    const char* class_name() const override { return "PhysicsObject"; }

    Vec3 angular_velocity() const { return angular_velocity_; }
    double friction() const { return friction_; }
    double bounciness() const { return bounciness_; }
    double angular_damping() const { return angular_damping_; }
    Shape shape() const { return shape_; }
    Vec3 size() const { return size_; }

    LuaSlot mesh() const;
    // The live target, or 0.
    InstanceId mesh_id() const;

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_angular_velocity(Vec3 velocity);
    std::optional<std::string> set_friction(double friction);
    std::optional<std::string> set_bounciness(double bounciness);
    std::optional<std::string> set_angular_damping(double damping);
    std::optional<std::string> set_shape(int shape);
    std::optional<std::string> set_size(Vec3 size);
    std::optional<std::string> set_mesh(const LuaSlot& value);

    // The physics world's side, beside store_simulated: what the body's spin
    // is now, with no Changed, no history, and no dirty mark.
    void store_angular_velocity(Vec3 velocity) { angular_velocity_ = velocity; }

    // One warning each, until the condition clears: its Hull fell back to a Box.
    bool warned_hull = false;
    // Once, until the condition clears or it is reused: the "Custom fell back
    // to Hull" warning.
    bool warned_custom = false;

protected:
    void on_reuse() override;

private:
    Vec3 angular_velocity_{};
    double friction_ = kDefaultFriction;
    double bounciness_ = 0.0;
    double angular_damping_ = 0.0;
    Shape shape_ = Shape::Box;
    Vec3 size_{1.f, 1.f, 1.f};
    InstanceRef mesh_ref_;
};

}  // namespace engine_core
