// Gamepads on UserInputService: posted states diffed into InputBegan,
// InputChanged, and InputEnded, the pads' own signals, and the direct polls.

#include "support.hpp"

#include "GamepadState.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "UserInputService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> output(ScriptRig& rig) {
    std::vector<std::string> out;
    for (const auto& line : rig.runtime.drain_output().lines) {
        UNSCOPED_INFO(line.text);
        out.push_back(line.text);
    }
    return out;
}

bool has_line(const std::vector<std::string>& lines, const std::string& text) {
    return std::find(lines.begin(), lines.end(), text + "\n") != lines.end();
}

bool near(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }

// As the play step runs it: input is dispatched at PreAnimation, then a frame.
void input_frame(ScriptRig& rig) {
    rig.scheduler.run_phase(engine_core::Phase::PreAnimation, 1.0 / 60.0);
    rig.game.events().drain();
    rig.frames(1);
}

// A connected pad at rest: triggers at GLFW's -1.
engine_core::GamepadState pad_at_rest() {
    engine_core::GamepadState state;
    state.connected = true;
    state.axes[4] = -1.f;
    state.axes[5] = -1.f;
    return state;
}

constexpr int kGlfwA = 0;
constexpr int kLeftX = 0;
constexpr int kLeftY = 1;
constexpr int kLeftTrigger = 4;

// Prints every input signal, so a test reads what fired from the output.
const char* const kListener = R"(
local input = game:GetService("UserInputService")
input.InputBegan:Connect(function(io)
    print("began", io.KeyCode, io.UserInputType, io.UserInputState)
end)
input.InputEnded:Connect(function(io)
    print("ended", io.KeyCode, io.UserInputType, io.UserInputState)
end)
input.InputChanged:Connect(function(io)
    print("changed", io.KeyCode.Name, io.UserInputType.Name, io.UserInputState.Name,
        string.format("%.3f %.3f", io.Position.X, io.Position.Y))
end)
input.GamepadConnected:Connect(function(pad)
    print("connected", pad, #input:GetConnectedGamepads())
end)
input.GamepadDisconnected:Connect(function(pad)
    print("disconnected", pad, #input:GetConnectedGamepads())
end)
_G.poll = function()
    local stick = input:GetGamepadAxis(Enum.UserInputType.Gamepad1, Enum.KeyCode.Thumbstick1)
    local trigger = input:GetGamepadAxis(Enum.UserInputType.Gamepad1, Enum.KeyCode.ButtonL2)
    print("poll", input:IsGamepadButtonDown(Enum.UserInputType.Gamepad1, Enum.KeyCode.ButtonA),
        input:IsGamepadButtonDown(Enum.UserInputType.Gamepad1, Enum.KeyCode.ButtonL2),
        string.format("%.3f %.3f %.3f %.3f", stick.X, stick.Y, trigger.X, trigger.Y), typeof(stick))
end
)";

void poll(ScriptRig& rig) {
    add_script(rig.game, "Poll", "_G.poll()");
    input_frame(rig);
}

}  // namespace

TEST_CASE("GP1 the stick dead zone is radial, rescaled, and turns GLFW's Y up", "[gamepad]") {
    using engine_core::stick_with_dead_zone;
    const engine_core::Vec3 inside = stick_with_dead_zone(0.1f, -0.1f);
    REQUIRE((inside.x == 0.f && inside.y == 0.f && inside.z == 0.f));
    REQUIRE(stick_with_dead_zone(0.15f, 0.f).x == 0.f);
    const engine_core::Vec3 up = stick_with_dead_zone(0.f, -1.f);
    REQUIRE(near(up.x, 0));
    REQUIRE(near(up.y, 1));
    const engine_core::Vec3 corner = stick_with_dead_zone(1.f, 1.f);
    REQUIRE(near(std::hypot(corner.x, corner.y), 1));
    REQUIRE(corner.x > 0);
    REQUIRE(corner.y < 0);
    // Halfway between the dead zone and the rim is half deflection.
    const engine_core::Vec3 half = stick_with_dead_zone(0.575f, 0.f);
    REQUIRE(near(half.x, 0.5));
    REQUIRE(near(half.y, 0));
    REQUIRE(near(engine_core::trigger_value(-1.f), 0));
    REQUIRE(near(engine_core::trigger_value(0.f), 0.5));
    REQUIRE(near(engine_core::trigger_value(1.f), 1));
}

TEST_CASE("GP2 a button begins and ends with the pad as UserInputType; an unchanged state fires nothing", "[gamepad]") {
    ScriptRig rig;
    add_script(rig.game, "Listen", kListener);
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    output(rig);

    engine_core::UserInputService& input = rig.game.input();
    engine_core::GamepadState state = pad_at_rest();
    input.post_gamepad(0, state);
    input_frame(rig);
    REQUIRE(has_line(output(rig), "connected\tEnum.UserInputType.Gamepad1\t1"));

    state.buttons[kGlfwA] = true;
    input.post_gamepad(0, state);
    input_frame(rig);
    REQUIRE(has_line(output(rig), "began\tEnum.KeyCode.ButtonA\tEnum.UserInputType.Gamepad1\tEnum.UserInputState.Begin"));
    REQUIRE(input.gamepad_button_down(0, engine_core::UserInputService::kButtonA));

    input.post_gamepad(0, state);
    input_frame(rig);
    REQUIRE(output(rig).empty());

    state.buttons[kGlfwA] = false;
    input.post_gamepad(0, state);
    input_frame(rig);
    REQUIRE(has_line(output(rig), "ended\tEnum.KeyCode.ButtonA\tEnum.UserInputType.Gamepad1\tEnum.UserInputState.End"));
    REQUIRE_FALSE(input.gamepad_button_down(0, engine_core::UserInputService::kButtonA));
    rig.game.stop_simulation();
}

TEST_CASE("GP3 a stick or trigger that moves fires InputChanged, and GetGamepadAxis reads it", "[gamepad]") {
    ScriptRig rig;
    add_script(rig.game, "Listen", kListener);
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());

    engine_core::UserInputService& input = rig.game.input();
    engine_core::GamepadState state = pad_at_rest();
    input.post_gamepad(0, state);
    input_frame(rig);
    output(rig);

    state.axes[kLeftY] = -1.f;  // pushed up
    state.axes[kLeftTrigger] = 0.6f;  // 0.8 of the way down
    input.post_gamepad(0, state);
    input_frame(rig);
    std::vector<std::string> lines = output(rig);
    REQUIRE(has_line(lines, "changed\tThumbstick1\tGamepad1\tChange\t0.000 1.000"));
    REQUIRE(has_line(lines, "changed\tButtonL2\tGamepad1\tChange\t0.800 0.000"));
    REQUIRE(has_line(lines, "began\tEnum.KeyCode.ButtonL2\tEnum.UserInputType.Gamepad1\tEnum.UserInputState.Begin"));

    poll(rig);
    REQUIRE(has_line(output(rig), "poll\tfalse\ttrue\t0.000 1.000 0.800 0.000\tVector2"));

    // Jitter under a thousandth moves nothing.
    state.axes[kLeftX] = 0.0004f;
    input.post_gamepad(0, state);
    input_frame(rig);
    REQUIRE(output(rig).empty());

    state.axes[kLeftY] = 0.f;
    state.axes[kLeftTrigger] = -1.f;
    input.post_gamepad(0, state);
    input_frame(rig);
    lines = output(rig);
    REQUIRE(has_line(lines, "changed\tThumbstick1\tGamepad1\tChange\t0.000 0.000"));
    REQUIRE(has_line(lines, "ended\tEnum.KeyCode.ButtonL2\tEnum.UserInputType.Gamepad1\tEnum.UserInputState.End"));
    rig.game.stop_simulation();
}

TEST_CASE("GP4 GamepadConnected, GamepadDisconnected, and GetConnectedGamepads", "[gamepad]") {
    ScriptRig rig;
    add_script(rig.game, "Listen", kListener);
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());

    engine_core::UserInputService& input = rig.game.input();
    input.post_gamepad(0, pad_at_rest());
    input.post_gamepad(2, pad_at_rest());
    input_frame(rig);
    std::vector<std::string> lines = output(rig);
    // Both are dispatched before either handler runs.
    REQUIRE(has_line(lines, "connected\tEnum.UserInputType.Gamepad1\t2"));
    REQUIRE(has_line(lines, "connected\tEnum.UserInputType.Gamepad3\t2"));
    REQUIRE(input.gamepad_connected(0));
    REQUIRE_FALSE(input.gamepad_connected(1));
    REQUIRE(input.connected_gamepads() == std::vector<int>{0, 2});

    // A disconnected pad posted again is still nothing.
    input.post_gamepad(1, engine_core::GamepadState{});
    input.post_gamepad(0, engine_core::GamepadState{});
    input_frame(rig);
    lines = output(rig);
    REQUIRE(lines.size() == 1);
    REQUIRE(has_line(lines, "disconnected\tEnum.UserInputType.Gamepad1\t1"));
    REQUIRE(input.connected_gamepads() == std::vector<int>{2});
    rig.game.stop_simulation();
}

TEST_CASE("GP5 a pad unplugged with buttons held ends each of them, and the polls read released", "[gamepad]") {
    ScriptRig rig;
    add_script(rig.game, "Listen", kListener);
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());

    engine_core::UserInputService& input = rig.game.input();
    engine_core::GamepadState state = pad_at_rest();
    state.buttons[kGlfwA] = true;
    state.axes[kLeftX] = 1.f;
    state.axes[kLeftTrigger] = 1.f;
    input.post_gamepad(0, state);
    input_frame(rig);
    output(rig);
    REQUIRE(input.gamepad_button_down(0, engine_core::UserInputService::kButtonA));

    input.post_gamepad(0, engine_core::GamepadState{});
    input_frame(rig);
    const std::vector<std::string> lines = output(rig);
    REQUIRE(has_line(lines, "ended\tEnum.KeyCode.ButtonA\tEnum.UserInputType.Gamepad1\tEnum.UserInputState.End"));
    REQUIRE(has_line(lines, "ended\tEnum.KeyCode.ButtonL2\tEnum.UserInputType.Gamepad1\tEnum.UserInputState.End"));
    REQUIRE(has_line(lines, "changed\tThumbstick1\tGamepad1\tChange\t0.000 0.000"));
    REQUIRE(has_line(lines, "changed\tButtonL2\tGamepad1\tChange\t0.000 0.000"));
    REQUIRE(has_line(lines, "disconnected\tEnum.UserInputType.Gamepad1\t0"));
    poll(rig);
    REQUIRE(has_line(output(rig), "poll\tfalse\tfalse\t0.000 0.000 0.000 0.000\tVector2"));

    // Losing focus ends a connected pad's buttons too; the pad stays connected.
    input.post_gamepad(0, state);
    input_frame(rig);
    output(rig);
    input.post_focus_lost();
    input_frame(rig);
    REQUIRE(has_line(output(rig), "ended\tEnum.KeyCode.ButtonA\tEnum.UserInputType.Gamepad1\tEnum.UserInputState.End"));
    REQUIRE_FALSE(input.gamepad_button_down(0, engine_core::UserInputService::kButtonA));
    REQUIRE(input.gamepad_connected(0));
    rig.game.stop_simulation();
}

TEST_CASE("GP6 strict analysis accepts the gamepad members; a non-gamepad type raises", "[gamepad]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Uses", R"(--!strict
local input = game:GetService("UserInputService")
local pads = input:GetConnectedGamepads()
local down: boolean = input:IsGamepadButtonDown(Enum.UserInputType.Gamepad1, Enum.KeyCode.ButtonA)
local stick: Vector2 = input:GetGamepadAxis(Enum.UserInputType.Gamepad1, Enum.KeyCode.Thumbstick1)
input.GamepadConnected:Connect(function(pad: EnumItem) print(pad) end)
input.GamepadDisconnected:Connect(function(pad: EnumItem) print(pad) end)
print(#pads, down, stick.X)
)");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!analysis.idle()) {
        analysis.pump();
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    std::string problems;
    for (const engine_core::Diagnostic& diagnostic : analysis.diagnostics(script.id())) {
        if (diagnostic.severity == engine_core::Severity::Error || diagnostic.severity == engine_core::Severity::Warning) {
            problems += diagnostic.code + " @" + std::to_string(diagnostic.range.start.line) + " " + diagnostic.message + "\n";
        }
    }
    INFO(problems);
    REQUIRE(problems.empty());

    add_script(rig.game, "Refuse", R"(
local input = game:GetService("UserInputService")
local function try(f) local ok, err = pcall(f) print(ok, err) end
try(function() return input:IsGamepadButtonDown(Enum.UserInputType.Keyboard, Enum.KeyCode.ButtonA) end)
try(function() return input:GetGamepadAxis(Enum.UserInputType.Gamepad1, Enum.KeyCode.ButtonA) end)
try(function() return input:IsGamepadButtonDown(Enum.UserInputType.Gamepad1, Enum.KeyCode.W) end)
print("unconnected", input:IsGamepadButtonDown(Enum.UserInputType.Gamepad4, Enum.KeyCode.ButtonA),
    input:GetGamepadAxis(Enum.UserInputType.Gamepad4, Enum.KeyCode.Thumbstick2).X)
)");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    const std::vector<std::string> lines = output(rig);
    const auto line_has = [&](const char* start, const char* part) {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.rfind(start, 0) == 0 && line.find(part) != std::string::npos;
        });
    };
    REQUIRE(line_has("false\t", "expected a gamepad, such as Enum.UserInputType.Gamepad1"));
    REQUIRE(line_has("false\t", "expected Thumbstick1, Thumbstick2, ButtonL2, or ButtonR2"));
    REQUIRE(line_has("false\t", "expected a gamepad button, such as Enum.KeyCode.ButtonA"));
    REQUIRE(has_line(lines, "unconnected\tfalse\t0"));
    rig.game.stop_simulation();
}

TEST_CASE("GP7 a pad plugged in before Test is announced and listed when the session starts", "[gamepad]") {
    ScriptRig rig;
    add_script(rig.game, "Listen", kListener);
    engine_core::UserInputService& input = rig.game.input();
    // In edit mode the attached runtime keeps input, and the tool step dispatches it.
    REQUIRE(input.active());
    input.post_gamepad(0, pad_at_rest());
    rig.frames(1);
    REQUIRE(input.gamepad_connected(0));

    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    output(rig);
    // The host keeps posting the same state it posted before Test.
    input.post_gamepad(0, pad_at_rest());
    input_frame(rig);
    REQUIRE(has_line(output(rig), "connected\tEnum.UserInputType.Gamepad1\t1"));
    REQUIRE(input.gamepad_connected(0));
    rig.game.stop_simulation();
}
