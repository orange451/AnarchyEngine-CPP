// Core: the studio's own service, outside the place.

#include "support.hpp"

#include "Contract.hpp"
#include "Folder.hpp"
#include "Project.hpp"
#include "SceneService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using engine_core::ContractViolation;
using engine_core::InstanceId;

InstanceId add_folder(engine_core::DataModel& game, const char* name, InstanceId parent) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    if (parent != engine_core::DataModel::kNoParent) {
        game.set_parent(folder.id(), parent);
    }
    return folder.id();
}

}  // namespace

TEST_CASE("CO9 a Game holds Core last, hidden from the explorer, and it cannot be moved, renamed, or destroyed",
          "[CO9]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    REQUIRE(core != 0);
    REQUIRE(game.parent(core) == 0);
    REQUIRE(game.get_children(0).back() == core);
    REQUIRE(std::string(game.instance(core)->class_name()) == "Core");
    REQUIRE(game.name(core) == "Core");
    REQUIRE(game.guid(core) == "core");
    REQUIRE(game.instance(core)->hidden_in_explorer());
    REQUIRE(game.instance(core)->is_service());
    REQUIRE_FALSE(game.instance(core)->is_scene_service());
    REQUIRE(game.scene_service("Core") == 0);
    REQUIRE_THROWS_AS(game.set_parent(core, game.scene_service("Workspace")), ContractViolation);
    REQUIRE_THROWS_AS(game.set_name(core, "Tools"), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy(core), ContractViolation);
    // What Workspace may hold, Core may hold.
    REQUIRE_NOTHROW(add_folder(game, "Tools", core));
}

TEST_CASE("CO2a Core's descendants are in Core, and Core holds itself and them", "[CO2a]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId inner = add_folder(game, "Inner", tools);
    const InstanceId place = add_folder(game, "Place", game.scene_service("Workspace"));
    REQUIRE(game.in_core(tools));
    REQUIRE(game.in_core(inner));
    REQUIRE_FALSE(game.in_core(core));
    REQUIRE(game.core_holds(core));
    REQUIRE(game.core_holds(inner));
    REQUIRE_FALSE(game.core_holds(place));
    REQUIRE_FALSE(game.core_holds(0));
    REQUIRE(game.in_game(inner));
    REQUIRE_FALSE(game.in_workspace(inner));
}

TEST_CASE("CO5 nothing moves across Core's edge, but an instance with no parent may go in", "[CO5]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId other = add_folder(game, "Other", core);
    const InstanceId part = add_folder(game, "Part", workspace);
    // Out of Core, to the place or to no parent.
    REQUIRE(game.parent_error(tools, workspace).has_value());
    REQUIRE(game.parent_error(tools, engine_core::DataModel::kNoParent).has_value());
    REQUIRE_THROWS_AS(game.set_parent(tools, workspace), ContractViolation);
    // From the place into Core.
    REQUIRE(game.parent_error(part, core).has_value());
    REQUIRE_THROWS_AS(game.set_parent(part, tools), ContractViolation);
    // Within Core.
    REQUIRE_NOTHROW(game.set_parent(tools, other));
    // An instance with no parent, including one that left the place.
    const InstanceId loose = add_folder(game, "Loose", engine_core::DataModel::kNoParent);
    REQUIRE_NOTHROW(game.set_parent(loose, core));
    game.set_parent(part, engine_core::DataModel::kNoParent);
    REQUIRE_NOTHROW(game.set_parent(part, core));
    REQUIRE(game.in_core(part));
}

TEST_CASE("CO1 New and Open leave Core's instances with their ids, names, and children", "[CO1][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId inner = add_folder(game, "Inner", tools);

    engine_core::Project::reset_place(game);
    REQUIRE(game.core() == core);
    REQUIRE(game.alive(tools));
    REQUIRE(game.parent(inner) == tools);
    REQUIRE(game.name(inner) == "Inner");

    engine_core::Project reopened = engine_core::Project::load(dir.path, game);
    REQUIRE(game.core() == core);
    REQUIRE(game.alive(inner));
    REQUIRE(game.parent(tools) == core);
    REQUIRE(game.get_children(0).back() == core);
}

TEST_CASE("CO2 what changes in Core during play is still there after Stop", "[CO2]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId tools = add_folder(game, "Tools", core);
    const InstanceId place = add_folder(game, "Place", game.scene_service("Workspace"));
    game.start_simulation();
    game.set_name(tools, "Renamed");
    const InstanceId made = add_folder(game, "Made", core);
    game.set_name(place, "PlayName");
    const InstanceId play_only = add_folder(game, "PlayOnly", game.scene_service("Workspace"));
    game.stop_simulation();
    REQUIRE(game.name(tools) == "Renamed");
    REQUIRE(game.alive(made));
    REQUIRE(game.parent(made) == core);
    REQUIRE(game.in_core(made));
    // The place outside Core is restored as before.
    REQUIRE(game.name(place) == "Place");
    REQUIRE_FALSE(game.alive(play_only));
    REQUIRE(game.get_children(0).back() == core);
}

TEST_CASE("CO2b a Core instance made after a captured one is destroyed in play survives Stop", "[CO2b]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId core = game.core();
    const InstanceId doomed = add_folder(game, "Doomed", game.scene_service("Workspace"));
    game.start_simulation();
    game.destroy(doomed);
    const InstanceId made = add_folder(game, "Made", core);
    game.stop_simulation();
    REQUIRE(game.alive(made));
    REQUIRE(game.parent(made) == core);
    REQUIRE(game.alive(doomed));
    REQUIRE(game.name(doomed) == "Doomed");
}

TEST_CASE("CO3 Core is never saved and never makes the place unsaved", "[CO3][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    REQUIRE_FALSE(project.unsaved());
    const std::uint64_t before = engine_core::Project::place_fingerprint(game);
    add_folder(game, "Tools", game.core());
    REQUIRE_FALSE(project.unsaved());
    REQUIRE(engine_core::Project::place_fingerprint(game) == before);
    project.save();

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    REQUIRE(other.get_children(other.core()).empty());
}
