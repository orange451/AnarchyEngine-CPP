#pragma once

#include "types.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core {

// One gamepad as the host polled it, laid out as GLFW's GLFWgamepadstate so the
// host copies it straight in: buttons by GLFW_GAMEPAD_BUTTON_A (0) to DPAD_LEFT
// (14), axes LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER, RIGHT_TRIGGER. The
// sticks' Y is down-positive, and the triggers rest at -1.
struct GamepadState {
    bool connected = false;
    bool buttons[15] = {};
    float axes[6] = {};
};

// A stick with a radial dead zone: at or under dead it is (0, 0); past it the
// length is rescaled so dead is 0 and the rim is 1. Y is turned up-positive.
inline Vec3 stick_with_dead_zone(float x, float y, float dead = 0.15f) {
    const float length = std::hypot(x, y);
    if (length <= dead) {
        return Vec3{};
    }
    const float scale = std::min((length - dead) / (1.f - dead), 1.f) / length;
    return Vec3{x * scale, -y * scale, 0.f};
}

// A trigger from GLFW's -1 at rest to 1 pressed, as 0 to 1.
inline float trigger_value(float axis) { return std::clamp((axis + 1.f) * 0.5f, 0.f, 1.f); }

}  // namespace engine_core
