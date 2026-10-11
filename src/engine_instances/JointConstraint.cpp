#include "JointConstraint.hpp"

#include "Containment.hpp"
#include "Contract.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>

namespace engine_core {
namespace {

constexpr double kHingeLimit = 178.2;
constexpr double kConeLimit = 90.0;

using R = JointRule;

// By JointParam, in its order.
const JointParamInfo kParams[] = {
    {"CollideConnected", R::Flag, 0.0},
    {"EnableSpring", R::Flag, 0.0},
    {"Hertz", R::NonNegative, 1.0},
    {"DampingRatio", R::NonNegative, 0.7},
    {"TargetAngle", R::Any, 0.0},
    {"TargetTranslation", R::Any, 0.0},
    {"EnableLimit", R::Flag, 0.0},
    {"LowerAngle", R::Hinge, -45.0},
    {"UpperAngle", R::Hinge, 45.0},
    {"LowerTranslation", R::Any, -1.0},
    {"UpperTranslation", R::Any, 1.0},
    {"EnableMotor", R::Flag, 0.0},
    {"MotorSpeed", R::Any, 0.0},
    {"MaxMotorTorque", R::NonNegative, 10000.0},
    {"MaxMotorForce", R::NonNegative, 10000.0},
    {"Length", R::NonNegative, 0.0},
    {"MinLength", R::NonNegative, 0.0},
    {"MaxLength", R::NonNegative, 10.0},
    {"EnableConeLimit", R::Flag, 0.0},
    {"ConeAngle", R::Cone, 45.0},
    {"EnableTwistLimit", R::Flag, 0.0},
    {"LowerTwistAngle", R::Hinge, -45.0},
    {"UpperTwistAngle", R::Hinge, 45.0},
    {"EnableSuspension", R::Flag, 1.0},
    {"SuspensionHertz", R::NonNegative, 4.0},
    {"SuspensionDampingRatio", R::NonNegative, 0.7},
    {"EnableSuspensionLimit", R::Flag, 0.0},
    {"LowerSuspensionLimit", R::Any, -0.5},
    {"UpperSuspensionLimit", R::Any, 0.5},
    {"EnableSpinMotor", R::Flag, 0.0},
    {"SpinMotorSpeed", R::Any, 0.0},
    {"MaxSpinTorque", R::NonNegative, 10000.0},
    {"EnableSteering", R::Flag, 0.0},
    {"SteeringHertz", R::NonNegative, 5.0},
    {"SteeringDampingRatio", R::NonNegative, 0.7},
    {"TargetSteeringAngle", R::Any, 0.0},
    {"MaxSteeringTorque", R::NonNegative, 10000.0},
    {"EnableSteeringLimit", R::Flag, 0.0},
    {"LowerSteeringLimit", R::Any, -30.0},
    {"UpperSteeringLimit", R::Any, 30.0},
};
static_assert(std::size(kParams) == static_cast<std::size_t>(kJointParamCount), "kParams lists every JointParam");

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot vec3_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

LuaSlot param_slot(const JointParamInfo& info, double value) {
    return info.rule == R::Flag ? bool_slot(value != 0.0) : number_slot(value);
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("JointConstraint setters run on SimulationThread");
    }
}

}  // namespace

const JointParamInfo& joint_param_info(JointParam param) { return kParams[static_cast<std::size_t>(param)]; }

JointConstraint::JointConstraint(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
    : Constraint(tag, state, id) {
    reset_values();
}

void JointConstraint::reset_values() {
    for (int i = 0; i < kJointParamCount; ++i) {
        params_[static_cast<std::size_t>(i)] = kParams[i].default_value;
    }
    motor_velocity_ = Vec3{};
    reading_ = 0.0;
    ++settings_revision_;
    ++capture_revision_;
}

void JointConstraint::on_reuse() {
    Constraint::on_reuse();
    reset_values();
}

std::optional<std::string> JointConstraint::set_param(JointParam p, double value) {
    require_thread(*this);
    const JointParamInfo& info = joint_param_info(p);
    if (!std::isfinite(value)) {
        return std::string(info.name) + " must be a finite number";
    }
    switch (info.rule) {
        case R::Flag:
            value = value != 0.0 ? 1.0 : 0.0;
            break;
        case R::NonNegative:
            if (value < 0.0) {
                return std::string(info.name) + " must not be negative";
            }
            break;
        case R::Hinge:
            value = std::clamp(value, -kHingeLimit, kHingeLimit);
            break;
        case R::Cone:
            value = std::clamp(value, 0.0, kConeLimit);
            break;
        case R::Any:
            break;
    }
    double& slot = params_[static_cast<std::size_t>(p)];
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    ++settings_revision_;
    note_property_change(info.name, param_slot(info, previous), param_slot(info, value));
    return std::nullopt;
}

std::optional<std::string> JointConstraint::set_motor_velocity(Vec3 velocity) {
    require_thread(*this);
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z)) {
        return std::string("MotorVelocity must be finite");
    }
    if (velocity.x == motor_velocity_.x && velocity.y == motor_velocity_.y && velocity.z == motor_velocity_.z) {
        return std::nullopt;
    }
    const Vec3 previous = motor_velocity_;
    motor_velocity_ = velocity;
    ++settings_revision_;
    note_property_change("MotorVelocity", vec3_slot(previous), vec3_slot(velocity));
    return std::nullopt;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

template <JointParam P>
bool read_param(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* joint = dynamic_cast<const JointConstraint*>(&object);
    if (joint == nullptr) {
        return false;
    }
    out = param_slot(joint_param_info(P), joint->param(P));
    return true;
}

template <JointParam P>
bool write_param(DataModel&, DataModel& object, LuaSlot& in) {
    auto* joint = dynamic_cast<JointConstraint*>(&object);
    if (joint == nullptr) {
        return false;
    }
    const double value = joint_param_info(P).rule == R::Flag ? (in.flag ? 1.0 : 0.0) : in.number;
    return refuse(in, joint->set_param(P, value));
}

bool read_reading(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* joint = dynamic_cast<const JointConstraint*>(&object);
    if (joint == nullptr) {
        return false;
    }
    out = number_slot(object.simulation_running() ? joint->reading() : 0.0);
    return true;
}

bool read_motor_velocity(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* joint = dynamic_cast<const JointConstraint*>(&object);
    if (joint == nullptr) {
        return false;
    }
    out = vec3_slot(joint->motor_velocity());
    return true;
}

bool write_motor_velocity(DataModel&, DataModel& object, LuaSlot& in) {
    auto* joint = dynamic_cast<JointConstraint*>(&object);
    return joint != nullptr && refuse(in, joint->set_motor_velocity(in.vec));
}

// Each default as a save would write it.
const char* default_json(JointParam p) {
    static const std::array<std::string, kJointParamCount> json = [] {
        std::array<std::string, kJointParamCount> out;
        for (int i = 0; i < kJointParamCount; ++i) {
            const JointParamInfo& info = kParams[i];
            out[static_cast<std::size_t>(i)] = info.rule == R::Flag ? (info.default_value != 0.0 ? "true" : "false")
                                                                    : write_json(JsonValue::number(info.default_value));
        }
        return out;
    }();
    return json[static_cast<std::size_t>(p)].c_str();
}

template <JointParam P>
LuaField param_field() {
    const JointParamInfo& info = joint_param_info(P);
    return lua_saved_property(info.name, info.rule == R::Flag ? "boolean" : "number", read_param<P>, write_param<P>,
                              default_json(P));
}

LuaField reading_field(const char* name) { return lua_property(name, "number", false, read_reading, nullptr); }

template <std::size_t N>
void register_joint(const char* name, const LuaField (&fields)[N]) {
    register_lua_class(name, "JointConstraint", fields, static_cast<int>(N));
    register_suited_parents(name, {"Workspace", "PVInstance"});
}

ANARCHY_LUA_REGISTER(register_joint_constraint_lua) {
    using P = JointParam;
    static const std::string zero_vector = [] {
        const float zeros[3] = {0.f, 0.f, 0.f};
        return write_json(json_floats(zeros, 3));
    }();
    const LuaField joint_fields[] = {
        lua_group("Joint"),
        param_field<P::CollideConnected>(),
    };
    // Abstract: no factory registers it.
    register_lua_class("JointConstraint", "Constraint", joint_fields, static_cast<int>(std::size(joint_fields)));
    const LuaField revolute[] = {
        lua_group("Spring"), param_field<P::EnableSpring>(), param_field<P::Hertz>(),
        param_field<P::DampingRatio>(), param_field<P::TargetAngle>(),
        lua_group("Limit"), param_field<P::EnableLimit>(), param_field<P::LowerAngle>(),
        param_field<P::UpperAngle>(),
        lua_group("Motor"), param_field<P::EnableMotor>(), param_field<P::MotorSpeed>(),
        param_field<P::MaxMotorTorque>(),
        lua_group("State"), reading_field("Angle"),
    };
    register_joint("RevoluteConstraint", revolute);
    const LuaField prismatic[] = {
        lua_group("Spring"), param_field<P::EnableSpring>(), param_field<P::Hertz>(),
        param_field<P::DampingRatio>(), param_field<P::TargetTranslation>(),
        lua_group("Limit"), param_field<P::EnableLimit>(), param_field<P::LowerTranslation>(),
        param_field<P::UpperTranslation>(),
        lua_group("Motor"), param_field<P::EnableMotor>(), param_field<P::MotorSpeed>(),
        param_field<P::MaxMotorForce>(),
        lua_group("State"), reading_field("Translation"),
    };
    register_joint("PrismaticConstraint", prismatic);
    const LuaField distance[] = {
        lua_group("Distance"), param_field<P::Length>(),
        lua_group("Spring"), param_field<P::EnableSpring>(), param_field<P::Hertz>(),
        param_field<P::DampingRatio>(),
        lua_group("Limit"), param_field<P::EnableLimit>(), param_field<P::MinLength>(),
        param_field<P::MaxLength>(),
        lua_group("Motor"), param_field<P::EnableMotor>(), param_field<P::MotorSpeed>(),
        param_field<P::MaxMotorForce>(),
        lua_group("State"), reading_field("CurrentLength"),
    };
    register_joint("DistanceConstraint", distance);
    const LuaField spherical[] = {
        lua_group("Spring"), param_field<P::EnableSpring>(), param_field<P::Hertz>(),
        param_field<P::DampingRatio>(),
        lua_group("Limit"), param_field<P::EnableConeLimit>(), param_field<P::ConeAngle>(),
        param_field<P::EnableTwistLimit>(), param_field<P::LowerTwistAngle>(), param_field<P::UpperTwistAngle>(),
        lua_group("Motor"), param_field<P::EnableMotor>(),
        lua_saved_property("MotorVelocity", "Vector3", read_motor_velocity, write_motor_velocity,
                           zero_vector.c_str()),
        param_field<P::MaxMotorTorque>(),
    };
    register_joint("SphericalConstraint", spherical);
    const LuaField wheel[] = {
        lua_group("Suspension"), param_field<P::EnableSuspension>(), param_field<P::SuspensionHertz>(),
        param_field<P::SuspensionDampingRatio>(), param_field<P::EnableSuspensionLimit>(),
        param_field<P::LowerSuspensionLimit>(), param_field<P::UpperSuspensionLimit>(),
        lua_group("Spin"), param_field<P::EnableSpinMotor>(), param_field<P::SpinMotorSpeed>(),
        param_field<P::MaxSpinTorque>(),
        lua_group("Steering"), param_field<P::EnableSteering>(), param_field<P::SteeringHertz>(),
        param_field<P::SteeringDampingRatio>(), param_field<P::TargetSteeringAngle>(),
        param_field<P::MaxSteeringTorque>(), param_field<P::EnableSteeringLimit>(),
        param_field<P::LowerSteeringLimit>(), param_field<P::UpperSteeringLimit>(),
        lua_group("State"), reading_field("SpinSpeed"),
    };
    register_joint("WheelConstraint", wheel);
}

}  // namespace

}  // namespace engine_core
