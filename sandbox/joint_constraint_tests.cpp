// JointConstraint's values (docs/superpowers/specs/2026-10-11-joint-constraints-design.md
// §1, §4): defaults, what a setter refuses or clamps, from C++ and Luau, and saving.

#include "support.hpp"

#include "Assemblies.hpp"
#include "Folder.hpp"
#include "InstanceFile.hpp"
#include "JointConstraint.hpp"
#include "PhysicsObject.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using engine_core::DataModel;
using engine_core::InstanceId;
using engine_core::JointConstraint;
using engine_core::JointKind;
using engine_core::JointParam;
using engine_core::PhysicsObject;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

bool has(const std::string& output, const std::string& text) { return output.find(text) != std::string::npos; }

std::string run(ScriptRig& rig, const char* source) {
    rig.runtime.drain_output();
    rig.runtime.run_chunk(source);
    rig.frames(1);
    std::string out;
    for (const auto& line : rig.runtime.drain_output().lines) {
        out += line.text;
        out += '\n';
    }
    return out;
}

PhysicsObject& part(DataModel& game, const char* name) {
    PhysicsObject& made = game.create<PhysicsObject>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), workspace_of(game));
    return made;
}

template <class T>
T& joint(DataModel& game, InstanceId base, InstanceId moving, InstanceId parent) {
    T& made = game.create<T>();
    REQUIRE_FALSE(made.set_pv_instance1(instance_slot(base)));
    REQUIRE_FALSE(made.set_pv_instance2(instance_slot(moving)));
    game.set_parent(made.id(), parent);
    return made;
}

}  // namespace

TEST_CASE("JC1 the five joints have their kinds, and every property starts at its default", "[joint]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(game.create<engine_core::RevoluteConstraint>().kind() == JointKind::Revolute);
    REQUIRE(game.create<engine_core::PrismaticConstraint>().kind() == JointKind::Prismatic);
    REQUIRE(game.create<engine_core::DistanceConstraint>().kind() == JointKind::Distance);
    REQUIRE(game.create<engine_core::SphericalConstraint>().kind() == JointKind::Spherical);
    engine_core::WheelConstraint& wheel = game.create<engine_core::WheelConstraint>();
    REQUIRE(wheel.kind() == JointKind::Wheel);
    REQUIRE(std::string(wheel.class_name()) == "WheelConstraint");
    REQUIRE_FALSE(wheel.rigid());
    REQUIRE_FALSE(wheel.joins());
    REQUIRE(wheel.joint());
    for (int i = 0; i < engine_core::kJointParamCount; ++i) {
        const auto param = static_cast<JointParam>(i);
        REQUIRE(wheel.param(param) == engine_core::joint_param_info(param).default_value);
    }
    // The table lines up with the enum at its start, middle, and end.
    REQUIRE(std::string(engine_core::joint_param_info(JointParam::CollideConnected).name) == "CollideConnected");
    REQUIRE(std::string(engine_core::joint_param_info(JointParam::MaxLength).name) == "MaxLength");
    REQUIRE(std::string(engine_core::joint_param_info(JointParam::UpperSteeringLimit).name) ==
            "UpperSteeringLimit");
    REQUIRE(wheel.flag(JointParam::EnableSuspension));
    REQUIRE_FALSE(wheel.flag(JointParam::CollideConnected));
    REQUIRE(wheel.param(JointParam::SuspensionHertz) == 4.0);
    REQUIRE(wheel.param(JointParam::MaxSpinTorque) == 10000.0);
    REQUIRE(wheel.param(JointParam::LowerSteeringLimit) == -30.0);
    REQUIRE(wheel.reading() == 0.0);
    REQUIRE(wheel.motor_velocity().y == 0.f);
}

TEST_CASE("JC2 a value that is not finite, or a negative one where none is allowed, is refused", "[joint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::RevoluteConstraint& hinge = game.create<engine_core::RevoluteConstraint>();
    REQUIRE(*hinge.set_param(JointParam::MotorSpeed, std::nan("")) == "MotorSpeed must be a finite number");
    REQUIRE(*hinge.set_param(JointParam::TargetAngle, std::numeric_limits<double>::infinity()) ==
            "TargetAngle must be a finite number");
    for (const JointParam param :
         {JointParam::Hertz, JointParam::DampingRatio, JointParam::MaxMotorTorque, JointParam::MaxMotorForce,
          JointParam::Length, JointParam::MinLength, JointParam::MaxLength, JointParam::SuspensionHertz,
          JointParam::SuspensionDampingRatio, JointParam::SteeringHertz, JointParam::SteeringDampingRatio,
          JointParam::MaxSpinTorque, JointParam::MaxSteeringTorque}) {
        const double before = hinge.param(param);
        const std::optional<std::string> refused = hinge.set_param(param, -1.0);
        REQUIRE(refused);
        REQUIRE(*refused == std::string(engine_core::joint_param_info(param).name) + " must not be negative");
        REQUIRE(hinge.param(param) == before);
    }
    // A speed may be negative: it turns the other way.
    REQUIRE_FALSE(hinge.set_param(JointParam::MotorSpeed, -90.0));
    REQUIRE(hinge.param(JointParam::MotorSpeed) == -90.0);
    engine_core::SphericalConstraint& ball = game.create<engine_core::SphericalConstraint>();
    REQUIRE(*ball.set_motor_velocity(engine_core::Vec3{std::nanf(""), 0.f, 0.f}) == "MotorVelocity must be finite");
    REQUIRE_FALSE(ball.set_motor_velocity(engine_core::Vec3{0.f, 90.f, 0.f}));
    REQUIRE(ball.motor_velocity().y == 90.f);
}

TEST_CASE("JC3 angle limits are clamped to Box3D's ranges", "[joint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::RevoluteConstraint& hinge = game.create<engine_core::RevoluteConstraint>();
    REQUIRE_FALSE(hinge.set_param(JointParam::LowerAngle, -500.0));
    REQUIRE(hinge.param(JointParam::LowerAngle) == -178.2);
    REQUIRE_FALSE(hinge.set_param(JointParam::UpperAngle, 179.0));
    REQUIRE(hinge.param(JointParam::UpperAngle) == 178.2);
    engine_core::SphericalConstraint& ball = game.create<engine_core::SphericalConstraint>();
    REQUIRE_FALSE(ball.set_param(JointParam::UpperTwistAngle, 200.0));
    REQUIRE(ball.param(JointParam::UpperTwistAngle) == 178.2);
    REQUIRE_FALSE(ball.set_param(JointParam::ConeAngle, 120.0));
    REQUIRE(ball.param(JointParam::ConeAngle) == 90.0);
    REQUIRE_FALSE(ball.set_param(JointParam::ConeAngle, -5.0));
    REQUIRE(ball.param(JointParam::ConeAngle) == 0.0);
    // A flag takes any non-zero as on.
    REQUIRE_FALSE(ball.set_param(JointParam::EnableConeLimit, 7.0));
    REQUIRE(ball.param(JointParam::EnableConeLimit) == 1.0);
}

TEST_CASE("JC4 a lower limit above its upper is kept as written", "[joint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::PrismaticConstraint& slider = game.create<engine_core::PrismaticConstraint>();
    REQUIRE_FALSE(slider.set_param(JointParam::LowerTranslation, 3.0));
    REQUIRE(slider.param(JointParam::LowerTranslation) == 3.0);
    REQUIRE(slider.param(JointParam::UpperTranslation) == 1.0);
}

TEST_CASE("JC5 a setting moves the settings revision; an end or Enabled, the capture revision", "[joint]") {
    SimRole role;
    engine_core::Game game;
    PhysicsObject& a = part(game, "A");
    engine_core::RevoluteConstraint& hinge = game.create<engine_core::RevoluteConstraint>();
    const std::uint32_t settings = hinge.settings_revision();
    REQUIRE_FALSE(hinge.set_param(JointParam::MotorSpeed, 30.0));
    REQUIRE(hinge.settings_revision() != settings);
    const std::uint32_t again = hinge.settings_revision();
    REQUIRE_FALSE(hinge.set_param(JointParam::MotorSpeed, 30.0));
    REQUIRE(hinge.settings_revision() == again);
    const std::uint32_t capture = hinge.capture_revision();
    REQUIRE_FALSE(hinge.set_pv_instance1(instance_slot(a.id())));
    REQUIRE(hinge.capture_revision() != capture);
    const std::uint32_t enabled = hinge.capture_revision();
    REQUIRE_FALSE(hinge.set_enabled(false));
    REQUIRE(hinge.capture_revision() != enabled);
    const std::uint32_t same = hinge.capture_revision();
    REQUIRE_FALSE(hinge.set_enabled(false));
    REQUIRE(hinge.capture_revision() == same);
    REQUIRE(hinge.settings_revision() == again);
}

TEST_CASE("JC6 Luau reads and writes a joint's properties, and is refused as C++ is", "[joint][lua]") {
    ScriptRig rig;
    const std::string out = run(rig, R"(
        local w = Instance.new("WheelConstraint")
        w.SpinMotorSpeed = -360
        w.EnableSteering = true
        print("spin=" .. tostring(w.SpinMotorSpeed))
        print("steer=" .. tostring(w.EnableSteering))
        print("hertz=" .. tostring(w.SuspensionHertz))
        print("reading=" .. tostring(w.SpinSpeed))
        print("collide=" .. tostring(w.CollideConnected))
        local ok, err = pcall(function() w.SteeringHertz = -1 end)
        print("refused=" .. tostring(ok) .. " " .. tostring(err))
        local b = Instance.new("SphericalConstraint")
        b.MotorVelocity = Vector3.new(0, 90, 0)
        print("velocity=" .. tostring(b.MotorVelocity.Y))
        print("isa=" .. tostring(b:IsA("JointConstraint")) .. tostring(b:IsA("Constraint")) .. tostring(b.Enabled))
        print("foreign=" .. tostring(pcall(function() w.LowerAngle = 1 end)))
    )");
    INFO(out);
    REQUIRE(has(out, "spin=-360"));
    REQUIRE(has(out, "steer=true"));
    REQUIRE(has(out, "hertz=4"));
    REQUIRE(has(out, "reading=0"));
    REQUIRE(has(out, "collide=false"));
    REQUIRE(has(out, "refused=false"));
    REQUIRE(has(out, "SteeringHertz must not be negative"));
    REQUIRE(has(out, "velocity=90"));
    REQUIRE(has(out, "isa=truetruetrue"));
    REQUIRE(has(out, "foreign=false"));
}

TEST_CASE("JC7 a saved joint loads with every value it had", "[joint]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        DataModel& game = project.datamodel();
        PhysicsObject& chassis = part(game, "Chassis");
        PhysicsObject& wheel = part(game, "Wheel");
        engine_core::WheelConstraint& axle =
            joint<engine_core::WheelConstraint>(game, chassis.id(), wheel.id(), workspace_of(game));
        game.set_name(axle.id(), "Axle");
        REQUIRE_FALSE(axle.set_param(JointParam::SpinMotorSpeed, -360.0));
        REQUIRE_FALSE(axle.set_param(JointParam::SuspensionHertz, 2.0));
        REQUIRE_FALSE(axle.set_param(JointParam::EnableSteering, 1.0));
        REQUIRE_FALSE(axle.set_param(JointParam::EnableSuspension, 0.0));
        REQUIRE_FALSE(axle.set_param(JointParam::CollideConnected, 1.0));
        engine_core::SphericalConstraint& ball =
            joint<engine_core::SphericalConstraint>(game, chassis.id(), wheel.id(), workspace_of(game));
        game.set_name(ball.id(), "Ball");
        REQUIRE_FALSE(ball.set_motor_velocity(engine_core::Vec3{0.f, 90.f, 0.f}));
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    auto* axle = dynamic_cast<engine_core::WheelConstraint*>(
        game.instance(game.find_first_child(workspace_of(game), "Axle")));
    REQUIRE(axle != nullptr);
    REQUIRE(axle->pv_instance1_id() == game.find_first_child(workspace_of(game), "Chassis"));
    REQUIRE(axle->pv_instance2_id() == game.find_first_child(workspace_of(game), "Wheel"));
    REQUIRE(axle->param(JointParam::SpinMotorSpeed) == -360.0);
    REQUIRE(axle->param(JointParam::SuspensionHertz) == 2.0);
    REQUIRE(axle->flag(JointParam::EnableSteering));
    REQUIRE_FALSE(axle->flag(JointParam::EnableSuspension));
    REQUIRE(axle->flag(JointParam::CollideConnected));
    auto* ball = dynamic_cast<engine_core::SphericalConstraint*>(
        game.instance(game.find_first_child(workspace_of(game), "Ball")));
    REQUIRE(ball != nullptr);
    REQUIRE(ball->motor_velocity().y == 90.f);
}

TEST_CASE("JC8 duplicating a jointed pair joins the copies, with the joint's values", "[joint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Folder& car = game.create<engine_core::Folder>();
    game.set_parent(car.id(), workspace_of(game));
    PhysicsObject& chassis = game.create<PhysicsObject>();
    game.set_parent(chassis.id(), car.id());
    PhysicsObject& wheel = game.create<PhysicsObject>();
    game.set_parent(wheel.id(), car.id());
    engine_core::WheelConstraint& axle =
        joint<engine_core::WheelConstraint>(game, chassis.id(), wheel.id(), car.id());
    REQUIRE_FALSE(axle.set_param(JointParam::SpinMotorSpeed, 45.0));
    std::vector<engine_core::CopiedNode> group{engine_core::copy_tree(game, car.id())};
    engine_core::CopyMap map;
    std::vector<InstanceId> made;
    {
        engine_core::AssemblySuspend suspend(game);
        REQUIRE(engine_core::paste_copies(game, group, workspace_of(game), &made, nullptr, &map));
        engine_core::remap_references(game, map);
    }
    REQUIRE(made.size() == 1);
    InstanceId copy_axle = 0;
    std::vector<InstanceId> parts;
    for (InstanceId child = game.first_child(made[0]); child != 0; child = game.next_sibling(child)) {
        if (dynamic_cast<engine_core::WheelConstraint*>(game.instance(child)) != nullptr) {
            copy_axle = child;
        } else {
            parts.push_back(child);
        }
    }
    auto* copied = dynamic_cast<engine_core::WheelConstraint*>(game.instance(copy_axle));
    REQUIRE(copied != nullptr);
    REQUIRE(copied->param(JointParam::SpinMotorSpeed) == 45.0);
    REQUIRE(parts.size() == 2);
    REQUIRE(copied->pv_instance1_id() == parts[0]);
    REQUIRE(copied->pv_instance2_id() == parts[1]);
}
