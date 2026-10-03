// Skybox: the sky under Lighting, whose Image, Exposure, LightScale,
// Rotation, and Tint the render snapshot carries for the Scene View.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
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

using engine_core::InstanceId;
using engine_core::Skybox;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

Skybox& add_skybox(engine_core::DataModel& game, InstanceId parent) {
    Skybox& sky = game.create<Skybox>();
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

TEST_CASE("SKY1 a Skybox's properties are checked, undo, save, and come back at Stop", "[skybox]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("Skybox"));
    Skybox& sky = add_skybox(game, game.scene_service("Lighting"));
    REQUIRE(sky.image().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(sky.exposure() == 1.0);
    REQUIRE(sky.light_scale() == 1.0);
    REQUIRE(sky.rotation() == 0.0);
    REQUIRE(sky.tint().r == 1.f);

    // A default Skybox saves none of them.
    engine_core::PropertyBag saved;
    sky.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set Exposure");
    REQUIRE_FALSE(sky.set_exposure(2.5));
    end_step(game);
    REQUIRE(sky.exposure() == 2.5);
    game.history().undo();
    REQUIRE(sky.exposure() == 1.0);
    game.history().redo();
    REQUIRE(sky.exposure() == 2.5);

    // Exposure is clamped to 0..10; Rotation wraps into 0..360.
    REQUIRE_FALSE(sky.set_exposure(50.0));
    REQUIRE(sky.exposure() == Skybox::kMaxExposure);
    REQUIRE_FALSE(sky.set_exposure(-1.0));
    REQUIRE(sky.exposure() == 0.0);
    REQUIRE_FALSE(sky.set_rotation(450.0));
    REQUIRE(sky.rotation() == 90.0);
    REQUIRE_FALSE(sky.set_rotation(-90.0));
    REQUIRE(sky.rotation() == 270.0);
    REQUIRE_FALSE(sky.set_rotation(360.0));
    REQUIRE(sky.rotation() == 0.0);
    REQUIRE_FALSE(sky.set_rotation(-1e-300));
    REQUIRE(sky.rotation() < 360.0);
    // LightScale is clamped to 0..10 too, and undoes.
    begin_step(game, "Set LightScale");
    REQUIRE_FALSE(sky.set_light_scale(0.25));
    end_step(game);
    REQUIRE(sky.light_scale() == 0.25);
    game.history().undo();
    REQUIRE(sky.light_scale() == 1.0);
    REQUIRE_FALSE(sky.set_light_scale(50.0));
    REQUIRE(sky.light_scale() == Skybox::kMaxLightScale);
    REQUIRE_FALSE(sky.set_light_scale(-1.0));
    REQUIRE(sky.light_scale() == 0.0);
    REQUIRE(*sky.set_light_scale(std::nan("")) == "LightScale must be a finite number");
    REQUIRE(*sky.set_exposure(std::nan("")) == "Exposure must be a finite number");
    REQUIRE(*sky.set_rotation(INFINITY) == "Rotation must be a finite number");
    REQUIRE(*sky.set_tint(engine_core::ColorRgb{std::nanf(""), 0.f, 0.f, 1.f}) == "Tint must be finite");

    // Image takes a Texture, and nothing else.
    engine_core::Texture& day = add_texture(game, "Day", "textures/day.hdr");
    REQUIRE_FALSE(sky.set_image(instance_slot(day.id())));
    REQUIRE(sky.image().id == day.id());
    engine_core::GameObject& part = create_part(game);
    REQUIRE(reason(sky.set_image(instance_slot(part.id()))) == "Image must be a Texture");
    REQUIRE(sky.image().id == day.id());

    REQUIRE_FALSE(sky.set_rotation(45.0));
    REQUIRE_FALSE(sky.set_tint(engine_core::ColorRgb{1.f, 0.5f, 0.25f, 1.f}));
    engine_core::PropertyBag changed;
    sky.save_properties(changed);
    for (const char* name : {"Image", "Exposure", "LightScale", "Rotation", "Tint"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(changed, name) != nullptr);
    }

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(sky.set_rotation(10.0));
    REQUIRE_FALSE(sky.set_image(engine_core::LuaSlot{}));
    game.stop_simulation();
    REQUIRE(sky.rotation() == 45.0);
    REQUIRE(sky.image().id == day.id());
    REQUIRE(sky.tint().g == 0.5f);
}

TEST_CASE("SKY2 a Skybox belongs under Lighting and nowhere else", "[skybox]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    REQUIRE_FALSE(placement_error("Lighting", "Skybox", "Sky"));
    REQUIRE(reason(placement_error("Workspace", "Skybox", "Sky")) == "A Skybox must be in Lighting");
    REQUIRE(reason(placement_error("Storage", "Skybox", "Sky")) == "A Skybox must be in Lighting");
    REQUIRE(reason(placement_error("Skybox", "Skybox", "Sky")) == "A Skybox must be in Lighting");

    const InstanceId lighting = game.scene_service("Lighting");
    Skybox& sky = add_skybox(game, lighting);
    REQUIRE(game.parent(sky.id()) == lighting);
    // In a Folder under Lighting, Lighting's rule decides.
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(sky.id(), folder.id()));
    REQUIRE(reason(game.parent_error(sky.id(), workspace_of(game))) == "A Skybox must be in Lighting");
    // A Folder holding one cannot leave Lighting.
    game.set_parent(sky.id(), folder.id());
    REQUIRE(reason(game.parent_error(folder.id(), workspace_of(game))) == "A Skybox must be in Lighting");

    // The explorer's Insert list ranks it under Lighting only.
    REQUIRE(engine_core::parent_suits("Lighting", "Skybox"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "Skybox"));
}

TEST_CASE("SKY3 the snapshot carries the first Skybox under Lighting", "[skybox][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    REQUIRE_FALSE(pump.front().sky.present);
    REQUIRE(pump.front().sky.image.empty());

    const InstanceId lighting = game.scene_service("Lighting");
    engine_core::Texture& day = add_texture(game, "Day", "textures/day.hdr");
    engine_core::Texture& chrome = add_texture(game, "Chrome", "textures/chrome.png");
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    Skybox& first = add_skybox(game, folder.id());
    Skybox& second = add_skybox(game, lighting);
    REQUIRE_FALSE(first.set_image(instance_slot(day.id())));
    REQUIRE_FALSE(first.set_exposure(2.0));
    REQUIRE_FALSE(first.set_light_scale(0.5));
    REQUIRE_FALSE(first.set_rotation(90.0));
    REQUIRE_FALSE(first.set_tint(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}));
    REQUIRE_FALSE(second.set_exposure(5.0));
    frame();
    // The Folder comes first in Lighting, so the Skybox inside it is the sky.
    {
        const engine_core::VisualSky& sky = pump.front().sky;
        REQUIRE(sky.present);
        REQUIRE(sky.image == "textures/day.hdr");
        REQUIRE(sky.exposure == 2.f);
        REQUIRE(sky.light_scale == 0.5f);
        REQUIRE(sky.rotation == 90.f);
        REQUIRE(sky.tint.g == 0.f);
    }

    // A Path that changes shows on the next frame.
    REQUIRE_FALSE(day.set_path("textures/night.hdr"));
    frame();
    REQUIRE(pump.front().sky.image == "textures/night.hdr");

    // Gone, the next one is the sky; a destroyed Texture reads as none.
    game.destroy(first.id());
    frame();
    REQUIRE(pump.front().sky.present);
    REQUIRE(pump.front().sky.exposure == 5.f);
    REQUIRE(pump.front().sky.image.empty());
    REQUIRE_FALSE(second.set_image(instance_slot(chrome.id())));
    frame();
    REQUIRE(pump.front().sky.image == "textures/chrome.png");
    game.destroy(chrome.id());
    frame();
    REQUIRE(pump.front().sky.image.empty());

    game.destroy(second.id());
    frame();
    REQUIRE_FALSE(pump.front().sky.present);
    REQUIRE(pump.front().sky.exposure == 1.f);
    REQUIRE(pump.front().sky.light_scale == 1.f);
}

TEST_CASE("SKY4 scripts make a Skybox and set it", "[skybox]") {
    ScriptRig rig;
    engine_core::Texture& day = rig.game.create<engine_core::Texture>();
    rig.game.set_name(day.id(), "Day");
    rig.game.set_parent(day.id(), rig.game.service("Textures"));
    add_script(rig.game, "Sky", R"(
        local sky = Instance.new("Skybox", game.Lighting)
        _G.defaults = sky.Exposure == 1 and sky.Rotation == 0 and sky.Tint == Color3.new(1, 1, 1)
            and sky.Image == nil and sky.LightScale == 1
        sky.Image = game.Assets.Textures.Day
        sky.Exposure = 20
        sky.LightScale = 0.3
        sky.Rotation = -90
        sky.Tint = Color3.new(0.5, 0.5, 1)
        _G.set = sky.Image == game.Assets.Textures.Day and sky.Exposure == 10
            and math.abs(sky.LightScale - 0.3) < 1e-9 and sky.Rotation == 270
            and sky.Tint == Color3.new(0.5, 0.5, 1)
        _G.refused = not pcall(function() sky.Image = workspace end)
            and not pcall(function() sky.Exposure = 0 / 0 end)
            and not pcall(function() Instance.new("Skybox", workspace) end)
            and not pcall(function() sky.Parent = workspace end)
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
