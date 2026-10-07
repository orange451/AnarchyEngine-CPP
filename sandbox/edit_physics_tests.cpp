// PhysicsWorld while the place is stopped: bodies exist and follow the tree,
// but nothing is simulated and nothing is written back to instances.

#include "physics_rig.hpp"
#include "support.hpp"

#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using engine_core::GameObject;
using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

}  // namespace

TEST_CASE("E1 stopped, a PhysicsObject in Workspace has a body", "[physics][edit]") {
    PhysicsRig rig;
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.sync_steps(1);
    REQUIRE(rig.physics.has_body(box.id()));
    rig.game.set_parent(box.id(), rig.game.scene_service("Storage"));
    rig.sync_steps(1);
    REQUIRE_FALSE(rig.physics.has_body(box.id()));
}

TEST_CASE("E2 stopped, nothing falls however many ticks pass", "[physics][edit]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.sync_steps(600);
    REQUIRE(y_of(box.transform()) == 5.f);
    REQUIRE(rig.physics.body_position(box.id())->y == 5.f);
}

TEST_CASE("E3 stopped, a body follows its GameObject without rewriting its own Transform", "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 8.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->y == 8.f);
    part.set_transform(at(5.f, 8.f, 0.f));
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->x == 5.f);
    REQUIRE(engine_core::same_matrix4(body.transform(), engine_core::matrix4_identity()));
}

TEST_CASE("E4 Play keeps the bodies made while stopped, and Stop remakes them", "[physics][edit]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.sync_steps(1);
    const std::uint64_t before = rig.physics.body_key(box.id());
    REQUIRE(before != 0);
    rig.play();
    rig.steps(1);
    REQUIRE(rig.physics.body_key(box.id()) == before);
    rig.seconds(0.5);
    REQUIRE(y_of(box.transform()) < 5.f);
    rig.game.stop_simulation();
    rig.sync_steps(1);
    REQUIRE(rig.physics.has_body(box.id()));
    REQUIRE(rig.physics.body_position(box.id())->y == 5.f);
}

TEST_CASE("E5 an anchored body made while stopped seeds its Transform on the first played tick", "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(2.f, 3.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, true);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.sync_steps(5);
    REQUIRE(engine_core::same_matrix4(body.transform(), engine_core::matrix4_identity()));
    rig.play();
    rig.steps(1);
    REQUIRE(x_of(body.transform()) == 2.f);
    REQUIRE(y_of(body.transform()) == 3.f);
}
