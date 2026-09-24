#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "PresentClock.hpp"
#include "ide/IdeLayout.hpp"
#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <string>

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;
constexpr int kMinWidth = 720;
constexpr int kMinHeight = 480;
constexpr const char* kTitle = "Anarchy Engine";

char g_glfw_error[512] = {};

void CaptureGlfwError(int /*code*/, const char* description) {
    std::snprintf(g_glfw_error, sizeof g_glfw_error, "%s", description != nullptr ? description : "");
}

void SetCoreProfileHints() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // macOS only creates an OpenGL 3.2+ core context when this hint is set.
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
}

GLFWwindow* CreateWindow() {
    g_glfw_error[0] = '\0';
    SetCoreProfileHints();
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(kWindowWidth, kWindowHeight, kTitle, nullptr, nullptr);
    if (window != nullptr) {
        return window;
    }

    std::fprintf(stderr, "4x multisampling is unavailable (%s). Retrying without it.\n", g_glfw_error);
    g_glfw_error[0] = '\0';
    glfwWindowHint(GLFW_SAMPLES, 0);
    window = glfwCreateWindow(kWindowWidth, kWindowHeight, kTitle, nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "Could not create an OpenGL 4.1 core window (%s).\n", g_glfw_error);
    }
    return window;
}

struct FrameState;

FrameState* StateOf(GLFWwindow* window) {
    return static_cast<FrameState*>(glfwGetWindowUserPointer(window));
}

std::string EncodeUtf8(unsigned int codepoint) {
    std::string text;
    if (codepoint < 0x80) {
        text.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return text;
}

bool NativeCursor(jadefx::CursorShape shape, int& native) {
    switch (shape) {
        case jadefx::CursorShape::IBeam:
            native = GLFW_IBEAM_CURSOR;
            return true;
        case jadefx::CursorShape::Crosshair:
            native = GLFW_CROSSHAIR_CURSOR;
            return true;
        case jadefx::CursorShape::Hand:
#if defined(GLFW_POINTING_HAND_CURSOR)
            native = GLFW_POINTING_HAND_CURSOR;
            return true;
#elif defined(GLFW_HAND_CURSOR)
            native = GLFW_HAND_CURSOR;
            return true;
#else
            return false;
#endif
        case jadefx::CursorShape::SizeWestEast:
#ifdef GLFW_RESIZE_EW_CURSOR
            native = GLFW_RESIZE_EW_CURSOR;
            return true;
#else
            return false;
#endif
        case jadefx::CursorShape::SizeNorthSouth:
#ifdef GLFW_RESIZE_NS_CURSOR
            native = GLFW_RESIZE_NS_CURSOR;
            return true;
#else
            return false;
#endif
        case jadefx::CursorShape::SizeNorthwestSoutheast:
#ifdef GLFW_RESIZE_NWSE_CURSOR
            native = GLFW_RESIZE_NWSE_CURSOR;
            return true;
#else
            return false;
#endif
        case jadefx::CursorShape::SizeNortheastSouthwest:
#ifdef GLFW_RESIZE_NESW_CURSOR
            native = GLFW_RESIZE_NESW_CURSOR;
            return true;
#else
            return false;
#endif
        case jadefx::CursorShape::SizeAll:
#ifdef GLFW_RESIZE_ALL_CURSOR
            native = GLFW_RESIZE_ALL_CURSOR;
            return true;
#else
            return false;
#endif
        case jadefx::CursorShape::NotAllowed:
#ifdef GLFW_NOT_ALLOWED_CURSOR
            native = GLFW_NOT_ALLOWED_CURSOR;
            return true;
#else
            return false;
#endif
        case jadefx::CursorShape::Arrow:
        case jadefx::CursorShape::Hidden:
            return false;
    }
    return false;
}

// Cocoa and Win32 do not return from the event wait while the pointer is dragging
// the window border. Size and refresh callbacks draw when the frame changes.
// Holding the border still never changes the frame, so PresentClock draws those
// blanks from a timer on the tracking run loop.
struct FrameState {
    GLFWwindow* window = nullptr;
    jadefx::Stage* stage = nullptr;
    PresentClock* clock = nullptr;
    GLFWcursor* cursors[12] = {};
    bool drawing = false;
    int pumping = 0;

    void ApplyCursor(jadefx::Cursor cursor) {
        if (window == nullptr) {
            return;
        }
        const jadefx::CursorShape shape = jadefx::cursorShape(cursor);
        if (shape == jadefx::CursorShape::Hidden) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
            return;
        }
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        int native = 0;
        const int index = static_cast<int>(shape);
        if (index < 0 || index >= static_cast<int>(sizeof cursors / sizeof cursors[0]) || !NativeCursor(shape, native)) {
            glfwSetCursor(window, nullptr);
            return;
        }
        if (cursors[index] == nullptr) {
            cursors[index] = glfwCreateStandardCursor(native);
        }
        glfwSetCursor(window, cursors[index]);
    }

    void DestroyCursors() {
        for (GLFWcursor*& cursor : cursors) {
            if (cursor != nullptr) {
                glfwDestroyCursor(cursor);
                cursor = nullptr;
            }
        }
    }
};

void OnKey(GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
    if (FrameState* state = StateOf(window); state != nullptr && state->stage != nullptr) {
        state->stage->pushKey(key, action != GLFW_RELEASE, mods, action == GLFW_REPEAT);
    }
}

void OnChar(GLFWwindow* window, unsigned int codepoint) {
    FrameState* state = StateOf(window);
    if (state == nullptr || state->stage == nullptr) {
        return;
    }
    state->stage->pushText(EncodeUtf8(codepoint));
}

void OnMouseMove(GLFWwindow* window, double x, double y) {
    if (FrameState* state = StateOf(window); state != nullptr && state->stage != nullptr) {
        state->stage->pushMove(x, y);
    }
}

void OnMouseButton(GLFWwindow* window, int button, int action, int /*mods*/) {
    FrameState* state = StateOf(window);
    if (state == nullptr || state->stage == nullptr) {
        return;
    }
    double x = 0;
    double y = 0;
    glfwGetCursorPos(window, &x, &y);
    state->stage->pushButton(button, action == GLFW_PRESS, x, y);
}

void OnScroll(GLFWwindow* window, double dx, double dy) {
    FrameState* state = StateOf(window);
    if (state == nullptr || state->stage == nullptr) {
        return;
    }
    double x = 0;
    double y = 0;
    glfwGetCursorPos(window, &x, &y);
    state->stage->pushScroll(x, y, dx, dy);
}

void OnCursorEnter(GLFWwindow* window, int entered) {
    FrameState* state = StateOf(window);
    if (state == nullptr || state->stage == nullptr) {
        return;
    }
    if (entered == GLFW_FALSE) {
        state->stage->pushPointerExit();
        return;
    }
    double x = 0;
    double y = 0;
    glfwGetCursorPos(window, &x, &y);
    state->stage->pushMove(x, y);
}

bool DrawFrame(FrameState& state) {
    if (state.drawing || state.window == nullptr || state.stage == nullptr) {
        return true;
    }
    state.drawing = true;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(state.window, &framebufferWidth, &framebufferHeight);
    bool ok = true;
    if (framebufferWidth > 0 && framebufferHeight > 0) {
        int pointWidth = 0;
        int pointHeight = 0;
        glfwGetWindowSize(state.window, &pointWidth, &pointHeight);
        if (pointWidth > 0 && pointHeight > 0) {
            ok = state.stage->frame(pointWidth, pointHeight, framebufferWidth, framebufferHeight);
            if (ok) {
                glfwSwapBuffers(state.window);
            }
        }
    }
    state.drawing = false;
    return ok;
}

void DrawFromPlatform(GLFWwindow* window) {
    FrameState* state = StateOf(window);
    if (state == nullptr) {
        return;
    }
    if (!DrawFrame(*state)) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    // The resize timer presents from the same blank counter. Consuming the
    // blank here keeps a moving border from presenting twice.
    if (state->clock != nullptr) {
        state->clock->acknowledge();
    }
}

void PresentResizeFrame(void* context) {
    FrameState& state = *static_cast<FrameState*>(context);
    if (!DrawFrame(state) && state.window != nullptr) {
        glfwSetWindowShouldClose(state.window, GLFW_TRUE);
    }
}

void OnContentChange(GLFWwindow* window, int, int) { DrawFromPlatform(window); }

void OnRefresh(GLFWwindow* window) { DrawFromPlatform(window); }

}  // namespace

int main() {
    glfwSetErrorCallback(CaptureGlfwError);
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "glfwInit failed (%s).\n", g_glfw_error);
        return 1;
    }

    GLFWwindow* window = CreateWindow();
    if (window == nullptr) {
        glfwTerminate();
        return 1;
    }

    glfwSetWindowSizeLimits(window, kMinWidth, kMinHeight, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    // Interval 0 keeps a swap from blocking. PresentClock is what limits presents
    // to one image per refresh, so the window server is not flooded.
    glfwSwapInterval(0);

    const auto proc_address = [](const char* name) -> void* {
        return reinterpret_cast<void*>(glfwGetProcAddress(name));
    };

    jadefx::Stage stage;
    if (!stage.initializeGraphics(proc_address)) {
        std::fprintf(stderr, "OpenGL setup failed.\n");
        stage.shutdownGraphics();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    int pointWidth = 0;
    int pointHeight = 0;
    glfwGetWindowSize(window, &pointWidth, &pointHeight);
    ide::IdeLayout layout(pointWidth, pointHeight);
    layout.mount(stage.getScene());

    FrameState frame;
    frame.window = window;
    frame.stage = &stage;
    glfwSetWindowUserPointer(window, &frame);
    glfwSetKeyCallback(window, OnKey);
    glfwSetCharCallback(window, OnChar);
    glfwSetCursorPosCallback(window, OnMouseMove);
    glfwSetMouseButtonCallback(window, OnMouseButton);
    glfwSetScrollCallback(window, OnScroll);
    glfwSetCursorEnterCallback(window, OnCursorEnter);
    glfwSetWindowSizeCallback(window, OnContentChange);
    glfwSetFramebufferSizeCallback(window, OnContentChange);
    glfwSetWindowRefreshCallback(window, OnRefresh);
    stage.setCursorHandler([&frame](jadefx::Cursor cursor) { frame.ApplyCursor(cursor); });
    stage.setClipboardHandlers(
        [window](const std::string& text) { glfwSetClipboardString(window, text.c_str()); },
        [window]() {
            const char* text = glfwGetClipboardString(window);
            return text != nullptr ? std::string(text) : std::string();
        });
    stage.setHostHandlers([window](int width, int height) { glfwSetWindowSize(window, width, height); },
                          [] {}, [window](const std::string& title) { glfwSetWindowTitle(window, title.c_str()); });
    // Alerts pump events from inside show(). A pump that draws keeps that dialog live.
    stage.setEventPump([&frame]() -> int {
        if (frame.drawing || frame.pumping > 0 || frame.window == nullptr) {
            return 0;
        }
        ++frame.pumping;
        glfwPollEvents();
        const bool closed = glfwWindowShouldClose(frame.window) == GLFW_TRUE;
        const bool ok = !closed && DrawFrame(frame);
        --frame.pumping;
        if (closed || !ok) {
            return -1;
        }
        return 1;
    });

    {
        // The clock is stopped before the window is destroyed. Its display-link
        // callback posts a GLFW event, and that has to happen while GLFW is still alive.
        PresentClock clock(window);
        frame.clock = &clock;
        clock.setResizePresent(PresentResizeFrame, &frame);
        if (!DrawFrame(frame)) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        while (glfwWindowShouldClose(window) != GLFW_TRUE) {
            clock.follow(window);
            if (!clock.waitForNewBlank()) {
                continue;
            }
            if (glfwWindowShouldClose(window) == GLFW_TRUE) {
                break;
            }
            if (!DrawFrame(frame)) {
                break;
            }
            clock.acknowledge();
        }
        frame.clock = nullptr;
    }

    frame.stage = nullptr;
    frame.DestroyCursors();
    stage.shutdownGraphics();
    glfwDestroyWindow(window);
    glfwTerminate();
    return stage.graphicsOk() ? 0 : 1;
}
