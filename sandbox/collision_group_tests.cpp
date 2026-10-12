// CollisionGroups: which groups PhysicsService has registered, their types,
// and whether two collide (docs/superpowers/specs/2026-10-11-collision-groups-design.md).

#include "CollisionGroups.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine_core::CollisionGroups;
using FilterType = CollisionGroups::FilterType;

TEST_CASE("CG1 Default alone, Exclude, collides with itself", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE(groups.registered("Default"));
    REQUIRE(groups.collidable("Default", "Default"));
    REQUIRE(groups.category("Default") == 1u);
    REQUIRE((groups.collides_with("Default") & 1u) == 1u);
    // An unregistered name counts as Default.
    REQUIRE(groups.category("Nope") == 1u);
    REQUIRE(groups.collidable("Nope", "Default"));
}

TEST_CASE("CG2 two Exclude groups collide unless set not to, both ways", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", FilterType::Exclude));
    REQUIRE_FALSE(groups.register_group("B", FilterType::Exclude));
    REQUIRE(groups.collidable("A", "B"));
    REQUIRE_FALSE(groups.set_collidable("A", "B", false));
    REQUIRE_FALSE(groups.collidable("A", "B"));
    REQUIRE_FALSE(groups.collidable("B", "A"));
    REQUIRE((groups.collides_with("A") & groups.category("B")) == 0u);
    REQUIRE((groups.collides_with("A") & groups.category("Default")) != 0u);
}

TEST_CASE("CG3 an Include group touches only what it is set to, including groups made later",
          "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("Ghost", FilterType::Include));
    REQUIRE_FALSE(groups.collidable("Ghost", "Default"));
    REQUIRE_FALSE(groups.set_collidable("Ghost", "Default", true));
    REQUIRE(groups.collidable("Ghost", "Default"));
    REQUIRE_FALSE(groups.register_group("Later", FilterType::Exclude));
    REQUIRE_FALSE(groups.collidable("Ghost", "Later"));
    REQUIRE_FALSE(groups.collidable("Ghost", "Ghost"));
}

TEST_CASE("CG4 a filter type change keeps explicit settings; lists are derived", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", FilterType::Exclude));
    REQUIRE_FALSE(groups.register_group("B", FilterType::Exclude));
    REQUIRE_FALSE(groups.register_group("C", FilterType::Exclude));
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
    REQUIRE_FALSE(groups.set_filter_type("A", FilterType::Include));
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
    REQUIRE(groups.register_group("", FilterType::Exclude) == std::string("Collision group name must not be empty"));
    REQUIRE(groups.unregister_group("Default") == std::string("Default cannot be unregistered"));
    REQUIRE(groups.set_filter_type("X", FilterType::Include) ==
            std::string("Unknown collision group \"X\"; did you forget to call RegisterCollisionGroup?"));
    REQUIRE(groups.set_collidable("Default", "X", false) ==
            std::string("Unknown collision group \"X\"; did you forget to call RegisterCollisionGroup?"));
    REQUIRE(groups.revision() == before);
    for (int i = 1; i < CollisionGroups::kMaxGroups; ++i) {
        REQUIRE_FALSE(groups.register_group("G" + std::to_string(i), FilterType::Exclude));
    }
    REQUIRE(groups.register_group("One too many", FilterType::Exclude) ==
            std::string("Too many collision groups (the most is 62)"));
    // Registering an existing name again changes nothing, not even its type.
    REQUIRE_FALSE(groups.register_group("G1", FilterType::Include));
    REQUIRE(groups.collidable("G1", "Default"));
}

TEST_CASE("CG6 unregistering frees the bit and its settings; reset leaves Default", "[collision-group]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", FilterType::Exclude));
    const std::uint64_t a_bit = groups.category("A");
    REQUIRE_FALSE(groups.set_collidable("A", "Default", false));
    REQUIRE_FALSE(groups.unregister_group("A"));
    REQUIRE_FALSE(groups.registered("A"));
    REQUIRE(groups.category("A") == 1u);
    REQUIRE_FALSE(groups.register_group("B", FilterType::Exclude));
    REQUIRE(groups.category("B") == a_bit);
    REQUIRE(groups.collidable("B", "Default"));
    const std::uint64_t before = groups.revision();
    groups.reset();
    REQUIRE(groups.revision() != before);
    REQUIRE(groups.groups().size() == 1);
}

TEST_CASE("CG7 save_text is empty for Default alone, and load_text rebuilds the registry from it",
          "[collision-group][editor]") {
    CollisionGroups groups;
    REQUIRE(groups.save_text().empty());
    REQUIRE_FALSE(groups.register_group("Players", FilterType::Exclude));
    REQUIRE_FALSE(groups.register_group("Ghosts", FilterType::Include));
    REQUIRE_FALSE(groups.register_group("Debris", FilterType::Exclude));
    REQUIRE_FALSE(groups.set_collidable("Players", "Players", false));
    REQUIRE_FALSE(groups.set_collidable("Ghosts", "Players", true));
    REQUIRE_FALSE(groups.unregister_group("Debris"));
    REQUIRE_FALSE(groups.register_group("Crates", FilterType::Exclude));
    const std::string text = groups.save_text();
    REQUIRE(text ==
            "{\"groups\":[[\"Default\",\"Exclude\"],[\"Players\",\"Exclude\"],[\"Ghosts\",\"Include\"],"
            "[\"Crates\",\"Exclude\"]],\"pairs\":[[\"Players\",\"Players\",false],[\"Players\",\"Ghosts\",true]]}");

    CollisionGroups loaded;
    const std::uint64_t before = loaded.revision();
    REQUIRE_FALSE(loaded.load_text(text));
    REQUIRE(loaded.revision() > before);
    REQUIRE(loaded.save_text() == text);
    REQUIRE_FALSE(loaded.collidable("Players", "Players"));
    REQUIRE(loaded.collidable("Ghosts", "Players"));
    REQUIRE_FALSE(loaded.collidable("Ghosts", "Crates"));
    REQUIRE(loaded.collidable("Crates", "Default"));

    // Loading "" is Default alone again, and the revision still only grows.
    const std::uint64_t later = loaded.revision();
    REQUIRE_FALSE(loaded.load_text(""));
    REQUIRE(loaded.revision() > later);
    REQUIRE(loaded.groups().size() == 1);
    REQUIRE(loaded.save_text().empty());

    // Only Default's type changed: still saved.
    REQUIRE_FALSE(loaded.set_filter_type("Default", FilterType::Include));
    REQUIRE(loaded.save_text() == "{\"groups\":[[\"Default\",\"Include\"]],\"pairs\":[]}");
}

TEST_CASE("CG8 load_text refuses a malformed table and changes nothing", "[collision-group][editor]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("Kept", FilterType::Include));
    const std::string kept = groups.save_text();
    const std::uint64_t revision = groups.revision();
    for (const char* bad : {
             "nope",
             "[]",
             "{\"groups\":[]}",
             "{\"groups\":[[\"Players\",\"Exclude\"]],\"pairs\":[]}",
             "{\"groups\":[[\"Default\",\"Exclude\"],[\"\",\"Exclude\"]],\"pairs\":[]}",
             "{\"groups\":[[\"Default\",\"Exclude\"],[\"A\",\"Exclude\"],[\"A\",\"Include\"]],\"pairs\":[]}",
             "{\"groups\":[[\"Default\",\"Greylist\"]],\"pairs\":[]}",
             "{\"groups\":[[\"Default\",\"Exclude\"]],\"pairs\":[[\"Default\",\"Nope\",true]]}",
             "{\"groups\":[[\"Default\",\"Exclude\"]],\"pairs\":[[\"Default\",\"Default\",1]]}",
         }) {
        INFO(bad);
        REQUIRE(groups.load_text(bad));
        REQUIRE(groups.save_text() == kept);
        REQUIRE(groups.revision() == revision);
    }
    std::string many = "{\"groups\":[[\"Default\",\"Exclude\"]";
    for (int i = 0; i < CollisionGroups::kMaxGroups; ++i) {
        many += ",[\"G" + std::to_string(i) + "\",\"Exclude\"]";
    }
    many += "],\"pairs\":[]}";
    REQUIRE(groups.load_text(many));
    REQUIRE(groups.save_text() == kept);
}

TEST_CASE("CG9 rename_group keeps the bit, the type, the order, and the pair settings", "[collision-group][editor]") {
    CollisionGroups groups;
    REQUIRE_FALSE(groups.register_group("A", FilterType::Include));
    REQUIRE_FALSE(groups.register_group("B", FilterType::Exclude));
    REQUIRE_FALSE(groups.set_collidable("A", "B", true));
    REQUIRE_FALSE(groups.set_collidable("A", "A", true));
    const std::uint64_t bit = groups.category("A");
    const std::uint64_t revision = groups.revision();
    REQUIRE_FALSE(groups.rename_group("A", "Renamed"));
    REQUIRE(groups.revision() > revision);
    REQUIRE_FALSE(groups.registered("A"));
    REQUIRE(groups.category("Renamed") == bit);
    REQUIRE(groups.collidable("Renamed", "B"));
    REQUIRE(groups.collidable("Renamed", "Renamed"));
    REQUIRE_FALSE(groups.collidable("Renamed", "Default"));
    REQUIRE(groups.groups()[1].name == "Renamed");
    REQUIRE(groups.groups()[1].filter_type == FilterType::Include);

    const std::string before = groups.save_text();
    REQUIRE(*groups.rename_group("Default", "Base") == "Default cannot be renamed");
    REQUIRE(*groups.rename_group("Renamed", "B") == "A collision group named \"B\" already exists");
    REQUIRE(*groups.rename_group("Renamed", "") == "Collision group name must not be empty");
    REQUIRE(*groups.rename_group("Nope", "Other") ==
            "Unknown collision group \"Nope\"; did you forget to call RegisterCollisionGroup?");
    REQUIRE(groups.save_text() == before);
    // To its own name: nothing to do.
    REQUIRE_FALSE(groups.rename_group("B", "B"));
}
