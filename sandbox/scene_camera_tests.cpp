// The scene camera: UserInputService's mouse lock and delta, edit-mode input,
// Workspace.CurrentCamera, and the built-in SceneCamera plugin.

#include "support.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "Matrix4.hpp"
#include "Project.hpp"
#include "SceneService.hpp"
#include "UserInputService.hpp"
#include "ide/PluginLoader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;
using engine_core::ScriptRuntime;
using engine_core::UserInputService;

std::vector<std::string> texts(const ScriptRuntime::OutputBatch& batch) {
    std::vector<std::string> out;
    for (const ScriptRuntime::OutputLine& line : batch.lines) {
        out.push_back(line.text);
    }
    return out;
}

}  // namespace

TEST_CASE("SC1 locked mouse motion adds up for one step and leaves the location", "[SC1]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    input.set_active(true);
    input.post_mouse_move(10.f, 20.f);
    input.post_mouse_delta(3.f, -1.f);
    input.post_mouse_delta(2.f, 4.f);
    input.dispatch(game.events());
    REQUIRE(input.mouse_delta().x == 5.f);
    REQUIRE(input.mouse_delta().y == 3.f);
    REQUIRE(input.mouse_location().x == 10.f);
    REQUIRE(input.mouse_location().y == 20.f);

    input.dispatch(game.events());
    REQUIRE(input.mouse_delta().x == 0.f);
    REQUIRE(input.mouse_delta().y == 0.f);
}

TEST_CASE("SC2 MouseBehavior holds what was asked, through a focus loss", "[SC2]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    input.set_active(true);
    REQUIRE(input.mouse_behavior() == UserInputService::kMouseBehaviorDefault);
    input.set_mouse_behavior(UserInputService::kLockCurrentPosition);
    REQUIRE(input.mouse_behavior() == UserInputService::kLockCurrentPosition);
    // The view locks again when it is clicked, so the ask stays.
    input.post_focus_lost();
    REQUIRE(input.mouse_behavior() == UserInputService::kLockCurrentPosition);
}

TEST_CASE("SC21 a lock that starts again drops the motion from before it", "[SC21]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    input.set_active(true);
    input.set_mouse_behavior(UserInputService::kLockCurrentPosition);
    // The pointer, free while the view was not focused, moves to the click.
    input.post_mouse_move(0.f, 0.f);
    input.post_mouse_move(40.f, 30.f);
    input.note_lock_started();
    input.dispatch(game.events());
    REQUIRE(input.mouse_delta().x == 0.f);
    REQUIRE(input.mouse_delta().y == 0.f);

    input.post_mouse_delta(2.f, -1.f);
    input.dispatch(game.events());
    REQUIRE(input.mouse_delta().x == 2.f);
    REQUIRE(input.mouse_delta().y == -1.f);
}

TEST_CASE("SC3 MouseDeltaSensitivity is at least 0 and refuses what is not a number", "[SC3]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    REQUIRE(input.mouse_delta_sensitivity() == 1.0);
    REQUIRE(input.set_mouse_delta_sensitivity(2.5));
    REQUIRE(input.mouse_delta_sensitivity() == 2.5);
    REQUIRE(input.set_mouse_delta_sensitivity(-3.0));
    REQUIRE(input.mouse_delta_sensitivity() == 0.0);
    REQUIRE_FALSE(input.set_mouse_delta_sensitivity(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE(input.mouse_delta_sensitivity() == 0.0);
}

namespace {

// GLFW's key numbers, which is what the scene view posts.
int key(char letter) { return UserInputService::key_code_from_glfw(static_cast<int>(letter)); }

// An unparented Script run as a plugin.
InstanceId add_plugin(ScriptRig& rig, const char* source) {
    engine_core::Script& script = rig.game.create<engine_core::Script>();
    rig.game.set_name(script.id(), "Plugin");
    script.set_source(source);
    REQUIRE(rig.runtime.register_plugin(script.id()));
    return script.id();
}

}  // namespace

TEST_CASE("SC4 a plugin hears keys in edit mode, and a focus loss ends them", "[SC4]") {
    ScriptRig rig;
    add_plugin(rig,
               "local uis = game:GetService('UserInputService')\n"
               "uis.InputBegan:Connect(function(input) print('began', input.KeyCode.Name) end)\n"
               "uis.InputEnded:Connect(function(input) print('ended', input.KeyCode.Name) end)\n"
               "game:GetService('RunService').Heartbeat:Connect(function()\n"
               "  if uis:IsKeyDown(Enum.KeyCode.W) then print('held') end\n"
               "end)");
    rig.runtime.drain_output();

    rig.game.input().post_key(key('W'), true);
    rig.frames(1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"began\tW\n", "held\n"});

    rig.game.input().post_focus_lost();
    rig.frames(1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"ended\tW\n"});
}

TEST_CASE("SC5 scripts read and write MouseBehavior", "[SC5]") {
    ScriptRig rig;
    rig.runtime.run_chunk(
        "local uis = game:GetService('UserInputService')\n"
        "print(uis.MouseBehavior == Enum.MouseBehavior.Default)\n"
        "uis.MouseBehavior = Enum.MouseBehavior.LockCurrentPosition\n"
        "print(uis.MouseBehavior.Name)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"true\n", "LockCurrentPosition\n"});
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kLockCurrentPosition);

    rig.runtime.run_chunk("game:GetService('UserInputService').MouseBehavior = 'Nope'");
    const ScriptRuntime::OutputBatch refused = rig.runtime.drain_output();
    REQUIRE(refused.lines.size() == 1);
    REQUIRE(refused.lines[0].kind == ScriptRuntime::OutputKind::Error);
}

TEST_CASE("SC6 GetMouseDelta is the step's motion times MouseDeltaSensitivity", "[SC6]") {
    ScriptRig rig;
    add_plugin(rig,
               "local uis = game:GetService('UserInputService')\n"
               "uis.MouseDeltaSensitivity = 2\n"
               "game:GetService('RunService').Heartbeat:Connect(function()\n"
               "  local d = uis:GetMouseDelta()\n"
               "  if d.Magnitude > 0 then print(d.X, d.Y) end\n"
               "end)");
    rig.runtime.drain_output();
    rig.game.input().post_mouse_delta(3.f, 4.f);
    rig.frames(2);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"6\t8\n"});
}

TEST_CASE("SC7 RunService:IsRunning is true only in a play session", "[SC7]") {
    ScriptRig rig;
    rig.runtime.run_chunk("print(game:GetService('RunService'):IsRunning())");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"false\n"});
    // Test clears the Output, so both lines below are printed after it.
    rig.game.start_simulation();
    rig.runtime.run_chunk("print(game:GetService('RunService'):IsRunning())");
    // A sensitivity the game sets is its own, and goes with the session.
    rig.runtime.run_chunk("game:GetService('UserInputService').MouseDeltaSensitivity = 3");
    REQUIRE(rig.game.input().mouse_delta_sensitivity() == 3.0);
    rig.game.stop_simulation();
    REQUIRE(rig.game.input().mouse_delta_sensitivity() == 1.0);
    rig.runtime.run_chunk("print(game:GetService('RunService'):IsRunning())");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"true\n", "false\n"});
}

namespace {

engine_core::Workspace& workspace_service(engine_core::DataModel& game) {
    auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    return *workspace;
}

engine_core::Camera& add_camera(engine_core::DataModel& game) {
    engine_core::Camera& camera = game.create<engine_core::Camera>();
    game.set_parent(camera.id(), workspace_of(game));
    return camera;
}

}  // namespace

TEST_CASE("SC8 CurrentCamera holds a Camera and forgets it when it is gone", "[SC8]") {
    ScriptRig rig;
    engine_core::Workspace& workspace = workspace_service(rig.game);
    engine_core::Camera& camera = add_camera(rig.game);
    REQUIRE(workspace.current_camera() == 0);
    REQUIRE(workspace.set_current_camera(camera.id()));
    REQUIRE(workspace.current_camera() == camera.id());
    REQUIRE_FALSE(workspace.set_current_camera(workspace_of(rig.game)));
    REQUIRE(workspace.current_camera() == camera.id());

    rig.runtime.drain_output();
    rig.runtime.run_chunk("print(workspace.CurrentCamera.ClassName)\nworkspace.CurrentCamera = nil\nprint(workspace.CurrentCamera)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"Camera\n", "nil\n"});

    REQUIRE(workspace.set_current_camera(camera.id()));
    rig.game.destroy(camera.id());
    REQUIRE(workspace.current_camera() == 0);
}

TEST_CASE("SC9 a script cannot make CurrentCamera anything but a Camera", "[SC9]") {
    ScriptRig rig;
    rig.runtime.run_chunk("workspace.CurrentCamera = workspace");
    const ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].kind == ScriptRuntime::OutputKind::Error);
}

TEST_CASE("SC10 CurrentCamera is not an edit: no undo step, not saved, cleared by a new place", "[SC10]") {
    ScriptRig rig;
    engine_core::Camera& camera = add_camera(rig.game);
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    const std::uint64_t revision = rig.game.authored_revision();
    const std::uint64_t fingerprint = engine_core::Project::place_fingerprint(rig.game);

    REQUIRE(workspace_service(rig.game).set_current_camera(camera.id()));
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE(rig.game.authored_revision() == revision);
    REQUIRE(engine_core::Project::place_fingerprint(rig.game) == fingerprint);

    engine_core::Project::reset_place(rig.game);
    REQUIRE(workspace_service(rig.game).current_camera() == 0);
}

TEST_CASE("SC10b a new CurrentCamera is heard by the Properties page and by Changed", "[SC10b]") {
    ScriptRig rig;
    engine_core::Workspace& workspace = workspace_service(rig.game);
    engine_core::Camera& camera = add_camera(rig.game);
    int heard = 0;
    const std::uint64_t watch = rig.game.watch_changes([&heard] { ++heard; });
    rig.game.set_watched(watch, {workspace.id()});
    rig.runtime.run_chunk("workspace.Changed:Connect(function(name) print(name) end)");
    rig.runtime.drain_output();

    REQUIRE(workspace.set_current_camera(camera.id()));
    REQUIRE(heard == 1);
    rig.game.events().drain();
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"CurrentCamera\n"});

    // The same Camera again changes nothing, and says nothing.
    REQUIRE(workspace.set_current_camera(camera.id()));
    REQUIRE(heard == 1);
    rig.game.unwatch_changes(watch);
}

TEST_CASE("SC11 moving a Camera marks the place changed but is not an undo step", "[SC11]") {
    ScriptRig rig;
    engine_core::Camera& camera = add_camera(rig.game);
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    const std::uint64_t revision = rig.game.authored_revision();

    for (int step = 1; step <= 50; ++step) {
        camera.set_transform(engine_core::matrix4_translation(0.f, 0.f, static_cast<float>(step)));
    }
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE(rig.game.authored_revision() != revision);

    part.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));
    rig.game.history().end_gesture();
    REQUIRE(rig.game.history().can_undo().first);
}

namespace {

ide::PluginFile scene_camera_file() {
    ide::PluginFile file;
    std::string error;
    REQUIRE(ide::read_plugin_file(std::filesystem::path(ANARCHY_SOURCE_DIR) / "resources/plugins/SceneCamera.luau", file,
                                  error));
    return file;
}

// A Camera at the origin looking down -Z, the view's current one, and the plugin loaded.
struct CameraRig : ScriptRig {
    ide::PluginLoader loader;
    InstanceId camera = 0;

    CameraRig() {
        camera = add_camera(game).id();
        REQUIRE(workspace_service(game).set_current_camera(camera));
        REQUIRE(loader.load(game, runtime, {scene_camera_file()}) == 1);
        runtime.drain_output();
    }

    // The plugin moves the camera on RenderStepped, which only runs in rig.render()
    // (the window), not in frames() (the sim step). Input dispatches in step_tools,
    // so a key pressed before a frames() call is current by the next render() call's
    // handler run. A test that wants the plugin to actually move or turn the camera
    // calls render() itself, same as ScriptRig::render does for any other window test.
    using ScriptRig::frames;

    engine_core::Matrix4 transform() { return dynamic_cast<engine_core::Camera*>(game.instance(camera))->transform(); }
    engine_core::Vec3 position() {
        const engine_core::Matrix4 m = transform();
        return {m.m[12], m.m[13], m.m[14]};
    }
    engine_core::Vec3 look() {
        const engine_core::Matrix4 m = transform();
        return {-m.m[8], -m.m[9], -m.m[10]};
    }
};

}  // namespace

TEST_CASE("SC12 W moves the camera where it looks, and E lifts it", "[SC12]") {
    CameraRig rig;
    rig.game.input().post_key(key('W'), true);
    rig.frames(1, 0.5);
    rig.render(0.5);
    REQUIRE(std::abs(rig.position().z + 8.f) < 1e-3f);  // 16 studs/s for half a second, down -Z
    rig.game.input().post_key(key('W'), false);
    rig.game.input().post_key(key('E'), true);
    rig.frames(1, 0.25);
    rig.render(0.25);
    REQUIRE(std::abs(rig.position().y - 4.f) < 1e-3f);
    REQUIRE(rig.runtime.drain_output().lines.empty());
}

TEST_CASE("SC13 the right button locks the pointer and the locked motion turns the camera", "[SC13]") {
    CameraRig rig;
    rig.game.input().post_mouse_button(1, true, 50.f, 50.f);
    rig.frames(1);
    rig.render();
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kLockCurrentPosition);

    // Right is +X from a camera looking down -Z.
    rig.game.input().post_mouse_delta(100.f, 0.f);
    rig.frames(1);
    rig.render();
    REQUIRE(rig.look().x > 0.3f);

    // Far up: the pitch stops at 89 degrees.
    rig.game.input().post_mouse_delta(0.f, -100000.f);
    rig.frames(1);
    rig.render();
    REQUIRE(rig.look().y > 0.99f);
    REQUIRE(std::asin(std::min(1.f, rig.look().y)) <= 89.01f * 3.14159265f / 180.f);

    rig.game.input().post_mouse_button(1, false, 50.f, 50.f);
    rig.frames(1);
    rig.render();
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kMouseBehaviorDefault);

    // Loading again, as after opening a place, leaves one plugin.
    REQUIRE(rig.loader.load(rig.game, rig.runtime, {scene_camera_file()}) == 1);
    REQUIRE(rig.runtime.plugins().size() == 1);
}

TEST_CASE("SC14 the scene camera moves per rendered frame in edit mode", "[SC14]") {
    CameraRig rig;
    rig.game.input().post_key(key('W'), true);
    rig.frames(1, 0.0);           // input reaches the plugin VM
    rig.render(0.25);             // one rendered frame moves the camera
    rig.render(0.25);
    REQUIRE(std::abs(rig.position().z + 8.f) < 1e-3f);  // 16 studs/s for half a second
}

TEST_CASE("SC22 loading the built-in plugins is not an edit to the place", "[SC22]") {
    ScriptRig rig;
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    const std::uint64_t fingerprint = engine_core::Project::place_fingerprint(rig.game);
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, {scene_camera_file()}) == 1);
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE(engine_core::Project::place_fingerprint(rig.game) == fingerprint);
    REQUIRE(rig.game.get_children(workspace_of(rig.game)).empty());
}

TEST_CASE("SC15 losing focus mid-turn lets the pointer go", "[SC15]") {
    CameraRig rig;
    rig.game.input().post_mouse_button(1, true, 50.f, 50.f);
    rig.frames(1);
    rig.render();
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kLockCurrentPosition);
    rig.game.input().post_focus_lost();
    rig.frames(1);
    rig.render();
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kMouseBehaviorDefault);
}

TEST_CASE("SC16 with no CurrentCamera, or in play, the plugin leaves the camera alone", "[SC16]") {
    CameraRig rig;
    const engine_core::Vec3 before = rig.position();
    rig.game.start_simulation();
    rig.game.input().post_key(key('W'), true);
    for (int i = 0; i < 3; ++i) {
        rig.frames(1);
        rig.render();
    }
    REQUIRE(rig.position().z == before.z);
    rig.game.stop_simulation();
    rig.runtime.drain_output();

    rig.game.destroy(rig.camera);
    rig.game.input().post_key(key('S'), true);
    for (int i = 0; i < 3; ++i) {
        rig.frames(1);
        rig.render();
    }
    REQUIRE(rig.runtime.drain_output().lines.empty());
}

TEST_CASE("SC17 a right-button release in play does not touch the game's own MouseBehavior", "[SC17]") {
    CameraRig rig;
    rig.game.start_simulation();
    rig.runtime.drain_output();
    // The game locked the pointer itself; the plugin must not be the one to let it go.
    rig.game.input().set_mouse_behavior(UserInputService::kLockCenter);
    rig.game.input().post_mouse_button(1, true, 50.f, 50.f);
    rig.game.input().post_mouse_button(1, false, 50.f, 50.f);
    // Input dispatches at PreAnimation in play; frames() only runs Heartbeat, so
    // drive the phase the way the real play step does.
    rig.scheduler.run_phase(engine_core::Phase::PreAnimation, 1.0 / 60.0);
    rig.game.events().drain();
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kLockCenter);
    rig.game.stop_simulation();
}

TEST_CASE("SC18 hover motion from before the lock is not turned into look motion", "[SC18]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    input.set_active(true);
    input.post_mouse_move(0.f, 0.f);
    input.dispatch(game.events());

    // Dispatched, then the lock starts in the same step, as a handler of the press does.
    input.post_mouse_move(30.f, 0.f);
    input.dispatch(game.events());
    REQUIRE(input.mouse_delta().x == 30.f);
    input.set_mouse_behavior(UserInputService::kLockCurrentPosition);
    REQUIRE(input.mouse_delta().x == 0.f);

    // Queued before the lock, dispatched after it.
    input.set_mouse_behavior(UserInputService::kMouseBehaviorDefault);
    input.post_mouse_move(50.f, 0.f);
    input.set_mouse_behavior(UserInputService::kLockCurrentPosition);
    input.post_mouse_delta(3.f, 0.f);
    input.dispatch(game.events());
    REQUIRE(input.mouse_delta().x == 3.f);
    REQUIRE(input.mouse_location().x == 50.f);

    // Once locked, a change to another lock does not drop the motion.
    input.post_mouse_delta(4.f, 0.f);
    input.dispatch(game.events());
    input.set_mouse_behavior(UserInputService::kLockCenter);
    REQUIRE(input.mouse_delta().x == 4.f);
}

TEST_CASE("SC19 the camera does not jump on the step the right button locks the pointer", "[SC19]") {
    CameraRig rig;
    rig.game.input().post_mouse_move(50.f, 50.f);
    rig.frames(1);
    rig.render();
    // The pointer moved on its way to the press, in the same step as the press.
    rig.game.input().post_mouse_move(250.f, 50.f);
    rig.game.input().post_mouse_button(1, true, 250.f, 50.f);
    rig.frames(1);
    rig.render();
    REQUIRE(rig.game.input().mouse_behavior() == UserInputService::kLockCurrentPosition);
    REQUIRE(std::abs(rig.look().x) < 1e-4f);
    REQUIRE(std::abs(rig.look().z + 1.f) < 1e-4f);
}

TEST_CASE("SC20 a plugin that cannot be read or run is reported, and the others still load", "[SC20]") {
    ide::PluginFile missing;
    std::string error;
    REQUIRE_FALSE(ide::read_plugin_file(std::filesystem::path(ANARCHY_SOURCE_DIR) / "resources/plugins/NoSuchPlugin.luau",
                                        missing, error));
    REQUIRE(error.find("NoSuchPlugin.luau") != std::string::npos);

    ScriptRig rig;
    ide::PluginFile good{"Good", "print('good ran')"};
    ide::PluginFile bad{"Bad", "this is not luau ("};
    ide::PluginLoader loader;
    loader.load(rig.game, rig.runtime, {bad, good});
    const ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    bool good_ran = false;
    bool bad_reported = false;
    for (const ScriptRuntime::OutputLine& line : batch.lines) {
        good_ran = good_ran || (line.kind == ScriptRuntime::OutputKind::Print && line.text == "good ran\n");
        bad_reported = bad_reported || (line.kind == ScriptRuntime::OutputKind::Error && line.text.find("Bad") != std::string::npos);
    }
    REQUIRE(good_ran);
    REQUIRE(bad_reported);
    REQUIRE(loader.loaded().size() == 2);
    REQUIRE(rig.runtime.is_plugin(loader.loaded()[1]));
}
