#pragma once

struct GLFWwindow;

// One image per display refresh while the swap interval stays 0.
// The wait runs inside the GLFW event loop so the window server can commit
// the frame that was just swapped.
class PresentClock {
public:
    explicit PresentClock(GLFWwindow* window);
    ~PresentClock();
    PresentClock(const PresentClock&) = delete;
    PresentClock& operator=(const PresentClock&) = delete;
    PresentClock(PresentClock&&) = delete;
    PresentClock& operator=(PresentClock&&) = delete;

    // Follow the display under the window. A move onto another monitor retargets the clock.
    void follow(GLFWwindow* window);

    // Sleep until a platform event arrives. True when a display blank has not been drawn yet.
    bool waitForNewBlank();

    // The border-drag loop does not return to waitForNewBlank, and it starts
    // before the window frame changes. `present` runs on the main thread for
    // each blank that arrives while that loop is waiting. The blank is
    // acknowledged after `present` returns.
    void setResizePresent(void (*present)(void* context), void* context);

    // Drop blanks that arrived while the frame was being drawn, so they are not presented late.
    void acknowledge();

private:
    struct Impl;
    Impl* impl_;
};
