#include "Camera.hpp"
#include "amesh.hpp"
#include "ide/MaterialBall.hpp"
#include "runner/MeshCache.hpp"
#include "runner/RenderMath.hpp"
#include "runner/Renderer.hpp"
#include "runner/TextureCache.hpp"
#include "runner/gl.hpp"

// Only GLFW's window calls: the GL names come from runner/gl.hpp.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// By hand, not ctest (it needs a GL 3.3 context), from the repository root so
// resources/shaders is found: scene-render-check draws what the Scene View
// draws for a GameObject with a Prefab, lit through the legacy pipeline. A cube baked to AMESH in a scratch
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

int Sum(Pixel p) { return p.r + p.g + p.b; }

// The legacy pipeline's stand-in sky lights every surface a faint gray, so a
// channel the surface's color has none of still reads a little above 0.
constexpr int kSkyTint = 4;

// An uncompressed 24-bit TGA, 4 by 4, written top row first: the top two rows
// red, the bottom two blue.
std::string StripesTga() {
    std::string bytes(18, '\0');
    bytes[2] = 2;     // true color, uncompressed
    bytes[12] = 4;    // width
    bytes[14] = 4;    // height
    bytes[16] = 24;   // bits per pixel
    bytes[17] = 0x20; // top row first
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            // Blue, green, red.
            bytes += row < 2 ? std::string("\x00\x00\xff", 3) : std::string("\xff\x00\x00", 3);
        }
    }
    return bytes;
}

// A Radiance HDR image, written top row first, uncompressed: color(x, y)
// gives each pixel's linear RGB, which may be brighter than 1.
template <typename Color>
std::string Hdr(int width, int height, Color color) {
    std::string bytes = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) + " +X " +
                        std::to_string(width) + "\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::array<float, 3> rgb = color(x, y);
            const float brightest = std::max({rgb[0], rgb[1], rgb[2]});
            if (brightest < 1e-32f) {
                bytes.append(4, '\0');
                continue;
            }
            int exponent = 0;
            const float scale = std::frexp(brightest, &exponent) * 256.f / brightest;
            for (const float channel : rgb) {
                bytes += static_cast<char>(static_cast<unsigned char>(channel * scale));
            }
            bytes += static_cast<char>(exponent + 128);
        }
    }
    return bytes;
}

// The test sky, 16 by 8: above the horizon red (4, brighter than white) on
// the image's left half and green on its right; below it, blue. The image's
// middle is straight down -Z, so a camera looking down -Z sees red on its
// left, green on its right, and blue below.
std::string TestSky() {
    return Hdr(16, 8, [](int x, int y) -> std::array<float, 3> {
        if (y >= 4) {
            return {0.f, 0.f, 1.f};
        }
        return x < 8 ? std::array<float, 3>{4.f, 0.f, 0.f} : std::array<float, 3>{0.f, 4.f, 0.f};
    });
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
    Expect(runner::rt_glTexImage3D != nullptr && runner::rt_glFramebufferTextureLayer != nullptr &&
               runner::rt_glPolygonOffset != nullptr && runner::rt_glReadBuffer != nullptr,
           "the shadow maps' GL calls load");
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
        // With no lights, only the ambient: every face the same gray.
        // From (0, 3, 7) the top face is 4 to 9 of 64 half-height pixels above the
        // middle, and the front face runs from 4 above to 10 below.
        const int topY = fbHeight / 2 + fbHeight * 3 / 64;
        const int frontY = fbHeight / 2 - fbHeight * 3 / 64;
        const Pixel ambientTop = ReadPixel(fbWidth / 2, topY);
        const Pixel ambientFront = ReadPixel(fbWidth / 2, frontY);
        Expect(Sum(ambientTop) > 0 && std::abs(Sum(ambientTop) - Sum(ambientFront)) <= 3,
               "the ambient lights every face alike (" + Text(ambientTop) + " and " + Text(ambientFront) + ")");
        // A PointLight above: the top face is brighter than the front face, and both are gray.
        runner::LightDraw above;
        above.position[1] = 3.f;
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &above, 1);
        const Pixel top = ReadPixel(fbWidth / 2, topY);
        const Pixel front = ReadPixel(fbWidth / 2, frontY);
        Expect(top.r > front.r && top.r == top.g && top.g == top.b, "a light above lights the top face brighter (" +
                                                                        Text(top) + " over " + Text(front) + ")");
        Expect(std::abs(Sum(front) - Sum(ambientFront)) <= 3, "and leaves the face turned from it at the ambient");
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

        // The legacy pipeline's lights, glow, see-through surfaces, and tone map.
        {
            const int midX = fbWidth / 2;
            const int midY = fbHeight / 2;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
            const Pixel unlit = ReadPixel(midX, midY);

            // A PointLight in front of the cube, within its Radius, lights the front face.
            runner::LightDraw point;
            point.position[2] = 3.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &point, 1);
            const Pixel pointLit = ReadPixel(midX, midY);
            Expect(Sum(pointLit) > Sum(unlit) + 30, "a PointLight lights the face toward it (" + Text(pointLit) +
                                                        " over " + Text(unlit) + ")");
            runner::LightDraw shortReach = point;
            shortReach.radius = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &shortReach, 1);
            Expect(std::abs(Sum(ReadPixel(midX, midY)) - Sum(unlit)) <= 3, "a light does not reach past its Radius");
            runner::LightDraw dark = point;
            dark.intensity = 0.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &dark, 1);
            Expect(std::abs(Sum(ReadPixel(midX, midY)) - Sum(unlit)) <= 3, "a light of Intensity 0 gives none");
            runner::LightDraw red = point;
            red.color[1] = 0.f;
            red.color[2] = 0.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &red, 1);
            const Pixel redLit = ReadPixel(midX, midY);
            Expect(redLit.r > redLit.g + 20 && redLit.g == redLit.b, "a light's Color tints it (" + Text(redLit) + ")");
            // Two lights add.
            const runner::LightDraw pair[2] = {point, point};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, pair, 2);
            Expect(Sum(ReadPixel(midX, midY)) > Sum(pointLit), "two lights are brighter than one");

            // A SpotLight lights what its cone points at, and nothing behind it.
            runner::LightDraw spot = point;
            spot.kind = runner::LightDraw::Kind::Spot;
            spot.direction[2] = -1.f;
            spot.outerFovDegrees = 60.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &spot, 1);
            Expect(Sum(ReadPixel(midX, midY)) > Sum(unlit) + 30, "a SpotLight lights what it points at");
            spot.direction[2] = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &spot, 1);
            Expect(std::abs(Sum(ReadPixel(midX, midY)) - Sum(unlit)) <= 3, "and not what is behind it");
            // Pointed straight down from above the cube's front edge, a narrow cone
            // misses the front face entirely.
            runner::LightDraw down = spot;
            down.position[1] = 3.f;
            down.position[2] = 0.f;
            down.direction[1] = -1.f;
            down.direction[2] = 0.f;
            down.outerFovDegrees = 10.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &down, 1);
            Expect(std::abs(Sum(ReadPixel(midX, frontY)) - Sum(ambientFront)) <= 3,
                   "a narrow cone leaves outside it unlit");
            Expect(Sum(ReadPixel(midX, topY)) > Sum(ambientTop) + 30, "and lights inside it");

            // A DirectionalLight shining straight down lights the top face and not the front.
            runner::LightDraw sun;
            sun.kind = runner::LightDraw::Kind::Directional;
            sun.direction[0] = 0.f;
            sun.direction[1] = -1.f;
            sun.direction[2] = 0.f;
            sun.radius = 0.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &sun, 1);
            const Pixel sunTop = ReadPixel(midX, topY);
            Expect(Sum(sunTop) > Sum(ambientTop) + 60, "a DirectionalLight needs no Radius and lights the face toward it (" +
                                                            Text(sunTop) + ")");
            Expect(std::abs(Sum(ReadPixel(midX, frontY)) - Sum(ambientFront)) <= 3,
                   "and leaves a face square to it at the ambient");
            // Where it is does not matter, only where it points.
            runner::LightDraw farSun = sun;
            farSun.position[0] = 500.f;
            farSun.position[1] = -40.f;
            farSun.position[2] = 90.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &farSun, 1);
            Expect(std::abs(Sum(ReadPixel(midX, topY)) - Sum(sunTop)) <= 3, "a DirectionalLight's position is ignored");
            runner::LightDraw upSun = sun;
            upSun.direction[1] = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &upSun, 1);
            Expect(std::abs(Sum(ReadPixel(midX, topY)) - Sum(ambientTop)) <= 3,
                   "a DirectionalLight shining up leaves the top face unlit");
            // Toward the camera's side, it lights the front face, and adds to a PointLight.
            runner::LightDraw frontSun = sun;
            frontSun.direction[1] = 0.f;
            frontSun.direction[2] = -1.f;
            const runner::LightDraw sunAndPoint[2] = {point, frontSun};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, sunAndPoint, 2);
            Expect(Sum(ReadPixel(midX, midY)) > Sum(pointLit) + 30, "a DirectionalLight adds to the other lights");
            // See-through surfaces take it too.
            runner::MeshDraw clearCube = draw;
            clearCube.transparency = 0.3f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &clearCube, 1);
            const Pixel clearUnlit = ReadPixel(midX, topY);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &clearCube, 1, &sun, 1);
            Expect(Sum(ReadPixel(midX, topY)) > Sum(clearUnlit) + 30, "a DirectionalLight lights see-through surfaces");

            // Emissive glows with no light at all.
            runner::MeshDraw glowing = draw;
            glowing.emissive[0] = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &glowing, 1);
            const Pixel glow = ReadPixel(midX, midY);
            // Green and blue gain nothing (Saturation above 1 even pulls them down a little).
            Expect(glow.r > unlit.r + 60 && glow.g <= unlit.g + 3 && glow.g == glow.b,
                   "Emissive glows (" + Text(glow) + ")");

            // Exposure and Saturation, from Lighting.
            runner::SceneLighting dim;
            dim.exposure = 0.4f;
            renderer.setLighting(dim);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &point, 1);
            Expect(Sum(ReadPixel(midX, midY)) + 20 < Sum(pointLit), "a lower Exposure is darker");
            runner::SceneLighting gray;
            gray.saturation = 0.f;
            renderer.setLighting(gray);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &red, 1);
            const Pixel grayLit = ReadPixel(midX, midY);
            Expect(grayLit.r == grayLit.g && grayLit.g == grayLit.b, "Saturation 0 is gray (" + Text(grayLit) + ")");
            renderer.setLighting(runner::SceneLighting{});

            // Transparency 1 draws nothing; between 0 and 1 the pane shows through.
            runner::MeshDraw gone = draw;
            gone.transparency = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &gone, 1);
            Expect(IsClear(ReadPixel(midX, midY)), "Transparency 1 draws nothing");
            runner::MeshDraw half = glowing;
            half.transparency = 0.5f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &half, 1);
            const Pixel halfOverClear = ReadPixel(midX, midY);
            Expect(!IsClear(halfOverClear) && halfOverClear.r < glow.r && halfOverClear.r > 30,
                   "half Transparency blends with the pane (" + Text(halfOverClear) + ")");
            Expect(IsClear(ReadPixel(2, 2)), "and the corner stays the clear color");
            // A half see-through red cube in front of an opaque green one: both
            // colors show. Blue is the baseline: the light's white highlight
            // adds to every channel alike, and neither cube has blue of its own.
            runner::MeshDraw greenCube = draw;
            greenCube.color[0] = 0.f;
            greenCube.color[2] = 0.f;
            runner::MeshDraw seeThrough = half;
            seeThrough.color[1] = 0.f;
            seeThrough.color[2] = 0.f;
            seeThrough.model = engine_core::matrix4_translation(0.f, 0.f, 1.5f);
            const runner::MeshDraw layered[2] = {seeThrough, greenCube};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, layered, 2, &point, 1);
            const Pixel mixed = ReadPixel(midX, midY);
            Expect(mixed.r > mixed.b + 30 && mixed.g > mixed.b + 30,
                   "a see-through surface blends over an opaque one behind it (" + Text(mixed) + ")");
            // Behind the opaque one, it is hidden: no red over the baseline.
            runner::MeshDraw hidden = seeThrough;
            hidden.model = engine_core::matrix4_translation(0.f, 0.f, -1.5f);
            const runner::MeshDraw occluded[2] = {hidden, greenCube};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, occluded, 2, &point, 1);
            const Pixel front2 = ReadPixel(midX, midY);
            Expect(front2.r <= front2.b + 2 && front2.g > front2.b + 30,
                   "the opaque surface hides one behind it (" + Text(front2) + ")");

            // The UI pass after a draw finds its GL state as it left it.
            runner::rt_glEnable(runner::GL_BLEND);
            runner::rt_glBlendFunc(runner::RT_GL_SRC_ALPHA, runner::RT_GL_ONE_MINUS_SRC_ALPHA);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, layered, 2, &point, 1);
            runner::GLint source = 0;
            runner::GLint framebuffer = -1;
            glGetIntegerv(runner::RT_GL_BLEND_SRC_RGB, &source);
            glGetIntegerv(runner::RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
            Expect(runner::rt_glIsEnabled(runner::GL_BLEND) == runner::GL_TRUE &&
                       source == static_cast<runner::GLint>(runner::RT_GL_SRC_ALPHA) && framebuffer == 0,
                   "drawing restores blending and the framebuffer");
            runner::rt_glDisable(runner::GL_BLEND);
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the pipeline leaves no GL error");

        // Shadows. A floor under the cube, lit from the -X side and down, so
        // the cube's shadow falls on the floor at +X: (1, -0.5, 0) is in it,
        // and (1, -0.5, 2) is open floor at about the same distance.
        {
            engine_core::Matrix4 floorModel = engine_core::matrix4_identity();
            floorModel.m[0] = 8.f;
            floorModel.m[5] = 0.2f;
            floorModel.m[10] = 8.f;
            floorModel.m[13] = -0.6f;
            runner::MeshDraw scene[2] = {runner::MeshDraw{cube, engine_core::matrix4_identity()},
                                         runner::MeshDraw{cube, floorModel}};
            // Where a world point lands in the pane, through the camera a Renderer starts with.
            const auto pixelOf = [&](engine_core::Vec3 world) {
                const engine_core::Matrix4 view =
                    runner::LookAtView({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
                const engine_core::Matrix4 projection = runner::Perspective(
                    runner::Renderer::kCameraFovYDegrees,
                    static_cast<float>(fbWidth) / static_cast<float>(fbHeight), 0.1f, 1000.f);
                const engine_core::Vec3 ndc =
                    engine_core::matrix4_point(engine_core::matrix4_multiply(projection, view), world);
                return std::array<int, 2>{static_cast<int>((ndc.x * 0.5f + 0.5f) * fbWidth),
                                          static_cast<int>((ndc.y * 0.5f + 0.5f) * fbHeight)};
            };
            const std::array<int, 2> inShadow = pixelOf({1.f, -0.5f, 0.f});
            const std::array<int, 2> openFloor = pixelOf({1.f, -0.5f, 2.f});
            const auto sample = [&](const std::array<int, 2>& at) { return Sum(ReadPixel(at[0], at[1])); };
            // The two floor points' brightness under one light.
            const auto lit = [&](const runner::LightDraw& light, const runner::MeshDraw* meshes, int count) {
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, meshes, count, &light, 1);
                return std::array<int, 2>{sample(inShadow), sample(openFloor)};
            };
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2);
            const int ambientFloor = sample(inShadow);

            runner::LightDraw spotLight;
            spotLight.kind = runner::LightDraw::Kind::Spot;
            spotLight.position[0] = -4.f;
            spotLight.position[1] = 2.f;
            spotLight.position[2] = 0.f;
            spotLight.direction[0] = 0.894f;
            spotLight.direction[1] = -0.447f;
            spotLight.direction[2] = 0.f;
            spotLight.outerFovDegrees = 90.f;
            spotLight.radius = 12.f;
            spotLight.intensity = 4.f;
            spotLight.id = 7;
            const std::array<int, 2> spotOpen = lit(spotLight, scene, 2);
            Expect(spotOpen[0] > ambientFloor + 20, "with no shadow, the SpotLight reaches the floor behind the cube (" +
                                                        std::to_string(spotOpen[0]) + " over " +
                                                        std::to_string(ambientFloor) + ")");
            spotLight.shadows = true;
            const std::array<int, 2> spotShadowed = lit(spotLight, scene, 2);
            Expect(std::abs(spotShadowed[0] - ambientFloor) <= 4,
                   "a SpotLight's shadow leaves the floor behind the cube at the ambient (" +
                       std::to_string(spotShadowed[0]) + ")");
            Expect(std::abs(spotShadowed[1] - spotOpen[1]) <= 3,
                   "and the open floor lit as before, with no acne (" + std::to_string(spotShadowed[1]) + " and " +
                       std::to_string(spotOpen[1]) + ")");
            Expect(lit(spotLight, scene, 2) == spotShadowed, "a second frame reuses the cached map and draws the same");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "drawing shadows leaves no GL error");

            // Moving the cube away moves its shadow: the cache sees the change.
            runner::MeshDraw movedScene[2] = {runner::MeshDraw{cube, engine_core::matrix4_translation(0.f, 0.f, -3.f)},
                                              scene[1]};
            Expect(std::abs(lit(spotLight, movedScene, 2)[0] - spotOpen[0]) <= 4, "moving the cube moves its shadow");
            Expect(std::abs(lit(spotLight, scene, 2)[0] - ambientFloor) <= 4, "and moving it back brings it back");

            // Geometry uploaded again in place, as MeshCache does when a mesh
            // file changes: the same GpuMesh, so the same address, but the
            // map is drawn again from the new shape.
            {
                GpuMesh rebaked;
                rebaked.upload(Cube());
                runner::MeshDraw rebakedScene[2] = {runner::MeshDraw{&rebaked, engine_core::matrix4_identity()},
                                                    scene[1]};
                Expect(std::abs(lit(spotLight, rebakedScene, 2)[0] - ambientFloor) <= 4,
                       "a second upload of the cube shadows the same");
                Data away = Cube();
                for (Vertex& vertex : away.vertices) {
                    vertex.p[2] -= 3.f;
                }
                rebaked.upload(away);
                Expect(std::abs(lit(spotLight, rebakedScene, 2)[0] - spotOpen[0]) <= 4,
                       "a mesh uploaded again in place, away from the light's path, takes its shadow with it");
            }

            // A see-through cube casts nothing.
            runner::MeshDraw glassScene[2] = {scene[0], scene[1]};
            glassScene[0].transparency = 0.5f;
            Expect(lit(spotLight, glassScene, 2)[0] > ambientFloor + 20, "a see-through mesh casts no shadow");

            // A light's own Prefab does not shadow it: a box around the light, owned by it.
            runner::MeshDraw lampScene[3] = {scene[0], scene[1],
                                             runner::MeshDraw{cube, engine_core::matrix4_translation(-4.f, 2.f, 0.f)}};
            lampScene[2].owner = 7;
            Expect(std::abs(lit(spotLight, lampScene, 3)[1] - spotOpen[1]) <= 3,
                   "a light's own Prefab does not shadow it");
            lampScene[2].owner = 8;
            Expect(lit(spotLight, lampScene, 3)[1] < spotOpen[1] - 20, "but anyone else's box around it does");

            // A PointLight in the same place: its cube faces in the atlas shadow the same way.
            runner::LightDraw pointLight;
            pointLight.position[0] = -4.f;
            pointLight.position[1] = 2.f;
            pointLight.radius = 7.5f;
            pointLight.intensity = 8.f;
            pointLight.id = 9;
            const std::array<int, 2> pointOpen = lit(pointLight, scene, 2);
            pointLight.shadows = true;
            const std::array<int, 2> pointShadowed = lit(pointLight, scene, 2);
            Expect(pointOpen[0] > ambientFloor + 20 && std::abs(pointShadowed[0] - ambientFloor) <= 4,
                   "a PointLight's shadow falls behind the cube (" + std::to_string(pointShadowed[0]) + ")");
            Expect(std::abs(pointShadowed[1] - pointOpen[1]) <= 3,
                   "and leaves the open floor lit, with no seam or acne (" + std::to_string(pointShadowed[1]) + ")");

            // A DirectionalLight shining from the -X side and down, as the others did.
            runner::LightDraw sunLight;
            sunLight.kind = runner::LightDraw::Kind::Directional;
            sunLight.direction[0] = 0.894f;
            sunLight.direction[1] = -0.447f;
            sunLight.direction[2] = 0.f;
            sunLight.intensity = 2.f;
            sunLight.id = 11;
            const std::array<int, 2> sunOpen = lit(sunLight, scene, 2);
            sunLight.shadows = true;
            const std::array<int, 2> sunShadowed = lit(sunLight, scene, 2);
            Expect(sunOpen[0] > ambientFloor + 20 && std::abs(sunShadowed[0] - ambientFloor) <= 4,
                   "a DirectionalLight's shadow falls behind the cube (" + std::to_string(sunShadowed[0]) + ")");
            Expect(std::abs(sunShadowed[1] - sunOpen[1]) <= 3,
                   "and leaves the open floor lit (" + std::to_string(sunShadowed[1]) + ")");
            Expect(lit(sunLight, scene, 2) == sunShadowed, "a still camera reuses the cascades and draws the same");
            // The floor is about 7.6 from the camera: past a ShadowDistance of 2, unshadowed.
            sunLight.shadowDistance = 2.f;
            Expect(std::abs(lit(sunLight, scene, 2)[0] - sunOpen[0]) <= 3, "past ShadowDistance there is no shadow");
            // Straight down: the cube's shadow is under it, hidden, and nothing is NaN.
            sunLight.shadowDistance = 100.f;
            sunLight.direction[0] = 0.f;
            sunLight.direction[1] = -1.f;
            Expect(lit(sunLight, scene, 2)[0] > ambientFloor + 20, "a sun straight down lights the floor beside the cube");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "cascades leave no GL error");

            // Shadows off in the settings draws every light unshadowed.
            runner::ShadowSettings off;
            off.enabled = false;
            renderer.setShadowSettings(off);
            Expect(lit(spotLight, scene, 2)[0] > ambientFloor + 20, "ShadowSettings::enabled false draws no shadows");
            renderer.setShadowSettings(runner::ShadowSettings{});
        }
        }

        // A Material's DiffuseTexture, read from the resources folder, and its Color.
        {
            std::filesystem::create_directories(root / "textures");
            std::ofstream(root / "textures" / "stripes.tga", std::ios::binary) << StripesTga();
            std::ofstream(root / "textures" / "junk.png", std::ios::binary) << "not an image";
            const std::string tga = StripesTga();
            runner::TexturePixels pixels;
            std::string why;
            Expect(runner::DecodeTexture(reinterpret_cast<const std::uint8_t*>(tga.data()), tga.size(), pixels, why) &&
                       pixels.width == 4 && pixels.height == 4,
                   "a TGA decodes to 4 by 4 (" + why + ")");
            Expect(pixels.rgba.size() == 64 && pixels.rgba[2] == 255 && pixels.rgba[0] == 0 && pixels.rgba[60] == 255,
                   "bottom row first, as OpenGL takes it: blue, then red at the top");

            std::vector<std::string> said;
            runner::TextureCache textures([&said](const std::string& message) { said.push_back(message); });
            Expect(textures.get("textures/stripes.tga") == 0, "no root loads no texture");
            textures.setRoot(root);
            const unsigned stripes = textures.get("textures/stripes.tga");
            Expect(stripes != 0, "the texture loads from the resources folder");
            Expect(textures.get("textures/stripes.tga") == stripes, "a second get is the same upload");
            Expect(textures.get("textures/missing.png") == 0 && textures.get("textures/junk.png") == 0,
                   "a missing file and a file that is not an image have no texture");
            Expect(said.size() == 2, "each says why once (" + std::to_string(said.size()) + " reports)");

            // The front face's v runs from 0 at its bottom, 10 of 128 rows below the middle, to 1 at its
            // top, 4 above: the middle is in the image's top half, 6 rows below in its bottom half.
            runner::MeshDraw textured{cube, engine_core::matrix4_identity(), stripes};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &textured, 1);
            const Pixel upper = ReadPixel(fbWidth / 2, fbHeight / 2);
            const Pixel lower = ReadPixel(fbWidth / 2, fbHeight / 2 - fbHeight * 3 / 64);
            Expect(upper.r > 30 && upper.b < kSkyTint && upper.g < kSkyTint,
                   "the image's top is at the face's top (" + Text(upper) + ")");
            Expect(lower.b > 30 && lower.r < kSkyTint && lower.g < kSkyTint,
                   "and its bottom at the bottom (" + Text(lower) + ")");

            // Color tints the texture, and draws alone with none.
            runner::MeshDraw tinted{cube, engine_core::matrix4_identity(), stripes, {0.f, 1.f, 1.f, 1.f}};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &tinted, 1);
            Expect(ReadPixel(fbWidth / 2, fbHeight / 2).r < kSkyTint, "a cyan Color takes out the red");
            runner::MeshDraw green{cube, engine_core::matrix4_identity(), 0, {0.f, 1.f, 0.f, 1.f}};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &green, 1);
            const Pixel plain = ReadPixel(fbWidth / 2, fbHeight / 2);
            Expect(plain.g > 30 && plain.r < kSkyTint && plain.b < kSkyTint,
                   "no texture draws the Color alone (" + Text(plain) + ")");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "textures leave no GL error");
            textures.clear();
        }

        // A Skybox: drawn behind everything, and lighting what it surrounds.
        {
            std::filesystem::create_directories(root / "textures");
            std::ofstream(root / "textures" / "sky.hdr", std::ios::binary) << TestSky();
            std::ofstream(root / "textures" / "white.hdr", std::ios::binary)
                << Hdr(16, 8, [](int, int) { return std::array<float, 3>{1.f, 1.f, 1.f}; });
            std::ofstream(root / "textures" / "sky.exr", std::ios::binary) << "v/1\x01 not really";

            // Decoded linear, brighter than white, bottom row first, and halved when too wide.
            const std::string hdr = TestSky();
            runner::LinearPixels linear;
            std::string why;
            Expect(runner::DecodeLinearTexture(reinterpret_cast<const std::uint8_t*>(hdr.data()), hdr.size(), linear,
                                               why) &&
                       linear.width == 16 && linear.height == 8,
                   "an HDR decodes to 16 by 8 (" + why + ")");
            Expect(linear.rgb.size() == 16 * 8 * 3 && linear.rgb[2] > 0.9f && linear.rgb[0] == 0.f,
                   "bottom row first: blue");
            Expect(linear.rgb.size() == 16 * 8 * 3 && linear.rgb[(7 * 16) * 3] > 3.9f,
                   "the top row keeps light brighter than white");
            runner::LinearPixels halved;
            runner::DecodeLinearTexture(reinterpret_cast<const std::uint8_t*>(hdr.data()), hdr.size(), halved, why, 8);
            Expect(halved.width == 8 && halved.height == 4 && halved.rgb[(3 * 8) * 3] > 3.9f,
                   "an image wider than the limit is halved");
            const std::string tga = StripesTga();
            runner::DecodeLinearTexture(reinterpret_cast<const std::uint8_t*>(tga.data()), tga.size(), linear, why);
            Expect(linear.width == 4 && std::abs(linear.rgb[2] - 1.f) < 1e-3f,
                   "an ordinary image decodes too, sRGB made linear");

            std::vector<std::string> said;
            runner::TextureCache textures([&said](const std::string& message) { said.push_back(message); });
            textures.setRoot(root);
            const runner::EnvironmentTexture skyImage = textures.getEnvironment("textures/sky.hdr");
            const runner::EnvironmentTexture white = textures.getEnvironment("textures/white.hdr");
            Expect(skyImage.texture != 0 && skyImage.revision != 0 && white.revision != skyImage.revision,
                   "a sky uploads with a revision of its own");
            Expect(textures.getEnvironment("textures/sky.hdr").revision == skyImage.revision,
                   "an unchanged file keeps its revision");
            Expect(textures.get("textures/sky.hdr") != skyImage.texture,
                   "a Material's upload of the same file is a separate texture");
            Expect(textures.getEnvironment("textures/sky.exr").texture == 0 && !said.empty() &&
                       said.back().find("OpenEXR") != std::string::npos,
                   "an .exr says it is OpenEXR, which cannot be read (" + (said.empty() ? "" : said.back()) + ")");

            runner::SceneLighting lit;
            lit.ambient[0] = lit.ambient[1] = lit.ambient[2] = 0.f;
            lit.sky.image = skyImage.texture;
            lit.sky.imageRevision = skyImage.revision;
            renderer.setLighting(lit);
            // On macOS the cubes cannot be drawn into the frame they are made in: draw again.
            const auto drawSky = [&](const runner::MeshDraw* meshes, int count) {
                bool drawn = false;
                for (int attempt = 0; attempt < 3 && !drawn; ++attempt) {
                    drawn = renderer.draw(0, 0, kSize, kSize, kSize, kSize, meshes, count);
                }
                return drawn;
            };

            // Looking straight down -Z, level: the sky fills the pane even with nothing in it.
            renderer.setCamera(engine_core::matrix4_look_at({0.f, 0.f, 7.f}, {0.f, 0.f, 0.f}, up), 60.f);
            Expect(drawSky(nullptr, 0), "a sky with no meshes draws");
            const Pixel upperLeft = ReadPixel(fbWidth / 4, fbHeight * 3 / 4);
            const Pixel upperRight = ReadPixel(fbWidth * 3 / 4, fbHeight * 3 / 4);
            const Pixel below = ReadPixel(fbWidth / 2, fbHeight / 4);
            Expect(upperLeft.r > 150 && upperLeft.g < 30 && upperLeft.b < 30,
                   "the image's left half is on the left, above the horizon (" + Text(upperLeft) + ")");
            Expect(upperRight.g > 150 && upperRight.r < 30 && upperRight.b < 30,
                   "and its right half on the right (" + Text(upperRight) + ")");
            Expect(below.b > 60 && below.r < 30 && below.g < 30, "and below the horizon, blue (" + Text(below) + ")");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "making the cubes leaves no GL error");

            // Rotation turns it about Y: half a turn swaps left and right.
            runner::SceneLighting turned = lit;
            turned.sky.rotationDegrees = 180.f;
            renderer.setLighting(turned);
            Expect(drawSky(nullptr, 0), "a turned sky draws at once, with the cubes it has");
            const Pixel turnedLeft = ReadPixel(fbWidth / 4, fbHeight * 3 / 4);
            Expect(turnedLeft.g > 150 && turnedLeft.r < 30, "Rotation 180 puts green on the left (" +
                                                                Text(turnedLeft) + ")");
            // Exposure 0 is black, not the pane's clear color; Tint multiplies.
            runner::SceneLighting dark = lit;
            dark.sky.exposure = 0.f;
            renderer.setLighting(dark);
            drawSky(nullptr, 0);
            Expect(Sum(ReadPixel(fbWidth / 4, fbHeight * 3 / 4)) < 6, "Exposure 0 is a black sky");
            runner::SceneLighting cyan = lit;
            cyan.sky.tint[0] = 0.f;
            renderer.setLighting(cyan);
            drawSky(nullptr, 0);
            const Pixel tinted = ReadPixel(fbWidth / 4, fbHeight * 3 / 4);
            const Pixel tintedRight = ReadPixel(fbWidth * 3 / 4, fbHeight * 3 / 4);
            Expect(Sum(tinted) < 10 && tintedRight.g > 150,
                   "a cyan Tint takes the red out and leaves the green (" + Text(tinted) + ")");

            // The sky lights a white cube: its top takes the red and green above,
            // its front some of the blue below too.
            renderer.setLighting(lit);
            renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                               runner::Renderer::kCameraFovYDegrees);
            runner::MeshDraw matte{cube, engine_core::matrix4_identity()};
            matte.roughness = 1.f;
            Expect(drawSky(&matte, 1), "a cube under a sky draws");
            const int topY = fbHeight / 2 + fbHeight * 3 / 64;
            const int frontY = fbHeight / 2 - fbHeight * 3 / 64;
            const Pixel skyTop = ReadPixel(fbWidth / 2, topY);
            const Pixel skyFront = ReadPixel(fbWidth / 2, frontY);
            Expect(skyTop.r > 40 && skyTop.g > 40 && skyTop.b + 20 < skyTop.r,
                   "the top face takes the sky above it (" + Text(skyTop) + ")");
            Expect(skyFront.b > skyTop.b + 10, "the front face takes more of the blue below (" + Text(skyFront) +
                                                   " against " + Text(skyTop) + ")");

            // LightScale dims the light the sky gives surfaces, and leaves the sky behind them alone.
            const Pixel skyCorner = ReadPixel(fbWidth / 8, fbHeight * 7 / 8);
            runner::SceneLighting dimLight = lit;
            dimLight.sky.lightScale = 0.f;
            renderer.setLighting(dimLight);
            drawSky(&matte, 1);
            Expect(Sum(ReadPixel(fbWidth / 2, topY)) < 6,
                   "LightScale 0 takes the sky's light off the cube (" + Text(ReadPixel(fbWidth / 2, topY)) + ")");
            const Pixel dimCorner = ReadPixel(fbWidth / 8, fbHeight * 7 / 8);
            Expect(dimCorner.r == skyCorner.r && dimCorner.g == skyCorner.g && dimCorner.b == skyCorner.b,
                   "and leaves the sky behind it as it was (" + Text(dimCorner) + " and " + Text(skyCorner) + ")");
            dimLight.sky.lightScale = 0.5f;
            renderer.setLighting(dimLight);
            drawSky(&matte, 1);
            const int halfTop = Sum(ReadPixel(fbWidth / 2, topY));
            Expect(halfTop > 6 && halfTop < Sum(skyTop), "LightScale 0.5 is between (" + std::to_string(halfTop) +
                                                             " under " + std::to_string(Sum(skyTop)) + ")");
            renderer.setLighting(lit);
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the sky leaves no GL error");

            // With no image, today's stand-in again, and the corner the clear color.
            renderer.setLighting(runner::SceneLighting{});
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &matte, 1);
            Expect(IsClear(ReadPixel(2, 2)), "no Skybox leaves the pane's clear color around the cube");
            textures.clear();
        }

        // The floor grid: from (0, 3, 7), the X axis crosses the view through
        // its middle, red, and the Z axis runs down from the middle, blue.
        {
            renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                               runner::Renderer::kCameraFovYDegrees);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, nullptr, 0);
            Expect(IsClear(ReadPixel(fbWidth / 4, fbHeight / 2)), "the grid is off until set");
            renderer.setGridVisible(true);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, nullptr, 0);
            const Pixel xAxis = ReadPixel(fbWidth / 4, fbHeight / 2);
            const Pixel zAxis = ReadPixel(fbWidth / 2, fbHeight / 4);
            Expect(xAxis.r > xAxis.g + 60 && xAxis.r > xAxis.b + 40, "the X axis is red (" + Text(xAxis) + ")");
            Expect(zAxis.b > zAxis.r + 60 && zAxis.b > zAxis.g + 20, "the Z axis is blue (" + Text(zAxis) + ")");
            Expect(IsClear(ReadPixel(2, fbHeight - 3)), "above the horizon there is no grid");
            // The cube hides the floor behind it, and stands on nothing it lets show.
            renderer.setGridVisible(false);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
            const Pixel bare = ReadPixel(fbWidth / 2, fbHeight / 2);
            renderer.setGridVisible(true);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
            const Pixel gridded = ReadPixel(fbWidth / 2, fbHeight / 2);
            Expect(std::abs(Sum(gridded) - Sum(bare)) <= 3,
                   "a surface in front of the floor hides the grid (" + Text(gridded) + " and " + Text(bare) + ")");
            Expect(ReadPixel(fbWidth / 4, fbHeight / 2).r > xAxis.r - 10, "and the axis beside it still shows");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the grid leaves no GL error");
            renderer.setGridVisible(false);
        }

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

    // A Material's preview: a ball in its Color, lit from the upper left and
    // clear around it, drawn off screen, leaving the window's framebuffer as it was.
    {
        ide::MaterialBall ball(64, 32);
        runner::GLint viewportBefore[4] = {};
        glGetIntegerv(runner::GL_VIEWPORT, viewportBefore);
        ide::MaterialLook red;
        red.color = {1.f, 0.f, 0.f, 1.f};
        runner::ViewPixels pixels;
        Expect(ball.draw(red, pixels), "a context with nothing waiting draws the ball at once");
        Expect(pixels.width == 32 && pixels.height == 32 && pixels.rgba.size() == 32 * 32 * 4,
               "the ball comes back at the size asked");
        if (pixels.rgba.size() == 32 * 32 * 4) {
            auto at = [&pixels](int x, int y) { return pixels.rgba.data() + (y * 32 + x) * 4; };
            auto text = [](const unsigned char* p) {
                return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," +
                       std::to_string(p[3]);
            };
            Expect(at(0, 0)[3] == 0 && at(31, 31)[3] == 0, "the corners are clear");
            const unsigned char* middle = at(16, 16);
            Expect(middle[3] == 255 && middle[0] > middle[1] + 20 && middle[0] > middle[2] + 20,
                   "the middle is the red ball (" + text(middle) + ")");
            const unsigned char* lit = at(11, 11);
            const unsigned char* shaded = at(21, 21);
            Expect(lit[0] > shaded[0] + 20, "its upper left is lit brighter than its lower right (" + text(lit) +
                                                " over " + text(shaded) + ")");
        }
        ide::MaterialLook glowing;
        glowing.color = {0.f, 0.f, 0.f, 1.f};
        glowing.emissive = {0.f, 1.f, 0.f, 1.f};
        runner::ViewPixels glow;
        ball.draw(glowing, glow);
        Expect(glow.rgba.size() == 32 * 32 * 4 && glow.rgba[(16 * 32 + 16) * 4 + 1] > glow.rgba[(16 * 32 + 16) * 4] + 40,
               "an Emissive glows on a black ball");
        runner::GLint viewportAfter[4] = {};
        glGetIntegerv(runner::GL_VIEWPORT, viewportAfter);
        runner::GLint framebuffer = -1;
        glGetIntegerv(runner::RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
        Expect(framebuffer == 0 && std::equal(viewportBefore, viewportBefore + 4, viewportAfter),
               "the window's framebuffer and viewport are as they were");
        ball.release();
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the ball leaves no GL error");
    }

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
