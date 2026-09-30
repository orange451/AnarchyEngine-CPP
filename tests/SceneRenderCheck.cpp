#include "Camera.hpp"
#include "amesh.hpp"
#include "runner/MeshCache.hpp"
#include "runner/Renderer.hpp"
#include "runner/gl.hpp"

// Only GLFW's window calls: the GL names come from runner/gl.hpp.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// By hand, not ctest (it needs a GL 3.3 context), from the repository root so
// resources/shaders is found: scene-render-check draws what the Scene View
// draws for a GameObject with a Prefab. A cube baked to AMESH in a scratch
// resources folder goes through MeshCache and Renderer, with the fixed
// camera, and the check reads the pixels back.
namespace {

using namespace anarchy::amesh;

int gFailures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

// A unit cube around the origin: 6 faces of 2 CCW triangles, normals out.
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
        for (const auto& corner : face) {
            Vertex v;
            std::copy(corner, corner + 3, v.p);
            data.vertices.push_back(v);
        }
        data.indices.insert(data.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    compute_normals(data);
    return data;
}

struct Pixel {
    int r = 0, g = 0, b = 0;
};

Pixel ReadPixel(int x, int y) {
    unsigned char rgba[4] = {};
    glReadPixels(x, y, 1, 1, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, rgba);
    return {rgba[0], rgba[1], rgba[2]};
}

bool IsClear(Pixel p) {
    return p.r == 30 && p.g == 30 && p.b == 30;
}

std::string Text(Pixel p) {
    return std::to_string(p.r) + "," + std::to_string(p.g) + "," + std::to_string(p.b);
}

}  // namespace

int main() {
    // A scratch project resources folder with the cube and a file that is not AMESH.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anarchy-scene-render-check";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "meshes");
    {
        const std::vector<std::byte> bytes = write(Cube());
        std::ofstream(root / "meshes" / "cube.amesh", std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::ofstream(root / "meshes" / "junk.amesh", std::ios::binary) << "not a mesh";
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    constexpr int kSize = 128;
    GLFWwindow* window = glfwCreateWindow(kSize, kSize, "scene-render-check", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "no GL 3.3 core context\n");
        glfwTerminate();
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

    {
        runner::Renderer renderer;
        Expect(renderer.initialize(), "the renderer and its mesh program build");
        std::vector<std::string> reports;
        runner::MeshCache meshes([&reports](const std::string& message) { reports.push_back(message); });
        Expect(meshes.get("meshes/cube.amesh") == nullptr, "no root loads nothing");
        meshes.setRoot(root);
        const GpuMesh* cube = meshes.get("meshes/cube.amesh");
        Expect(cube != nullptr && cube->valid(), "the cube loads from the resources folder");
        Expect(meshes.get("meshes/cube.amesh") == cube, "a second get is the same upload");
        Expect(meshes.get("meshes/missing.amesh") == nullptr && meshes.get("meshes/junk.amesh") == nullptr,
               "a missing file and a file that is not AMESH draw nothing");
        Expect(reports.size() == 2, "each says why once (" + std::to_string(reports.size()) + " reports)");
        meshes.get("meshes/missing.amesh");
        Expect(reports.size() == 2, "a file still missing is not reported again");

        // The cube at the origin, under the camera a Renderer starts with, fills the middle of the pane.
        runner::MeshDraw draw{cube, engine_core::matrix4_identity()};
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
        const Pixel middle = ReadPixel(fbWidth / 2, fbHeight / 2);
        const Pixel corner = ReadPixel(2, 2);
        Expect(!IsClear(middle), "the cube covers the middle of the view (" + Text(middle) + ")");
        Expect(IsClear(corner), "the corner is the clear color (" + Text(corner) + ")");
        // Lit from above: the top face is brighter than the front face, and both are gray.
        // From (0, 3, 7) the top face is 4 to 9 of 64 half-height pixels above the
        // middle, and the front face runs from 4 above to 10 below.
        const Pixel top = ReadPixel(fbWidth / 2, fbHeight / 2 + fbHeight * 3 / 64);
        const Pixel front = ReadPixel(fbWidth / 2, fbHeight / 2 - fbHeight * 3 / 64);
        Expect(top.r > front.r && top.r == top.g && top.g == top.b, "the top face is lit brighter (" + Text(top) +
                                                                        " over " + Text(front) + ")");
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "drawing leaves no GL error");

        // Moved by its Transform: 3 units right leaves the middle clear.
        runner::MeshDraw moved{cube, engine_core::matrix4_translation(3.f, 0.f, 0.f)};
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &moved, 1);
        Expect(IsClear(ReadPixel(fbWidth / 2, fbHeight / 2)), "a GameObject moved right leaves the middle");
        // x = 3 at that depth is about 0.68 of the half-width right of the middle.
        Expect(!IsClear(ReadPixel(fbWidth * 27 / 32, fbHeight / 2)), "and draws to the right of it");

        // A new place's Camera sees just what the Renderer's own camera does.
        const engine_core::Vec3 up{0.f, 1.f, 0.f};
        runner::ViewPixels fixed;
        runner::ViewPixels followed;
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
        renderer.read(0, 0, kSize, kSize, kSize, kSize, fixed);
        renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                           static_cast<float>(engine_core::Camera::kNewPlaceFieldOfView));
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
        renderer.read(0, 0, kSize, kSize, kSize, kSize, followed);
        Expect(!fixed.empty() && fixed.rgba == followed.rgba, "a new place's Camera draws what the fixed camera drew");

        // A Camera's Transform and FieldOfView. Turned away from the origin, it sees nothing.
        renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 3.f, 14.f}, up), 60.f);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
        Expect(IsClear(ReadPixel(fbWidth / 2, fbHeight / 2)), "a Camera turned away does not see the cube");
        // Moved 3 right and looking straight ahead, it sees the moved cube in the middle.
        renderer.setCamera(engine_core::matrix4_look_at({3.f, 0.f, 7.f}, {3.f, 0.f, 0.f}, up), 60.f);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &moved, 1);
        Expect(!IsClear(ReadPixel(fbWidth / 2, fbHeight / 2)), "a Camera over the moved cube sees it in the middle");
        Expect(IsClear(ReadPixel(fbWidth * 7 / 8, fbHeight / 2)), "and at 60 degrees the cube stays in the middle");
        // A narrow FieldOfView magnifies: the cube reaches out to the same pixel.
        renderer.setCamera(engine_core::matrix4_look_at({3.f, 0.f, 7.f}, {3.f, 0.f, 0.f}, up), 10.f);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &moved, 1);
        Expect(!IsClear(ReadPixel(fbWidth * 7 / 8, fbHeight / 2)), "a narrow FieldOfView fills more of the view");
        // A Transform with no inverse, or an angle out of range, changes nothing.
        renderer.setCamera(engine_core::Matrix4{}, 60.f);
        renderer.setCamera(engine_core::matrix4_identity(), 0.f);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &moved, 1);
        Expect(!IsClear(ReadPixel(fbWidth * 7 / 8, fbHeight / 2)), "a bad Camera is ignored");
        renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                           runner::Renderer::kCameraFovYDegrees);

        // A file that changes is read again after the recheck interval: this one stops drawing.
        std::ofstream(root / "meshes" / "cube.amesh", std::ios::binary | std::ios::trunc) << "broken now";
        std::filesystem::last_write_time(root / "meshes" / "cube.amesh",
                                         std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        Expect(meshes.get("meshes/cube.amesh") == nullptr, "a mesh whose file broke stops drawing");
        Expect(reports.size() == 3, "and says why");

        // A play session's geometry: uploaded from memory, again only when its revision changes.
        const Data session = Cube();
        const GpuMesh* live = meshes.getSession(9, session, 1);
        Expect(live != nullptr && live->valid(), "a session mesh uploads from memory");
        Expect(meshes.getSession(9, session, 1) == live, "the same revision is the same upload");
        runner::MeshDraw from_session{live, engine_core::matrix4_identity()};
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &from_session, 1);
        Expect(!IsClear(ReadPixel(fbWidth / 2, fbHeight / 2)), "a session mesh draws like a file's");
        Expect(meshes.getSession(9, Data{}, 2) == nullptr, "a session Cleared to nothing draws nothing");
        Expect(meshes.getSession(9, session, 3) != nullptr, "a new revision uploads again");
        meshes.sweepSessions();
        Expect(live->valid(), "a sweep keeps the uploads this frame asked for");
        meshes.sweepSessions();
        Expect(meshes.getSession(9, session, 3) != nullptr, "one swept away uploads again when asked");
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "session uploads leave no GL error");
        meshes.clear();
        renderer.shutdown();
    }
    Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "shutdown leaves no GL error");

    glfwDestroyWindow(window);
    glfwTerminate();
    std::filesystem::remove_all(root);
    if (gFailures != 0) {
        std::fprintf(stderr, "%d scene render check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("scene render checks passed\n");
    return 0;
}
