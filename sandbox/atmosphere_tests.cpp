// Atmosphere: the air under Lighting, whose density, height falloff, color,
// and haze the render snapshot carries for the Scene View.

#include "support.hpp"

#include "Atmosphere.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Folder.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"
#include "runner/AtmosphereMath.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace {

using engine_core::Atmosphere;
using engine_core::InstanceId;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

Atmosphere& add_atmosphere(engine_core::DataModel& game, InstanceId parent) {
    Atmosphere& air = game.create<Atmosphere>();
    game.set_parent(air.id(), parent);
    return air;
}

bool same(engine_core::ColorRgb a, engine_core::ColorRgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

}  // namespace

TEST_CASE("ATM1 an Atmosphere's properties are checked, undo, save, and come back at Stop", "[atmosphere]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("Atmosphere"));
    Atmosphere& air = add_atmosphere(game, game.scene_service("Lighting"));
    REQUIRE(air.enabled());
    REQUIRE(air.density() == 0.25);
    REQUIRE(air.height_falloff() == 0.01);
    REQUIRE(air.base_height() == 0.0);
    REQUIRE(same(air.color(), engine_core::ColorRgb{0.75f, 0.8f, 0.88f, 1.f}));
    REQUIRE(air.aerial_perspective() == 0.5);
    REQUIRE(air.sun_scatter() == 0.25);
    REQUIRE(air.sky_haze() == 1.0);

    // A default Atmosphere saves none of them.
    engine_core::PropertyBag saved;
    air.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set Density");
    REQUIRE_FALSE(air.set_density(0.6));
    end_step(game);
    REQUIRE(air.density() == 0.6);
    game.history().undo();
    REQUIRE(air.density() == 0.25);
    game.history().redo();
    REQUIRE(air.density() == 0.6);

    begin_step(game, "Set Color");
    REQUIRE_FALSE(air.set_color({1.f, 0.f, 0.f, 1.f}));
    end_step(game);
    REQUIRE(same(air.color(), {1.f, 0.f, 0.f, 1.f}));
    game.history().undo();
    REQUIRE(same(air.color(), Atmosphere::kDefaultColor));

    // Each bounded number is clamped to its range.
    REQUIRE_FALSE(air.set_density(5.0));
    REQUIRE(air.density() == Atmosphere::kMaxDensity);
    REQUIRE_FALSE(air.set_density(-1.0));
    REQUIRE(air.density() == 0.0);
    REQUIRE_FALSE(air.set_height_falloff(1.0));
    REQUIRE(air.height_falloff() == Atmosphere::kMaxHeightFalloff);
    REQUIRE_FALSE(air.set_height_falloff(-1.0));
    REQUIRE(air.height_falloff() == 0.0);
    REQUIRE_FALSE(air.set_aerial_perspective(2.0));
    REQUIRE(air.aerial_perspective() == 1.0);
    REQUIRE_FALSE(air.set_aerial_perspective(-2.0));
    REQUIRE(air.aerial_perspective() == 0.0);
    REQUIRE_FALSE(air.set_sun_scatter(2.0));
    REQUIRE(air.sun_scatter() == 1.0);
    REQUIRE_FALSE(air.set_sun_scatter(-2.0));
    REQUIRE(air.sun_scatter() == 0.0);
    REQUIRE_FALSE(air.set_sky_haze(2.0));
    REQUIRE(air.sky_haze() == 1.0);
    REQUIRE_FALSE(air.set_sky_haze(-2.0));
    REQUIRE(air.sky_haze() == 0.0);
    // BaseHeight takes any finite height.
    REQUIRE_FALSE(air.set_base_height(-1e6));
    REQUIRE(air.base_height() == -1e6);

    REQUIRE(*air.set_density(std::nan("")) == "Density must be a finite number");
    REQUIRE(*air.set_height_falloff(INFINITY) == "HeightFalloff must be a finite number");
    REQUIRE(*air.set_base_height(-INFINITY) == "BaseHeight must be a finite number");
    REQUIRE(*air.set_aerial_perspective(std::nan("")) == "AerialPerspective must be a finite number");
    REQUIRE(*air.set_sun_scatter(std::nan("")) == "SunScatter must be a finite number");
    REQUIRE(*air.set_sky_haze(std::nan("")) == "SkyHaze must be a finite number");
    REQUIRE(*air.set_color({INFINITY, 0.f, 0.f, 1.f}) == "Color must be finite");

    REQUIRE_FALSE(air.set_enabled(false));
    REQUIRE_FALSE(air.set_density(0.4));
    REQUIRE_FALSE(air.set_height_falloff(0.05));
    REQUIRE_FALSE(air.set_base_height(12.0));
    REQUIRE_FALSE(air.set_color({0.2f, 0.3f, 0.4f, 1.f}));
    REQUIRE_FALSE(air.set_aerial_perspective(0.7));
    REQUIRE_FALSE(air.set_sun_scatter(0.6));
    REQUIRE_FALSE(air.set_sky_haze(0.3));
    engine_core::PropertyBag changed;
    air.save_properties(changed);
    for (const char* name : {"Enabled", "Density", "HeightFalloff", "BaseHeight", "Color", "AerialPerspective",
                             "SunScatter", "SkyHaze"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(changed, name) != nullptr);
    }

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(air.set_density(0.9));
    REQUIRE_FALSE(air.set_enabled(true));
    game.stop_simulation();
    REQUIRE(air.density() == 0.4);
    REQUIRE_FALSE(air.enabled());
    REQUIRE(air.base_height() == 12.0);
}

TEST_CASE("ATM2 an Atmosphere belongs under Lighting and nowhere else", "[atmosphere]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    REQUIRE_FALSE(placement_error("Lighting", "Atmosphere", "Atmosphere"));
    REQUIRE(reason(placement_error("Workspace", "Atmosphere", "Atmosphere")) == "An Atmosphere must be in Lighting");

    const InstanceId lighting = game.scene_service("Lighting");
    Atmosphere& air = add_atmosphere(game, lighting);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(air.id(), folder.id()));
    REQUIRE(reason(game.parent_error(air.id(), workspace_of(game))) == "An Atmosphere must be in Lighting");
    REQUIRE(engine_core::parent_suits("Lighting", "Atmosphere"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "Atmosphere"));
}

TEST_CASE("ATM3 scripts make an Atmosphere and set it", "[atmosphere]") {
    ScriptRig rig;
    add_script(rig.game, "Air", R"(
        local air = Instance.new("Atmosphere", game.Lighting)
        _G.defaults = air.Enabled == true and air.Density == 0.25 and math.abs(air.HeightFalloff - 0.01) < 1e-9
            and air.BaseHeight == 0 and air.AerialPerspective == 0.5 and air.SunScatter == 0.25
            and air.SkyHaze == 1
        air.Density = 2
        air.BaseHeight = -40
        air.Color = Color3.new(1, 0, 0)
        _G.set = air.Density == 1 and air.BaseHeight == -40 and air.Color == Color3.new(1, 0, 0)
        _G.refused = not pcall(function() air.Density = 0 / 0 end)
            and not pcall(function() Instance.new("Atmosphere", workspace) end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"defaults", "set", "refused"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

TEST_CASE("ATM4 the snapshot carries the first Atmosphere under Lighting", "[atmosphere][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    REQUIRE_FALSE(pump.front().atmosphere.present);
    REQUIRE_FALSE(runner::ToSceneAtmosphere(pump.front().atmosphere).enabled);

    const InstanceId lighting = game.scene_service("Lighting");
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    Atmosphere& first = add_atmosphere(game, folder.id());
    Atmosphere& second = add_atmosphere(game, lighting);
    REQUIRE_FALSE(first.set_density(0.6));
    REQUIRE_FALSE(first.set_height_falloff(0.05));
    REQUIRE_FALSE(first.set_base_height(-30.0));
    REQUIRE_FALSE(first.set_color({0.2f, 0.4f, 0.6f, 1.f}));
    REQUIRE_FALSE(first.set_aerial_perspective(0.1));
    REQUIRE_FALSE(first.set_sun_scatter(0.9));
    REQUIRE_FALSE(first.set_sky_haze(0.3));
    REQUIRE_FALSE(second.set_density(0.8));
    frame();
    // The Folder comes first in Lighting, so the Atmosphere inside it is the air.
    {
        const engine_core::VisualAtmosphere& air = pump.front().atmosphere;
        REQUIRE(air.present);
        REQUIRE(air.enabled);
        REQUIRE(air.density == 0.6f);
        REQUIRE(air.height_falloff == 0.05f);
        REQUIRE(air.base_height == -30.f);
        REQUIRE(same(air.color, {0.2f, 0.4f, 0.6f, 1.f}));
        REQUIRE(air.aerial_perspective == 0.1f);
        REQUIRE(air.sun_scatter == 0.9f);
        REQUIRE(air.sky_haze == 0.3f);
        const runner::SceneAtmosphere scene = runner::ToSceneAtmosphere(air);
        REQUIRE(scene.enabled);
        REQUIRE(scene.density == 0.6f);
        REQUIRE(scene.color[1] == 0.4f);
    }

    // Turned off, it is still there, and draws nothing.
    REQUIRE_FALSE(first.set_enabled(false));
    frame();
    REQUIRE(pump.front().atmosphere.present);
    REQUIRE_FALSE(pump.front().atmosphere.enabled);
    REQUIRE_FALSE(runner::ToSceneAtmosphere(pump.front().atmosphere).enabled);
    // So does air of Density 0.
    REQUIRE_FALSE(first.set_enabled(true));
    REQUIRE_FALSE(first.set_density(0.0));
    frame();
    REQUIRE_FALSE(runner::ToSceneAtmosphere(pump.front().atmosphere).enabled);

    // Gone, the next one is the air.
    game.destroy(first.id());
    frame();
    REQUIRE(pump.front().atmosphere.density == 0.8f);
    game.destroy(second.id());
    frame();
    REQUIRE_FALSE(pump.front().atmosphere.present);
}
