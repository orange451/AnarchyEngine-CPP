// BillboardGui in the engine: how the snapshot finds and publishes it.

#include "support.hpp"

#include "GameObject.hpp"
#include "Gui.hpp"
#include "Matrix4.hpp"
#include "Project.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

TEST_CASE("BB1 DataModel lists every BillboardGui under game, and no other class", "[billboard]") {
    SimRole role;
    engine_core::Game game;
    std::vector<engine_core::InstanceId> found;
    game.billboards(found);
    REQUIRE(found.empty());
    engine_core::BillboardGui& a = game.create<engine_core::BillboardGui>();
    engine_core::BillboardGui& b = game.create<engine_core::BillboardGui>();
    engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
    game.set_parent(a.id(), game.scene_service("Workspace"));
    game.set_parent(b.id(), game.scene_service("Storage"));
    game.set_parent(screen.id(), game.scene_service("Gui"));
    game.billboards(found);
    std::sort(found.begin(), found.end());
    std::vector<engine_core::InstanceId> expected{a.id(), b.id()};
    std::sort(expected.begin(), expected.end());
    REQUIRE(found == expected);
    game.set_parent(b.id(), engine_core::DataModel::kNoParent);
    game.billboards(found);
    REQUIRE(found == std::vector<engine_core::InstanceId>{a.id()});

    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& saved = project.datamodel();
        engine_core::BillboardGui& board = saved.create<engine_core::BillboardGui>();
        saved.set_parent(board.id(), saved.scene_service("Workspace"));
        project.save();
    }
    engine_core::Game loadedGame;
    engine_core::Project loaded = engine_core::Project::load(dir.path, loadedGame);
    loadedGame.billboards(found);
    REQUIRE(found.size() == 1);
}
