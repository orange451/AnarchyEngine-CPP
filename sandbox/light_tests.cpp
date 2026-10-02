// PointLight, SpotLight, and DirectionalLight: GameObjects whose Color,
// Intensity, Radius, Enabled, and cone the render snapshot carries for the
// Scene View, and the Lighting properties the snapshot carries beside them.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Folder.hpp"
#include "Light.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using engine_core::DirectionalLight;
using engine_core::PointLight;
using engine_core::SpotLight;
using engine_core::VisualLight;

template <typename T>
T& add_light(engine_core::DataModel& game) {
    T& light = game.create<T>();
    game.set_parent(light.id(), workspace_of(game));
    return light;
}

void require_globals(ScriptRig& rig, std::initializer_list<const char*> names) {
    INFO(rig.runtime.last_error());
    for (const char* name : names) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

}  // namespace

TEST_CASE("LIT1 a Light's properties are checked, undo, save, and come back at Stop", "[light]") {
    SimRole role;
    engine_core::Game game;
    PointLight& light = add_light<PointLight>(game);
    REQUIRE(game.game_object(light.id()) == &light);
    REQUIRE(light.intensity() == PointLight::kDefaultIntensity);
    REQUIRE(light.radius() == PointLight::kDefaultRadius);
    REQUIRE(light.enabled());
    REQUIRE(engine_core::project_class_known("PointLight"));
    REQUIRE(engine_core::project_class_known("SpotLight"));

    // A default Light saves none of them.
    engine_core::PropertyBag saved;
    light.save_properties(saved);
    for (const char* name : {"Color", "Intensity", "Radius", "Enabled"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(saved, name) == nullptr);
    }

    game.history().set_pending_gesture("Set Radius");
    REQUIRE_FALSE(light.set_radius(20.0));
    game.history().end_gesture();
    REQUIRE(light.radius() == 20.0);
    game.history().undo();
    REQUIRE(light.radius() == PointLight::kDefaultRadius);
    game.history().redo();
    REQUIRE(light.radius() == 20.0);

    // Negative is taken as 0; not finite is refused.
    REQUIRE_FALSE(light.set_intensity(-3.0));
    REQUIRE(light.intensity() == 0.0);
    REQUIRE(*light.set_intensity(std::nan("")) == "Intensity must be a finite number");
    REQUIRE(*light.set_color(engine_core::ColorRgb{std::nanf(""), 0.f, 0.f, 1.f}) == "Color must be finite");

    REQUIRE_FALSE(light.set_color(engine_core::ColorRgb{1.f, 0.5f, 0.f, 1.f}));
    light.set_enabled(false);
    engine_core::PropertyBag changed;
    light.save_properties(changed);
    REQUIRE(engine_core::bag_find(changed, "Color") != nullptr);
    REQUIRE(engine_core::bag_find(changed, "Enabled") != nullptr);

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(light.set_radius(2.0));
    light.set_enabled(true);
    game.stop_simulation();
    REQUIRE(light.radius() == 20.0);
    REQUIRE_FALSE(light.enabled());
    REQUIRE(light.color().g == 0.5f);
}

TEST_CASE("LIT2 a SpotLight's cone is clamped", "[light]") {
    SimRole role;
    engine_core::Game game;
    SpotLight& spot = add_light<SpotLight>(game);
    REQUIRE(spot.outer_fov() == SpotLight::kDefaultOuterFov);
    REQUIRE(spot.inner_fov_scale() == SpotLight::kDefaultInnerFovScale);
    REQUIRE_FALSE(spot.set_outer_fov(500.0));
    REQUIRE(spot.outer_fov() == SpotLight::kMaxOuterFov);
    REQUIRE_FALSE(spot.set_outer_fov(0.0));
    REQUIRE(spot.outer_fov() == SpotLight::kMinOuterFov);
    REQUIRE_FALSE(spot.set_inner_fov_scale(2.0));
    REQUIRE(spot.inner_fov_scale() == 1.0);
    REQUIRE_FALSE(spot.set_inner_fov_scale(-1.0));
    REQUIRE(spot.inner_fov_scale() == 0.0);
    REQUIRE(*spot.set_outer_fov(std::nan("")) == "OuterFOV must be a finite number");
}

TEST_CASE("LIT3 scripts make lights and set them", "[light]") {
    ScriptRig rig;
    add_script(rig.game, "Lights", R"(
        local point = Instance.new("PointLight", workspace)
        _G.isa = point:IsA("Light") and point:IsA("GameObject") and point.ClassName == "PointLight"
        _G.defaults = point.Intensity == 1 and point.Radius == 8 and point.Enabled == true
            and point.Color == Color3.new(1, 1, 1)
        point.Radius = 16
        point.Color = Color3.new(1, 0, 0)
        point.Enabled = false
        _G.set = point.Radius == 16 and point.Color == Color3.new(1, 0, 0) and point.Enabled == false
        local spot = Instance.new("SpotLight", workspace)
        _G.spot = spot:IsA("Light") and spot.OuterFOV == 80 and math.abs(spot.InnerFOVScale - 0.1) < 1e-9
        spot.OuterFOV = 1000
        _G.clamped = spot.OuterFOV == 179
        _G.no_nan = not pcall(function() spot.Radius = 0 / 0 end)
        local sun = Instance.new("DirectionalLight", workspace)
        _G.sun = not sun:IsA("GameObject") and sun.Intensity == 1 and sun.Enabled == true
            and sun.Direction == Vector3.new(1, 1, 1)
            and not pcall(function() return sun.Radius end)
            and not pcall(function() return sun.Transform end)
        sun.Direction = Vector3.new(0, 1, 0)
        _G.sun_set = sun.Direction == Vector3.new(0, 1, 0)
            and not pcall(function() sun.Direction = Vector3.new(0 / 0, 0, 0) end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"isa", "defaults", "set", "spot", "clamped", "no_nan", "sun", "sun_set"});
}

TEST_CASE("LIT6 a DirectionalLight has a Direction, and no Transform or Radius", "[light]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("DirectionalLight"));
    engine_core::DirectionalLight& sun = add_light<engine_core::DirectionalLight>(game);
    REQUIRE(game.game_object(sun.id()) == nullptr);
    REQUIRE_FALSE(engine_core::lua_class_inherits("DirectionalLight", "GameObject"));
    std::vector<std::string> names;
    for (const engine_core::LuaField& field : engine_core::lua_saved_fields("DirectionalLight")) {
        names.emplace_back(field.name);
    }
    const auto has = [&names](const char* name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    REQUIRE(has("Direction"));
    REQUIRE(has("Color"));
    REQUIRE(has("Intensity"));
    REQUIRE(has("Enabled"));
    REQUIRE_FALSE(has("Transform"));
    REQUIRE_FALSE(has("Radius"));

    // The legacy default, and none of it saved while it is the default.
    REQUIRE(sun.direction().x == 1.f);
    REQUIRE(sun.direction().y == 1.f);
    REQUIRE(sun.direction().z == 1.f);
    engine_core::PropertyBag saved;
    sun.save_properties(saved);
    REQUIRE(saved.empty());

    game.history().set_pending_gesture("Set Direction");
    REQUIRE_FALSE(sun.set_direction(engine_core::Vec3{0.f, 1.f, 0.f}));
    game.history().end_gesture();
    REQUIRE(sun.direction().x == 0.f);
    game.history().undo();
    REQUIRE(sun.direction().x == 1.f);
    game.history().redo();
    REQUIRE(sun.direction().y == 1.f);
    REQUIRE(sun.direction().x == 0.f);
    REQUIRE(*sun.set_direction(engine_core::Vec3{std::nanf(""), 0.f, 0.f}) == "Direction must be finite");
    engine_core::PropertyBag changed;
    sun.save_properties(changed);
    REQUIRE(engine_core::bag_find(changed, "Direction") != nullptr);

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(sun.set_direction(engine_core::Vec3{0.f, 0.f, 1.f}));
    REQUIRE_FALSE(sun.set_intensity(-2.0));
    REQUIRE(sun.intensity() == 0.0);
    game.stop_simulation();
    REQUIRE(sun.direction().y == 1.f);
    REQUIRE(sun.intensity() == 1.0);
}

TEST_CASE("LIT7 a DirectionalLight in Workspace has a snapshot row", "[light][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    engine_core::DirectionalLight& sun = add_light<engine_core::DirectionalLight>(game);
    frame();
    REQUIRE(pump.find(sun.id()) != nullptr);
    const VisualLight& shone = pump.find(sun.id())->light;
    REQUIRE(shone.kind == VisualLight::Kind::Directional);
    REQUIRE(shone.enabled);
    REQUIRE(shone.radius == 0.f);
    REQUIRE(shone.intensity == 1.f);
    REQUIRE(shone.direction[0] == 1.f);

    REQUIRE_FALSE(sun.set_direction(engine_core::Vec3{0.f, 2.f, 0.f}));
    REQUIRE_FALSE(sun.set_color(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}));
    sun.set_enabled(false);
    frame();
    REQUIRE(pump.find(sun.id())->light.direction[0] == 0.f);
    REQUIRE(pump.find(sun.id())->light.direction[1] == 2.f);
    REQUIRE(pump.find(sun.id())->light.color[1] == 0.f);
    REQUIRE_FALSE(pump.find(sun.id())->light.enabled);

    // In a Folder that leaves Workspace, its row goes; back in, it reads every field.
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));
    game.set_parent(sun.id(), folder.id());
    frame();
    REQUIRE(pump.find(sun.id()) != nullptr);
    game.set_parent(folder.id(), game.scene_service("Storage"));
    frame();
    REQUIRE(pump.find(sun.id()) == nullptr);
    game.set_parent(folder.id(), workspace_of(game));
    frame();
    REQUIRE(pump.find(sun.id()) != nullptr);
    REQUIRE(pump.find(sun.id())->light.direction[1] == 2.f);

    // An overflowing queue resyncs, and the resync finds it too.
    bool refused = false;
    for (std::size_t i = 0; i <= engine_core::DataModel::kMaxInvalidations; ++i) {
        refused = sun.set_direction(engine_core::Vec3{0.f, static_cast<float>(i + 3), 0.f}).has_value() || refused;
    }
    REQUIRE_FALSE(refused);
    REQUIRE(game.invalidations().overflow());
    frame();
    REQUIRE(pump.find(sun.id()) != nullptr);
    REQUIRE(pump.find(sun.id())->light.kind == VisualLight::Kind::Directional);
    REQUIRE(pump.find(sun.id())->light.direction[1] == sun.direction().y);

    game.destroy(sun.id());
    frame();
    REQUIRE(pump.find(sun.id()) == nullptr);
}

TEST_CASE("LIT4 a Light's snapshot row carries what it shines", "[light][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    PointLight& point = add_light<PointLight>(game);
    SpotLight& spot = add_light<SpotLight>(game);
    engine_core::GameObject& part = create_part(game);
    frame();
    REQUIRE(pump.find(part.id())->light.kind == VisualLight::Kind::None);
    const VisualLight& shone = pump.find(point.id())->light;
    REQUIRE(shone.kind == VisualLight::Kind::Point);
    REQUIRE(shone.enabled);
    REQUIRE(shone.radius == 8.f);
    REQUIRE(shone.intensity == 1.f);
    REQUIRE(pump.find(spot.id())->light.kind == VisualLight::Kind::Spot);
    REQUIRE(pump.find(spot.id())->light.outer_fov == 80.f);

    // Each change shows on the next frame.
    REQUIRE_FALSE(point.set_radius(30.0));
    REQUIRE_FALSE(point.set_color(engine_core::ColorRgb{0.f, 1.f, 0.f, 1.f}));
    point.set_enabled(false);
    REQUIRE_FALSE(spot.set_outer_fov(45.0));
    REQUIRE_FALSE(spot.set_inner_fov_scale(0.5));
    frame();
    REQUIRE(pump.find(point.id())->light.radius == 30.f);
    REQUIRE(pump.find(point.id())->light.color[0] == 0.f);
    REQUIRE(pump.find(point.id())->light.color[1] == 1.f);
    REQUIRE_FALSE(pump.find(point.id())->light.enabled);
    REQUIRE(pump.find(spot.id())->light.outer_fov == 45.f);
    REQUIRE(pump.find(spot.id())->light.inner_fov_scale == 0.5f);

    // Out of Workspace the row is gone; back in, it reads every field again.
    game.set_parent(point.id(), game.scene_service("Storage"));
    frame();
    REQUIRE(pump.find(point.id()) == nullptr);
    game.set_parent(point.id(), workspace_of(game));
    frame();
    REQUIRE(pump.find(point.id())->light.radius == 30.f);
    REQUIRE_FALSE(pump.find(point.id())->light.enabled);
}

TEST_CASE("LIT5 the snapshot carries Lighting's Ambient, Exposure, Saturation, and Gamma", "[light][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    auto* lighting = dynamic_cast<engine_core::Lighting*>(game.instance(game.scene_service("Lighting")));
    REQUIRE(lighting != nullptr);
    frame();
    REQUIRE(pump.front().lighting.exposure == 1.f);
    REQUIRE(pump.front().lighting.saturation == 1.2f);
    REQUIRE(pump.front().lighting.gamma == 2.2f);
    REQUIRE(pump.front().lighting.ambient.r == 0.5f);

    REQUIRE_FALSE(lighting->set_exposure(0.5));
    REQUIRE_FALSE(lighting->set_saturation(0.0));
    REQUIRE_FALSE(lighting->set_gamma(1.8));
    REQUIRE_FALSE(lighting->set_ambient(engine_core::ColorRgb{0.1f, 0.2f, 0.3f, 1.f}));
    frame();
    REQUIRE(pump.front().lighting.exposure == 0.5f);
    REQUIRE(pump.front().lighting.saturation == 0.f);
    REQUIRE(pump.front().lighting.gamma == 1.8f);
    REQUIRE(pump.front().lighting.ambient.b == 0.3f);
    REQUIRE_FALSE(lighting->set_exposure(-1.0));
    REQUIRE(lighting->exposure() == 0.0);
}

TEST_CASE("LIT8 lights under Lighting shine as they do in Workspace", "[light][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    const engine_core::InstanceId lighting = game.scene_service("Lighting");
    PointLight& point = game.create<PointLight>();
    game.set_parent(point.id(), lighting);
    // At any depth, as in a Folder.
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    SpotLight& spot = game.create<SpotLight>();
    game.set_parent(spot.id(), folder.id());
    engine_core::DirectionalLight& sun = game.create<engine_core::DirectionalLight>();
    game.set_parent(sun.id(), folder.id());
    // Anything else under Lighting draws nothing.
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), lighting);
    REQUIRE(game.in_lighting(spot.id()));
    REQUIRE_FALSE(game.in_workspace(spot.id()));
    frame();
    REQUIRE(pump.find(point.id())->light.kind == VisualLight::Kind::Point);
    REQUIRE(pump.find(spot.id())->light.kind == VisualLight::Kind::Spot);
    REQUIRE(pump.find(sun.id())->light.kind == VisualLight::Kind::Directional);
    REQUIRE(pump.find(part.id()) == nullptr);

    // Its Transform and properties follow.
    point.set_transform(engine_core::matrix4_translation(1.f, 2.f, 3.f));
    REQUIRE_FALSE(point.set_radius(5.0));
    frame();
    REQUIRE(pump.find(point.id())->world.m[13] == 2.f);
    REQUIRE(pump.find(point.id())->light.radius == 5.f);

    // Under Lighting a light only shines: its Prefab draws in Workspace alone.
    engine_core::Prefab& lamp = game.create<engine_core::Prefab>();
    game.set_parent(lamp.id(), game.service("Prefabs"));
    engine_core::LuaSlot lamp_slot;
    lamp_slot.kind = engine_core::LuaSlot::Kind::Instance;
    lamp_slot.id = lamp.id();
    REQUIRE_FALSE(point.set_prefab(lamp_slot));
    frame();
    REQUIRE(pump.find(point.id())->prefab == 0);
    game.set_parent(point.id(), workspace_of(game));
    frame();
    REQUIRE(pump.find(point.id())->prefab != 0);
    game.set_parent(point.id(), lighting);
    frame();
    REQUIRE(pump.find(point.id())->prefab == 0);

    // Out of both services, the rows go.
    game.set_parent(folder.id(), game.scene_service("Storage"));
    frame();
    REQUIRE(pump.find(spot.id()) == nullptr);
    REQUIRE(pump.find(sun.id()) == nullptr);
    game.set_parent(folder.id(), lighting);
    frame();
    REQUIRE(pump.find(spot.id()) != nullptr);
    REQUIRE(pump.find(sun.id()) != nullptr);

    // A resync finds them all again, and still not the plain GameObject.
    bool refused = false;
    for (std::size_t i = 0; i <= engine_core::DataModel::kMaxInvalidations; ++i) {
        refused = spot.set_outer_fov(10.0 + static_cast<double>(i % 100)).has_value() || refused;
    }
    REQUIRE_FALSE(refused);
    REQUIRE(game.invalidations().overflow());
    frame();
    REQUIRE(pump.find(point.id()) != nullptr);
    REQUIRE(pump.find(point.id())->prefab == 0);
    REQUIRE(pump.find(point.id())->world.m[13] == 2.f);
    REQUIRE(pump.find(spot.id())->light.outer_fov == static_cast<float>(spot.outer_fov()));
    REQUIRE(pump.find(sun.id()) != nullptr);
    REQUIRE(pump.find(part.id()) == nullptr);
}

TEST_CASE("LIT9 Shadows and ShadowDistance are checked, undo, save, and come back at Stop", "[light][shadow]") {
    SimRole role;
    engine_core::Game game;
    PointLight& point = add_light<PointLight>(game);
    SpotLight& spot = add_light<SpotLight>(game);
    DirectionalLight& sun = game.create<DirectionalLight>();
    game.set_parent(sun.id(), workspace_of(game));
    REQUIRE_FALSE(point.shadows());
    REQUIRE(sun.shadows());
    REQUIRE(sun.shadow_distance() == DirectionalLight::kDefaultShadowDistance);

    // Defaults are not saved.
    engine_core::PropertyBag point_saved;
    point.save_properties(point_saved);
    REQUIRE(engine_core::bag_find(point_saved, "Shadows") == nullptr);
    engine_core::PropertyBag sun_saved;
    sun.save_properties(sun_saved);
    REQUIRE(engine_core::bag_find(sun_saved, "Shadows") == nullptr);
    REQUIRE(engine_core::bag_find(sun_saved, "ShadowDistance") == nullptr);

    game.history().set_pending_gesture("Set Shadows");
    point.set_shadows(true);
    game.history().end_gesture();
    REQUIRE(point.shadows());
    game.history().undo();
    REQUIRE_FALSE(point.shadows());
    game.history().redo();
    REQUIRE(point.shadows());

    // A SpotLight has it too, and saves it.
    spot.set_shadows(true);
    engine_core::PropertyBag spot_saved;
    spot.save_properties(spot_saved);
    REQUIRE(engine_core::bag_find(spot_saved, "Shadows") != nullptr);

    // Negative is taken as 0; not finite is refused.
    REQUIRE_FALSE(sun.set_shadow_distance(-5.0));
    REQUIRE(sun.shadow_distance() == 0.0);
    REQUIRE(*sun.set_shadow_distance(std::nan("")) == "ShadowDistance must be a finite number");
    REQUIRE_FALSE(sun.set_shadow_distance(250.0));
    sun.set_shadows(false);
    engine_core::PropertyBag changed;
    sun.save_properties(changed);
    REQUIRE(engine_core::bag_find(changed, "Shadows") != nullptr);
    REQUIRE(engine_core::bag_find(changed, "ShadowDistance") != nullptr);

    game.capture_place();
    game.start_simulation();
    point.set_shadows(false);
    REQUIRE_FALSE(sun.set_shadow_distance(10.0));
    game.stop_simulation();
    REQUIRE(point.shadows());
    REQUIRE(sun.shadow_distance() == 250.0);
    REQUIRE_FALSE(sun.shadows());
}

TEST_CASE("LIT10 a light's snapshot row carries its shadows", "[light][render][shadow]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    PointLight& point = add_light<PointLight>(game);
    DirectionalLight& sun = game.create<DirectionalLight>();
    game.set_parent(sun.id(), workspace_of(game));
    frame();
    REQUIRE_FALSE(pump.find(point.id())->light.shadows);
    REQUIRE(pump.find(sun.id())->light.shadows);
    REQUIRE(pump.find(sun.id())->light.shadow_distance == 100.f);

    point.set_shadows(true);
    sun.set_shadows(false);
    REQUIRE_FALSE(sun.set_shadow_distance(40.0));
    frame();
    REQUIRE(pump.find(point.id())->light.shadows);
    REQUIRE_FALSE(pump.find(sun.id())->light.shadows);
    REQUIRE(pump.find(sun.id())->light.shadow_distance == 40.f);
}
