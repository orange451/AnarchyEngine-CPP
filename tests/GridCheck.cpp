#include "amesh.hpp"
#include "runner/MeshCache.hpp"
#include "runner/Renderer.hpp"
#include "profiler/ProfileStats.hpp"
#include "profiler/Profiler.hpp"
#include "runner/gl.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// By hand, not ctest: grid-check out-dir draws a cube on the floor grid from a
// few fixed cameras in a hidden window, prints the floor grid's GPU and CPU time
// for each, and writes each picture's raw RGBA bytes to out-dir, so two builds'
// grids can be compared byte for byte.
namespace {

using namespace anarchy::amesh;

Data Cube() {
    Data data;
    const float faces[6][4][3] = {
        {{0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}},        // +X
        {{-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}},    // -X
        {{-0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}},        // +Y
        {{-0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}},    // -Y
        {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},        // +Z
        {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}},    // -Z
    };
    for (const auto& face : faces) {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        // Corner order runs bottom left, bottom right, top right, top left on the +Z face.
        const float uvs[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
        for (int corner = 0; corner < 4; ++corner) {
            Vertex v;
            std::copy(face[corner], face[corner] + 3, v.p);
            v.uv[0] = uvs[corner][0];
            v.uv[1] = uvs[corner][1];
            data.vertices.push_back(v);
        }
        data.indices.insert(data.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    compute_normals(data);
    return data;
}


struct Pose {
    const char* name;
    engine_core::Vec3 eye;
    engine_core::Vec3 target;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anarchy-grid-check";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "meshes");
    {
        const std::vector<std::byte> bytes = write(Cube());
        std::ofstream(root / "meshes" / "cube.amesh", std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!glfwInit()) {
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    constexpr int kWidth = 1280;
    constexpr int kHeight = 760;
    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "grid-check", nullptr, nullptr);
    if (window == nullptr) {
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!runner::LoadGl([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); })) {
        return 1;
    }
    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);
    std::printf("framebuffer %dx%d\n", fbWidth, fbHeight);
    profiler::register_thread("UI");
    int failures = 0;
    {
        runner::Renderer renderer;
        if (!renderer.initialize()) {
            std::fprintf(stderr, "renderer did not initialize\n");
            return 1;
        }
        renderer.setGridVisible(true);
        runner::MeshCache meshes([](const std::string&) {});
        meshes.setRoot(root);
        const auto* cube = meshes.get("meshes/cube.amesh");
        const Pose poses[] = {
            {"high", {6.f, 8.f, 12.f}, {0.f, 0.f, 0.f}},
            {"low", {0.f, 1.5f, 9.f}, {0.f, 1.f, 0.f}},
            {"grazing", {2.f, 0.3f, 20.f}, {0.f, 0.3f, 0.f}},
            {"down", {0.5f, 25.f, 0.5f}, {0.f, 0.f, 0.f}},
            {"axes", {3.f, 2.f, 3.f}, {0.f, 0.f, 0.f}},
        };
        const engine_core::Matrix4 placements[] = {
            engine_core::matrix4_translation(0.f, 0.5f, 0.f),
            engine_core::matrix4_translation(3.f, 0.25f, -2.f),
        };
        std::vector<runner::MeshDraw> draws;
        for (const auto& at : placements) {
            draws.push_back(runner::MeshDraw{cube, at});
        }
        for (const Pose& pose : poses) {
            renderer.setCamera(engine_core::matrix4_look_at(pose.eye, pose.target, {0.f, 1.f, 0.f}), 60.f);
            profiler::reset_for_testing();
            profiler::acquire();
            for (int frame = 0; frame < 90; ++frame) {
                profiler::frame_boundary();
                glViewport(0, 0, fbWidth, fbHeight);
                renderer.draw(0, 0, kWidth, kHeight, kWidth, kHeight, draws.data(), static_cast<int>(draws.size()));
                glfwSwapBuffers(window);
            }
            profiler::frame_boundary();
            profiler::collect();
            double gpu = 0;
            double cpu = 0;
            profiler::with_live([&](const profiler::History& history) {
                for (const profiler::ScopeStat& stat : profiler::compute_stats(history, 0)) {
                    if (history.scopes[stat.scope].name == "Floor grid") {
                        (history.rows[stat.row] == "GPU" ? gpu : cpu) = stat.avg_ms;
                    }
                }
            });
            profiler::release();
            renderer.draw(0, 0, kWidth, kHeight, kWidth, kHeight, draws.data(), static_cast<int>(draws.size()));
            std::vector<unsigned char> pixels(static_cast<std::size_t>(fbWidth) * fbHeight * 4);
            glReadPixels(0, 0, fbWidth, fbHeight, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream(out + "/" + pose.name + ".rgba", std::ios::binary)
                .write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            if (runner::rt_glGetError() != runner::GL_NO_ERROR) {
                std::fprintf(stderr, "FAIL GL error at %s\n", pose.name);
                ++failures;
            }
            std::printf("%-8s grid GPU %.3f ms  CPU %.3f ms\n", pose.name, gpu, cpu);
        }
        renderer.shutdown();
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    std::filesystem::remove_all(root);
    return failures == 0 ? 0 : 1;
}
