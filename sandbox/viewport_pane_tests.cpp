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
#include "Animator.hpp"
#include "GameObject.hpp"
#include "Light.hpp"
#include "PhysicsObject.hpp"
#include "SceneService.hpp"
#include "Skybox.hpp"
#include "SnapshotPump.hpp"
#include "SoundEmitter.hpp"
#include "WireframeAdornment.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

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

bool listed(const std::vector<InstanceId>& ids, InstanceId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
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

TEST_CASE("VW1 what is in a ViewportPane is InViewport, never in Workspace or Lighting", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId workspace = game.scene_service("Workspace");
    // A pane under a BillboardGui in Workspace: the case that leaked.
    engine_core::GameObject& anchor = game.create_game_object();
    game.set_parent(anchor.id(), workspace);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), anchor.id());
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), board.id());
    engine_core::GameObject& inside = game.create_game_object();
    game.set_parent(inside.id(), pane.id());
    REQUIRE(game.in_workspace(pane.id()));
    REQUIRE_FALSE(game.in_viewport(pane.id()));
    REQUIRE(game.in_viewport(inside.id()));
    REQUIRE_FALSE(game.in_workspace(inside.id()));
    REQUIRE(game.in_game(inside.id()));
    std::vector<InstanceId> panes;
    game.viewport_panes(panes);
    REQUIRE(listed(panes, pane.id()));
    // Out of the pane, back in Workspace.
    game.set_parent(inside.id(), workspace);
    REQUIRE_FALSE(game.in_viewport(inside.id()));
    REQUIRE(game.in_workspace(inside.id()));
}

TEST_CASE("VW2 a pane in Core keeps InCore below it but draws nothing in the Scene View", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), game.core());
    engine_core::GameObject& inside = game.create_game_object();
    game.set_parent(inside.id(), pane.id());
    engine_core::PointLight& light = game.create<engine_core::PointLight>();
    game.set_parent(light.id(), pane.id());
    REQUIRE(game.in_core(inside.id()));
    REQUIRE(game.in_viewport(inside.id()));
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE(pump.find(inside.id()) == nullptr);
    REQUIRE(pump.find(light.id()) == nullptr);
}

TEST_CASE("VW3 a light in a pane under Lighting does not light the place", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), game.scene_service("Lighting"));
    engine_core::DirectionalLight& sun = game.create<engine_core::DirectionalLight>();
    game.set_parent(sun.id(), pane.id());
    engine_core::PointLight& lamp = game.create<engine_core::PointLight>();
    game.set_parent(lamp.id(), pane.id());
    REQUIRE_FALSE(game.in_lighting(sun.id()));
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE(pump.find(sun.id()) == nullptr);
    REQUIRE(pump.find(lamp.id()) == nullptr);
    // Stop asks for a resync, which walks Lighting and must stop at the pane too.
    game.capture_place();
    game.start_simulation();
    game.stop_simulation();
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE(pump.find(sun.id()) == nullptr);
    REQUIRE(pump.find(lamp.id()) == nullptr);
}

TEST_CASE("VW4 physics, sound, and Draggers skip a pane's contents; Animators there have their own list", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::GameObject& anchor = game.create_game_object();
    game.set_parent(anchor.id(), workspace);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), anchor.id());
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), board.id());
    engine_core::PhysicsObject& body = game.create<engine_core::PhysicsObject>();
    game.set_parent(body.id(), pane.id());
    engine_core::SoundEmitter& sound = game.create<engine_core::SoundEmitter>();
    game.set_parent(sound.id(), pane.id());
    engine_core::GameObject& rig = game.create_game_object();
    game.set_parent(rig.id(), pane.id());
    engine_core::Animator& animator = game.create<engine_core::Animator>();
    game.set_parent(animator.id(), rig.id());
    std::vector<InstanceId> ids;
    game.physics_bodies(ids);
    REQUIRE_FALSE(listed(ids, body.id()));
    game.sound_sources(ids);
    REQUIRE_FALSE(listed(ids, sound.id()));
    game.animators(ids);
    REQUIRE_FALSE(listed(ids, animator.id()));
    game.viewport_animators(ids);
    REQUIRE(listed(ids, animator.id()));
}

TEST_CASE("VW5 a Simulated GameObject with Velocity does not move inside a pane", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    engine_core::GameObject& thing = game.create_game_object();
    game.set_parent(thing.id(), pane.id());
    game.set_simulated(thing.id(), true);
    thing.set_linear_velocity(1.f, 0.f, 0.f);
    game.integrate_simulated(1.0);
    REQUIRE(thing.transform().m[12] == 0.f);
}

TEST_CASE("VW6 Workspace.CurrentCamera refuses a Camera inside a ViewportPane", "[viewport]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local screen = Instance.new("ScreenGui", game.Gui)
        local pane = Instance.new("ViewportPane", screen)
        local camera = Instance.new("Camera", pane)
        local ok, why = pcall(function() workspace.CurrentCamera = camera end)
        _G.refused = not ok and string.find(tostring(why), "CurrentCamera cannot be a Camera inside a ViewportPane", 1, true) ~= nil
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"refused"});
}

TEST_CASE("VW7 a WireframeAdornment in a pane under Workspace is not drawn", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& anchor = game.create_game_object();
    game.set_parent(anchor.id(), game.scene_service("Workspace"));
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), anchor.id());
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), board.id());
    engine_core::WireframeAdornment& wire = game.create<engine_core::WireframeAdornment>();
    game.set_parent(wire.id(), pane.id());
    REQUIRE_FALSE(wire.drawn());
}

TEST_CASE("VW8 a Skybox in a pane under Lighting is not the place's sky", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), game.scene_service("Lighting"));
    engine_core::Skybox& sky = game.create<engine_core::Skybox>();
    game.set_parent(sky.id(), pane.id());
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE_FALSE(pump.front().sky.present);
}
