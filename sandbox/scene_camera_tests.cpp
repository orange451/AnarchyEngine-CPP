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
