// Visibility: which of a frame's meshes are in the camera's view, with no GL.

#include "runner/RenderMath.hpp"
#include "runner/Visibility.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

using Catch::Approx;
using engine_core::Vec3;
using namespace runner;

namespace {

const float kUnitMin[3] = {-0.5f, -0.5f, -0.5f};
const float kUnitMax[3] = {0.5f, 0.5f, 0.5f};

// From eye toward target, 60 degrees tall, square, 100 pixels tall.
CameraView Camera(Vec3 eye, Vec3 target) {
    CameraView camera;
    camera.world = engine_core::matrix4_look_at(eye, target, {0.f, 1.f, 0.f});
    camera.viewProjection = engine_core::matrix4_multiply(Perspective(60.f, 1.f, 0.1f, 1000.f),
                                                          LookAtView(eye, target, {0.f, 1.f, 0.f}));
    camera.fovYDegrees = 60.f;
    camera.aspect = 1.f;
    camera.nearZ = 0.1f;
    camera.paneHeight = 100;
    return camera;
}

engine_core::Matrix4 At(float x, float y, float z, float scale = 1.f) {
    engine_core::Matrix4 model = engine_core::matrix4_translation(x, y, z);
    for (int column = 0; column < 3; ++column) {
        for (int axis = 0; axis < 3; ++axis) {
            model.m[column * 4 + axis] *= scale;
        }
    }
    return model;
}

DrawItem Unit(const engine_core::Matrix4& model, float transparency = 0.f) {
    DrawItem item;
    item.model = &model;
    item.boundsMin = kUnitMin;
    item.boundsMax = kUnitMax;
    item.transparency = transparency;
    item.drawable = true;
    return item;
}

std::vector<int> Indices(const std::vector<VisibleDraw>& draws) {
    std::vector<int> out;
    for (const VisibleDraw& draw : draws) {
        out.push_back(draw.index);
    }
    return out;
}

}  // namespace

TEST_CASE("V1 a mesh ahead is visible; one behind the camera is culled", "[visibility]") {
    const CameraView camera = Camera({0.f, 0.f, 10.f}, {0.f, 0.f, 0.f});
    const engine_core::Matrix4 ahead = At(0.f, 0.f, 0.f);
    const engine_core::Matrix4 behind = At(0.f, 0.f, 20.f);
    const DrawItem items[2] = {Unit(ahead), Unit(behind)};
    VisibilityResult out;
    FindVisible(items, 2, camera, true, out);
    REQUIRE(Indices(out.opaque) == std::vector<int>{0});
    REQUIRE(out.transparent.empty());
    REQUIRE(out.culled == 1);
    REQUIRE(out.spheres.size() == 2);
    REQUIRE(out.spheres[1].center.z == Approx(20.f));
}

TEST_CASE("V2 past the side of the view is culled; straddling the edge is not", "[visibility]") {
    // At distance 10 a 60 degree square view reaches tan(30) * 10 = 5.77 to each side.
    const CameraView camera = Camera({0.f, 0.f, 10.f}, {0.f, 0.f, 0.f});
    const engine_core::Matrix4 straddling = At(6.f, 0.f, 0.f);  // sphere radius 0.87 reaches 5.13
    const engine_core::Matrix4 outside = At(8.f, 0.f, 0.f);
    const DrawItem items[2] = {Unit(straddling), Unit(outside)};
    VisibilityResult out;
    FindVisible(items, 2, camera, true, out);
    REQUIRE(Indices(out.opaque) == std::vector<int>{0});
    REQUIRE(out.culled == 1);
}

TEST_CASE("V3 beyond the far plane is culled", "[visibility]") {
    const CameraView camera = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    const engine_core::Matrix4 near = At(0.f, 0.f, -999.f);
    const engine_core::Matrix4 far = At(0.f, 0.f, -1010.f);
    const DrawItem items[2] = {Unit(near), Unit(far)};
    VisibilityResult out;
    FindVisible(items, 2, camera, true, out);
    REQUIRE(Indices(out.opaque) == std::vector<int>{0});
}

TEST_CASE("V4 with culling off every drawable mesh is visible, and spheres are still made", "[visibility]") {
    const CameraView camera = Camera({0.f, 0.f, 10.f}, {0.f, 0.f, 0.f});
    const engine_core::Matrix4 behind = At(0.f, 0.f, 20.f);
    const DrawItem items[1] = {Unit(behind)};
    VisibilityResult out;
    FindVisible(items, 1, camera, false, out);
    REQUIRE(Indices(out.opaque) == std::vector<int>{0});
    REQUIRE(out.culled == 0);
    REQUIRE(out.spheres[0].radius == Approx(std::sqrt(0.75f)));
}

TEST_CASE("V5 undrawable meshes are in neither list; see-through ones keep their order", "[visibility]") {
    const CameraView camera = Camera({0.f, 0.f, 10.f}, {0.f, 0.f, 0.f});
    const engine_core::Matrix4 a = At(-1.f, 0.f, 0.f);
    const engine_core::Matrix4 b = At(0.f, 0.f, 0.f);
    const engine_core::Matrix4 c = At(1.f, 0.f, 0.f);
    DrawItem items[4] = {Unit(a, 0.5f), Unit(b), Unit(c, 0.2f), Unit(b)};
    items[3].drawable = false;
    VisibilityResult out;
    FindVisible(items, 4, camera, true, out);
    REQUIRE(Indices(out.opaque) == std::vector<int>{1});
    REQUIRE(Indices(out.transparent) == std::vector<int>{0, 2});
    REQUIRE(out.culled == 0);
    REQUIRE(out.spheres[3].radius == 0.f);
}

TEST_CASE("V6 a mesh around the camera is visible, with an infinite screen radius", "[visibility]") {
    const CameraView camera = Camera({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f});
    const engine_core::Matrix4 room = At(0.f, 0.f, 0.f, 50.f);
    const DrawItem items[1] = {Unit(room)};
    VisibilityResult out;
    FindVisible(items, 1, camera, true, out);
    REQUIRE(out.opaque.size() == 1);
    REQUIRE(std::isinf(out.opaque[0].screenRadius));
}

TEST_CASE("V7 a mesh at Scale 0 is a point: culled out of view, kept in it, never NaN", "[visibility]") {
    const CameraView camera = Camera({0.f, 0.f, 10.f}, {0.f, 0.f, 0.f});
    const engine_core::Matrix4 inView = At(0.f, 0.f, 0.f, 0.f);
    const engine_core::Matrix4 outOfView = At(0.f, 0.f, 20.f, 0.f);
    const DrawItem items[2] = {Unit(inView), Unit(outOfView)};
    VisibilityResult out;
    FindVisible(items, 2, camera, true, out);
    REQUIRE(Indices(out.opaque) == std::vector<int>{0});
    REQUIRE(out.spheres[0].radius == 0.f);
    REQUIRE(out.opaque[0].screenRadius == 0.f);
}

TEST_CASE("V8 the screen radius is the sphere's projected radius in pixels", "[visibility]") {
    const CameraView camera = Camera({0.f, 0.f, 10.f}, {0.f, 0.f, 0.f});
    // radius * paneHeight / (2 * distance * tan(30 degrees)).
    const Sphere sphere{{0.f, 0.f, 0.f}, 1.f};
    REQUIRE(ScreenRadius(sphere, camera) == Approx(100.f / (20.f * std::tan(0.5235988f))));
    const Sphere around{{0.f, 0.f, 9.f}, 2.f};
    REQUIRE(std::isinf(ScreenRadius(around, camera)));
}

TEST_CASE("V9 a result is reused: a second frame replaces the first", "[visibility]") {
    const CameraView camera = Camera({0.f, 0.f, 10.f}, {0.f, 0.f, 0.f});
    const engine_core::Matrix4 a = At(0.f, 0.f, 0.f);
    const DrawItem two[2] = {Unit(a), Unit(a, 0.5f)};
    VisibilityResult out;
    FindVisible(two, 2, camera, true, out);
    FindVisible(two, 1, camera, true, out);
    REQUIRE(out.spheres.size() == 1);
    REQUIRE(Indices(out.opaque) == std::vector<int>{0});
    REQUIRE(out.transparent.empty());
    FindVisible(nullptr, 0, camera, true, out);
    REQUIRE(out.spheres.empty());
    REQUIRE(out.opaque.empty());
}
