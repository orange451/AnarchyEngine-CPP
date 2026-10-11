#pragma once

#include "Constraint.hpp"
#include "JointFrame.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace engine_core {

// Every property a joint may have (spec
// docs/superpowers/specs/2026-10-11-joint-constraints-design.md §1). A name is
// one slot whichever class shows it: Hertz is a Revolute's and a Prismatic's,
// with one rule and one default. Each class registers only its own. Keep in
// step with kParams in JointConstraint.cpp.
enum class JointParam : int {
    CollideConnected,
    EnableSpring,
    Hertz,
    DampingRatio,
    TargetAngle,
    TargetTranslation,
    EnableLimit,
    LowerAngle,
    UpperAngle,
    LowerTranslation,
    UpperTranslation,
    EnableMotor,
    MotorSpeed,
    MaxMotorTorque,
    MaxMotorForce,
    Length,
    MinLength,
    MaxLength,
    EnableConeLimit,
    ConeAngle,
    EnableTwistLimit,
    LowerTwistAngle,
    UpperTwistAngle,
    EnableSuspension,
    SuspensionHertz,
    SuspensionDampingRatio,
    EnableSuspensionLimit,
    LowerSuspensionLimit,
    UpperSuspensionLimit,
    EnableSpinMotor,
    SpinMotorSpeed,
    MaxSpinTorque,
    EnableSteering,
    SteeringHertz,
    SteeringDampingRatio,
    TargetSteeringAngle,
    MaxSteeringTorque,
    EnableSteeringLimit,
    LowerSteeringLimit,
    UpperSteeringLimit,
    Count
};
constexpr int kJointParamCount = static_cast<int>(JointParam::Count);

// What a value may be (spec §4).
enum class JointRule {
    Flag,         // a boolean, kept as 0 or 1
    Any,          // any finite number
    NonNegative,  // refused below 0
    Hinge,        // degrees, clamped to ±178.2 (Box3D's ±0.99π)
    Cone,         // degrees, clamped to 0..90 (Box3D's cone is at most π/2)
    Angle,        // degrees, wrapped into (-180, 180] (Box3D's targets are within ±π)
};

struct JointParamInfo {
    const char* name;
    JointRule rule;
    double default_value;
};

const JointParamInfo& joint_param_info(JointParam param);

// A Constraint that links two assemblies with a moving Box3D joint (spec §1,
// §2). Abstract: the Lua class JointConstraint cannot be made; its five
// subclasses can. It joins nothing, so each end stays in its own assembly.
// PVInstance1 is the base (Box3D's body A), PVInstance2 the moving part; the
// joint sits at PVInstance2's pivot and turns or slides along its Y.
//
// CollideConnected  boolean  false. Saved. Whether the two ends collide.
//
// Each subclass shows its own saved JointParams, and every one but a
// SphericalConstraint one read-only reading, which PhysicsWorld sets after
// each step in play and which reads 0 while the place is stopped.
class JointConstraint : public Constraint {
public:
    JointConstraint(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    bool rigid() const override { return false; }
    bool joins() const override { return false; }
    bool joint() const override { return true; }
    virtual JointKind kind() const = 0;

    double param(JointParam p) const { return params_[static_cast<std::size_t>(p)]; }
    bool flag(JointParam p) const { return param(p) != 0.0; }
    // A SphericalConstraint's MotorVelocity: degrees per second about each world axis.
    Vec3 motor_velocity() const { return motor_velocity_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing (spec §4). A Flag takes any non-zero as 1.
    std::optional<std::string> set_param(JointParam p, double value);
    std::optional<std::string> set_motor_velocity(Vec3 velocity);

    // Moves on every change to a value above: PhysicsWorld gives Box3D the
    // settings again.
    std::uint32_t settings_revision() const { return settings_revision_; }
    // Moves on every change to an end or Enabled: PhysicsWorld takes the
    // joint's frame again from where things are then (spec §2, Capture).
    std::uint32_t capture_revision() const { return capture_revision_; }

    // The read-only value (Angle, Translation, CurrentLength, or SpinSpeed), in
    // its property's units. PhysicsWorld sets it after each step, as a plain
    // field: no Changed, no history.
    double reading() const { return reading_; }
    void set_reading(double value) { reading_ = value; }

protected:
    void on_reuse() override;
    void link_changed() override { ++capture_revision_; }

private:
    void reset_values();

    std::array<double, kJointParamCount> params_{};
    Vec3 motor_velocity_{};
    double reading_ = 0.0;
    std::uint32_t settings_revision_ = 0;
    std::uint32_t capture_revision_ = 0;
};

// Body B turns about the axis (b3RevoluteJoint).
class RevoluteConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "RevoluteConstraint"; }
    JointKind kind() const override { return JointKind::Revolute; }
};

// Body B slides along the axis and does not turn (b3PrismaticJoint).
class PrismaticConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "PrismaticConstraint"; }
    JointKind kind() const override { return JointKind::Prismatic; }
};

// Holds the distance between the two ends' pivots: a rod, a spring, or a
// rope (b3DistanceJoint).
class DistanceConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "DistanceConstraint"; }
    JointKind kind() const override { return JointKind::Distance; }
};

// Body B turns freely about the pivot (b3SphericalJoint).
class SphericalConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "SphericalConstraint"; }
    JointKind kind() const override { return JointKind::Spherical; }
};

// A car wheel: PVInstance1 the chassis, PVInstance2 the wheel, spinning about
// its own Y, on a suspension along the chassis's up, steering about it
// (b3WheelJoint).
class WheelConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "WheelConstraint"; }
    JointKind kind() const override { return JointKind::Wheel; }
};

}  // namespace engine_core
