#include "amesh.hpp"
#include "runner/MeshCache.hpp"
#include "runner/Renderer.hpp"
#include "profiler/ProfileStats.hpp"
#include "profiler/Profiler.hpp"
#include "runner/gl.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// By hand, not ctest: grid-check out-dir draws a cube on the floor grid from a
// few fixed cameras in a hidden window, prints what the floor grid, FXAA, and
// the depth probe cost on the GPU and the CPU for each, and the draw's CPU time
// with the profiler off, recording, and timing every pass, and writes each
// picture's raw RGBA bytes to out-dir, so two builds' grids can be compared
// byte for byte. DUMP_SCOPES=1 also lists every scope over 0.05 ms per pass timed.
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
        // The cursor over the middle of the pane, as the Scene View asks for the depth under it.
        renderer.setDepthProbe(kWidth * 0.5, kHeight * 0.5);
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
            // The whole draw's GPU time, the median of many frames with each
            // setting taken in turn, so the GPU's clocks drift alike for all.
            // Grid and FXAA are each what the draw costs with them over without.
            constexpr int kSettings = 3;
            constexpr int kRounds = 200;
            std::vector<double> times[kSettings];
            unsigned query = 0;
            glGenQueries(1, &query);
            for (int frame = 0; frame < kRounds * kSettings + kSettings * 4; ++frame) {
                const int setting = frame % kSettings;
                renderer.setGridVisible(setting != 1);
                runner::SceneLighting lighting;
                lighting.antialiasing =
                    setting != 2 ? runner::SceneAntialiasing::FXAA : runner::SceneAntialiasing::None;
                renderer.setLighting(lighting);
                glViewport(0, 0, fbWidth, fbHeight);
                glBeginQuery(runner::GL_TIME_ELAPSED, query);
                renderer.draw(0, 0, kWidth, kHeight, kWidth, kHeight, draws.data(), static_cast<int>(draws.size()));
                glEndQuery(runner::GL_TIME_ELAPSED);
                glfwSwapBuffers(window);
                runner::GLuint64 ns = 0;
                glGetQueryObjectui64v(query, runner::GL_QUERY_RESULT, &ns);
                // The first rounds warm the programs and the bands.
                if (frame >= kSettings * 4) {
                    times[setting].push_back(static_cast<double>(ns) / 1e6);
                }
            }
            runner::rt_glDeleteQueries(1, &query);
            double median[kSettings] = {};
            for (int setting = 0; setting < kSettings; ++setting) {
                std::vector<double>& t = times[setting];
                std::nth_element(t.begin(), t.begin() + static_cast<long>(t.size() / 2), t.end());
                median[setting] = t[t.size() / 2];
            }
            // Each pass as the profiler's per-pass GPU detail shows it, which on
            // macOS also counts what splitting the frame at every pass costs.
            renderer.setGridVisible(true);
            renderer.setLighting(runner::SceneLighting{});
            double gridPass = 0;
            double fxaaPass = 0;
            profiler::reset_for_testing();
            profiler::acquire();
            profiler::set_gpu_detail(true);
            for (int frame = 0; frame < 90; ++frame) {
                profiler::frame_boundary();
                glViewport(0, 0, fbWidth, fbHeight);
                renderer.draw(0, 0, kWidth, kHeight, kWidth, kHeight, draws.data(), static_cast<int>(draws.size()));
                glfwSwapBuffers(window);
            }
            profiler::frame_boundary();
            profiler::collect();
            profiler::with_live([&](const profiler::History& history) {
                if (std::getenv("DUMP_SCOPES") != nullptr) {
                    for (const profiler::ScopeStat& stat : profiler::compute_stats(history, 0)) {
                        if (stat.avg_ms > 0.05) {
                            std::printf("   detail-on %-12s %-28s %.3f ms\n", history.rows[stat.row].c_str(),
                                        history.scopes[stat.scope].name.c_str(), stat.avg_ms);
                        }
                    }
                }
                for (const profiler::ScopeStat& stat : profiler::compute_stats(history, 0)) {
                    if (history.rows[stat.row] != "GPU") {
                        continue;
                    }
                    if (history.scopes[stat.scope].name == "Floor grid") {
                        gridPass = stat.avg_ms;
                    } else if (history.scopes[stat.scope].name == "FXAA") {
                        fxaaPass = stat.avg_ms;
                    }
                }
            });
            profiler::set_gpu_detail(false);
            profiler::release();
            // The CPU side, as the profiler's Render draw row shows it, per-pass
            // GPU detail off: the camera still, then orbiting a little each frame.
            double cpu[2][3] = {};
            for (int moving = 0; moving < 2; ++moving) {
                profiler::reset_for_testing();
                profiler::acquire();
                for (int frame = 0; frame < 90; ++frame) {
                    if (moving != 0) {
                        const float angle = 0.002f * static_cast<float>(frame);
                        const engine_core::Vec3 eye{pose.eye.x * std::cos(angle) - pose.eye.z * std::sin(angle), pose.eye.y,
                                                    pose.eye.x * std::sin(angle) + pose.eye.z * std::cos(angle)};
                        renderer.setCamera(engine_core::matrix4_look_at(eye, pose.target, {0.f, 1.f, 0.f}), 60.f);
                    }
                    profiler::frame_boundary();
                    glViewport(0, 0, fbWidth, fbHeight);
                    renderer.draw(0, 0, kWidth, kHeight, kWidth, kHeight, draws.data(), static_cast<int>(draws.size()));
                    glfwSwapBuffers(window);
                }
                profiler::frame_boundary();
                profiler::collect();
                profiler::with_live([&](const profiler::History& history) {
                    for (const profiler::ScopeStat& stat : profiler::compute_stats(history, 0)) {
                        if (history.rows[stat.row] == "GPU") {
                            continue;
                        }
                        if (history.scopes[stat.scope].name == "Floor grid") {
                            cpu[moving][0] = stat.avg_ms;
                        } else if (history.scopes[stat.scope].name == "FXAA") {
                            cpu[moving][1] = stat.avg_ms;
                        } else if (history.scopes[stat.scope].name == "Take probe" ||
                                   history.scopes[stat.scope].name == "Read probe") {
                            cpu[moving][2] = stat.avg_ms;
                        }
                    }
                });
                profiler::release();
            }
            renderer.setCamera(engine_core::matrix4_look_at(pose.eye, pose.target, {0.f, 1.f, 0.f}), 60.f);
            // The draw's own CPU time by the clock, with the profiler not
            // recording, recording, and recording with per-pass GPU detail.
            double wall[3] = {};
            double whole[3] = {};
            for (int mode = 0; mode < 3; ++mode) {
                if (mode > 0) {
                    profiler::reset_for_testing();
                    profiler::acquire();
                    profiler::set_gpu_detail(mode == 2);
                }
                std::vector<double> t;
                std::vector<double> w;
                for (int frame = 0; frame < 120; ++frame) {
                    if (mode > 0) {
                        profiler::frame_boundary();
                    }
                    glViewport(0, 0, fbWidth, fbHeight);
                    const auto start = std::chrono::steady_clock::now();
                    renderer.draw(0, 0, kWidth, kHeight, kWidth, kHeight, draws.data(), static_cast<int>(draws.size()));
                    t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                    glfwSwapBuffers(window);
                    w.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                }
                if (mode > 0) {
                    profiler::set_gpu_detail(false);
                    profiler::release();
                }
                std::nth_element(t.begin(), t.begin() + static_cast<long>(t.size() / 2), t.end());
                wall[mode] = t[t.size() / 2];
                std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
                whole[mode] = w[w.size() / 2];
            }
            std::printf("%-8s draw CPU by the clock: %.3f ms not recording, %.3f ms recording, %.3f ms with GPU detail\n",
                        "", wall[0], wall[1], wall[2]);
            std::printf("%-8s draw and swap by the clock: %.3f ms, %.3f ms, %.3f ms\n", "", whole[0], whole[1], whole[2]);
            const double both = median[0];
            const double noGrid = median[1];
            const double noFxaa = median[2];
            renderer.setGridVisible(true);
            renderer.setLighting(runner::SceneLighting{});
            renderer.draw(0, 0, kWidth, kHeight, kWidth, kHeight, draws.data(), static_cast<int>(draws.size()));
            std::vector<unsigned char> pixels(static_cast<std::size_t>(fbWidth) * fbHeight * 4);
            glReadPixels(0, 0, fbWidth, fbHeight, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream(out + "/" + pose.name + ".rgba", std::ios::binary)
                .write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            if (runner::rt_glGetError() != runner::GL_NO_ERROR) {
                std::fprintf(stderr, "FAIL GL error at %s\n", pose.name);
                ++failures;
            }
            std::printf("%-8s GPU: scene %.3f ms | grid %.3f ms (pass %.3f) | FXAA %.3f ms (pass %.3f)\n", pose.name,
                        both, both - noGrid, gridPass, both - noFxaa, fxaaPass);
            std::printf("%-8s CPU: grid %.3f ms still, %.3f ms moving | FXAA %.3f ms still, %.3f ms moving | probe "
                        "%.3f ms\n",
                        "", cpu[0][0], cpu[1][0], cpu[0][1], cpu[1][1], cpu[0][2]);
            const std::optional<float> probed = renderer.probedDepth();
            std::printf("%-8s depth under the probe %.7f\n", "", probed ? *probed : -1.f);
        }
        renderer.shutdown();
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    std::filesystem::remove_all(root);
    return failures == 0 ? 0 : 1;
}
