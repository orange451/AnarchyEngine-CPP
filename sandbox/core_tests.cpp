// Core: the studio's own service, outside the place.

#include "support.hpp"

#include "Contract.hpp"
#include "Folder.hpp"
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
