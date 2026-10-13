// find_guid: the live instance with a GUID, found through an index the
// DataModel keeps as instances come and go, never by walking every slot.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;

InstanceId add_folder(engine_core::DataModel& game, const std::string& name, InstanceId parent) {
    engine_core::Folder& made = game.create<engine_core::Folder>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), parent);
    return made.id();
}

// find_guid finds what a walk of every slot (guid_index) finds, for every
// live GUID, and nothing for a GUID no instance has.
void agrees(const engine_core::DataModel& game) {
    for (const auto& [guid, id] : game.guid_index()) {
        const std::optional<InstanceId> found = game.find_guid(guid);
        REQUIRE(found.has_value());
        REQUIRE(*found == id);
    }
    REQUIRE_FALSE(game.find_guid("no-instance-has-this").has_value());
}

}  // namespace

TEST_CASE("GI1 find_guid finds each live instance by GUID through make, set_guid, destroy, undo, redo and Stop",
          "[guid]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId workspace = game.scene_service("Workspace");
    std::vector<InstanceId> made;
    for (int i = 0; i < 300; ++i) {
        made.push_back(add_folder(game, "F" + std::to_string(i), workspace));
    }
    agrees(game);

    // A GUID set by hand, as a load sets the one on disk: the old one finds nothing.
    const std::string old_guid = game.guid(made[0]);
    game.set_guid(made[0], "set-by-hand");
    REQUIRE(game.find_guid("set-by-hand") == made[0]);
    REQUIRE_FALSE(game.find_guid(old_guid).has_value());
    agrees(game);

    // Destroyed, its GUID finds nothing; undone, it finds it again; redone, nothing.
    const std::string doomed = game.guid(made[5]);
    begin_step(game, "Delete");
    game.destroy(made[5]);
    end_step(game);
    REQUIRE_FALSE(game.find_guid(doomed).has_value());
    agrees(game);
    game.history().undo();
    REQUIRE(game.find_guid(doomed).has_value());
    agrees(game);
    game.history().redo();
    REQUIRE_FALSE(game.find_guid(doomed).has_value());
    agrees(game);

    // A test: what play makes is gone at Stop, and what it destroys is back.
    const std::string kept = game.guid(made[7]);
    game.capture_place();
    game.start_simulation();
    const InstanceId played = add_folder(game, "Played", workspace);
    const std::string played_guid = game.guid(played);
    game.destroy(made[7]);
    REQUIRE(game.find_guid(played_guid) == played);
    REQUIRE_FALSE(game.find_guid(kept).has_value());
    agrees(game);
    game.stop_simulation();
    REQUIRE_FALSE(game.find_guid(played_guid).has_value());
    REQUIRE(game.find_guid(kept).has_value());
    agrees(game);
}

TEST_CASE("GI2 find_guid in a place of 200,000 instances does not walk them", "[guid]") {
    SimRole role;
    engine_core::Game game;
    std::vector<std::string> guids;
    guids.reserve(200000);
    for (int i = 0; i < 200000; ++i) {
        guids.push_back(game.guid(game.create<engine_core::Folder>().id()));
    }
    const auto start = std::chrono::steady_clock::now();
    std::size_t found = 0;
    for (std::size_t i = 0; i < 2000; ++i) {
        found += game.find_guid(guids[(i * 7919) % guids.size()]).has_value() ? 1 : 0;
    }
    found += game.find_guid("no-instance-has-this").has_value() ? 1 : 0;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(found == 2000);
    // A walk of 200,000 slots takes about a millisecond a lookup: these would take seconds.
    REQUIRE(ms < 200.0);
}
