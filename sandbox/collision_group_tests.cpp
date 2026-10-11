// CollisionGroups: which groups PhysicsService has registered, their types,
// and whether two collide (docs/superpowers/specs/2026-10-11-collision-groups-design.md).

#include "CollisionGroups.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine_core::CollisionGroups;
using Type = CollisionGroups::Type;

TEST_CASE("CG1 Default alone, Blacklist, collides with itself", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE(groups.registered("Default"));
    REQUIRE(groups.collidable("Default", "Default"));
    REQUIRE(groups.category("Default") == 1u);
    REQUIRE((groups.collides_with("Default") & 1u) == 1u);
    // An unregistered name counts as Default.
    REQUIRE(groups.category("Nope") == 1u);
    REQUIRE(groups.collidable("Nope", "Default"));
}

TEST_CASE("CG2 two Blacklist groups collide unless set not to, both ways", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", Type::Blacklist));
    REQUIRE_FALSE(groups.register_group("B", Type::Blacklist));
    REQUIRE(groups.collidable("A", "B"));
    REQUIRE_FALSE(groups.set_collidable("A", "B", false));
    REQUIRE_FALSE(groups.collidable("A", "B"));
    REQUIRE_FALSE(groups.collidable("B", "A"));
    REQUIRE((groups.collides_with("A") & groups.category("B")) == 0u);
    REQUIRE((groups.collides_with("A") & groups.category("Default")) != 0u);
}

TEST_CASE("CG3 a Whitelist group touches only what it is set to, including groups made later",
          "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("Ghost", Type::Whitelist));
    REQUIRE_FALSE(groups.collidable("Ghost", "Default"));
    REQUIRE_FALSE(groups.set_collidable("Ghost", "Default", true));
    REQUIRE(groups.collidable("Ghost", "Default"));
    REQUIRE_FALSE(groups.register_group("Later", Type::Blacklist));
    REQUIRE_FALSE(groups.collidable("Ghost", "Later"));
    REQUIRE_FALSE(groups.collidable("Ghost", "Ghost"));
}

TEST_CASE("CG4 a type change keeps explicit settings; lists are derived", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", Type::Blacklist));
    REQUIRE_FALSE(groups.register_group("B", Type::Blacklist));
    REQUIRE_FALSE(groups.register_group("C", Type::Blacklist));
    REQUIRE_FALSE(groups.set_collidable("A", "B", false));
    REQUIRE_FALSE(groups.set_collidable("A", "C", true));
    auto list_of = [&](const char* name) {
        for (const CollisionGroups::Info& info : groups.groups()) {
            if (info.name == name) {
                return info.list;
            }
        }
        return std::vector<std::string>{"<missing>"};
    };
    REQUIRE(list_of("A") == std::vector<std::string>{"B"});
    REQUIRE_FALSE(groups.set_type("A", Type::Whitelist));
    REQUIRE_FALSE(groups.collidable("A", "B"));
    REQUIRE(groups.collidable("A", "C"));
    REQUIRE_FALSE(groups.collidable("A", "Default"));
    REQUIRE(list_of("A") == std::vector<std::string>{"C"});
    const std::vector<CollisionGroups::Info> all = groups.groups();
    REQUIRE(all.size() == 4);
    REQUIRE(all[0].name == "Default");
    REQUIRE(all[3].name == "C");
}

TEST_CASE("CG5 refusals change nothing", "[collision-group]") {
    CollisionGroups groups;
    const std::uint64_t before = groups.revision();
    REQUIRE(groups.register_group("", Type::Blacklist) == std::string("Collision group name must not be empty"));
    REQUIRE(groups.unregister_group("Default") == std::string("Default cannot be unregistered"));
    REQUIRE(groups.set_type("X", Type::Whitelist) ==
            std::string("Unknown collision group \"X\"; did you forget to call RegisterCollisionGroup?"));
    REQUIRE(groups.set_collidable("Default", "X", false) ==
            std::string("Unknown collision group \"X\"; did you forget to call RegisterCollisionGroup?"));
    REQUIRE(groups.revision() == before);
    for (int i = 1; i < CollisionGroups::kMaxGroups; ++i) {
        REQUIRE_FALSE(groups.register_group("G" + std::to_string(i), Type::Blacklist));
    }
    REQUIRE(groups.register_group("One too many", Type::Blacklist) ==
            std::string("Too many collision groups (the most is 62)"));
    // Registering an existing name again changes nothing, not even its type.
    REQUIRE_FALSE(groups.register_group("G1", Type::Whitelist));
    REQUIRE(groups.collidable("G1", "Default"));
}

TEST_CASE("CG6 unregistering frees the bit and its settings; reset leaves Default", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", Type::Blacklist));
    const std::uint64_t a_bit = groups.category("A");
    REQUIRE_FALSE(groups.set_collidable("A", "Default", false));
    REQUIRE_FALSE(groups.unregister_group("A"));
    REQUIRE_FALSE(groups.registered("A"));
    REQUIRE(groups.category("A") == 1u);
    REQUIRE_FALSE(groups.register_group("B", Type::Blacklist));
    REQUIRE(groups.category("B") == a_bit);
    REQUIRE(groups.collidable("B", "Default"));
    const std::uint64_t before = groups.revision();
    groups.reset();
    REQUIRE(groups.revision() != before);
    REQUIRE(groups.groups().size() == 1);
}
