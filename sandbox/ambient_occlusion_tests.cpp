// AmbientOcclusionEffect: shading where nearby geometry hides the sky, under
// Lighting, whose Enabled, Intensity, Radius, and Quality the snapshot carries.

#include "support.hpp"

#include "AmbientOcclusionEffect.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace {

using engine_core::AmbientOcclusionEffect;
using engine_core::EffectQuality;
using engine_core::InstanceId;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

AmbientOcclusionEffect& add_occlusion(engine_core::DataModel& game, InstanceId parent) {
    AmbientOcclusionEffect& ao = game.create<AmbientOcclusionEffect>();
    game.set_parent(ao.id(), parent);
    return ao;
}

}  // namespace

TEST_CASE("AO0 Enum.EffectQuality has Low, Medium, and High", "[occlusion]") {
    const engine_core::EnumType& type = engine_core::effect_quality_enum();
    REQUIRE(engine_core::enum_item_value(type, "Low") == 0);
    REQUIRE(engine_core::enum_item_value(type, "Medium") == 1);
    REQUIRE(engine_core::enum_item_value(type, "High") == 2);
    REQUIRE(engine_core::enum_item_name(type, 3) == nullptr);
}

TEST_CASE("AO1 an AmbientOcclusionEffect's properties are checked, undo, save, and come back at Stop", "[occlusion]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("AmbientOcclusionEffect"));
    AmbientOcclusionEffect& ao = add_occlusion(game, game.scene_service("Lighting"));
    REQUIRE(ao.enabled());
    REQUIRE(ao.intensity() == 1.0);
    REQUIRE(ao.radius() == 1.0);
    REQUIRE(ao.quality() == EffectQuality::Medium);

    engine_core::PropertyBag saved;
    ao.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set Radius");
    REQUIRE_FALSE(ao.set_radius(2.5));
    end_step(game);
    REQUIRE(ao.radius() == 2.5);
    game.history().undo();
    REQUIRE(ao.radius() == 1.0);
    game.history().redo();
    REQUIRE(ao.radius() == 2.5);

    begin_step(game, "Set Quality");
    REQUIRE_FALSE(ao.set_quality(2));
    end_step(game);
    REQUIRE(ao.quality() == EffectQuality::High);
    game.history().undo();
    REQUIRE(ao.quality() == EffectQuality::Medium);
    REQUIRE(*ao.set_quality(5) == "Quality must be an Enum.EffectQuality");

    REQUIRE_FALSE(ao.set_intensity(9.0));
    REQUIRE(ao.intensity() == AmbientOcclusionEffect::kMaxIntensity);
    REQUIRE_FALSE(ao.set_intensity(-1.0));
    REQUIRE(ao.intensity() == 0.0);
    REQUIRE_FALSE(ao.set_radius(50.0));
    REQUIRE(ao.radius() == AmbientOcclusionEffect::kMaxRadius);
    REQUIRE_FALSE(ao.set_radius(-1.0));
    REQUIRE(ao.radius() == 0.0);
    REQUIRE(*ao.set_intensity(std::nan("")) == "Intensity must be a finite number");
    REQUIRE(*ao.set_radius(INFINITY) == "Radius must be a finite number");

    REQUIRE_FALSE(ao.set_enabled(false));
    REQUIRE_FALSE(ao.set_intensity(2.0));
    REQUIRE_FALSE(ao.set_radius(3.0));
    REQUIRE_FALSE(ao.set_quality(0));
    engine_core::PropertyBag changed;
    ao.save_properties(changed);
    for (const char* name : {"Enabled", "Intensity", "Radius", "Quality"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(changed, name) != nullptr);
    }

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(ao.set_radius(0.5));
    REQUIRE_FALSE(ao.set_quality(2));
    game.stop_simulation();
    REQUIRE(ao.radius() == 3.0);
    REQUIRE(ao.quality() == EffectQuality::Low);
    REQUIRE_FALSE(ao.enabled());
}

TEST_CASE("AO2 an AmbientOcclusionEffect belongs under Lighting and nowhere else", "[occlusion]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    const std::string message = "An AmbientOcclusionEffect must be in Lighting";
    REQUIRE_FALSE(placement_error("Lighting", "AmbientOcclusionEffect", "Occlusion"));
    REQUIRE(reason(placement_error("Workspace", "AmbientOcclusionEffect", "Occlusion")) == message);

    const InstanceId lighting = game.scene_service("Lighting");
    AmbientOcclusionEffect& ao = add_occlusion(game, lighting);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(ao.id(), folder.id()));
    REQUIRE(reason(game.parent_error(ao.id(), workspace_of(game))) == message);
    REQUIRE(engine_core::parent_suits("Lighting", "AmbientOcclusionEffect"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "AmbientOcclusionEffect"));
}

TEST_CASE("AO4 scripts make an AmbientOcclusionEffect and set it", "[occlusion]") {
    ScriptRig rig;
    add_script(rig.game, "Shade", R"(
        local ao = Instance.new("AmbientOcclusionEffect", game.Lighting)
        _G.defaults = ao.Enabled == true and ao.Intensity == 1 and ao.Radius == 1
            and ao.Quality == Enum.EffectQuality.Medium
        ao.Intensity = 2
        ao.Radius = 20
        ao.Quality = Enum.EffectQuality.High
        _G.set = ao.Intensity == 2 and ao.Radius == 10 and ao.Quality == Enum.EffectQuality.High
        ao.Quality = 0
        _G.by_value = ao.Quality == Enum.EffectQuality.Low
        ao.Quality = "Medium"
        _G.by_name = ao.Quality == Enum.EffectQuality.Medium
        _G.refused = not pcall(function() ao.Radius = 0 / 0 end)
            and not pcall(function() ao.Quality = Enum.TransformSpace.World end)
            and not pcall(function() ao.Quality = "Ultra" end)
            and not pcall(function() Instance.new("AmbientOcclusionEffect", workspace) end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"defaults", "set", "by_value", "by_name", "refused"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

TEST_CASE("AO3 the snapshot carries the first AmbientOcclusionEffect under Lighting", "[occlusion][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    REQUIRE_FALSE(pump.front().occlusion.present);

    const InstanceId lighting = game.scene_service("Lighting");
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    AmbientOcclusionEffect& first = add_occlusion(game, folder.id());
    AmbientOcclusionEffect& second = add_occlusion(game, lighting);
    REQUIRE_FALSE(first.set_enabled(false));
    REQUIRE_FALSE(first.set_intensity(2.0));
    REQUIRE_FALSE(first.set_radius(3.0));
    REQUIRE_FALSE(first.set_quality(2));
    REQUIRE_FALSE(second.set_radius(7.0));
    frame();
    {
        const engine_core::VisualAmbientOcclusion& ao = pump.front().occlusion;
        REQUIRE(ao.present);
        REQUIRE_FALSE(ao.enabled);
        REQUIRE(ao.intensity == 2.f);
        REQUIRE(ao.radius == 3.f);
        REQUIRE(ao.quality == 2);
    }
    game.destroy(first.id());
    frame();
    REQUIRE(pump.front().occlusion.radius == 7.f);
    REQUIRE(pump.front().occlusion.quality == 1);
    game.destroy(second.id());
    frame();
    REQUIRE_FALSE(pump.front().occlusion.present);
}
