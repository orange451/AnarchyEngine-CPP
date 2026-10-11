// Joint constraints in Assemblies and in play
// (docs/superpowers/specs/2026-10-11-joint-constraints-design.md §2, §3, §6).

#include "physics_rig.hpp"
#include "support.hpp"

#include "alloc_counter.hpp"

#include "Assemblies.hpp"
#include "Attachment.hpp"
#include "Constraint.hpp"
#include "GameObject.hpp"
#include "JointConstraint.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

namespace {

using engine_core::DataModel;
using engine_core::InstanceId;
using engine_core::JointConstraint;
using engine_core::JointParam;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

constexpr double kPi = 3.14159265358979323846;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

template <class T>
T& joint(DataModel& game, InstanceId base, InstanceId moving) {
    T& made = game.create<T>();
    REQUIRE_FALSE(made.set_pv_instance1(instance_slot(base)));
    REQUIRE_FALSE(made.set_pv_instance2(instance_slot(moving)));
    game.set_parent(made.id(), workspace_of(game));
    return made;
}

engine_core::WeldConstraint& weld(DataModel& game, InstanceId a, InstanceId b) {
    engine_core::WeldConstraint& made = game.create<engine_core::WeldConstraint>();
    REQUIRE_FALSE(made.set_pv_instance1(instance_slot(a)));
    REQUIRE_FALSE(made.set_pv_instance2(instance_slot(b)));
    game.set_parent(made.id(), workspace_of(game));
    return made;
}

bool active(DataModel& game, const engine_core::Constraint& constraint) {
    game.assemblies().refresh(game);
    return game.assemblies().active(constraint.id());
}

void set(JointConstraint& joint, JointParam param, double value) { REQUIRE_FALSE(joint.set_param(param, value)); }

void no_gravity(DataModel& game) {
    auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    REQUIRE_FALSE(workspace->set_gravity(0.0));
}

// where, turned by degrees about axis.
Matrix4 turned(const Matrix4& where, Vec3 axis, double degrees) {
    return engine_core::matrix4_multiply(where, engine_core::matrix4_axis_angle(axis, degrees * kPi / 180.0));
}

Vec3 column(const Matrix4& m, int c) { return Vec3{m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }

float distance(const Matrix4& a, const Matrix4& b) {
    const float dx = a.m[12] - b.m[12];
    const float dy = a.m[13] - b.m[13];
    const float dz = a.m[14] - b.m[14];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float degrees_between(Vec3 a, Vec3 b) {
    const float la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const float lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    const float cosine = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
    return static_cast<float>(std::acos(std::max(-1.f, std::min(1.f, cosine))) * 180.0 / kPi);
}

bool finite(const Matrix4& m) {
    for (const float v : m.m) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("JA1 a joint between two bodies is Active, listed base first, and joins nothing", "[joint][assembly]") {
    PhysicsRig rig;
    PhysicsObject& chassis = rig.body(at(0.f, 2.f, 0.f), Vec3{2.f, 1.f, 2.f}, false);
    PhysicsObject& wheel = rig.body(at(2.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, chassis.id(), wheel.id());
    REQUIRE(active(rig.game, hinge));
    const engine_core::Assemblies& assemblies = rig.game.assemblies();
    REQUIRE(assemblies.joints().size() == 1);
    REQUIRE(assemblies.joints()[0].constraint == hinge.id());
    REQUIRE(assemblies.joints()[0].node1 == chassis.id());
    REQUIRE(assemblies.joints()[0].node2 == wheel.id());
    REQUIRE(assemblies.group_of(chassis.id()) == -1);
    REQUIRE(assemblies.root_of(rig.game, wheel.id()) == wheel.id());
    // Order matters for a joint: swapping the ends swaps the nodes.
    REQUIRE_FALSE(hinge.set_pv_instance1(instance_slot(wheel.id())));
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(chassis.id())));
    REQUIRE(active(rig.game, hinge));
    REQUIRE(rig.game.assemblies().joints()[0].node1 == wheel.id());
}

TEST_CASE("JA2 a joint is inactive disabled, outside Workspace, inside one assembly, between two anchored "
          "assemblies, or at a controller, a plain object, or Terrain",
          "[joint][assembly]") {
    PhysicsRig rig;
    PhysicsObject& a = rig.body(at(0.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& b = rig.body(at(2.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, a.id(), b.id());
    REQUIRE(active(rig.game, hinge));
    REQUIRE_FALSE(hinge.set_enabled(false));
    REQUIRE_FALSE(active(rig.game, hinge));
    REQUIRE_FALSE(hinge.set_enabled(true));
    rig.game.set_parent(hinge.id(), rig.game.scene_service("Storage"));
    REQUIRE_FALSE(active(rig.game, hinge));
    rig.game.set_parent(hinge.id(), workspace_of(rig.game));
    REQUIRE(active(rig.game, hinge));
    engine_core::WeldConstraint& welded = weld(rig.game, a.id(), b.id());
    REQUIRE_FALSE(active(rig.game, hinge));
    rig.game.destroy(welded.id());
    REQUIRE(active(rig.game, hinge));
    a.set_anchored(true);
    REQUIRE(active(rig.game, hinge));
    b.set_anchored(true);
    REQUIRE_FALSE(active(rig.game, hinge));
    b.set_anchored(false);
    REQUIRE(active(rig.game, hinge));
    engine_core::PlayerController& player = rig.controller(at(5.f, 0.f, 0.f));
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(player.id())));
    REQUIRE_FALSE(active(rig.game, hinge));
    engine_core::GameObject& plain = create_part(rig.game);
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(plain.id())));
    REQUIRE_FALSE(active(rig.game, hinge));
    engine_core::Terrain& terrain = rig.game.create<engine_core::Terrain>();
    rig.game.set_parent(terrain.id(), workspace_of(rig.game));
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(terrain.id())));
    REQUIRE_FALSE(active(rig.game, hinge));
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(b.id())));
    REQUIRE(active(rig.game, hinge));
}

TEST_CASE("JA3 an Attachment end stands for its holder; a welded assembly's anchoring and controller count",
          "[joint][assembly]") {
    PhysicsRig rig;
    PhysicsObject& chassis = rig.body(at(0.f, 2.f, 0.f), Vec3{2.f, 1.f, 2.f}, false);
    PhysicsObject& wheel = rig.body(at(2.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    engine_core::Attachment& axle = rig.game.create<engine_core::Attachment>();
    rig.game.set_parent(axle.id(), wheel.id());
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, chassis.id(), axle.id());
    REQUIRE(active(rig.game, hinge));
    REQUIRE(rig.game.assemblies().joints()[0].node2 == wheel.id());
    // The wheel welded to an anchored stand: its assembly is anchored, the chassis's is not.
    PhysicsObject& stand = rig.body(at(4.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    weld(rig.game, wheel.id(), stand.id());
    REQUIRE(active(rig.game, hinge));
    chassis.set_anchored(true);
    REQUIRE_FALSE(active(rig.game, hinge));
    chassis.set_anchored(false);
    // The chassis welded to a PlayerController: the controller is its root.
    engine_core::PlayerController& player = rig.controller(at(-3.f, 0.f, 0.f));
    weld(rig.game, chassis.id(), player.id());
    REQUIRE_FALSE(active(rig.game, hinge));
}

TEST_CASE("JA4 joints are listed by constraint id, and read the same in play", "[joint][assembly]") {
    PhysicsRig rig;
    PhysicsObject& a = rig.body(at(0.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& b = rig.body(at(2.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& c = rig.body(at(4.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& first = joint<engine_core::WheelConstraint>(rig.game, b.id(), c.id());
    auto& second = joint<engine_core::DistanceConstraint>(rig.game, a.id(), b.id());
    rig.game.assemblies().refresh(rig.game);
    const auto& joints = rig.game.assemblies().joints();
    REQUIRE(joints.size() == 2);
    REQUIRE(joints[0].constraint == std::min(first.id(), second.id()));
    REQUIRE(joints[1].constraint == std::max(first.id(), second.id()));
    rig.play();
    REQUIRE(active(rig.game, first));
    REQUIRE(active(rig.game, second));
}

TEST_CASE("JP1 a jointed part hangs where it was placed from an anchored base", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    // A Revolute and a Spherical hold their pivot (the part's centre); a
    // Prismatic slides across gravity; a Distance hangs straight down.
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& balled = rig.body(at(-3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& slid = rig.body(turned(at(0.f, 5.f, 3.f), Vec3{0.f, 0.f, 1.f}, 90.0), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& hung = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    joint<engine_core::SphericalConstraint>(rig.game, base.id(), balled.id());
    joint<engine_core::PrismaticConstraint>(rig.game, base.id(), slid.id());
    joint<engine_core::DistanceConstraint>(rig.game, base.id(), hung.id());
    const Matrix4 placed[4] = {hinged.transform(), balled.transform(), slid.transform(), hung.transform()};
    rig.play();
    rig.seconds(1.0);
    REQUIRE(rig.physics.joint_count() == 4);
    REQUIRE(distance(hinged.transform(), placed[0]) < 0.02f);
    REQUIRE(distance(balled.transform(), placed[1]) < 0.02f);
    REQUIRE(distance(slid.transform(), placed[2]) < 0.02f);
    REQUIRE(distance(hung.transform(), placed[3]) < 0.02f);
}

TEST_CASE("JP2 with gravity off, nothing jointed moves at Play, however its parts are turned", "[joint][physics]") {
    PhysicsRig rig;
    no_gravity(rig.game);
    PhysicsObject& base = rig.body(turned(at(0.f, 2.f, 0.f), Vec3{0.f, 1.f, 0.f}, 25.0), Vec3{2.f, 1.f, 2.f}, false);
    std::vector<PhysicsObject*> parts;
    for (int i = 0; i < 5; ++i) {
        parts.push_back(&rig.body(turned(at(3.f * static_cast<float>(i) - 6.f, 4.f, 2.f), Vec3{1.f, 0.f, 0.f},
                                         30.0 + 10.0 * i),
                                  Vec3{1.f, 1.f, 1.f}, false));
    }
    joint<engine_core::RevoluteConstraint>(rig.game, base.id(), parts[0]->id());
    joint<engine_core::PrismaticConstraint>(rig.game, base.id(), parts[1]->id());
    joint<engine_core::DistanceConstraint>(rig.game, base.id(), parts[2]->id());
    joint<engine_core::SphericalConstraint>(rig.game, base.id(), parts[3]->id());
    joint<engine_core::WheelConstraint>(rig.game, base.id(), parts[4]->id());
    std::vector<Matrix4> placed;
    for (PhysicsObject* part : parts) {
        placed.push_back(part->transform());
    }
    const Matrix4 base_placed = base.transform();
    rig.play();
    rig.seconds(1.0);
    REQUIRE(rig.physics.joint_count() == 5);
    REQUIRE(distance(base.transform(), base_placed) < 1e-3f);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        INFO(i);
        REQUIRE(distance(parts[i]->transform(), placed[i]) < 1e-3f);
        REQUIRE(degrees_between(column(parts[i]->transform(), 1), column(placed[i], 1)) < 0.1f);
    }
}

TEST_CASE("JP3 a weld made in play remakes the joint on the new body, without a jump", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& extra = rig.body(at(3.f, 6.f, 0.f), Vec3{0.5f, 0.5f, 0.5f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    const Matrix4 placed = hinged.transform();
    rig.play();
    rig.seconds(0.5);
    const std::uint64_t before = rig.physics.joint_key(hinge.id());
    REQUIRE(before != 0);
    weld(rig.game, hinged.id(), extra.id());
    rig.seconds(1.0);
    REQUIRE(rig.physics.joint_count() == 1);
    REQUIRE(rig.physics.joint_key(hinge.id()) != before);
    REQUIRE(distance(hinged.transform(), placed) < 0.02f);
}

TEST_CASE("JP4 an end destroyed in play takes its joint with it", "[joint][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    rig.play();
    rig.seconds(0.5);
    REQUIRE(rig.physics.joint_count() == 1);
    rig.game.destroy(hinged.id());
    rig.seconds(0.5);
    REQUIRE(rig.physics.joint_count() == 0);
    REQUIRE(rig.physics.joint_key(hinge.id()) == 0);
    REQUIRE_FALSE(active(rig.game, hinge));
}

TEST_CASE("JP5 a weld made in play between a joint's two ends ends the joint", "[joint][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& a = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& b = rig.body(at(2.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, a.id(), b.id());
    rig.play();
    rig.seconds(0.25);
    REQUIRE(rig.physics.joint_count() == 1);
    weld(rig.game, a.id(), b.id());
    rig.seconds(1.0);
    REQUIRE(rig.physics.joint_count() == 0);
    REQUIRE_FALSE(active(rig.game, hinge));
    REQUIRE(finite(a.transform()));
    REQUIRE(near(distance(a.transform(), b.transform()), 2.f, 0.02f));
}

TEST_CASE("JP6 Stop takes every joint away, and the next Play makes them again", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    // Nothing is made while stopped.
    rig.sync_steps(2);
    REQUIRE(rig.physics.joint_count() == 0);
    rig.play();
    rig.steps(1);
    REQUIRE(rig.physics.joint_count() == 1);
    rig.game.stop_simulation();
    rig.sync_steps(1);
    REQUIRE(rig.physics.joint_count() == 0);
    rig.game.start_simulation();
    rig.steps(1);
    REQUIRE(rig.physics.joint_count() == 1);
}

TEST_CASE("JP7 a jointed part a script moves away is pulled back, and stays finite", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    const Matrix4 placed = hinged.transform();
    rig.play();
    rig.seconds(0.25);
    REQUIRE_FALSE(hinged.set_transform(at(6.f, 7.f, 1.f)));
    for (int i = 0; i < 480; ++i) {
        rig.steps(1);
        REQUIRE(finite(hinged.transform()));
    }
    REQUIRE(rig.physics.joint_count() == 1);
    REQUIRE(distance(hinged.transform(), placed) < 0.05f);
}

TEST_CASE("JB1 a Revolute's motor turns it, its limit stops it, and its spring brings it to TargetAngle",
          "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    // Its Y is up: it turns about a vertical axis, so gravity does not turn it.
    PhysicsObject& turner = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), turner.id());
    set(hinge, JointParam::EnableMotor, 1.0);
    set(hinge, JointParam::MotorSpeed, 90.0);
    rig.play();
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(hinge.reading()), 90.f, 5.f));
    set(hinge, JointParam::EnableLimit, 1.0);
    set(hinge, JointParam::LowerAngle, -45.0);
    set(hinge, JointParam::UpperAngle, 120.0);
    rig.seconds(1.5);
    REQUIRE(near(static_cast<float>(hinge.reading()), 120.f, 3.f));
    set(hinge, JointParam::EnableMotor, 0.0);
    set(hinge, JointParam::EnableSpring, 1.0);
    set(hinge, JointParam::Hertz, 4.0);
    set(hinge, JointParam::DampingRatio, 1.0);
    set(hinge, JointParam::TargetAngle, -30.0);
    rig.seconds(3.0);
    REQUIRE(near(static_cast<float>(hinge.reading()), -30.f, 3.f));
}

TEST_CASE("JB2 a Prismatic's motor slides it, and its limit stops it", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    // Its Y lies along -X: it slides across gravity.
    PhysicsObject& slider = rig.body(turned(at(0.f, 5.f, 3.f), Vec3{0.f, 0.f, 1.f}, 90.0), Vec3{1.f, 1.f, 1.f}, false);
    auto& slide = joint<engine_core::PrismaticConstraint>(rig.game, base.id(), slider.id());
    set(slide, JointParam::EnableMotor, 1.0);
    set(slide, JointParam::MotorSpeed, 1.0);
    rig.play();
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(slide.reading()), 1.f, 0.05f));
    REQUIRE(near(x_of(slider.transform()), -1.f, 0.05f));
    set(slide, JointParam::EnableLimit, 1.0);
    set(slide, JointParam::UpperTranslation, 1.5);
    rig.seconds(1.5);
    REQUIRE(near(static_cast<float>(slide.reading()), 1.5f, 0.05f));
}

TEST_CASE("JB3 a Distance holds its length as a rod, sags as a spring, and takes a Length", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& weight = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& rod = joint<engine_core::DistanceConstraint>(rig.game, base.id(), weight.id());
    rig.play();
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(rod.reading()), 2.f, 0.01f));
    set(rod, JointParam::EnableSpring, 1.0);
    set(rod, JointParam::Hertz, 1.0);
    set(rod, JointParam::DampingRatio, 1.0);
    rig.seconds(3.0);
    // g / (2πf)² ≈ 0.25 below the rest length.
    REQUIRE(rod.reading() > 2.1);
    REQUIRE(rod.reading() < 2.4);
    set(rod, JointParam::EnableSpring, 0.0);
    set(rod, JointParam::Length, 3.0);
    rig.seconds(2.0);
    REQUIRE(near(static_cast<float>(rod.reading()), 3.f, 0.02f));
}

TEST_CASE("JB4 a Spherical's cone limit holds a swinging weight", "[joint][physics]") {
    const auto widest_swing = [](bool limited) {
        PhysicsRig rig;
        PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{0.5f, 0.5f, 0.5f}, true);
        PhysicsObject& weight = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
        // The ball is 2 above the weight, at the base.
        engine_core::Attachment& top = rig.game.create<engine_core::Attachment>();
        REQUIRE_FALSE(top.set_offset(at(0.f, 2.f, 0.f)));
        rig.game.set_parent(top.id(), weight.id());
        auto& ball = joint<engine_core::SphericalConstraint>(rig.game, base.id(), top.id());
        if (limited) {
            set(ball, JointParam::EnableConeLimit, 1.0);
            set(ball, JointParam::ConeAngle, 20.0);
        }
        REQUIRE_FALSE(weight.set_velocity(Vec3{5.f, 0.f, 0.f}));
        rig.play();
        float widest = 0.f;
        for (int i = 0; i < 480; ++i) {
            rig.steps(1);
            const Matrix4 at_now = weight.transform();
            widest = std::max(widest, degrees_between(Vec3{x_of(at_now), y_of(at_now) - 5.f, z_of(at_now)},
                                                      Vec3{0.f, -1.f, 0.f}));
        }
        return widest;
    };
    REQUIRE(widest_swing(false) > 45.f);
    REQUIRE(widest_swing(true) < 22.f);
}

TEST_CASE("JB5 a Wheel rides its suspension, spins at its motor's speed, and steers", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& chassis = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 0.5f, 2.f}, true);
    // A cylinder on its side: its Y, the axle, lies along -X.
    PhysicsObject& wheel = rig.body(turned(at(1.5f, 3.f, 0.f), Vec3{0.f, 0.f, 1.f}, 90.0), Vec3{1.f, 0.4f, 1.f}, false);
    REQUIRE_FALSE(wheel.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    auto& axle = joint<engine_core::WheelConstraint>(rig.game, chassis.id(), wheel.id());
    set(axle, JointParam::SuspensionHertz, 1.0);
    set(axle, JointParam::SuspensionDampingRatio, 1.0);
    rig.play();
    rig.seconds(3.0);
    // g / (2πf)² ≈ 0.25 below where it was placed, and nowhere else.
    REQUIRE(near(y_of(wheel.transform()), 2.75f, 0.05f));
    REQUIRE(near(x_of(wheel.transform()), 1.5f, 0.01f));
    REQUIRE(near(z_of(wheel.transform()), 0.f, 0.01f));
    set(axle, JointParam::EnableSpinMotor, 1.0);
    set(axle, JointParam::SpinMotorSpeed, 180.0);
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(axle.reading()), 180.f, 5.f));
    set(axle, JointParam::EnableSpinMotor, 0.0);
    set(axle, JointParam::EnableSteering, 1.0);
    set(axle, JointParam::TargetSteeringAngle, 20.0);
    rig.seconds(2.0);
    const Vec3 spin = column(wheel.transform(), 1);
    REQUIRE(near(degrees_between(Vec3{spin.x, 0.f, spin.z}, Vec3{-1.f, 0.f, 0.f}), 20.f, 3.f));
}

TEST_CASE("JB6 a setting written in play wakes a sleeping body, and Stop puts the settings back",
          "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& turner = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), turner.id());
    rig.play();
    rig.seconds(3.0);  // long enough for Box3D to put it to sleep
    set(hinge, JointParam::EnableMotor, 1.0);
    set(hinge, JointParam::MotorSpeed, 90.0);
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(hinge.reading()), 90.f, 5.f));
    rig.game.stop_simulation();
    REQUIRE_FALSE(hinge.flag(JointParam::EnableMotor));
    REQUIRE(hinge.param(JointParam::MotorSpeed) == 0.0);
}

TEST_CASE("JB7 a base unanchored in play keeps its joint", "[joint][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& base = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& turner = rig.body(at(2.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), turner.id());
    set(hinge, JointParam::EnableMotor, 1.0);
    set(hinge, JointParam::MotorSpeed, 45.0);
    rig.play();
    rig.seconds(0.5);
    base.set_anchored(false);
    rig.seconds(2.0);
    REQUIRE(rig.physics.joint_count() == 1);
    REQUIRE(finite(turner.transform()));
    REQUIRE(near(distance(base.transform(), turner.transform()), 2.f, 0.05f));
}

TEST_CASE("JB8 a setting written every step changes the joint in place", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& turner = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), turner.id());
    set(hinge, JointParam::EnableMotor, 1.0);
    rig.play();
    rig.steps(1);
    const std::uint64_t made = rig.physics.joint_key(hinge.id());
    for (int i = 0; i < 240; ++i) {
        set(hinge, JointParam::MotorSpeed, 30.0 + 0.25 * i);
        rig.steps(1);
    }
    REQUIRE(rig.physics.joint_key(hinge.id()) == made);
    REQUIRE(hinge.reading() > 20.0);
}
