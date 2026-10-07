// PhysicsWorld::raycast, stopped and playing.

#include "physics_rig.hpp"
#include "support.hpp"

#include "Folder.hpp"
#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using engine_core::PhysicsObject;
using engine_core::RayFilter;
using engine_core::Vec3;

using namespace physics_rig;

}  // namespace

TEST_CASE("R1 a ray down hits the floor's top, stopped", "[physics][raycast]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    const auto hit = rig.physics.raycast(rig.game, Vec3{1.f, 10.f, 2.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{});
    REQUIRE(hit.has_value());
    REQUIRE(hit->instance == floor.id());
    REQUIRE(near(hit->position.y, 0.f, 1e-3f));
    REQUIRE(near(hit->normal.y, 1.f, 1e-3f));
    REQUIRE(near(hit->distance, 10.f, 1e-3f));
    REQUIRE_FALSE(hit->has_material);
}

TEST_CASE("R2 a ray too short, or with no length, hits nothing", "[physics][raycast]") {
    PhysicsRig rig;
    rig.floor();
    REQUIRE_FALSE(rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -5.f, 0.f}, RayFilter{}));
    REQUIRE_FALSE(rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{}, RayFilter{}));
}

TEST_CASE("R3 Exclude and Include filter whole subtrees", "[physics][raycast]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(folder.id(), workspace_of(rig.game));
    engine_core::Folder& inner = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(inner.id(), folder.id());
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f}, true, inner.id());
    const Vec3 origin{0.f, 10.f, 0.f};
    const Vec3 down{0.f, -20.f, 0.f};

    REQUIRE(rig.physics.raycast(rig.game, origin, down, RayFilter{})->instance == box.id());

    RayFilter exclude;
    exclude.instances = {folder.id()};
    REQUIRE(rig.physics.raycast(rig.game, origin, down, exclude)->instance == floor.id());

    RayFilter include;
    include.include = true;
    include.instances = {floor.id()};
    REQUIRE(rig.physics.raycast(rig.game, origin, down, include)->instance == floor.id());

    include.instances = {};
    REQUIRE_FALSE(rig.physics.raycast(rig.game, origin, down, include));
}

TEST_CASE("R4 a ray that starts inside a box sees past it", "[physics][raycast]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    rig.body(at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f}, true);
    const auto hit = rig.physics.raycast(rig.game, Vec3{0.f, 5.f, 0.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{});
    REQUIRE(hit.has_value());
    REQUIRE(hit->instance == floor.id());
}

TEST_CASE("R5 a ray sees a Transform set just before it, and works while playing", "[physics][raycast]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f}, true);
    rig.physics.sync(rig.game);
    REQUIRE_FALSE(box.set_transform(at(10.f, 5.f, 0.f)));
    auto hit = rig.physics.raycast(rig.game, Vec3{10.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{});
    REQUIRE(hit->instance == box.id());

    rig.play();
    rig.steps(1);
    hit = rig.physics.raycast(rig.game, Vec3{10.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{});
    REQUIRE(hit->instance == box.id());
    REQUIRE(near(hit->position.y, 6.f, 1e-3f));
}
