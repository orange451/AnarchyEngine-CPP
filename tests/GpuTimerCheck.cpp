#include "profiler/Profiler.hpp"
#include "runner/GpuTimer.hpp"
#include "runner/gl.hpp"

// Only GLFW's window calls: the GL names come from runner/gl.hpp.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

// By hand, not ctest (it needs a GL 3.3 context): gpu-timer-check times a few
// clears with GpuTimer in a hidden window, and checks glGetError after every
// call, that the times reach the profiler's GPU row a frame or more late, and
// that a timer that never started does nothing.
namespace {

int gFailures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

void ExpectNoGlError(const char* where) {
    const runner::GLenum error = glGetError();
    Expect(error == runner::GL_NO_ERROR, std::string(where) + ": glGetError " + std::to_string(error));
}

}  // namespace

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "gpu-timer-check", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "no GL 3.3 window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!runner::LoadGl([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); })) {
        std::fprintf(stderr, "LoadGl failed\n");
        return 1;
    }
    ExpectNoGlError("load");

    profiler::register_thread("UI");
    profiler::acquire();
    const profiler::ScopeId clear = profiler::intern("Clear", profiler::Group::Gpu);

    // Never started: every call does nothing.
    {
        runner::GpuTimer idle;
        Expect(!idle.available(), "a timer is unavailable before init");
        idle.begin(clear);
        idle.end();
        idle.frame();
        ExpectNoGlError("idle timer");
    }

    runner::GpuTimer timer;
    Expect(runner::GlTimerQueries(), "this context has timer queries");
    Expect(timer.init(), "init makes the query pool");
    ExpectNoGlError("init");
    for (int frame = 0; frame < 12; ++frame) {
        profiler::frame_boundary();
        glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
        timer.begin(clear);
        ExpectNoGlError("begin");
        for (int pass = 0; pass < 20; ++pass) {
            glClear(runner::GL_COLOR_BUFFER_BIT);
        }
        timer.end();
        ExpectNoGlError("end");
        timer.frame();
        ExpectNoGlError("frame");
        glfwSwapBuffers(window);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (int settle = 0; settle < 5; ++settle) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        profiler::frame_boundary();
        timer.frame();
        ExpectNoGlError("settle");
    }
    profiler::frame_boundary();
    profiler::collect();
    int found = 0;
    int lag = -1;
    profiler::with_live([&](const profiler::History& history) {
        lag = history.gpu_lag_frames;
        for (const profiler::Frame& frame : history.frames) {
            for (const profiler::ScopeRecord& record : frame.scopes) {
                if (record.scope == clear && record.row == 3 && record.end_ns >= record.start_ns) {
                    ++found;
                }
            }
        }
    });
    Expect(found >= 8, "most of the 12 clears reached the GPU row (" + std::to_string(found) + ")");
    Expect(lag >= 0 && lag <= 8, "the lag is a few frames (" + std::to_string(lag) + ")");
    timer.shutdown();
    ExpectNoGlError("shutdown");
    profiler::release();

    glfwDestroyWindow(window);
    glfwTerminate();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return 1;
    }
    std::printf("gpu timer ok\n");
    return 0;
}
