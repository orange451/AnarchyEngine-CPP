// PhysicsService's collision groups and RaycastParams.CollisionGroup, from Luau.

#include "physics_rig.hpp"
#include "support.hpp"

#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

bool has(const std::string& output, const std::string& text) { return output.find(text) != std::string::npos; }

struct GroupsRig : ScriptRig {
    engine_core::PhysicsWorld physics;
    GroupsRig() { game.set_physics(&physics); }
    ~GroupsRig() { game.set_physics(nullptr); }

    std::string run(const char* source) {
        runtime.drain_output();
        runtime.run_chunk(source);
        frames(1);
        std::string out;
        for (const auto& line : runtime.drain_output().lines) {
            out += line.text;
        }
        return out;
    }
};

}  // namespace

TEST_CASE("CGL1 PhysicsService registers groups and answers whether they collide", "[collision-group][lua]") {
    GroupsRig rig;
    const std::string out = rig.run(R"(
        local P = game:GetService("PhysicsService")
        P:RegisterCollisionGroup("A")
        P:RegisterCollisionGroup("W", "Whitelist")
        print(P:IsCollisionGroupRegistered("A"), P:IsCollisionGroupRegistered("Nope"), P:GetMaxCollisionGroups())
        print(P:CollisionGroupsAreCollidable("A", "Default"), P:CollisionGroupsAreCollidable("W", "A"))
        P:CollisionGroupSetCollidableBulk("W", { "A", "Default" }, true)
        print(P:CollisionGroupsAreCollidable("W", "A"), P:CollisionGroupsAreCollidable("W", "Default"))
        P:SetCollisionGroupCollisionType("A", "Whitelist")
        local groups = P:GetRegisteredCollisionGroups()
        print(#groups, groups[1].name, groups[3].name, groups[3].type, table.concat(groups[3].list, ","))
        P:UnregisterCollisionGroup("A")
        print(P:IsCollisionGroupRegistered("A"))
    )");
    INFO(out);
    REQUIRE(has(out, "true\tfalse\t62\n"));
    REQUIRE(has(out, "true\tfalse\n"));
    REQUIRE(has(out, "true\ttrue\n"));
    // A group's list is in registration order, as the groups are.
    REQUIRE(has(out, "3\tDefault\tW\tWhitelist\tDefault,A\n"));
    REQUIRE(has(out, "false\n"));
}

TEST_CASE("CGL2 PhysicsService raises on bad calls", "[collision-group][lua]") {
    GroupsRig rig;
    const std::string out = rig.run(R"(
        local P = game:GetService("PhysicsService")
        print(pcall(function() P:SetCollisionGroupCollisionType("X", "Whitelist") end))
        print(pcall(function() P:RegisterCollisionGroup("Y", "Greylist") end))
        print(pcall(function() P:UnregisterCollisionGroup("Default") end))
    )");
    INFO(out);
    REQUIRE(has(out, "Unknown collision group \"X\"; did you forget to call RegisterCollisionGroup?"));
    REQUIRE(has(out, "collisionType must be \"Blacklist\" or \"Whitelist\""));
    REQUIRE(has(out, "Default cannot be unregistered"));
}

TEST_CASE("CGL3 RaycastParams.CollisionGroup filters what a ray hits", "[collision-group][lua][raycast]") {
    GroupsRig rig;
    PhysicsObject& floor = rig.game.create<PhysicsObject>();
    REQUIRE_FALSE(floor.set_transform(at(0.f, -0.5f, 0.f)));
    REQUIRE_FALSE(floor.set_size(Vec3{40.f, 1.f, 40.f}));
    floor.set_anchored(true);
    rig.game.set_name(floor.id(), "Floor");
    rig.game.set_parent(floor.id(), workspace_of(rig.game));
    PhysicsObject& box = rig.game.create<PhysicsObject>();
    REQUIRE_FALSE(box.set_transform(at(0.f, 3.f, 0.f)));
    REQUIRE_FALSE(box.set_size(Vec3{2.f, 2.f, 2.f}));
    box.set_anchored(true);
    REQUIRE_FALSE(box.set_collision_group("Boxes"));
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    const std::string out = rig.run(R"(
        local P = game:GetService("PhysicsService")
        P:RegisterCollisionGroup("Boxes")
        P:RegisterCollisionGroup("Rays")
        P:CollisionGroupSetCollidable("Boxes", "Rays", false)
        local p = RaycastParams.new()
        print(p.CollisionGroup)
        print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0), p).Instance.Name)
        p.CollisionGroup = "Rays"
        print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0), p).Instance.Name)
    )");
    INFO(out);
    REQUIRE(has(out, "Default\nBox\nFloor\n"));
}
