// Lighting.Antialiasing: how the 3D scene's edges are smoothed, an
// Enum.AntialiasingMode the render snapshot carries.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "Enum.hpp"
#include "Lighting.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::AntialiasingMode;
using engine_core::Lighting;

Lighting& lighting_in(engine_core::Game& game) {
    auto* lighting = dynamic_cast<Lighting*>(game.instance(game.scene_service("Lighting")));
    REQUIRE(lighting != nullptr);
    return *lighting;
}

}  // namespace

TEST_CASE("AA1 Enum.AntialiasingMode has None and FXAA", "[antialiasing]") {
    const engine_core::EnumType& type = engine_core::antialiasing_mode_enum();
    REQUIRE(engine_core::enum_item_value(type, "None") == 0);
    REQUIRE(engine_core::enum_item_value(type, "FXAA") == 1);
    REQUIRE(engine_core::enum_item_name(type, 2) == nullptr);
}

TEST_CASE("AA2 Lighting.Antialiasing defaults to FXAA, undoes, saves, and comes back at Stop", "[antialiasing]") {
    SimRole role;
    engine_core::Game game;
    Lighting& lighting = lighting_in(game);
    REQUIRE(lighting.antialiasing() == AntialiasingMode::FXAA);

    // The default is not saved, so places that already exist get FXAA.
    engine_core::PropertyBag saved;
    lighting.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Antialiasing") == nullptr);

    begin_step(game, "Set Antialiasing");
    REQUIRE_FALSE(lighting.set_antialiasing(0));
    end_step(game);
    REQUIRE(lighting.antialiasing() == AntialiasingMode::None);
    game.history().undo();
    REQUIRE(lighting.antialiasing() == AntialiasingMode::FXAA);
    game.history().redo();
    REQUIRE(lighting.antialiasing() == AntialiasingMode::None);

    REQUIRE(*lighting.set_antialiasing(7) == "Antialiasing must be an Enum.AntialiasingMode");
    REQUIRE(lighting.antialiasing() == AntialiasingMode::None);

    // What save writes, a load reads back.
    saved.clear();
    lighting.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Antialiasing") != nullptr);
    engine_core::Game other;
    Lighting& loaded = lighting_in(other);
    for (const auto& member : saved) {
        std::string error;
        INFO(member.first);
        REQUIRE(loaded.load_property(member.first, member.second, error));
        REQUIRE(error.empty());
    }
    REQUIRE(loaded.antialiasing() == AntialiasingMode::None);

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(lighting.set_antialiasing(1));
    game.stop_simulation();
    REQUIRE(lighting.antialiasing() == AntialiasingMode::None);
}

TEST_CASE("AA3 the snapshot carries Lighting.Antialiasing", "[antialiasing][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    REQUIRE(pump.front().lighting.antialiasing == 1);
    REQUIRE_FALSE(lighting_in(game).set_antialiasing(0));
    frame();
    REQUIRE(pump.front().lighting.antialiasing == 0);
}

TEST_CASE("AA4 scripts read and set Lighting.Antialiasing", "[antialiasing]") {
    ScriptRig rig;
    add_script(rig.game, "Smooth", R"(
        _G.default = game.Lighting.Antialiasing == Enum.AntialiasingMode.FXAA
        game.Lighting.Antialiasing = Enum.AntialiasingMode.None
        _G.set = game.Lighting.Antialiasing == Enum.AntialiasingMode.None
        -- As every enum property does, it also takes an item's value or name.
        game.Lighting.Antialiasing = 1
        _G.by_value = game.Lighting.Antialiasing == Enum.AntialiasingMode.FXAA
        game.Lighting.Antialiasing = "None"
        _G.by_name = game.Lighting.Antialiasing == Enum.AntialiasingMode.None
        _G.refused = not pcall(function() game.Lighting.Antialiasing = Enum.TransformSpace.World end)
            and not pcall(function() game.Lighting.Antialiasing = "Blurry" end)
            and not pcall(function() game.Lighting.Antialiasing = 7 end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"default", "set", "by_value", "by_name", "refused"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}
