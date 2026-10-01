// PointLight and SpotLight: GameObjects whose Color, Intensity, Radius,
// Enabled, and cone the render snapshot carries for the Scene View, and the
// Lighting properties the snapshot carries beside them.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "Light.hpp"
#include "Lighting.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <string>

namespace {

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
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"isa", "defaults", "set", "spot", "clamped", "no_nan"});
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
