// The scene camera: UserInputService's mouse lock and delta, edit-mode input,
// Workspace.CurrentCamera, and the built-in SceneCamera plugin.

#include "support.hpp"

#include "UserInputService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

TEST_CASE("SC2 MouseBehavior holds what was asked until the view loses focus", "[SC2]") {
    SimRole role;
    engine_core::Game game;
    UserInputService& input = game.input();
    input.set_active(true);
    REQUIRE(input.mouse_behavior() == UserInputService::kMouseBehaviorDefault);
    input.set_mouse_behavior(UserInputService::kLockCurrentPosition);
    REQUIRE(input.mouse_behavior() == UserInputService::kLockCurrentPosition);
    input.post_focus_lost();
    REQUIRE(input.mouse_behavior() == UserInputService::kMouseBehaviorDefault);
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
    rig.game.stop_simulation();
    rig.runtime.run_chunk("print(game:GetService('RunService'):IsRunning())");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"true\n", "false\n"});
}
