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

namespace {

const engine_core::VisualSnapshot& publish(engine_core::SnapshotPump& pump, engine_core::DataModel& game) {
    pump.prepare_copy(game);
    pump.publish();
    return pump.front();
}

engine_core::GameObject& part_at(engine_core::DataModel& game, float x, float y, float z) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_parent(part.id(), game.scene_service("Workspace"));
    part.set_transform(engine_core::matrix4_translation(x, y, z));
    return part;
}

engine_core::LuaSlot boolean(bool value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

}  // namespace

TEST_CASE("BB2 a drawn, visible BillboardGui publishes one row; others none", "[billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), game.scene_service("Storage"));
    REQUIRE(publish(pump, game).billboards.empty());

    game.set_parent(board.id(), game.scene_service("Workspace"));
    REQUIRE_FALSE(board.set_value(engine_core::GuiProperty::AlwaysOnTop, boolean(true)));
    {
        const engine_core::VisualSnapshot& shot = publish(pump, game);
        REQUIRE(shot.billboards.size() == 1);
        REQUIRE(shot.billboards[0].id == board.id());
        REQUIRE(shot.billboards[0].anchor_instance == 0);
        REQUIRE(shot.billboards[0].anchor.x == 0.f);
        REQUIRE(shot.billboards[0].always_on_top);
    }
    REQUIRE_FALSE(board.set_value(engine_core::GuiProperty::Visible, boolean(false)));
    REQUIRE(publish(pump, game).billboards.empty());
}

TEST_CASE("BB3 a billboard's anchor and its adornee's row come from the same frame", "[billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& part = part_at(game, 0.f, 0.f, -10.f);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), part.id());
    for (int step = 0; step < 20; ++step) {
        part.set_transform(engine_core::matrix4_translation(static_cast<float>(step), 1.f, -10.f));
        const engine_core::VisualSnapshot& shot = publish(pump, game);
        REQUIRE(shot.billboards.size() == 1);
        const engine_core::VisualInstance* row = nullptr;
        for (const engine_core::VisualInstance& inst : shot.instances) {
            if (inst.id == part.id()) {
                row = &inst;
            }
        }
        REQUIRE(row != nullptr);
        REQUIRE(shot.billboards[0].anchor_instance == part.id());
        REQUIRE(shot.billboards[0].anchor.x == row->world.m[12]);
        REQUIRE(shot.billboards[0].anchor.x == static_cast<float>(step));
    }
    // A path-C override moves the drawn row; the anchor follows it.
    // override_visual is only legal from the render thread inside its
    // window, so this borrows that role for the one call, the way SimRole
    // borrows the simulation role for the rest of the test.
    engine_core::SnapshotOverride moved;
    moved.id = part.id();
    moved.transform = engine_core::matrix4_translation(50.f, 0.f, 0.f);
    engine_core::set_thread_role(engine_core::ThreadRole::Render);
    pump.begin_prerender_window(game);
    pump.override_visual(moved);
    pump.end_prerender_window(game);
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    REQUIRE(publish(pump, game).billboards[0].anchor.x == 50.f);
}
