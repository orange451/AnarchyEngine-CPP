// DynamicSky: a sky drawn from a shader under Lighting, whose clock, place,
// clouds, sun, and moon the render snapshot carries.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "DynamicSky.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "Skybox.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace {

using engine_core::DynamicSky;
using engine_core::InstanceId;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

DynamicSky& add_dynamic_sky(engine_core::DataModel& game, InstanceId parent) {
    DynamicSky& sky = game.create<DynamicSky>();
    game.set_parent(sky.id(), parent);
    return sky;
}

engine_core::Texture& add_texture(engine_core::DataModel& game, const char* name, const char* path) {
    engine_core::Texture& texture = game.create<engine_core::Texture>();
    game.set_name(texture.id(), name);
    REQUIRE_FALSE(texture.set_path(path));
    game.set_parent(texture.id(), game.service("Textures"));
    return texture;
}

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

}  // namespace

TEST_CASE("DS1 a DynamicSky's properties are checked, undo, save, and come back at Stop", "[dynamic_sky]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("DynamicSky"));
    DynamicSky& sky = add_dynamic_sky(game, game.scene_service("Lighting"));
    REQUIRE(sky.time_of_day() == 14.0);
    REQUIRE(sky.latitude() == 35.0);
    REQUIRE(sky.brightness() == 3.0);
    REQUIRE(sky.shadows());
    REQUIRE(sky.cloud_cover() == 0.5);
    REQUIRE(sky.cloud_density() == 0.5);
    REQUIRE(sky.wind_direction().x == 1.f);
    REQUIRE(sky.wind_direction().z == 0.3f);
    REQUIRE(sky.sun_texture().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(sky.moon_texture().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(sky.sun_size() == 2.0);
    REQUIRE(sky.moon_size() == 2.0);
    REQUIRE(sky.reflection_quality() == engine_core::EffectQuality::Medium);

    // A default DynamicSky saves none of them.
    engine_core::PropertyBag saved;
    sky.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set TimeOfDay");
    REQUIRE_FALSE(sky.set_time_of_day(6.5));
    end_step(game);
    REQUIRE(sky.time_of_day() == 6.5);
    game.history().undo();
    REQUIRE(sky.time_of_day() == 14.0);
    game.history().redo();
    REQUIRE(sky.time_of_day() == 6.5);

    // TimeOfDay wraps into [0, 24).
    REQUIRE_FALSE(sky.set_time_of_day(25.0));
    REQUIRE(sky.time_of_day() == 1.0);
    REQUIRE_FALSE(sky.set_time_of_day(-1.0));
    REQUIRE(sky.time_of_day() == 23.0);
    REQUIRE_FALSE(sky.set_time_of_day(24.0));
    REQUIRE(sky.time_of_day() == 0.0);
    REQUIRE_FALSE(sky.set_time_of_day(-1e-300));
    REQUIRE(sky.time_of_day() < 24.0);
    // The rest clamp.
    REQUIRE_FALSE(sky.set_latitude(100.0));
    REQUIRE(sky.latitude() == 90.0);
    REQUIRE_FALSE(sky.set_latitude(-100.0));
    REQUIRE(sky.latitude() == -90.0);
    REQUIRE_FALSE(sky.set_brightness(50.0));
    REQUIRE(sky.brightness() == DynamicSky::kMaxBrightness);
    REQUIRE_FALSE(sky.set_brightness(-1.0));
    REQUIRE(sky.brightness() == 0.0);
    REQUIRE_FALSE(sky.set_cloud_cover(2.0));
    REQUIRE(sky.cloud_cover() == 1.0);
    REQUIRE_FALSE(sky.set_cloud_density(-2.0));
    REQUIRE(sky.cloud_density() == 0.0);
    REQUIRE_FALSE(sky.set_sun_size(0.0));
    REQUIRE(sky.sun_size() == DynamicSky::kMinBodySize);
    REQUIRE_FALSE(sky.set_moon_size(30.0));
    REQUIRE(sky.moon_size() == DynamicSky::kMaxBodySize);
    // Not a number is refused, and changes nothing.
    REQUIRE(reason(sky.set_time_of_day(std::nan(""))) == "TimeOfDay must be a finite number");
    REQUIRE(reason(sky.set_latitude(INFINITY)) == "Latitude must be a finite number");
    REQUIRE(reason(sky.set_brightness(std::nan(""))) == "Brightness must be a finite number");
    REQUIRE(reason(sky.set_cloud_cover(std::nan(""))) == "CloudCover must be a finite number");
    REQUIRE(reason(sky.set_cloud_density(std::nan(""))) == "CloudDensity must be a finite number");
    REQUIRE(reason(sky.set_sun_size(std::nan(""))) == "SunSize must be a finite number");
    REQUIRE(reason(sky.set_moon_size(std::nan(""))) == "MoonSize must be a finite number");
    REQUIRE(reason(sky.set_wind_direction(engine_core::Vec3{std::nanf(""), 0.f, 0.f})) ==
            "WindDirection must be finite");
    REQUIRE(reason(sky.set_reflection_quality(7)) == "ReflectionQuality must be an Enum.EffectQuality");
    REQUIRE(sky.reflection_quality() == engine_core::EffectQuality::Medium);

    // The textures take a Texture, and nothing else.
    engine_core::Texture& sun = add_texture(game, "Sun", "textures/sun.png");
    REQUIRE_FALSE(sky.set_sun_texture(instance_slot(sun.id())));
    REQUIRE(sky.sun_texture().id == sun.id());
    engine_core::GameObject& part = create_part(game);
    REQUIRE(reason(sky.set_sun_texture(instance_slot(part.id()))) == "SunTexture must be a Texture");
    REQUIRE(reason(sky.set_moon_texture(instance_slot(part.id()))) == "MoonTexture must be a Texture");
    REQUIRE_FALSE(sky.set_moon_texture(instance_slot(sun.id())));

    REQUIRE_FALSE(sky.set_time_of_day(20.0));
    REQUIRE_FALSE(sky.set_shadows(false));
    REQUIRE_FALSE(sky.set_wind_direction(engine_core::Vec3{0.f, 0.f, 5.f}));
    REQUIRE_FALSE(sky.set_reflection_quality(2));
    engine_core::PropertyBag changed;
    sky.save_properties(changed);
    for (const char* name : {"TimeOfDay", "Latitude", "Brightness", "Shadows", "CloudCover", "CloudDensity",
                             "WindDirection", "SunTexture", "MoonTexture", "SunSize", "MoonSize",
                             "ReflectionQuality"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(changed, name) != nullptr);
    }

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(sky.set_time_of_day(3.0));
    REQUIRE_FALSE(sky.set_shadows(true));
    REQUIRE_FALSE(sky.set_sun_texture(engine_core::LuaSlot{}));
    game.stop_simulation();
    REQUIRE(sky.time_of_day() == 20.0);
    REQUIRE_FALSE(sky.shadows());
    REQUIRE(sky.sun_texture().id == sun.id());
    REQUIRE(sky.wind_direction().z == 5.f);
    REQUIRE(sky.reflection_quality() == engine_core::EffectQuality::High);
}

TEST_CASE("DS2 a DynamicSky belongs under Lighting and nowhere else", "[dynamic_sky]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    REQUIRE_FALSE(placement_error("Lighting", "DynamicSky", "Sky"));
    REQUIRE(reason(placement_error("Workspace", "DynamicSky", "Sky")) == "A DynamicSky must be in Lighting");
    REQUIRE(reason(placement_error("Storage", "DynamicSky", "Sky")) == "A DynamicSky must be in Lighting");

    const InstanceId lighting = game.scene_service("Lighting");
    DynamicSky& sky = add_dynamic_sky(game, lighting);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(sky.id(), folder.id()));
    REQUIRE(reason(game.parent_error(sky.id(), workspace_of(game))) == "A DynamicSky must be in Lighting");

    REQUIRE(engine_core::parent_suits("Lighting", "DynamicSky"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "DynamicSky"));
}

TEST_CASE("DS4 scripts make a DynamicSky and set it", "[dynamic_sky]") {
    ScriptRig rig;
    engine_core::Texture& moon = rig.game.create<engine_core::Texture>();
    rig.game.set_name(moon.id(), "Moon");
    rig.game.set_parent(moon.id(), rig.game.service("Textures"));
    add_script(rig.game, "Sky", R"(
        local sky = Instance.new("DynamicSky", game.Lighting)
        _G.defaults = sky.TimeOfDay == 14 and sky.Latitude == 35 and sky.Brightness == 3 and sky.Shadows
            and sky.CloudCover == 0.5 and sky.CloudDensity == 0.5 and sky.SunTexture == nil
            and sky.MoonTexture == nil and sky.SunSize == 2 and sky.MoonSize == 2
            and sky.ReflectionQuality == Enum.EffectQuality.Medium
            and math.abs(sky.WindDirection.Z - 0.3) < 1e-6
        sky.TimeOfDay = 30
        sky.Latitude = -200
        sky.MoonTexture = game.Assets.Textures.Moon
        sky.WindDirection = Vector3.new(2, 0, 0)
        sky.ReflectionQuality = Enum.EffectQuality.Low
        _G.set = sky.TimeOfDay == 6 and sky.Latitude == -90 and sky.MoonTexture == game.Assets.Textures.Moon
            and sky.WindDirection == Vector3.new(2, 0, 0) and sky.ReflectionQuality == Enum.EffectQuality.Low
        _G.refused = not pcall(function() sky.SunTexture = workspace end)
            and not pcall(function() sky.TimeOfDay = 0 / 0 end)
            and not pcall(function() sky.ReflectionQuality = 7 end)
            and not pcall(function() Instance.new("DynamicSky", workspace) end)
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

TEST_CASE("DS3 the snapshot carries the first DynamicSky under Lighting", "[dynamic_sky][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    // With none, the class's defaults.
    {
        const engine_core::VisualDynamicSky& sky = pump.front().dynamic_sky;
        REQUIRE_FALSE(sky.present);
        REQUIRE(sky.time_of_day == static_cast<float>(DynamicSky::kDefaultTimeOfDay));
        REQUIRE(sky.latitude == static_cast<float>(DynamicSky::kDefaultLatitude));
        REQUIRE(sky.brightness == static_cast<float>(DynamicSky::kDefaultBrightness));
        REQUIRE(sky.shadows == DynamicSky::kDefaultShadows);
        REQUIRE(sky.cloud_cover == static_cast<float>(DynamicSky::kDefaultCloudCover));
        REQUIRE(sky.cloud_density == static_cast<float>(DynamicSky::kDefaultCloudDensity));
        REQUIRE(sky.wind.x == DynamicSky::kDefaultWindDirection.x);
        REQUIRE(sky.wind.z == DynamicSky::kDefaultWindDirection.z);
        REQUIRE(sky.sun_size == static_cast<float>(DynamicSky::kDefaultSunSize));
        REQUIRE(sky.moon_size == static_cast<float>(DynamicSky::kDefaultMoonSize));
        REQUIRE(sky.reflection_quality == static_cast<int>(DynamicSky::kDefaultReflectionQuality));
        REQUIRE(sky.sun_texture.empty());
    }

    const InstanceId lighting = game.scene_service("Lighting");
    engine_core::Texture& sun = add_texture(game, "Sun", "textures/sun.png");
    DynamicSky& sky = add_dynamic_sky(game, lighting);
    REQUIRE_FALSE(sky.set_time_of_day(18.5));
    REQUIRE_FALSE(sky.set_latitude(-20.0));
    REQUIRE_FALSE(sky.set_brightness(5.0));
    REQUIRE_FALSE(sky.set_shadows(false));
    REQUIRE_FALSE(sky.set_cloud_cover(0.25));
    REQUIRE_FALSE(sky.set_cloud_density(0.75));
    REQUIRE_FALSE(sky.set_wind_direction(engine_core::Vec3{3.f, 1.f, -2.f}));
    REQUIRE_FALSE(sky.set_sun_texture(instance_slot(sun.id())));
    REQUIRE_FALSE(sky.set_sun_size(4.0));
    REQUIRE_FALSE(sky.set_moon_size(6.0));
    REQUIRE_FALSE(sky.set_reflection_quality(0));
    frame();
    {
        const engine_core::VisualDynamicSky& seen = pump.front().dynamic_sky;
        REQUIRE(seen.present);
        REQUIRE_FALSE(pump.front().sky.present);
        REQUIRE(seen.time_of_day == 18.5f);
        REQUIRE(seen.latitude == -20.f);
        REQUIRE(seen.brightness == 5.f);
        REQUIRE_FALSE(seen.shadows);
        REQUIRE(seen.cloud_cover == 0.25f);
        REQUIRE(seen.cloud_density == 0.75f);
        REQUIRE(seen.wind.x == 3.f);
        REQUIRE(seen.wind.z == -2.f);
        REQUIRE(seen.sun_texture == "textures/sun.png");
        REQUIRE(seen.moon_texture.empty());
        REQUIRE(seen.sun_size == 4.f);
        REQUIRE(seen.moon_size == 6.f);
        REQUIRE(seen.reflection_quality == 0);
    }
    // A destroyed Texture reads as none; a destroyed sky as no sky.
    game.destroy(sun.id());
    frame();
    REQUIRE(pump.front().dynamic_sky.sun_texture.empty());
    game.destroy(sky.id());
    frame();
    REQUIRE_FALSE(pump.front().dynamic_sky.present);
    REQUIRE(pump.front().dynamic_sky.time_of_day == static_cast<float>(DynamicSky::kDefaultTimeOfDay));
}

TEST_CASE("DS5 the first Skybox or DynamicSky in the tree is the sky", "[dynamic_sky][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    const InstanceId lighting = game.scene_service("Lighting");
    DynamicSky& dynamic = add_dynamic_sky(game, lighting);
    engine_core::Skybox& image = game.create<engine_core::Skybox>();
    game.set_parent(image.id(), lighting);
    frame();
    REQUIRE(pump.front().dynamic_sky.present);
    REQUIRE_FALSE(pump.front().sky.present);

    // The DynamicSky into a Folder after the Skybox: the Skybox is first now.
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    game.set_parent(dynamic.id(), folder.id());
    frame();
    REQUIRE(pump.front().sky.present);
    REQUIRE_FALSE(pump.front().dynamic_sky.present);

    game.destroy(image.id());
    frame();
    REQUIRE(pump.front().dynamic_sky.present);
    REQUIRE_FALSE(pump.front().sky.present);
}
