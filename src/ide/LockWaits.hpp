#pragma once

#include <chrono>

namespace ide {

// How long the UI thread waits for the world's read lock. A read made every
// frame waits little: missing it costs one frame, and the next one tries
// again. A read the user asked for, such as a completion or a script load,
// waits longer before it gives up.
inline constexpr std::chrono::milliseconds kFrameLockWait{1};
inline constexpr std::chrono::milliseconds kActionLockWait{5};

}  // namespace ide
