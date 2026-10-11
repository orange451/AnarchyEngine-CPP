// NoCollisionConstraint: two bodies that never collide with each other, and
// still collide with everything else. It joins nothing into an assembly.

#include "physics_rig.hpp"
#include "support.hpp"

#include "Assemblies.hpp"
#include "Constraint.hpp"
#include "GameObject.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using engine_core::InstanceId;
using engine_core::NoCollisionConstraint;
using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

NoCollisionConstraint& no_collision(engine_core::DataModel& game, InstanceId a, InstanceId b) {
    NoCollisionConstraint& made = game.create<NoCollisionConstraint>();
    REQUIRE_FALSE(made.set_pv_instance1(instance_slot(a)));
    REQUIRE_FALSE(made.set_pv_instance2(instance_slot(b)));
    game.set_parent(made.id(), workspace_of(game));
    return made;
}

void weld(engine_core::DataModel& game, InstanceId a, InstanceId b) {
    engine_core::WeldConstraint& made = game.create<engine_core::WeldConstraint>();
    REQUIRE_FALSE(made.set_pv_instance1(instance_slot(a)));
    REQUIRE_FALSE(made.set_pv_instance2(instance_slot(b)));
    game.set_parent(made.id(), workspace_of(game));
}

bool active(engine_core::DataModel& game, const NoCollisionConstraint& constraint) {
    game.assemblies().refresh(game);
    return game.assemblies().active(constraint.id());
}

}  // namespace

TEST_CASE("NC1 two bodies a NoCollisionConstraint names pass through each other, and hit everything else",
          "[physics][no-collision]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& block = rig.body(at(0.f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& box = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    no_collision(rig.game, block.id(), box.id());
    rig.play();
    rig.seconds(2.0);
    // Through the anchored block, onto the floor.
    REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));
}

TEST_CASE("NC2 with welds, only the member it names passes through; its partner still collides",
          "[physics][no-collision]") {
    PhysicsRig rig;
    rig.floor();
    // Two anchored blocks, tops at y = 2; the pair falls onto them.
    PhysicsObject& under_low = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 2.f, 1.f}, true);
    rig.body(at(3.f, 1.f, 0.f), Vec3{1.f, 2.f, 1.f}, true);
    PhysicsObject& low = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& high = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, low.id(), high.id());
    no_collision(rig.game, under_low.id(), low.id());
    rig.play();
    rig.seconds(3.0);
    // low passes through its block; high stops on the other one.
    REQUIRE(near(y_of(high.transform()), 2.5f, 0.05f));
    REQUIRE(near(y_of(low.transform()), 0.5f, 0.05f));
}

TEST_CASE("NC3 enabling and disabling during play takes effect at once", "[physics][no-collision]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& block = rig.body(at(0.f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& box = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    NoCollisionConstraint& constraint = no_collision(rig.game, block.id(), box.id());
    REQUIRE_FALSE(constraint.set_enabled(false));
    rig.play();
    rig.seconds(2.0);
    REQUIRE(near(y_of(box.transform()), 1.5f, 0.05f));
    // Already touching: turned on, the pair lets go.
    REQUIRE_FALSE(constraint.set_enabled(true));
    rig.seconds(2.0);
    REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));
    REQUIRE_FALSE(constraint.set_enabled(false));
    REQUIRE_FALSE(box.set_transform(at(0.f, 3.f, 0.f)));
    rig.seconds(2.0);
    REQUIRE(near(y_of(box.transform()), 1.5f, 0.05f));
}

TEST_CASE("NC4 it is Active only between two bodies, and joins no assembly", "[physics][no-collision]") {
    SimRole role;
    engine_core::Game game;
    PhysicsObject& a = game.create<PhysicsObject>();
    game.set_parent(a.id(), workspace_of(game));
    PhysicsObject& b = game.create<PhysicsObject>();
    game.set_parent(b.id(), workspace_of(game));
    engine_core::GameObject& plain = create_part(game);
    NoCollisionConstraint& constraint = no_collision(game, a.id(), b.id());
    REQUIRE(active(game, constraint));
    REQUIRE(game.assemblies().group_of(a.id()) == -1);
    REQUIRE(game.assemblies().root_of(game, b.id()) == b.id());
    REQUIRE_FALSE(constraint.set_enabled(false));
    REQUIRE_FALSE(active(game, constraint));
    REQUIRE_FALSE(constraint.set_enabled(true));
    REQUIRE_FALSE(constraint.set_pv_instance2(instance_slot(plain.id())));
    REQUIRE_FALSE(active(game, constraint));
    REQUIRE_FALSE(constraint.set_pv_instance2(instance_slot(a.id())));
    REQUIRE_FALSE(active(game, constraint));
    REQUIRE_FALSE(constraint.set_pv_instance2(engine_core::LuaSlot{}));
    REQUIRE_FALSE(active(game, constraint));
}

TEST_CASE("NC5 a saved NoCollisionConstraint loads with its ends and Enabled", "[physics][no-collision]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        PhysicsObject& a = game.create<PhysicsObject>();
        game.set_name(a.id(), "A");
        game.set_parent(a.id(), workspace_of(game));
        PhysicsObject& b = game.create<PhysicsObject>();
        game.set_name(b.id(), "B");
        game.set_parent(b.id(), workspace_of(game));
        NoCollisionConstraint& constraint = no_collision(game, a.id(), b.id());
        game.set_name(constraint.id(), "Apart");
        REQUIRE_FALSE(constraint.set_enabled(false));
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    const InstanceId found = game.find_first_child(workspace_of(game), "Apart");
    auto* constraint = dynamic_cast<NoCollisionConstraint*>(game.instance(found));
    REQUIRE(constraint != nullptr);
    REQUIRE(constraint->pv_instance1_id() == game.find_first_child(workspace_of(game), "A"));
    REQUIRE(constraint->pv_instance2_id() == game.find_first_child(workspace_of(game), "B"));
    REQUIRE_FALSE(constraint->enabled());
}
