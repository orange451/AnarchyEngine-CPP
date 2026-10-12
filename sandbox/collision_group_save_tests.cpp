// Collision groups saved with the place, as Workspace's hidden CollisionGroups
// property, and RenameCollisionGroup
// (docs/superpowers/specs/2026-10-12-collision-groups-editor-design.md).

#include "support.hpp"

#include "CollisionGroups.hpp"
#include "LuaApi.hpp"
#include "PhysicsObject.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::CollisionGroups;
using engine_core::InstanceId;
using engine_core::JsonValue;
using engine_core::PhysicsObject;
using FilterType = CollisionGroups::FilterType;

bool has(const std::string& output, const std::string& text) { return output.find(text) != std::string::npos; }

engine_core::DataModel& workspace(engine_core::DataModel& game) {
    return *game.instance(game.scene_service("Workspace"));
}

std::string run(ScriptRig& rig, const char* source) {
    rig.runtime.drain_output();
    rig.runtime.run_chunk(source);
    rig.frames(1);
    std::string out;
    for (const auto& line : rig.runtime.drain_output().lines) {
        out += line.text;
    }
    return out;
}

PhysicsObject& body(engine_core::DataModel& game, const char* name, const char* group, const char* service) {
    PhysicsObject& object = game.create<PhysicsObject>();
    game.set_name(object.id(), name);
    REQUIRE_FALSE(object.set_collision_group(group));
    game.set_parent(object.id(), game.scene_service(service));
    return object;
}

}  // namespace

TEST_CASE("CGS1 Workspace saves the groups as its hidden CollisionGroups, and a load brings them back",
          "[collision-group][editor]") {
    const engine_core::LuaField* field = engine_core::lua_class_find("Workspace", "CollisionGroups");
    REQUIRE(field != nullptr);
    REQUIRE(field->saved);
    REQUIRE(field->hidden);

    JsonValue saved;
    {
        SimRole role;
        engine_core::Game game;
        engine_core::PropertyBag plain;
        workspace(game).save_properties(plain);
        // Default alone saves nothing.
        REQUIRE(engine_core::bag_find(plain, "CollisionGroups") == nullptr);

        REQUIRE_FALSE(game.collision_groups().register_group("Crates", FilterType::Include));
        REQUIRE_FALSE(game.collision_groups().set_collidable("Crates", "Default", true));
        engine_core::PropertyBag bag;
        workspace(game).save_properties(bag);
        const JsonValue* value = engine_core::bag_find(bag, "CollisionGroups");
        REQUIRE(value != nullptr);
        // Saved as the table itself, not a string holding it, so a file diffs by group.
        REQUIRE(value->is_object());
        REQUIRE(engine_core::compact_json(*value) == game.collision_groups().save_text());
        saved = *value;
    }
    SimRole role;
    engine_core::Game game;
    std::string error;
    REQUIRE(workspace(game).load_property("CollisionGroups", saved, error));
    REQUIRE(error.empty());
    REQUIRE(game.collision_groups().registered("Crates"));
    REQUIRE(game.collision_groups().collidable("Crates", "Default"));
    // The table as a string, as a hand-edited file may hold it, loads too.
    REQUIRE(workspace(game).load_property("CollisionGroups", JsonValue::string(engine_core::compact_json(saved)), error));
    REQUIRE(error.empty());
    REQUIRE(game.collision_groups().registered("Crates"));
    // Text that is no table never stops a place loading: it changes nothing.
    REQUIRE(workspace(game).load_property("CollisionGroups", JsonValue::string("nope"), error));
    REQUIRE(error.empty());
    REQUIRE(game.collision_groups().registered("Crates"));
}

TEST_CASE("CGS5 a table a merge damaged still loads, keeping what is sound", "[collision-group][editor]") {
    SimRole role;
    engine_core::Game game;
    // Both branches added Ghosts; one deleted X while the other set a pair with it.
    JsonValue table;
    std::string error;
    REQUIRE(engine_core::parse_json(
        "{\"groups\":[[\"Default\",\"Exclude\"],[\"Ghosts\",\"Include\"],[\"Ghosts\",\"Include\"],"
        "[\"\",\"Exclude\"],[\"Odd\",\"Greylist\"]],"
        "\"pairs\":[[\"Ghosts\",\"Default\",true],[\"Ghosts\",\"X\",false],[\"Ghosts\"]]}",
        table, error));
    REQUIRE(workspace(game).load_property("CollisionGroups", table, error));
    REQUIRE(error.empty());
    CollisionGroups& groups = game.collision_groups();
    REQUIRE(groups.groups().size() == 3);
    REQUIRE(groups.registered("Ghosts"));
    REQUIRE(groups.collidable("Ghosts", "Default"));
    // A filter type no one knows is Exclude.
    REQUIRE(groups.registered("Odd"));
    REQUIRE(groups.collidable("Odd", "Default"));
}

TEST_CASE("CGS2 a PhysicsService change inside a recording is one undo step, and fires Changed",
          "[collision-group][editor]") {
    ScriptRig rig;
    const std::optional<std::string> recording = rig.game.history().try_begin_recording("Groups");
    REQUIRE(recording);
    const std::string out = run(rig, R"(
        workspace.Changed:Connect(function(name) print("changed", name) end)
        local P = game:GetService("PhysicsService")
        P:RegisterCollisionGroup("A")
        P:CollisionGroupSetCollidable("A", "Default", false)
    )");
    rig.game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    INFO(out);
    REQUIRE(has(out, "changed\tCollisionGroups"));
    REQUIRE(rig.game.history().dirty());
    CollisionGroups& groups = rig.game.collision_groups();
    REQUIRE(groups.registered("A"));
    REQUIRE_FALSE(groups.collidable("A", "Default"));
    rig.game.history().undo();
    REQUIRE_FALSE(groups.registered("A"));
    rig.game.history().redo();
    REQUIRE(groups.registered("A"));
    REQUIRE_FALSE(groups.collidable("A", "Default"));
}

TEST_CASE("CGS3 Play starts with the place's groups, and Stop puts them back after a script changes them",
          "[collision-group][editor]") {
    ScriptRig rig;
    CollisionGroups& groups = rig.game.collision_groups();
    REQUIRE_FALSE(groups.register_group("Authored", FilterType::Include));
    rig.game.capture_place();
    rig.game.start_simulation();
    REQUIRE(groups.registered("Authored"));
    const std::string out = run(rig, R"(
        local P = game:GetService("PhysicsService")
        P:RegisterCollisionGroup("PlayOnly")
        P:UnregisterCollisionGroup("Authored")
        print(P:IsCollisionGroupRegistered("PlayOnly"), P:IsCollisionGroupRegistered("Authored"))
    )");
    INFO(out);
    REQUIRE(has(out, "true\tfalse"));
    rig.game.stop_simulation();
    REQUIRE(groups.registered("Authored"));
    REQUIRE_FALSE(groups.registered("PlayOnly"));
    REQUIRE_FALSE(groups.collidable("Authored", "Default"));
}

TEST_CASE("CGS4 RenameCollisionGroup keeps the group's settings and moves its bodies, as one undo step",
          "[collision-group][editor][lua]") {
    ScriptRig rig;
    PhysicsObject& a = body(rig.game, "A", "Old", "Workspace");
    PhysicsObject& b = body(rig.game, "B", "Old", "Storage");
    PhysicsObject& c = body(rig.game, "C", "Other", "Workspace");
    REQUIRE_FALSE(rig.game.collision_groups().register_group("Old", FilterType::Include));
    REQUIRE_FALSE(rig.game.collision_groups().set_collidable("Old", "Default", true));
    const std::optional<std::string> recording = rig.game.history().try_begin_recording("Rename");
    REQUIRE(recording);
    const std::string out = run(rig, R"(
        local P = game:GetService("PhysicsService")
        P:RenameCollisionGroup("Old", "New")
        print(P:IsCollisionGroupRegistered("Old"), P:IsCollisionGroupRegistered("New"),
            P:CollisionGroupsAreCollidable("New", "Default"), P:CollisionGroupsAreCollidable("New", "New"))
        print(pcall(function() P:RenameCollisionGroup("Default", "X") end))
        print(pcall(function() P:RenameCollisionGroup("New", "Default") end))
        print(pcall(function() P:RenameCollisionGroup("Nope", "X") end))
        print(pcall(function() P:RenameCollisionGroup("New", "") end))
    )");
    rig.game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    INFO(out);
    REQUIRE(has(out, "false\ttrue\ttrue\tfalse\n"));
    REQUIRE(has(out, "Default cannot be renamed"));
    REQUIRE(has(out, "A collision group named \"Default\" already exists"));
    REQUIRE(has(out, "Unknown collision group \"Nope\""));
    REQUIRE(has(out, "Collision group name must not be empty"));
    REQUIRE(a.collision_group() == "New");
    REQUIRE(b.collision_group() == "New");
    REQUIRE(c.collision_group() == "Other");

    rig.game.history().undo();
    REQUIRE(rig.game.collision_groups().registered("Old"));
    REQUIRE_FALSE(rig.game.collision_groups().registered("New"));
    REQUIRE(a.collision_group() == "Old");
    REQUIRE(b.collision_group() == "Old");
}
