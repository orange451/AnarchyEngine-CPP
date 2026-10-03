#pragma once

#include "types.hpp"

namespace engine_core {

// One keyboard or mouse event, as a script's InputObject reads it. The enum
// fields hold Roblox values: KeyCode, UserInputType, and UserInputState.
struct InputRecord {
    int type = 22;  // UserInputType.None
    int state = 4;  // UserInputState.None
    int key = 0;    // KeyCode.Unknown
    // Pointer position in the scene view, in points from its top-left corner.
    // z is 0, except for MouseWheel, where z of position is the wheel movement.
    Vec3 position{};
    Vec3 delta{};
    // True when the studio, not the game, took the event.
    bool processed = false;
};

}  // namespace engine_core
