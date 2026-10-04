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
#include <utility>

// By hand, not ctest (it needs a GL 3.3 context): gpu-timer-check times a few
// heavy fullscreen draws with GpuTimer in a hidden window, and checks glGetError after every
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

// A fullscreen triangle whose every pixel does real work, so the GPU has time to measure.
runner::GLuint HeavyProgram() {
    const char* vertex = R"(#version 330 core
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";
    const char* fragment = R"(#version 330 core
out vec4 color;
void main() {
    float sum = 0.0;
    for (int i = 0; i < 400; ++i) {
        sum += sin(gl_FragCoord.x * 0.01 + float(i)) * cos(gl_FragCoord.y * 0.01 - float(i));
    }
    color = vec4(fract(sum), 0.0, 0.0, 1.0);
}
)";
    const runner::GLuint program = glCreateProgram();
    for (const auto& [type, source] : {std::pair<runner::GLenum, const char*>{runner::GL_VERTEX_SHADER, vertex},
                                       std::pair<runner::GLenum, const char*>{runner::GL_FRAGMENT_SHADER, fragment}}) {
        const runner::GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    glLinkProgram(program);
    runner::GLint linked = 0;
    glGetProgramiv(program, runner::GL_LINK_STATUS, &linked);
    Expect(linked != 0, "the heavy program links");
    return program;
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
    GLFWwindow* window = glfwCreateWindow(512, 512, "gpu-timer-check", nullptr, nullptr);
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

    const runner::GLuint heavy = HeavyProgram();
    runner::GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(window, &fb_width, &fb_height);
    glViewport(0, 0, fb_width, fb_height);
    ExpectNoGlError("program");
    runner::GpuTimer timer;
    Expect(runner::GlTimerQueries(), "this context has timer queries");
    Expect(timer.init(), "init makes the query pool");
    ExpectNoGlError("init");
    for (int frame = 0; frame < 12; ++frame) {
        profiler::frame_boundary();
        glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
        timer.begin(clear);
        ExpectNoGlError("begin");
        glClear(runner::GL_COLOR_BUFFER_BIT);
        glUseProgram(heavy);
        glDrawArrays(runner::GL_TRIANGLES, 0, 3);
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
    std::uint64_t total_ns = 0;
    std::uint64_t last_end = 0;
    bool overlap = false;
    profiler::with_live([&](const profiler::History& history) {
        lag = history.gpu_lag_frames;
        for (const profiler::Frame& frame : history.frames) {
            for (const profiler::ScopeRecord& record : frame.scopes) {
                if (record.scope == clear && record.row == 3 && record.end_ns >= record.start_ns) {
                    ++found;
                    total_ns += record.end_ns - record.start_ns;
                    overlap = overlap || record.start_ns < last_end;
                    last_end = record.end_ns;
                }
            }
        }
    });
    Expect(found >= 8, "most of the 12 clears reached the GPU row (" + std::to_string(found) + ")");
    // macOS answers timestamp queries with 0; real lengths prove the timer measures.
    Expect(total_ns > 0, "the clears took measurable GPU time (" + std::to_string(total_ns) + " ns)");
    Expect(!overlap, "passes do not overlap on the GPU row");
    Expect(lag >= 0 && lag <= 8, "the lag is a few frames (" + std::to_string(lag) + ")");
    timer.shutdown();
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(heavy);
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
