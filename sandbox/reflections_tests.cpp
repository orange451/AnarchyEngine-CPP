// ScreenSpaceReflections: on-screen reflections under Lighting, whose
// Enabled, Intensity, MaxDistance, and MaxRoughness the snapshot carries.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Folder.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "ScreenSpaceReflections.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace {

using engine_core::InstanceId;
using engine_core::ScreenSpaceReflections;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

ScreenSpaceReflections& add_reflections(engine_core::DataModel& game, InstanceId parent) {
    ScreenSpaceReflections& reflections = game.create<ScreenSpaceReflections>();
    game.set_parent(reflections.id(), parent);
    return reflections;
}

}  // namespace

TEST_CASE("SSR1 a ScreenSpaceReflections' properties are checked, undo, save, and come back at Stop", "[reflections]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("ScreenSpaceReflections"));
    ScreenSpaceReflections& ssr = add_reflections(game, game.scene_service("Lighting"));
    REQUIRE(ssr.enabled());
    REQUIRE(ssr.intensity() == 1.0);
    REQUIRE(ssr.max_distance() == 50.0);
    REQUIRE(ssr.max_roughness() == 0.5);

    engine_core::PropertyBag saved;
    ssr.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set MaxDistance");
    REQUIRE_FALSE(ssr.set_max_distance(20.0));
    end_step(game);
    REQUIRE(ssr.max_distance() == 20.0);
    game.history().undo();
    REQUIRE(ssr.max_distance() == 50.0);
    game.history().redo();
    REQUIRE(ssr.max_distance() == 20.0);

    begin_step(game, "Set Enabled");
    REQUIRE_FALSE(ssr.set_enabled(false));
    end_step(game);
    game.history().undo();
    REQUIRE(ssr.enabled());

    REQUIRE_FALSE(ssr.set_intensity(3.0));
    REQUIRE(ssr.intensity() == ScreenSpaceReflections::kMaxIntensity);
    REQUIRE_FALSE(ssr.set_intensity(-1.0));
    REQUIRE(ssr.intensity() == 0.0);
    REQUIRE_FALSE(ssr.set_max_distance(5000.0));
    REQUIRE(ssr.max_distance() == ScreenSpaceReflections::kMaxMaxDistance);
    REQUIRE_FALSE(ssr.set_max_distance(-2.0));
    REQUIRE(ssr.max_distance() == 0.0);
    REQUIRE_FALSE(ssr.set_max_roughness(2.0));
    REQUIRE(ssr.max_roughness() == ScreenSpaceReflections::kMaxMaxRoughness);
    REQUIRE_FALSE(ssr.set_max_roughness(-0.5));
    REQUIRE(ssr.max_roughness() == 0.0);
    REQUIRE(*ssr.set_intensity(std::nan("")) == "Intensity must be a finite number");
    REQUIRE(*ssr.set_max_distance(INFINITY) == "MaxDistance must be a finite number");
    REQUIRE(*ssr.set_max_roughness(-INFINITY) == "MaxRoughness must be a finite number");

    REQUIRE_FALSE(ssr.set_enabled(false));
    REQUIRE_FALSE(ssr.set_intensity(0.7));
    REQUIRE_FALSE(ssr.set_max_distance(80.0));
    REQUIRE_FALSE(ssr.set_max_roughness(0.3));
    engine_core::PropertyBag changed;
    ssr.save_properties(changed);
    for (const char* name : {"Enabled", "Intensity", "MaxDistance", "MaxRoughness"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(changed, name) != nullptr);
    }

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(ssr.set_max_distance(5.0));
    REQUIRE_FALSE(ssr.set_enabled(true));
    game.stop_simulation();
    REQUIRE(ssr.max_distance() == 80.0);
    REQUIRE_FALSE(ssr.enabled());
    REQUIRE(ssr.max_roughness() == 0.3);
}

TEST_CASE("SSR2 a ScreenSpaceReflections belongs under Lighting and nowhere else", "[reflections]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    const std::string message = "A ScreenSpaceReflections must be in Lighting";
    REQUIRE_FALSE(placement_error("Lighting", "ScreenSpaceReflections", "Reflections"));
    REQUIRE(reason(placement_error("Workspace", "ScreenSpaceReflections", "Reflections")) == message);
    REQUIRE(reason(placement_error("Storage", "ScreenSpaceReflections", "Reflections")) == message);

    const InstanceId lighting = game.scene_service("Lighting");
    ScreenSpaceReflections& ssr = add_reflections(game, lighting);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(ssr.id(), folder.id()));
    REQUIRE(reason(game.parent_error(ssr.id(), workspace_of(game))) == message);
    game.set_parent(ssr.id(), folder.id());
    REQUIRE(reason(game.parent_error(folder.id(), workspace_of(game))) == message);
    REQUIRE(engine_core::parent_suits("Lighting", "ScreenSpaceReflections"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "ScreenSpaceReflections"));
}

TEST_CASE("SSR4 scripts make a ScreenSpaceReflections and set it", "[reflections]") {
    ScriptRig rig;
    add_script(rig.game, "Mirror", R"(
        local ssr = Instance.new("ScreenSpaceReflections", game.Lighting)
        _G.defaults = ssr.Enabled == true and ssr.Intensity == 1 and ssr.MaxDistance == 50
            and ssr.MaxRoughness == 0.5
        ssr.Enabled = false
        ssr.Intensity = 0.25
        ssr.MaxDistance = 2000
        ssr.MaxRoughness = 0.2
        _G.set = ssr.Enabled == false and ssr.Intensity == 0.25 and ssr.MaxDistance == 1000
            and math.abs(ssr.MaxRoughness - 0.2) < 1e-9
        _G.refused = not pcall(function() ssr.MaxDistance = 0 / 0 end)
            and not pcall(function() Instance.new("ScreenSpaceReflections", workspace) end)
            and not pcall(function() ssr.Parent = workspace end)
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
