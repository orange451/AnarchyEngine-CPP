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
