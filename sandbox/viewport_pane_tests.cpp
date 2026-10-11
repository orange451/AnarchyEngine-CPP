// ViewportPane: a pane that draws the 3D world inside it. Its properties, how
// a project saves them, and where a Skybox may go.

#include "support.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "Skybox.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>

namespace {

using engine_core::GuiProperty;
using engine_core::InstanceId;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
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

// A ViewportPane in a ScreenGui under Gui.
engine_core::ViewportPane& make_pane(engine_core::DataModel& game) {
    engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
    game.set_parent(screen.id(), game.scene_service("Gui"));
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), screen.id());
    return pane;
}

}  // namespace

TEST_CASE("VP1 ViewportPane is a pane with CurrentCamera, Ambient, and ImageTransparency", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::lua_class_inherits("ViewportPane", "GuiBasePane"));
    engine_core::ViewportPane& pane = make_pane(game);
    REQUIRE(pane.viewport_pane());
    REQUIRE(pane.vec2(GuiProperty::Size).x == 100.f);
    REQUIRE(pane.current_camera().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(pane.current_camera_id() == 0);
    REQUIRE(pane.ambient().r == 0.5f);
    REQUIRE(pane.number(GuiProperty::ImageTransparency) == 0);
    engine_core::PropertyBag defaults;
    pane.save_properties(defaults);
    REQUIRE(defaults.empty());
}

TEST_CASE("VP2 CurrentCamera takes a Camera, refuses anything else, and undoes", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    REQUIRE(*pane.set_current_camera(instance_slot(folder.id())) == "CurrentCamera must be a Camera");
    engine_core::Camera& camera = game.create<engine_core::Camera>();
    game.set_parent(camera.id(), pane.id());
    const std::uint64_t before = pane.revision();
    begin_step(game, "Set CurrentCamera");
    REQUIRE_FALSE(pane.set_current_camera(instance_slot(camera.id())));
    end_step(game);
    REQUIRE(pane.revision() != before);
    REQUIRE(pane.current_camera_id() == camera.id());
    game.history().undo();
    REQUIRE(pane.current_camera_id() == 0);
}

TEST_CASE("VP3 Ambient is a Color3 that records undo and moves the revision", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    const std::uint64_t before = pane.revision();
    begin_step(game, "Set Ambient");
    REQUIRE_FALSE(pane.set_ambient(engine_core::ColorRgb{0.2f, 0.3f, 0.4f, 1.f}));
    end_step(game);
    REQUIRE(pane.revision() != before);
    REQUIRE(pane.ambient().g == 0.3f);
    game.history().undo();
    REQUIRE(pane.ambient().g == 0.5f);
}

TEST_CASE("VP4 a CurrentCamera that names nothing is not looked for again until the tree changes", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    engine_core::Camera& camera = game.create<engine_core::Camera>();
    game.set_parent(camera.id(), pane.id());
    REQUIRE_FALSE(pane.set_current_camera(instance_slot(camera.id())));
    begin_step(game, "Delete");
    game.destroy_tree(camera.id());
    end_step(game);
    REQUIRE(pane.current_camera_id() == 0);
    REQUIRE(pane.current_camera_id() == 0);
    game.history().undo();
    // The undo relinks the tree, so the Camera is found again under its GUID.
    const InstanceId back = pane.current_camera_id();
    REQUIRE(back != 0);
    REQUIRE(dynamic_cast<engine_core::Camera*>(game.instance(back)) != nullptr);
}

TEST_CASE("VP5 a project saves and loads a ViewportPane with its Camera and Ambient", "[viewport][project]") {
    SimRole role;
    TempDir dir;
    std::string pane_guid;
    std::string camera_guid;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        engine_core::ViewportPane& pane = make_pane(game);
        engine_core::Camera& camera = game.create<engine_core::Camera>();
        game.set_parent(camera.id(), pane.id());
        REQUIRE_FALSE(pane.set_current_camera(instance_slot(camera.id())));
        REQUIRE_FALSE(pane.set_ambient(engine_core::ColorRgb{0.1f, 0.2f, 0.3f, 1.f}));
        pane_guid = game.guid(pane.id());
        camera_guid = game.guid(camera.id());
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    const std::optional<InstanceId> pane_id = game.find_guid(pane_guid);
    const std::optional<InstanceId> camera_id = game.find_guid(camera_guid);
    REQUIRE(pane_id);
    REQUIRE(camera_id);
    const auto* back = dynamic_cast<const engine_core::ViewportPane*>(game.instance(*pane_id));
    REQUIRE(back != nullptr);
    REQUIRE(back->ambient().b == 0.3f);
    REQUIRE(back->current_camera_id() == *camera_id);
}

TEST_CASE("VP6 a Skybox goes in Lighting or a ViewportPane; DynamicSky only in Lighting", "[viewport]") {
    REQUIRE_FALSE(engine_core::placement_error("ViewportPane", "Skybox", "Sky"));
    REQUIRE(*engine_core::placement_error("Workspace", "Skybox", "Sky") ==
            "A Skybox must be in Lighting or a ViewportPane");
    REQUIRE(*engine_core::placement_error("ViewportPane", "DynamicSky", "Sky") == "A DynamicSky must be in Lighting");
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), pane.id());
    engine_core::Skybox& sky = game.create<engine_core::Skybox>();
    REQUIRE_FALSE(game.parent_error(sky.id(), folder.id()));
    REQUIRE(engine_core::parent_suits("ViewportPane", "Skybox"));
    REQUIRE(engine_core::parent_suits("ViewportPane", "GameObject"));
    REQUIRE(engine_core::parent_suits("ViewportPane", "Camera"));
}

TEST_CASE("VP7 scripts make a ViewportPane and set its CurrentCamera", "[viewport]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local screen = Instance.new("ScreenGui", game.Gui)
        local pane = Instance.new("ViewportPane", screen)
        local camera = Instance.new("Camera", pane)
        _G.defaults = pane.CurrentCamera == nil and pane:IsA("GuiBasePane") and pane.Size == Vector2.new(100, 100)
        pane.CurrentCamera = camera
        pane.Ambient = Color3.new(1, 0, 0)
        _G.set = pane.CurrentCamera == camera and pane.Ambient.R == 1
        _G.refused = not pcall(function() pane.CurrentCamera = screen end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"defaults", "set", "refused"});
}
