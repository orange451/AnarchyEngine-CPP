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
