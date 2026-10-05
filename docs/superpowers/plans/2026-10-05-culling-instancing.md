# Frustum Culling and GameObject Instancing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Scene View skips meshes outside the camera's view, and draws every GameObject that shares a Prefab Model in one instanced call, in the geometry, transparency and shadow passes.

**Architecture:**

- Two GL-free units in `studio_core`, tested in the sandbox:
  - `Visibility` turns a frame's `DrawItem`s (plain views of each `MeshDraw`) into world spheres and visible lists.
  - `DrawBatches` sorts the visible opaque draws into runs keyed by (slot, LOD, mirrored) and writes 112-byte `InstanceData` rows.
- On the GPU:
  - `InstanceBuffer` uploads the rows once per frame and points vertex slots 7–14 at a run's first row with divisor 1.
  - `GpuMesh::draw_instanced(lod, count)` draws the run.
  - `geometry.vert` and `shadow.vert` read the world matrix (and, for geometry, the normal matrix and tint) from those slots instead of `uModel`.
- Every mesh draw is instanced; a draw with nothing to share is a run of 1.

**Tech Stack:** C++17 (Apple clang 13 / libc++ 13: nothing past C++17), OpenGL 3.3 core / GLSL 330 within GLSL ES 3.00, Catch2 (`sandbox`), the by-hand GL checks `scene-render-check` and `amesh-gl-check`, the studio through the `anarchy` MCP.

**Spec:** `docs/superpowers/specs/2026-10-05-culling-instancing-design.md`

## Global Constraints

- Branch `culling-instancing` in `~/Documents/AnarchyEngine-CPP-culling`, with its own `build/`. Never commit on `main`; another session may move it.
  - Configure with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DJADEFX_CPP_DIR=/Users/yaoli/Documents/JadeFX_CPP`.
  - Never pass `FETCHCONTENT_BASE_DIR` pointing at the main checkout's `build/_deps`: it breaks the main build.
- Before each commit, `git show --stat HEAD` lists only this task's files.
- Instance layout, 112 bytes, divisor 1:

  | Data | Type | Slots | Byte offset |
  | --- | --- | --- | --- |
  | world matrix | `mat4` | 7–10 | 0 |
  | normal matrix | `mat3` | 11–13 | 64 |
  | tint | `vec3` | 14 | 100 |

  Slot 15 stays free.
- The batch key is `slot << 32 | lod << 1 | mirrored`. Slot 0 never batches. Mirrored is a negative 3×3 determinant.
- `InstanceData::tint` is linear: the GameObject's Color to the power 2.2, which matches `toLinear` in `surface.glsl` exactly.
- Shadows see every draw, never the camera-culled list.
- Efficiency:
  - one shader path, with no `#ifdef` variants;
  - no `inverse()` in any vertex shader;
  - one upload per instance buffer per pass group;
  - no per-frame allocation once vectors have grown;
  - a frame that redraws no shadow map adds no shadow uploads or draws.
- Any GL error quits the studio. Every GL check ends with a `glGetError() == GL_NO_ERROR` expectation, and new GL paths run first in `amesh-gl-check`, never first in the studio.
- Studio checks run on a scratch copy of a project in the session scratchpad, never on the user's projects in `~/Documents/AnarchyEngineProjects`.
- Screenshots go to the user as soon as the stress place draws, and again after each phase that changes what is drawn.

## Review Focus

1. **A mirrored GameObject sharing a Prefab with unmirrored ones** (a negative Scale axis in its Transform). It must draw its outside faces, not its inside. Covered by B2 in Task 5 and IN3 in Task 7.
2. **The camera inside a large object**, such as a building interior. Its sphere contains the camera, so it must still draw. Covered by V6 in Task 3.
3. **Two GameObjects of one Prefab in different Colors.** In one run, each must keep its own tint. Covered by B7 in Task 5 and IN2 in Task 7.
4. **A caster out of view whose shadow falls into view**, such as a tall object behind the camera with the sun low. Its shadow must stay. Covered by CL4 in Task 4.
5. **A GameObject with Scale 0.** There must be no NaN in the normal matrix, no GL error, and nothing drawn. Covered by V7 in Task 3 and B6 in Task 5.

---

### Task 1: Build the worktree, make the stress place, and record the baseline

No code changes. This records the "before" numbers while the branch still equals `main`.

**Files:**
- Create: `scripts/stress_place.lua`

**Interfaces:**
- Produces: `scripts/stress_place.lua`, which Task 10 runs again; and baseline numbers recorded in the task report and later in the spec.

- [ ] **Step 1: Configure and build**

```bash
cd ~/Documents/AnarchyEngine-CPP-culling
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DJADEFX_CPP_DIR=/Users/yaoli/Documents/JadeFX_CPP
cmake --build build --target bundle-resources --parallel && cmake --build build --parallel
```

Expected: the build succeeds. `./build/sandbox` passes, `./build/scene-render-check` prints `scene render checks passed`, and `./build/amesh-gl-check` prints `GpuMesh checks passed`.

- [ ] **Step 2: Write the stress script**

Before writing it, check the names with the MCP. `get_class GameObject` must list `Transform` (Matrix4), `Prefab`, `Color`, `Scale`. `get_class Prefab` must be creatable. `run_lua print(Matrix4.new(1,2,3))` must show a translation. If any name differs, use the real one.

`scripts/stress_place.lua`:

```lua
-- The culling and instancing stress place (docs/superpowers/plans/2026-10-05-culling-instancing.md).
-- Run once on a scratch copy of a project with at least one Prefab in game.Assets.Prefabs.
-- 10,000 GameObjects over 3 Prefabs on a 100 by 100 grid, 4 studs apart, plus 50 one-off
-- Prefabs in a row along -Z, each copied from the first so each has a slot of its own.
local prefabs = game.Assets.Prefabs:GetChildren()
assert(#prefabs >= 1, "the place needs a Prefab in game.Assets.Prefabs")
local shared = {}
for i = 1, 3 do
	local copy = prefabs[1]:Clone()
	copy.Name = "StressShared" .. i
	copy.Parent = game.Assets.Prefabs
	shared[i] = copy
end
local folder = Instance.new("Folder")
folder.Name = "Stress"
folder.Parent = workspace
for x = 0, 99 do
	for z = 0, 99 do
		local object = Instance.new("GameObject")
		object.Prefab = shared[(x + z) % 3 + 1]
		object.Transform = Matrix4.new(x * 4 - 200, 0, z * 4 - 200)
		object.Color = Color3.fromHSV(((x * 7 + z * 3) % 100) / 100, 0.4, 1)
		object.Parent = folder
	end
end
for i = 1, 50 do
	local copy = prefabs[1]:Clone()
	copy.Name = "StressOne" .. i
	copy.Parent = game.Assets.Prefabs
	local object = Instance.new("GameObject")
	object.Prefab = copy
	object.Transform = Matrix4.new(0, 0, -210 - i * 4)
	object.Parent = folder
end
print("stress place: " .. #folder:GetChildren() .. " GameObjects")
```

Expected when run: `stress place: 10050 GameObjects`.

- [ ] **Step 3: Open a scratch copy and build the place**

```bash
SCRATCH=<session scratchpad>/StressDemo
cp -R ~/Documents/AnarchyEngineProjects/Mitsuba "$SCRATCH"
open -n build/AnarchyStudio.app --args "$SCRATCH"
```

Then use the MCP:
- `list_studios`, then `select_studio` by the new pid.
- `run_lua` with the script's text.
- Make sure the place has a DirectionalLight with Shadows on (`find_instances` with class DirectionalLight). If it has none, `create_instance` one under Lighting with Shadows true.

- [ ] **Step 4: Record the baseline at two cameras**

A scene view shows a Camera instance (`find_instances` with class Camera; the view's camera combo names it). Move it with `run_lua`, for example `workspace.Camera.Transform = Matrix4.lookAt(Vector3.new(0, 120, 260), Vector3.new(0, 0, 0))`, using that Camera's real path and the constructor names `get_class Matrix4` lists. Use the same two poses in Task 10:
- **Overview**: from (0, 120, 260) looking at (0, 0, 0). All the grid is in view.
- **Corner**: from (-205, 6, -205) looking at (-190, 0, -190). About one corner is in view.

At each pose, let it settle for 3 seconds, then `get_profile`. Record:
- the GPU `3D scene` time;
- the CPU `Scene View`, `Snapshot read` and `Geometry` scopes;
- with GPU detail on, the GPU `Geometry` and `Shadows` passes.

Take a `screenshot` at each pose and send both to the user, with the numbers.

- [ ] **Step 5: Commit the script**

```bash
git add scripts/stress_place.lua
git commit -m "Add the culling and instancing stress place script

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Keep the studio open for Task 10, or quit it by pid and rebuild the place then.

---

### Task 2: Pixel regression scenes in scene-render-check

The renderer's output must not change, beyond float rounding, when culling and instancing land. This saves fixed frames now and compares them after each later task.

**Files:**
- Modify: `tests/SceneRenderCheck.cpp` (`main`'s signature; a new function in the anonymous namespace; one call at the end of the first block, before `meshes.clear();`)

**Interfaces:**
- Produces: `./build/scene-render-check --save <dir>` and `./build/scene-render-check --compare <dir>`. Tasks 4, 6, 7, 8 and 9 run `--compare build/regression`.

- [ ] **Step 1: Add the scenes and the save and compare modes**

In the anonymous namespace, after `TestSky()`:

```cpp
// The regression scenes: frames that must not change, beyond 1 per channel,
// when the renderer draws the same meshes another way. 25 cubes in a 5 by 5
// grid, turned and sized differently, one mirrored in X and one stretched in
// Y, colored in turn, on a floor; then under a shadowing sun; then with a
// shadowing spot and three see-through cubes in front.
std::vector<runner::MeshDraw> RegressionDraws(const GpuMesh* cube, bool seeThrough) {
    std::vector<runner::MeshDraw> draws;
    for (int i = 0; i < 25; ++i) {
        const float x = static_cast<float>(i % 5) * 1.6f - 3.2f;
        const float z = 1.f - static_cast<float>(i / 5) * 1.6f;
        engine_core::Matrix4 model = engine_core::matrix4_multiply(
            engine_core::matrix4_translation(x, 0.f, z), engine_core::matrix4_axis_angle({0.f, 1.f, 0.f}, 0.3 * i));
        const float scale = 0.5f + 0.05f * static_cast<float>(i % 7);
        for (int column = 0; column < 3; ++column) {
            for (int axis = 0; axis < 3; ++axis) {
                model.m[column * 4 + axis] *= scale;
            }
        }
        if (i == 7) {
            for (int axis = 0; axis < 3; ++axis) {
                model.m[axis] *= -1.f;
            }
        }
        if (i == 12) {
            for (int axis = 0; axis < 3; ++axis) {
                model.m[4 + axis] *= 2.f;
            }
        }
        runner::MeshDraw draw{cube, model};
        draw.color[0] = i % 3 == 0 ? 1.f : 0.3f;
        draw.color[1] = i % 3 == 1 ? 1.f : 0.3f;
        draw.color[2] = i % 3 == 2 ? 1.f : 0.3f;
        draws.push_back(draw);
    }
    engine_core::Matrix4 floor = engine_core::matrix4_translation(0.f, -0.6f, -2.f);
    floor.m[0] = 12.f;
    floor.m[5] = 0.2f;
    floor.m[10] = 12.f;
    draws.push_back(runner::MeshDraw{cube, floor});
    if (seeThrough) {
        for (int i = 0; i < 3; ++i) {
            runner::MeshDraw glass{cube, engine_core::matrix4_translation(static_cast<float>(i) - 1.f, 0.5f, 3.f)};
            glass.transparency = 0.4f;
            glass.color[i] = 1.f;
            draws.push_back(glass);
        }
    }
    return draws;
}

// The three regression frames, each width * height * 4 bytes, read from the window.
std::vector<std::vector<unsigned char>> RegressionFrames(const GpuMesh* cube, int size, int width, int height) {
    runner::Renderer renderer;
    Expect(renderer.initialize(), "the renderer builds for the regression scenes");
    runner::LightDraw sun;
    sun.kind = runner::LightDraw::Kind::Directional;
    sun.direction[0] = 0.5f;
    sun.direction[1] = -0.8f;
    sun.direction[2] = -0.3f;
    sun.intensity = 2.f;
    sun.shadows = true;
    sun.shadowDistance = 100.f;
    sun.id = 21;
    runner::LightDraw spot;
    spot.kind = runner::LightDraw::Kind::Spot;
    spot.position[0] = -4.f;
    spot.position[1] = 4.f;
    spot.position[2] = 2.f;
    spot.direction[0] = 0.6f;
    spot.direction[1] = -0.7f;
    spot.direction[2] = -0.4f;
    spot.outerFovDegrees = 90.f;
    spot.radius = 20.f;
    spot.intensity = 4.f;
    spot.shadows = true;
    spot.id = 22;
    std::vector<std::vector<unsigned char>> frames;
    const auto capture = [&](const std::vector<runner::MeshDraw>& draws, const runner::LightDraw* light) {
        // Twice, so cached shadow maps are what the frame reads, as in the studio.
        for (int pass = 0; pass < 2; ++pass) {
            renderer.draw(0, 0, size, size, size, size, draws.data(), static_cast<int>(draws.size()), light,
                          light != nullptr ? 1 : 0);
        }
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, pixels.data());
        frames.push_back(std::move(pixels));
    };
    capture(RegressionDraws(cube, false), nullptr);
    capture(RegressionDraws(cube, false), &sun);
    capture(RegressionDraws(cube, true), &spot);
    Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the regression scenes leave no GL error");
    renderer.shutdown();
    return frames;
}

// --save writes each frame to dir/regression-N.rgba; --compare reads them back
// and expects every channel within 1.
void SaveOrCompareRegression(const std::vector<std::vector<unsigned char>>& frames, const std::string& mode,
                             const std::filesystem::path& dir) {
    for (std::size_t n = 0; n < frames.size(); ++n) {
        const std::filesystem::path file = dir / ("regression-" + std::to_string(n) + ".rgba");
        if (mode == "--save") {
            std::filesystem::create_directories(dir);
            std::ofstream(file, std::ios::binary)
                .write(reinterpret_cast<const char*>(frames[n].data()), static_cast<std::streamsize>(frames[n].size()));
            continue;
        }
        std::ifstream in(file, std::ios::binary);
        std::vector<unsigned char> saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (saved.size() != frames[n].size()) {
            Expect(false, "regression frame " + std::to_string(n) + " has the saved size");
            continue;
        }
        int worst = 0;
        int differing = 0;
        for (std::size_t i = 0; i < saved.size(); ++i) {
            const int difference = std::abs(static_cast<int>(saved[i]) - static_cast<int>(frames[n][i]));
            worst = std::max(worst, difference);
            differing += difference > 0 ? 1 : 0;
        }
        Expect(worst <= 1, "regression frame " + std::to_string(n) + " matches within 1 (worst " +
                               std::to_string(worst) + ", " + std::to_string(differing) + " channels differ)");
    }
}
```

Add `#include <iterator>` to the includes. Change `int main()` to `int main(int argc, char** argv)`. At the end of the first block, just before `meshes.clear();`, add:

```cpp
        if (argc == 3 && (std::string(argv[1]) == "--save" || std::string(argv[1]) == "--compare")) {
            SaveOrCompareRegression(RegressionFrames(cube, kSize, fbWidth, fbHeight), argv[1], argv[2]);
        }
```

Update the comment above `namespace {` to name the two modes: "With --save dir or --compare dir it also writes, or checks against, the regression scenes' frames."

- [ ] **Step 2: Save the baseline**

```bash
cmake --build build --target scene-render-check --parallel
./build/scene-render-check --save build/regression; echo "exit $?"
./build/scene-render-check --compare build/regression; echo "exit $?"
```

Expected: both print `scene render checks passed`, and exit 0. `build/regression/regression-{0,1,2}.rgba` exist. Read the three frames by eye once: write each to PNG with `sips` or Python, and check that cubes, the floor, a shadow and the glass show. Send them to the user.

- [ ] **Step 3: Commit**

```bash
git add tests/SceneRenderCheck.cpp
git commit -m "Save and compare scene-render-check's regression frames

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Visibility, with no GL

**Files:**
- Create: `src/runner/Visibility.hpp`, `src/runner/Visibility.cpp`, `sandbox/visibility_tests.cpp`
- Modify: `CMakeLists.txt`: `STUDIO_CORE_SOURCES` gets `src/runner/Visibility.cpp` after `src/runner/ShadowPlanner.cpp`; the sandbox list gets `sandbox/visibility_tests.cpp` after `sandbox/shadow_planner_tests.cpp`.

**Interfaces:**
- Consumes: `runner::Sphere`, `MakeFrustum`, `SphereInFrustum`, `WorldBounds` (`ShadowMath.hpp`); `runner::CameraView` (`ShadowPlanner.hpp`).
- Produces:
  - `struct DrawItem { const engine_core::Matrix4* model; const float* boundsMin; const float* boundsMax; const float* tint; float transparency; std::uint32_t slot; bool drawable; }`
  - `struct VisibleDraw { int index; float screenRadius; std::uint8_t lod; }`
  - `struct VisibilityResult { std::vector<Sphere> spheres; std::vector<VisibleDraw> opaque; std::vector<VisibleDraw> transparent; int culled; }`
  - `void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out);`
  - `float ScreenRadius(const Sphere& sphere, const CameraView& camera);`

- [ ] **Step 1: Write the failing tests**

`sandbox/visibility_tests.cpp`:

```cpp
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
```

Add both CMake lines (see Files) and run `cmake -S . -B build`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: the build fails with `'runner/Visibility.hpp' file not found`.

- [ ] **Step 3: Write Visibility**

`src/runner/Visibility.hpp`:

```cpp
#pragma once

#include "Matrix4.hpp"
#include "ShadowMath.hpp"
#include "ShadowPlanner.hpp"

#include <cstdint>
#include <vector>

namespace runner {

// What visibility and batching read from one MeshDraw, with no GL: the
// Renderer fills one per draw each frame, pointing into that draw.
struct DrawItem {
    // World space, column-major.
    const engine_core::Matrix4* model = nullptr;
    // The mesh's local box.
    const float* boundsMin = nullptr;
    const float* boundsMax = nullptr;
    // The GameObject's Color, RGB as its Color3 holds it. Null is white.
    const float* tint = nullptr;
    // 0 is opaque; above 0 is drawn in the see-through pass.
    float transparency = 0.f;
    // Which Prefab Model it draws. Draws with the same nonzero slot share a
    // mesh and every Material value. 0 never batches.
    std::uint32_t slot = 0;
    // An uploaded mesh, and transparency below 1. Anything else draws nothing.
    bool drawable = false;
};

// A drawable mesh the camera may see.
struct VisibleDraw {
    // Into the frame's DrawItems, and so its MeshDraws.
    int index = 0;
    // Its sphere's radius as projected, in pixels; infinite with the camera inside it.
    float screenRadius = 0.f;
    // Which LOD it draws. 0 until LOD selection fills it from screenRadius.
    std::uint8_t lod = 0;
};

// One frame's visibility, reused frame to frame so it allocates only while it grows.
struct VisibilityResult {
    // One per DrawItem, world space, for the shadow pass too. Zero for one that is not drawable.
    std::vector<Sphere> spheres;
    // In DrawItem order.
    std::vector<VisibleDraw> opaque;
    std::vector<VisibleDraw> transparent;
    // Drawable, but outside the view.
    int culled = 0;
};

// Each drawable item's world sphere and, when cull is set, whether it is in
// camera's view. With cull false every drawable item is visible. Conservative:
// an item just past a corner of the view may count as visible; one in view never
// counts as culled.
void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out);

// sphere's radius in pixels as camera sees it: radius * paneHeight /
// (2 * distance * tan(fovY / 2)). Infinite when the camera is inside it.
float ScreenRadius(const Sphere& sphere, const CameraView& camera);

}  // namespace runner
```

`src/runner/Visibility.cpp`:

```cpp
#include "Visibility.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace runner {

float ScreenRadius(const Sphere& sphere, const CameraView& camera) {
    const float* eye = camera.world.m + 12;
    const float dx = sphere.center.x - eye[0];
    const float dy = sphere.center.y - eye[1];
    const float dz = sphere.center.z - eye[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= sphere.radius) {
        return std::numeric_limits<float>::infinity();
    }
    const float halfFov = camera.fovYDegrees * 0.5f * 0.01745329252f;
    return sphere.radius * static_cast<float>(camera.paneHeight) / (2.f * distance * std::tan(halfFov));
}

void FindVisible(const DrawItem* items, int count, const CameraView& camera, bool cull, VisibilityResult& out) {
    count = std::max(count, 0);
    out.spheres.assign(static_cast<std::size_t>(count), Sphere{});
    out.opaque.clear();
    out.transparent.clear();
    out.culled = 0;
    const Frustum frustum = MakeFrustum(camera.viewProjection);
    for (int index = 0; index < count; ++index) {
        const DrawItem& item = items[index];
        if (!item.drawable) {
            continue;
        }
        const Sphere sphere = WorldBounds(*item.model, item.boundsMin, item.boundsMax);
        out.spheres[static_cast<std::size_t>(index)] = sphere;
        if (cull && !SphereInFrustum(frustum, sphere)) {
            ++out.culled;
            continue;
        }
        VisibleDraw visible;
        visible.index = index;
        visible.screenRadius = ScreenRadius(sphere, camera);
        (item.transparency > 0.f ? out.transparent : out.opaque).push_back(visible);
    }
}

}  // namespace runner
```

- [ ] **Step 4: Run the tests and see them pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[visibility]"`
Expected: `All tests passed (… assertions in 9 test cases)`.

If V2's straddling case fails, check that `WorldBounds`'s radius is `0.866 * scale`, and that `SphereInFrustum` divides by each plane's length. Fix the test's numbers only if they are wrong; the spec requires a conservative test.

- [ ] **Step 5: Run the whole sandbox, then commit**

Run: `./build/sandbox`
Expected: no failures.

```bash
git add src/runner/Visibility.hpp src/runner/Visibility.cpp sandbox/visibility_tests.cpp CMakeLists.txt
git commit -m "Add Visibility: each mesh's world sphere, and which the camera can see

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The renderer culls, and shadows reuse its spheres

**Files:**
- Modify: `src/runner/Renderer.hpp` (`RenderStats`; `setCulling`, `stats`; private `cameraView`, `findVisible`, members `drawItems_`, `visibility_`, `culling_`, `stats_`; `shadowPass`'s signature)
- Modify: `src/runner/Renderer.cpp` (`draw`, `shadowPass`, `geometryPass`, `transparencyPass`)
- Modify: `src/runner/ShadowRenderer.hpp`, `src/runner/ShadowRenderer.cpp` (`draw` takes the spheres)
- Test: `tests/SceneRenderCheck.cpp` (checks CL1–CL4)

**Interfaces:**
- Consumes: `DrawItem`, `VisibilityResult`, `FindVisible` (Task 3).
- Produces:
  - `struct runner::RenderStats { int draws; int visible; int culled; int runs; int instancedCalls; }`
  - `void Renderer::setCulling(bool)`, `const RenderStats& Renderer::stats() const`
  - `bool ShadowRenderer::draw(const std::vector<ShadowRequest>& requests, const MeshDraw* meshes, int count, const Sphere* spheres, const CameraView& camera, const ShadowSettings& settings)`

- [ ] **Step 1: Write the failing checks**

In `tests/SceneRenderCheck.cpp`, inside the first block, just before the `if (argc == 3 …)` regression call from Task 2, add:

```cpp
        {
            // CL1–CL3: culling. From (0, 0, 8) down -Z with a 60 degree view, a cube
            // at the origin is in view, one at (0, 0, 20) is behind the camera, and
            // one at (40, 0, 0) is far past the right edge.
            runner::Renderer culler;
            Expect(culler.initialize(), "the renderer builds for culling");
            culler.setCamera(engine_core::matrix4_translation(0.f, 0.f, 8.f), 60.f);
            const runner::MeshDraw three[3] = {runner::MeshDraw{cube, engine_core::matrix4_identity()},
                                               runner::MeshDraw{cube, engine_core::matrix4_translation(0.f, 0.f, 20.f)},
                                               runner::MeshDraw{cube, engine_core::matrix4_translation(40.f, 0.f, 0.f)}};
            culler.draw(0, 0, kSize, kSize, kSize, kSize, three, 3);
            const runner::RenderStats culled = culler.stats();
            Expect(culled.draws == 3 && culled.visible == 1 && culled.culled == 2 && culled.runs == 1,
                   "CL1 two of three cubes are culled (" + std::to_string(culled.visible) + " visible, " +
                       std::to_string(culled.culled) + " culled)");
            std::vector<unsigned char> withCulling(static_cast<std::size_t>(fbWidth) * fbHeight * 4);
            glReadPixels(0, 0, fbWidth, fbHeight, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, withCulling.data());
            culler.setCulling(false);
            culler.draw(0, 0, kSize, kSize, kSize, kSize, three, 3);
            Expect(culler.stats().visible == 3 && culler.stats().culled == 0, "CL2 culling off draws all three");
            std::vector<unsigned char> without(withCulling.size());
            glReadPixels(0, 0, fbWidth, fbHeight, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, without.data());
            Expect(withCulling == without, "CL3 culling changes no pixel");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "culling leaves no GL error");
            culler.shutdown();
        }
```

In the sun shadow checks, just after `Expect(lit(sunLight, scene, 2)[0] > ambientFloor + 20, "a sun straight down lights the floor beside the cube");`, add:

```cpp
            // CL4: a caster the camera cannot see still shadows what it can. A cube
            // 12 studs above the open floor point is far above the view.
            const runner::MeshDraw withHigh[3] = {scene[0], scene[1],
                                                  runner::MeshDraw{cube, engine_core::matrix4_translation(1.f, 12.f, 2.f)}};
            const int openUnder = lit(sunLight, scene, 2)[1];
            const std::array<int, 2> high = lit(sunLight, withHigh, 3);
            Expect(renderer.stats().culled == 1, "CL4 the high cube is out of view (" +
                                                     std::to_string(renderer.stats().culled) + " culled)");
            Expect(openUnder > ambientFloor + 20 && std::abs(high[1] - ambientFloor) <= 4,
                   "CL4 and its shadow still falls on the open floor (" + std::to_string(high[1]) + " against " +
                       std::to_string(ambientFloor) + ")");
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target scene-render-check --parallel`
Expected: the build fails: `no member named 'stats' in 'runner::Renderer'`.

- [ ] **Step 3: Add the stats and culling API to Renderer.hpp**

Add `#include "Visibility.hpp"` to the includes. Above `class Renderer`, after `struct LightDraw`:

```cpp
// What the last draw did with its meshes.
struct RenderStats {
    // MeshDraws given.
    int draws = 0;
    // Opaque and see-through draws in view.
    int visible = 0;
    // Drawable, but outside the view.
    int culled = 0;
    // Draw calls the geometry pass made.
    int runs = 0;
    // GpuMesh::draw_instanced calls in every pass, shadows included.
    int instancedCalls = 0;
};
```

In the public section, after `setShadowSettings`:

```cpp
    // Whether meshes outside the view are skipped: true unless turned off to compare.
    void setCulling(bool culling) { culling_ = culling; }
    // The last draw's counts. Zero for a draw that drew no meshes.
    const RenderStats& stats() const { return stats_; }
```

In the private section, change `bool shadowPass(const MeshDraw* meshes, int count, const float* projection);` to:

```cpp
    // The camera as the shadow planner and visibility see it, for this frame's targets.
    CameraView cameraView(const float* projection) const;
    // Fills drawItems_ from meshes and finds what the camera sees into visibility_.
    void findVisible(const MeshDraw* meshes, int count, const CameraView& camera);
    bool shadowPass(const MeshDraw* meshes, int count, const CameraView& camera);
```

Replace the members `std::vector<int> transparent_;` and `std::vector<float> transparentDepth_;` with:

```cpp
    std::vector<int> transparent_;
    std::vector<float> transparentDepth_;
    std::vector<DrawItem> drawItems_;
    VisibilityResult visibility_;
    bool culling_ = true;
    RenderStats stats_;
```

(`transparent_` and `transparentDepth_` go in Task 6.)

- [ ] **Step 4: Cull in Renderer.cpp**

At the top of `Renderer::draw`, after `probedDepth_.reset();`, add `stats_ = RenderStats{};`.

Add the two helpers before `Renderer::shadowPass`:

```cpp
CameraView Renderer::cameraView(const float* projection) const {
    CameraView camera;
    camera.world = engine_core::matrix4_inverse(view_);
    Matrix viewProjection;
    Multiply(projection, view_.m, viewProjection);
    std::copy(viewProjection, viewProjection + 16, camera.viewProjection.m);
    camera.fovYDegrees = fovYDegrees_;
    camera.aspect = static_cast<float>(targetWidth_) / static_cast<float>(targetHeight_);
    camera.nearZ = kSceneNear;
    camera.paneHeight = targetHeight_;
    return camera;
}

void Renderer::findVisible(const MeshDraw* meshes, int count, const CameraView& camera) {
    PROFILE_SCOPE("Visibility", profiler::Group::Render);
    drawItems_.resize(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const MeshDraw& draw = meshes[index];
        DrawItem& item = drawItems_[static_cast<std::size_t>(index)];
        item = DrawItem{};
        item.model = &draw.model;
        item.transparency = draw.transparency;
        // As the passes have always skipped: no mesh, not uploaded, or wholly see-through.
        item.drawable = draw.mesh != nullptr && draw.mesh->valid() && !(draw.transparency >= 1.f);
        if (item.drawable) {
            item.boundsMin = draw.mesh->bounds_min();
            item.boundsMax = draw.mesh->bounds_max();
        }
    }
    FindVisible(drawItems_.data(), count, camera, culling_, visibility_);
    stats_.draws = count;
    stats_.visible = static_cast<int>(visibility_.opaque.size() + visibility_.transparent.size());
    stats_.culled = visibility_.culled;
}
```

Replace `shadowPass` so it takes the camera instead of building it:

```cpp
bool Renderer::shadowPass(const MeshDraw* meshes, int count, const CameraView& camera) {
    RENDER_PASS("Shadows");
    shadowLookups_.assign(shadowRequests_.size(), ShadowLookup{});
    sunLookup_ = ShadowLookup{};
    if (shadowRequests_.empty() && !hasSunShadow_) {
        return true;
    }
    if (!shadows_.draw(shadowRequests_, meshes, count, visibility_.spheres.data(), camera, shadowSettings_)) {
        return false;
    }
    if (!shadows_.drawSun(hasSunShadow_ ? &sunShadow_ : nullptr, meshes, camera, shadowSettings_)) {
        return false;
    }
    sunLookup_ = shadows_.sunLookup();
    for (std::size_t index = 0; index < shadowRequests_.size(); ++index) {
        shadowLookups_[index] = shadows_.lookup(shadowRequests_[index].key);
    }
    return true;
}
```

In `draw`, replace

```cpp
        drawn = cubesReady && shadowPass(meshes, meshCount, projection) &&
```

with

```cpp
        const CameraView camera = cameraView(projection);
        findVisible(meshes, meshCount, camera);
        drawn = cubesReady && shadowPass(meshes, meshCount, camera) &&
```

In `geometryPass`, replace the loop with one over the visible opaque draws. The see-through list now comes from visibility:

```cpp
    transparent_.clear();
    for (const VisibleDraw& visible : visibility_.transparent) {
        transparent_.push_back(visible.index);
    }
    bool asked = false;
    for (const VisibleDraw& visible : visibility_.opaque) {
        const MeshDraw& draw = meshes[visible.index];
        bindMaterial(geometry_, draw);
        CullBackFaces(draw.model.m);
        draw.mesh->bind();
        if (!asked && !CanDraw(geometry_.id)) {
            return false;
        }
        asked = true;
        draw.mesh->draw(visible.lod);
        ++stats_.runs;
    }
```

`geometryPass`'s `count` parameter is now unused. Keep the signature and mark it `(void)count;`; Task 6 replaces the pass. `transparencyPass` is unchanged: it still sorts `transparent_`.

- [ ] **Step 5: Shadows take the spheres**

In `ShadowRenderer.hpp`, change `draw`'s declaration and comment:

```cpp
    // Draws this frame's due tiles for requests, casting from meshes, whose
    // world spheres are spheres (one per mesh). False, having committed nothing,
    // when the program cannot draw yet (macOS). A driver that will not draw into
    // the atlas gets every light unshadowed, said once.
    bool draw(const std::vector<ShadowRequest>& requests, const MeshDraw* meshes, int count, const Sphere* spheres,
              const CameraView& camera, const ShadowSettings& settings);
```

In `ShadowRenderer.cpp`, change the definition to match, and replace
`caster.bounds = WorldBounds(mesh.model, mesh.mesh->bounds_min(), mesh.mesh->bounds_max());`
with `caster.bounds = spheres[index];`.

Then `grep -rn "shadows_.draw\|\.draw(shadow" src tests` and update any other caller (only `Renderer::shadowPass` is expected).

- [ ] **Step 6: Run the checks and see them pass**

```bash
cmake --build build --parallel
./build/scene-render-check --compare build/regression; echo "exit $?"
./build/sandbox
```

Expected: `scene render checks passed`, exit 0. No `FAIL` lines, including CL1–CL4 and the three regression frames. The sandbox passes.

- [ ] **Step 7: Commit**

```bash
git add src/runner/Renderer.hpp src/runner/Renderer.cpp src/runner/ShadowRenderer.hpp src/runner/ShadowRenderer.cpp tests/SceneRenderCheck.cpp
git commit -m "Skip meshes outside the view, and shadow from the spheres visibility made

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: DrawBatches, with no GL

**Files:**
- Create: `src/runner/DrawBatches.hpp`, `src/runner/DrawBatches.cpp`, `sandbox/draw_batches_tests.cpp`
- Modify: `CMakeLists.txt`: `STUDIO_CORE_SOURCES` gets `src/runner/DrawBatches.cpp` after `src/runner/Visibility.cpp`; the sandbox list gets `sandbox/draw_batches_tests.cpp` after `sandbox/visibility_tests.cpp`.

**Interfaces:**
- Consumes: `DrawItem`, `VisibleDraw`, `VisibilityResult` (Task 3).
- Produces:
  - `struct InstanceData { float model[16]; float normal[9]; float tint[3]; }`, 112 bytes
  - `struct DrawRun { int first; int count; int draw; std::uint8_t lod; bool mirrored; }`
  - `struct DrawBatches { std::vector<DrawRun> runs; std::vector<InstanceData> instances; int opaqueRuns; }`
  - `void BuildBatches(const DrawItem* items, const VisibilityResult& visible, const engine_core::Matrix4& view, DrawBatches& out);`
  - `std::uint64_t BatchKey(std::uint32_t slot, std::uint8_t lod, bool mirrored);`
  - `bool Mirrored(const engine_core::Matrix4& model);`
  - `void NormalMatrix(const engine_core::Matrix4& model, float out[9]);`

- [ ] **Step 1: Write the failing tests**

`sandbox/draw_batches_tests.cpp`:

```cpp
// DrawBatches: the visible meshes sorted into instanced runs, and each
// instance's data, with no GL.

#include "runner/DrawBatches.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using Catch::Approx;
using namespace runner;

namespace {

const float kBox[3] = {0.5f, 0.5f, 0.5f};

// A frame of items: each one's model, slot, tint and transparency.
struct Frame {
    std::vector<engine_core::Matrix4> models;
    std::vector<std::array<float, 3>> tints;
    std::vector<DrawItem> items;
    VisibilityResult visible;

    // Every item is visible, opaque below transparency 0, at LOD lod.
    void add(const engine_core::Matrix4& model, std::uint32_t slot, float transparency = 0.f,
             std::array<float, 3> tint = {1.f, 1.f, 1.f}, std::uint8_t lod = 0) {
        models.push_back(model);
        tints.push_back(tint);
        DrawItem item;
        item.boundsMin = kBox;
        item.boundsMax = kBox;
        item.slot = slot;
        item.transparency = transparency;
        item.drawable = true;
        items.push_back(item);
        VisibleDraw draw;
        draw.index = static_cast<int>(items.size()) - 1;
        draw.lod = lod;
        (transparency > 0.f ? visible.transparent : visible.opaque).push_back(draw);
    }

    // Points each item at its model and tint, once the vectors stop growing.
    const DrawItem* ready() {
        for (std::size_t i = 0; i < items.size(); ++i) {
            items[i].model = &models[i];
            items[i].tint = tints[i].data();
        }
        return items.data();
    }
};

// A camera at the origin looking down -Z: the view is the identity.
const engine_core::Matrix4 kView = engine_core::matrix4_identity();

engine_core::Matrix4 At(float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); }

}  // namespace

TEST_CASE("B1 draws of one slot make one run, nearest first", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -30.f), 5);
    frame.add(At(0.f, 0.f, -10.f), 5);
    frame.add(At(0.f, 0.f, -20.f), 5);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 1);
    REQUIRE(out.opaqueRuns == 1);
    REQUIRE(out.runs[0].first == 0);
    REQUIRE(out.runs[0].count == 3);
    REQUIRE(out.runs[0].draw == 1);  // its nearest MeshDraw, whose mesh and Material all share
    REQUIRE(out.instances.size() == 3);
    REQUIRE(out.instances[0].model[14] == -10.f);
    REQUIRE(out.instances[1].model[14] == -20.f);
    REQUIRE(out.instances[2].model[14] == -30.f);
}

TEST_CASE("B2 a mirrored draw never shares a run with unmirrored ones", "[batches]") {
    Frame frame;
    engine_core::Matrix4 mirrored = At(1.f, 0.f, -5.f);
    mirrored.m[0] = -1.f;
    frame.add(At(0.f, 0.f, -5.f), 5);
    frame.add(mirrored, 5);
    frame.add(At(2.f, 0.f, -5.f), 5);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 2);
    REQUIRE(Mirrored(mirrored));
    REQUIRE_FALSE(Mirrored(At(0.f, 0.f, 0.f)));
    int mirroredRuns = 0;
    for (const DrawRun& run : out.runs) {
        mirroredRuns += run.mirrored ? 1 : 0;
        REQUIRE(run.count == (run.mirrored ? 1 : 2));
    }
    REQUIRE(mirroredRuns == 1);
}

TEST_CASE("B3 slot 0 never batches, even with identical draws", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 0);
    frame.add(At(0.f, 0.f, -5.f), 0);
    frame.add(At(0.f, 0.f, -5.f), 0);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 3);
    for (const DrawRun& run : out.runs) {
        REQUIRE(run.count == 1);
    }
}

TEST_CASE("B4 different LODs and different slots make different runs", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5, 0.f, {1.f, 1.f, 1.f}, 0);
    frame.add(At(0.f, 0.f, -6.f), 5, 0.f, {1.f, 1.f, 1.f}, 1);
    frame.add(At(0.f, 0.f, -7.f), 6);
    frame.add(At(0.f, 0.f, -8.f), 5, 0.f, {1.f, 1.f, 1.f}, 0);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 3);
    // Sorted by key: slot 5 LOD 0 (two), slot 5 LOD 1, slot 6.
    REQUIRE(out.runs[0].count == 2);
    REQUIRE(out.runs[0].lod == 0);
    REQUIRE(out.runs[1].lod == 1);
    REQUIRE(out.runs[2].draw == 2);
    REQUIRE(BatchKey(5, 0, false) < BatchKey(5, 1, false));
    REQUIRE(BatchKey(5, 1, true) < BatchKey(6, 0, false));
}

TEST_CASE("B5 see-through draws follow as runs of 1, farthest first", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5, 0.5f);
    frame.add(At(0.f, 0.f, -9.f), 5, 0.5f);
    frame.add(At(0.f, 0.f, -2.f), 5);
    frame.add(At(0.f, 0.f, -7.f), 5, 0.5f);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.opaqueRuns == 1);
    REQUIRE(out.runs.size() == 4);
    REQUIRE(out.runs[1].draw == 1);
    REQUIRE(out.runs[2].draw == 3);
    REQUIRE(out.runs[3].draw == 0);
    for (std::size_t r = 1; r < out.runs.size(); ++r) {
        REQUIRE(out.runs[r].count == 1);
        REQUIRE(out.instances[static_cast<std::size_t>(out.runs[r].first)].model[14] ==
                frame.models[static_cast<std::size_t>(out.runs[r].draw)].m[14]);
    }
}

TEST_CASE("B6 the normal matrix is the inverse transpose, and zeros when there is none", "[batches]") {
    float normal[9];
    // A turn keeps normals as the model turns them.
    const engine_core::Matrix4 turn = engine_core::matrix4_axis_angle({0.f, 1.f, 0.f}, 0.7);
    NormalMatrix(turn, normal);
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            REQUIRE(normal[column * 3 + row] == Approx(turn.m[column * 4 + row]).margin(1e-6));
        }
    }
    // Stretched 2 in X, a surface's normal in X shrinks by half.
    engine_core::Matrix4 stretched = engine_core::matrix4_identity();
    stretched.m[0] = 2.f;
    NormalMatrix(stretched, normal);
    REQUIRE(normal[0] == Approx(0.5f));
    REQUIRE(normal[4] == Approx(1.f));
    REQUIRE(normal[8] == Approx(1.f));
    // Scale 0 has no inverse.
    engine_core::Matrix4 flat = engine_core::matrix4_identity();
    flat.m[0] = flat.m[5] = flat.m[10] = 0.f;
    NormalMatrix(flat, normal);
    for (const float value : normal) {
        REQUIRE(value == 0.f);
    }
}

TEST_CASE("B7 each instance keeps its own tint, made linear", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5, 0.f, {1.f, 0.f, 0.f});
    frame.add(At(1.f, 0.f, -6.f), 5, 0.f, {0.f, 0.5f, 0.f});
    frame.items.push_back(frame.items[0]);
    frame.models.push_back(At(2.f, 0.f, -7.f));
    frame.tints.push_back({1.f, 1.f, 1.f});
    frame.visible.opaque.push_back(VisibleDraw{2, 0.f, 0});
    const DrawItem* items = frame.ready();
    frame.items[2].tint = nullptr;  // no tint is white
    DrawBatches out;
    BuildBatches(items, frame.visible, kView, out);
    REQUIRE(out.runs.size() == 1);
    REQUIRE(out.instances[0].tint[0] == 1.f);
    REQUIRE(out.instances[0].tint[1] == 0.f);
    REQUIRE(out.instances[1].tint[1] == Approx(std::pow(0.5f, 2.2f)));
    REQUIRE(out.instances[2].tint[2] == 1.f);
}

TEST_CASE("B8 a second frame replaces the first", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5);
    frame.add(At(0.f, 0.f, -6.f), 6, 0.5f);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 2);
    VisibilityResult none;
    BuildBatches(frame.items.data(), none, kView, out);
    REQUIRE(out.runs.empty());
    REQUIRE(out.instances.empty());
    REQUIRE(out.opaqueRuns == 0);
}

TEST_CASE("B9 InstanceData is 112 bytes, in slot order", "[batches]") {
    REQUIRE(sizeof(InstanceData) == 112);
    REQUIRE(offsetof(InstanceData, normal) == 64);
    REQUIRE(offsetof(InstanceData, tint) == 100);
}
```

Add `#include <array>` and `#include <cstddef>` at the top with the other includes. Add the CMake lines and run `cmake -S . -B build`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: the build fails with `'runner/DrawBatches.hpp' file not found`.

- [ ] **Step 3: Write DrawBatches**

`src/runner/DrawBatches.hpp`:

```cpp
#pragma once

#include "Matrix4.hpp"
#include "Visibility.hpp"

#include <cstdint>
#include <vector>

namespace runner {

// One instance as vertex slots 7 to 14 read it (amesh.hpp's kAttribInstance*),
// 112 bytes with no padding.
struct InstanceData {
    // World space, column-major: slots 7 to 10.
    float model[16];
    // The inverse transpose of model's 3 by 3, column-major, or zeros when it
    // has no inverse: slots 11 to 13.
    float normal[9];
    // The GameObject's Color made linear, as surface.glsl's toLinear: slot 14.
    float tint[3];
};
static_assert(sizeof(InstanceData) == 112, "vertex slots 7 to 14 read 112 bytes per instance");

// Instances first to first + count, all drawn with MeshDraw draw's mesh and Material.
struct DrawRun {
    int first = 0;
    int count = 0;
    int draw = 0;
    std::uint8_t lod = 0;
    // Its models turn the mesh inside out, so front faces are culled instead of back.
    bool mirrored = false;
};

// One frame's runs, reused frame to frame so it allocates only while it grows.
struct DrawBatches {
    // The opaque runs, then one run of 1 per see-through draw, farthest first.
    std::vector<DrawRun> runs;
    std::vector<InstanceData> instances;
    int opaqueRuns = 0;

    // Sort scratch, kept between frames.
    struct Entry {
        std::uint64_t key = 0;
        float depth = 0.f;
        int index = 0;
        std::uint8_t lod = 0;
    };
    std::vector<Entry> order;
};

// Sorts visible's opaque draws by BatchKey, nearest first within a key, into
// runs; a slot 0 draw is always a run of its own. Then each see-through draw,
// farthest first by its Transform's depth in view, as a run of 1.
void BuildBatches(const DrawItem* items, const VisibilityResult& visible, const engine_core::Matrix4& view,
                  DrawBatches& out);

// slot << 32 | lod << 1 | mirrored.
std::uint64_t BatchKey(std::uint32_t slot, std::uint8_t lod, bool mirrored);
// Whether model's 3 by 3 has a negative determinant.
bool Mirrored(const engine_core::Matrix4& model);
// The inverse transpose of model's 3 by 3, column-major; zeros when it has no inverse.
void NormalMatrix(const engine_core::Matrix4& model, float out[9]);

}  // namespace runner
```

`src/runner/DrawBatches.cpp`:

```cpp
#include "DrawBatches.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

namespace {

// How far along the view's -Z model's origin is: larger is nearer.
float ViewDepth(const engine_core::Matrix4& view, const engine_core::Matrix4& model) {
    const float* v = view.m;
    const float* t = model.m + 12;
    return v[2] * t[0] + v[6] * t[1] + v[10] * t[2] + v[14];
}

void Cross(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

InstanceData MakeInstance(const DrawItem& item) {
    InstanceData data{};
    std::copy(item.model->m, item.model->m + 16, data.model);
    NormalMatrix(*item.model, data.normal);
    for (int channel = 0; channel < 3; ++channel) {
        const float tint = item.tint != nullptr ? item.tint[channel] : 1.f;
        data.tint[channel] = std::pow(std::max(tint, 0.f), 2.2f);
    }
    return data;
}

}  // namespace

std::uint64_t BatchKey(std::uint32_t slot, std::uint8_t lod, bool mirrored) {
    return static_cast<std::uint64_t>(slot) << 32 | static_cast<std::uint64_t>(lod) << 1 |
           static_cast<std::uint64_t>(mirrored ? 1 : 0);
}

bool Mirrored(const engine_core::Matrix4& model) {
    const float* m = model.m;
    const float determinant = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                              m[8] * (m[1] * m[6] - m[5] * m[2]);
    return determinant < 0.f;
}

void NormalMatrix(const engine_core::Matrix4& model, float out[9]) {
    // With columns a0, a1, a2, the inverse transpose's columns are
    // (a1 x a2, a2 x a0, a0 x a1) over the determinant a0 . (a1 x a2).
    const float* a0 = model.m;
    const float* a1 = model.m + 4;
    const float* a2 = model.m + 8;
    Cross(a1, a2, out);
    Cross(a2, a0, out + 3);
    Cross(a0, a1, out + 6);
    const float determinant = a0[0] * out[0] + a0[1] * out[1] + a0[2] * out[2];
    if (!(std::abs(determinant) > 1e-30f) || !std::isfinite(determinant)) {
        std::fill(out, out + 9, 0.f);
        return;
    }
    for (int i = 0; i < 9; ++i) {
        out[i] /= determinant;
    }
}

void BuildBatches(const DrawItem* items, const VisibilityResult& visible, const engine_core::Matrix4& view,
                  DrawBatches& out) {
    out.runs.clear();
    out.instances.clear();
    out.opaqueRuns = 0;
    std::vector<DrawBatches::Entry>& order = out.order;
    order.clear();
    for (const VisibleDraw& draw : visible.opaque) {
        const DrawItem& item = items[draw.index];
        DrawBatches::Entry entry;
        entry.key = BatchKey(item.slot, draw.lod, Mirrored(*item.model));
        entry.depth = ViewDepth(view, *item.model);
        entry.index = draw.index;
        entry.lod = draw.lod;
        order.push_back(entry);
    }
    std::sort(order.begin(), order.end(), [](const DrawBatches::Entry& a, const DrawBatches::Entry& b) {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        if (a.depth != b.depth) {
            return a.depth > b.depth;  // nearer first, so depth testing rejects more
        }
        return a.index < b.index;
    });
    for (std::size_t i = 0; i < order.size(); ++i) {
        const DrawBatches::Entry& entry = order[i];
        const DrawItem& item = items[entry.index];
        const bool joins = i > 0 && item.slot != 0 && order[i - 1].key == entry.key;
        if (!joins) {
            DrawRun run;
            run.first = static_cast<int>(out.instances.size());
            run.draw = entry.index;
            run.lod = entry.lod;
            run.mirrored = (entry.key & 1u) != 0;
            out.runs.push_back(run);
        }
        ++out.runs.back().count;
        out.instances.push_back(MakeInstance(item));
    }
    out.opaqueRuns = static_cast<int>(out.runs.size());

    // See-through: farthest first by the Transform's depth, as the pass always sorted them.
    order.clear();
    for (const VisibleDraw& draw : visible.transparent) {
        DrawBatches::Entry entry;
        entry.depth = ViewDepth(view, *items[draw.index].model);
        entry.index = draw.index;
        entry.lod = draw.lod;
        order.push_back(entry);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const DrawBatches::Entry& a, const DrawBatches::Entry& b) { return a.depth < b.depth; });
    for (const DrawBatches::Entry& entry : order) {
        const DrawItem& item = items[entry.index];
        DrawRun run;
        run.first = static_cast<int>(out.instances.size());
        run.count = 1;
        run.draw = entry.index;
        run.lod = entry.lod;
        run.mirrored = Mirrored(*item.model);
        out.runs.push_back(run);
        out.instances.push_back(MakeInstance(item));
    }
}

}  // namespace runner
```

- [ ] **Step 4: Run the tests and see them pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[batches]"`
Expected: `All tests passed (… assertions in 9 test cases)`.

- [ ] **Step 5: Run the sandbox, then commit**

Run: `./build/sandbox`. Expected: no failures.

```bash
git add src/runner/DrawBatches.hpp src/runner/DrawBatches.cpp sandbox/draw_batches_tests.cpp CMakeLists.txt
git commit -m "Add DrawBatches: visible meshes sorted into instanced runs, with each instance's data

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Instanced draws on the GPU, checked alone first

**Files:**
- Modify: `src/runner/gl.hpp`, `src/runner/gl.cpp` (two entry points)
- Modify: `src/amesh/amesh.hpp`, `src/amesh/amesh_gl.cpp` (slot constants, `draw_instanced`)
- Create: `src/runner/InstanceBuffer.hpp`, `src/runner/InstanceBuffer.cpp`
- Modify: `CMakeLists.txt`: `src/runner/InstanceBuffer.cpp` goes in the studio sources, beside `src/runner/ShadowRenderer.cpp`. It needs GL, so not in `STUDIO_CORE_SOURCES`.
- Test: `tests/AmeshGlCheck.cpp`

**Interfaces:**
- Consumes: `InstanceData` (Task 5).
- Produces:
  - `anarchy::amesh::kAttribInstanceModel = 7`, `kAttribInstanceNormal = 11`, `kAttribInstanceTint = 14`
  - `void GpuMesh::draw_instanced(int lod, int count) const`
  - `class runner::InstanceBuffer { void upload(const InstanceData*, int); void attach(int first) const; void destroy(); }`

- [ ] **Step 1: Write the failing check**

In `tests/AmeshGlCheck.cpp`, add `#include "runner/InstanceBuffer.hpp"`. After `kFragmentShader`, add an instanced shader:

```cpp
// Instanced: each instance's model places the quad, its normal matrix's z
// column scales its tint, and the tint is the color.
const char* kInstancedVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 7) in mat4 aModel;
layout(location = 11) in mat3 aNormalMatrix;
layout(location = 14) in vec3 aTint;
out vec3 vColor;
void main() {
    vColor = aTint * (aNormalMatrix * vec3(0.0, 0.0, 1.0)).z;
    gl_Position = aModel * vec4(aPosition, 1.0);
}
)";
```

In `main`, after `glDeleteShader(fragment);`, build a second program the same way from `kInstancedVertexShader` and `kFragmentShader`, named `instanced`, and expect it links.

Inside the block, after `Expect(threw, "drawing a missing LOD throws");`, add:

```cpp
        // Three instances of the 0..1 quad, each moved into a quadrant of clip
        // space (a quadrant is 1 wide, as the quad is): bottom left red, bottom
        // right green, top left blue. Identity normal matrices pass the tint on.
        mesh.upload(data);
        runner::InstanceData rows[3] = {};
        const float corners[3][2] = {{-1.f, -1.f}, {0.f, -1.f}, {-1.f, 0.f}};
        for (int i = 0; i < 3; ++i) {
            rows[i].model[0] = rows[i].model[5] = rows[i].model[10] = rows[i].model[15] = 1.f;
            rows[i].model[12] = corners[i][0];
            rows[i].model[13] = corners[i][1];
            rows[i].normal[0] = rows[i].normal[4] = rows[i].normal[8] = 1.f;
            rows[i].tint[i] = 1.f;
        }
        runner::InstanceBuffer instances;
        instances.upload(rows, 3);
        glUseProgram(instanced);
        glClear(runner::GL_COLOR_BUFFER_BIT);
        mesh.bind();
        instances.attach(0);
        mesh.draw_instanced(0, 3);
        ExpectNoGlError("draw_instanced");
        const int q = kSize / 4;
        const Pixel bottomLeft = ReadPixel(q, q);
        const Pixel bottomRight = ReadPixel(3 * q, q);
        const Pixel topLeft = ReadPixel(q, 3 * q);
        const Pixel topRight = ReadPixel(3 * q, 3 * q);
        Expect(bottomLeft.r > 200 && bottomLeft.g < 50, "instance 0 draws red bottom left");
        Expect(bottomRight.g > 200 && bottomRight.r < 50, "instance 1 draws green bottom right");
        Expect(topLeft.b > 200 && topLeft.r < 50, "instance 2 draws blue top left");
        Expect(topRight.r == 0 && topRight.g == 0 && topRight.b == 0, "nothing draws top right");

        // attach(1) starts at the second row: two instances, green and blue.
        glClear(runner::GL_COLOR_BUFFER_BIT);
        instances.attach(1);
        mesh.draw_instanced(0, 2);
        Expect(ReadPixel(q, q).r == 0 && ReadPixel(3 * q, q).r > 200 && ReadPixel(q, 3 * q).g > 200,
               "attach(first) starts at row first");
        ExpectNoGlError("attach(1)");

        // A plain draw of the same mesh still works after an instanced one.
        glUseProgram(program);
        glClear(runner::GL_COLOR_BUFFER_BIT);
        mesh.bind();
        mesh.draw(0);
        Expect(Near(ReadPixel(kSize / 2, 0).r, 128), "a plain draw after an instanced one still draws");
        ExpectNoGlError("plain after instanced");

        bool instancedThrew = false;
        try {
            mesh.draw_instanced(1, 1);
        } catch (const std::out_of_range&) {
            instancedThrew = true;
        }
        Expect(instancedThrew, "draw_instanced with a missing LOD throws");
        mesh.draw_instanced(0, 0);
        ExpectNoGlError("draw_instanced of 0");
        instances.destroy();
        ExpectNoGlError("InstanceBuffer::destroy");
```

At the end of `main`, before `glfwDestroyWindow`, add `glDeleteProgram(instanced);`.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --target amesh-gl-check --parallel`
Expected: the build fails with `'runner/InstanceBuffer.hpp' file not found`.

- [ ] **Step 3: Load the entry points**

`gl.hpp`, after `rt_glDrawElements`'s declaration:

```cpp
extern void (*rt_glDrawElementsInstanced)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                          GLsizei instancecount);
extern void (*rt_glVertexAttribDivisor)(GLuint index, GLuint divisor);
```

Next to the `#define glDrawElements` line, add:

```cpp
#define glDrawElementsInstanced ::runner::rt_glDrawElementsInstanced
#define glVertexAttribDivisor ::runner::rt_glVertexAttribDivisor
```

`gl.cpp`, after `rt_glDrawElements`'s definition:

```cpp
void (*rt_glDrawElementsInstanced)(GLenum, GLsizei, GLenum, const void*, GLsizei) = nullptr;
void (*rt_glVertexAttribDivisor)(GLuint, GLuint) = nullptr;
```

After `LOAD(DrawElements);`:

```cpp
    LOAD(DrawElementsInstanced);
    LOAD(VertexAttribDivisor);
```

Both are core in 3.3, so a missing one fails `LoadGl` like any other.

- [ ] **Step 4: GpuMesh::draw_instanced**

`amesh.hpp`, after `kAttribWeight`:

```cpp
// Per-instance slots, divisor 1, which runner::InstanceBuffer::attach points
// at its rows: a mat4 world matrix (7 to 10), a mat3 normal matrix (11 to 13),
// and a linear RGB tint (14). upload never touches them.
inline constexpr unsigned kAttribInstanceModel = 7;
inline constexpr unsigned kAttribInstanceNormal = 11;
inline constexpr unsigned kAttribInstanceTint = 14;
```

In `GpuMesh`, after `draw_subset`:

```cpp
    // count instances of lod, with the mesh and its instance slots bound.
    // Nothing for count 0 or before an upload; throws std::out_of_range for a
    // missing LOD once uploaded, as draw does.
    void draw_instanced(int lod, int count) const;
```

and in the private section, after `draw_range`:

```cpp
    void draw_range_instanced(std::uint32_t tri_begin, std::uint32_t tri_count, int instances) const;
```

`amesh_gl.cpp`, after `GpuMesh::draw_subset`:

```cpp
void GpuMesh::draw_instanced(int lod, int count) const {
    if (!valid() || count <= 0) {
        return;
    }
    if (lod < 0 || static_cast<std::size_t>(lod) >= lods_.size()) {
        throw std::out_of_range("GpuMesh::draw_instanced: LOD " + std::to_string(lod) + " of " +
                                std::to_string(lods_.size()));
    }
    draw_range_instanced(lods_[static_cast<std::size_t>(lod)].tri_begin,
                         lods_[static_cast<std::size_t>(lod)].tri_count, count);
}
```

In the `#ifdef AE_MESH_NO_GL` section, after the empty `draw_range`:

```cpp
void GpuMesh::draw_range_instanced(std::uint32_t, std::uint32_t, int) const {}
```

In the GL section, after `draw_range`:

```cpp
void GpuMesh::draw_range_instanced(std::uint32_t tri_begin, std::uint32_t tri_count, int instances) const {
    if (tri_count == 0) {
        return;
    }
    const std::size_t first_index = std::size_t{tri_begin} * 3;
    glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(std::size_t{tri_count} * 3), GL_UNSIGNED_INT,
                            reinterpret_cast<const void*>(first_index * sizeof(std::uint32_t)),
                            static_cast<GLsizei>(instances));
}
```

- [ ] **Step 5: InstanceBuffer**

`src/runner/InstanceBuffer.hpp`:

```cpp
#pragma once

#include "DrawBatches.hpp"

namespace runner {

// One frame's InstanceData in a GL buffer, and the vertex slots that read it.
// Every call needs the GL context it was made in. destroy before that context goes.
class InstanceBuffer {
public:
    // Replaces the buffer's contents with count rows. Each upload gets new
    // storage (glBufferData, GL_STREAM_DRAW), so draws still reading the last
    // upload never stall the CPU. count 0 keeps what is there.
    void upload(const InstanceData* data, int count);
    // With a mesh's vertex array bound: enables slots 7 to 14 at divisor 1,
    // reading from row first on. GL 3.3 has no base instance, so each run attaches.
    void attach(int first) const;
    void destroy();

private:
    unsigned buffer_ = 0;
};

}  // namespace runner
```

`src/runner/InstanceBuffer.cpp`:

```cpp
#include "InstanceBuffer.hpp"

#include "amesh.hpp"
#include "gl.hpp"

#include <cstddef>

namespace runner {

void InstanceBuffer::upload(const InstanceData* data, int count) {
    if (count <= 0) {
        return;
    }
    if (buffer_ == 0) {
        glGenBuffers(1, &buffer_);
    }
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(InstanceData) * static_cast<std::size_t>(count)),
                 data, RT_GL_STREAM_DRAW);
}

void InstanceBuffer::attach(int first) const {
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    const auto stride = static_cast<GLsizei>(sizeof(InstanceData));
    const std::size_t base = sizeof(InstanceData) * static_cast<std::size_t>(first);
    const auto slot = [&](unsigned location, GLint size, std::size_t offset) {
        glEnableVertexAttribArray(location);
        glVertexAttribPointer(location, size, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(base + offset));
        glVertexAttribDivisor(location, 1);
    };
    for (unsigned column = 0; column < 4; ++column) {
        slot(anarchy::amesh::kAttribInstanceModel + column, 4, offsetof(InstanceData, model) + column * 16);
    }
    for (unsigned column = 0; column < 3; ++column) {
        slot(anarchy::amesh::kAttribInstanceNormal + column, 3, offsetof(InstanceData, normal) + column * 12);
    }
    slot(anarchy::amesh::kAttribInstanceTint, 3, offsetof(InstanceData, tint));
}

void InstanceBuffer::destroy() {
    if (buffer_ != 0) {
        glDeleteBuffers(1, &buffer_);
        buffer_ = 0;
    }
}

}  // namespace runner
```

If `GL_ARRAY_BUFFER`, `GL_FLOAT` or `GL_FALSE` are spelled differently in `gl.hpp` (`grep -n "ARRAY_BUFFER\|GL_FALSE" src/runner/gl.hpp`), use its names.

Add the source to CMake and run `cmake -S . -B build`.

- [ ] **Step 6: Run the checks**

```bash
cmake --build build --parallel
./build/amesh-gl-check; echo "exit $?"
./build/amesh-tests; echo "exit $?"
./build/scene-render-check --compare build/regression; echo "exit $?"
```

Expected: `GpuMesh checks passed` and exit 0. `amesh-tests` passes (the NO_GL build links `draw_range_instanced`). `scene render checks passed` and exit 0: nothing in the renderer uses the new path yet.

- [ ] **Step 7: Commit**

```bash
git add src/runner/gl.hpp src/runner/gl.cpp src/amesh/amesh.hpp src/amesh/amesh_gl.cpp src/runner/InstanceBuffer.hpp src/runner/InstanceBuffer.cpp tests/AmeshGlCheck.cpp CMakeLists.txt
git commit -m "Add instanced mesh draws: GpuMesh::draw_instanced and InstanceBuffer

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: The geometry and transparency passes draw instanced runs

**Files:**
- Modify: `src/runner/Renderer.hpp` (`MeshDraw::tint`, `MeshDraw::slot`; members)
- Modify: `src/runner/Renderer.cpp` (`findVisible`, `bindMaterial`, `geometryPass`, `transparencyPass`, `shutdown`)
- Modify: `resources/shaders/pipeline/geometry.vert`
- Test: `tests/SceneRenderCheck.cpp` (checks IN1–IN4)

**Interfaces:**
- Consumes: `BuildBatches`, `DrawBatches`, `DrawRun` (Task 5); `InstanceBuffer`, `GpuMesh::draw_instanced` (Task 6); `stats_`, `visibility_`, `drawItems_` (Task 4).
- Produces: `MeshDraw::tint` (`float[3]`, default white, as its Color3 holds it) and `MeshDraw::slot` (`std::uint32_t`, default 0), which GameView sets in Task 8.

- [ ] **Step 1: Write the failing checks**

In `tests/SceneRenderCheck.cpp`, after the CL1–CL3 block from Task 4, add:

```cpp
        {
            // IN1–IN4: instancing. From (0, 0, 8) down -Z, white cubes of one
            // slot, left tinted red and right green, draw as one run.
            runner::Renderer batcher;
            Expect(batcher.initialize(), "the renderer builds for instancing");
            batcher.setCamera(engine_core::matrix4_translation(0.f, 0.f, 8.f), 60.f);
            runner::MeshDraw pair[2] = {runner::MeshDraw{cube, engine_core::matrix4_translation(-1.5f, 0.f, 0.f)},
                                        runner::MeshDraw{cube, engine_core::matrix4_translation(1.5f, 0.f, 0.f)}};
            pair[0].slot = pair[1].slot = 1;
            pair[0].tint[1] = pair[0].tint[2] = 0.f;
            pair[1].tint[0] = pair[1].tint[2] = 0.f;
            batcher.draw(0, 0, kSize, kSize, kSize, kSize, pair, 2);
            const Pixel left = ReadPixel(fbWidth * 40 / kSize, fbHeight / 2);
            const Pixel right = ReadPixel(fbWidth * 88 / kSize, fbHeight / 2);
            Expect(batcher.stats().runs == 1 && batcher.stats().instancedCalls == 1,
                   "IN1 two cubes of one slot draw in one call (" + std::to_string(batcher.stats().runs) + " runs)");
            Expect(left.r > left.g + 30 && right.g > right.r + 30,
                   "IN2 each keeps its own tint (" + Text(left) + " and " + Text(right) + ")");

            // A mirrored cube draws the same as an unmirrored one, in a run of its own.
            runner::MeshDraw plain{cube, engine_core::matrix4_identity()};
            plain.slot = 1;
            batcher.draw(0, 0, kSize, kSize, kSize, kSize, &plain, 1);
            const Pixel unmirrored = ReadPixel(fbWidth / 2, fbHeight / 2);
            runner::MeshDraw both[2] = {plain, plain};
            both[1].model.m[0] = -1.f;
            both[0].model = engine_core::matrix4_translation(0.f, 0.f, -30.f);  // hidden behind it, same slot
            batcher.draw(0, 0, kSize, kSize, kSize, kSize, both, 2);
            const Pixel mirrored = ReadPixel(fbWidth / 2, fbHeight / 2);
            Expect(batcher.stats().runs == 2, "IN3 a mirrored cube draws in a run of its own");
            Expect(std::abs(Sum(mirrored) - Sum(unmirrored)) <= 3,
                   "IN3 and shows its outside (" + Text(mirrored) + " against " + Text(unmirrored) + ")");

            // Slot 0 never batches.
            const runner::MeshDraw loose[3] = {runner::MeshDraw{cube, engine_core::matrix4_translation(-1.5f, 0.f, 0.f)},
                                               runner::MeshDraw{cube, engine_core::matrix4_identity()},
                                               runner::MeshDraw{cube, engine_core::matrix4_translation(1.5f, 0.f, 0.f)}};
            batcher.draw(0, 0, kSize, kSize, kSize, kSize, loose, 3);
            Expect(batcher.stats().runs == 3, "IN4 slot 0 draws each alone");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "instancing leaves no GL error");
            batcher.shutdown();
        }
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target scene-render-check --parallel`
Expected: the build fails: `no member named 'slot' in 'runner::MeshDraw'`.

- [ ] **Step 3: MeshDraw's tint and slot, and the renderer's members**

In `Renderer.hpp`, change the `color` comment and add two fields after `owner`:

```cpp
    // RGBA, 0 to 1, as the Material's Color3 holds it (sRGB). Alpha is unused.
    // The GameObject's Color is tint, not multiplied in here.
    float color[4] = {1.f, 1.f, 1.f, 1.f};
```

```cpp
    // The GameObject's Color, as its Color3 holds it (sRGB). It multiplies color.
    float tint[3] = {1.f, 1.f, 1.f};
    // Which Prefab Model it draws, numbered from 1 each frame. Draws with the
    // same slot share a mesh and every Material value, and draw as one
    // instanced call. 0 draws alone.
    std::uint32_t slot = 0;
```

Add `#include "DrawBatches.hpp"` and `#include "InstanceBuffer.hpp"`. Replace the members `std::vector<int> transparent_;` and `std::vector<float> transparentDepth_;` with:

```cpp
    DrawBatches batches_;
    InstanceBuffer instances_;
```

Change `bool geometryPass(const MeshDraw* meshes, int count, const float* projection);` to `bool geometryPass(const MeshDraw* meshes, const float* projection);`, and `transparencyPass`'s declaration to `bool transparencyPass(const MeshDraw* meshes, const float* projection, const float* inverseProjection);`.

- [ ] **Step 4: Build the batches and draw them**

In `findVisible`, set the two new item fields with the others:

```cpp
        item.tint = draw.tint;
        item.slot = draw.slot;
```

and after `FindVisible(…)`:

```cpp
    BuildBatches(drawItems_.data(), visibility_, view_, batches_);
```

In `bindMaterial`, delete `glUniformMatrix4fv(program.model, 1, GL_FALSE, draw.model.m);`.

Replace `geometryPass`:

```cpp
bool Renderer::geometryPass(const MeshDraw* meshes, const float* projection) {
    RENDER_PASS("Geometry");
    glViewport(0, 0, targetWidth_, targetHeight_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, gbufferFbo_);
    glDisable(GL_BLEND);
    glEnable(RT_GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RT_GL_LESS);
    glDepthMask(GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(geometry_.id);
    glUniformMatrix4fv(geometry_.view, 1, GL_FALSE, view_.m);
    glUniformMatrix4fv(geometry_.projection, 1, GL_FALSE, projection);
    // Every instance of the frame, opaque and see-through, in one upload.
    instances_.upload(batches_.instances.data(), static_cast<int>(batches_.instances.size()));
    bool asked = false;
    for (int index = 0; index < batches_.opaqueRuns; ++index) {
        const DrawRun& run = batches_.runs[static_cast<std::size_t>(index)];
        const MeshDraw& draw = meshes[run.draw];
        bindMaterial(geometry_, draw);
        glCullFace(run.mirrored ? RT_GL_FRONT : RT_GL_BACK);
        draw.mesh->bind();
        instances_.attach(run.first);
        if (!asked && !CanDraw(geometry_.id)) {
            return false;
        }
        asked = true;
        draw.mesh->draw_instanced(run.lod, run.count);
        ++stats_.runs;
        ++stats_.instancedCalls;
    }
    glDisable(RT_GL_CULL_FACE);
    glCullFace(RT_GL_BACK);
    return true;
}
```

In `transparencyPass`, change the signature to drop `count`. Replace the early return `if (transparent_.empty())` with `if (batches_.opaqueRuns == static_cast<int>(batches_.runs.size()))`, and delete the depth-sort block (`transparentDepth_` and the `std::stable_sort`): `BuildBatches` sorts them now. Replace the draw loop with:

```cpp
    glEnable(RT_GL_CULL_FACE);
    bool asked = false;
    for (std::size_t index = static_cast<std::size_t>(batches_.opaqueRuns); index < batches_.runs.size(); ++index) {
        const DrawRun& run = batches_.runs[index];
        const MeshDraw& draw = meshes[run.draw];
        bindMaterial(forward_, draw);
        glCullFace(run.mirrored ? RT_GL_FRONT : RT_GL_BACK);
        draw.mesh->bind();
        instances_.attach(run.first);
        if (!asked && !CanDraw(forward_.id)) {
            return false;
        }
        asked = true;
        draw.mesh->draw_instanced(run.lod, run.count);
        ++stats_.instancedCalls;
    }
```

Update the two calls in `draw`: `geometryPass(meshes, projection)` and `transparencyPass(meshes, projection, inverseProjection.m)`.

In `Renderer::shutdown`, after `shadows_.shutdown();`, add `instances_.destroy();`.

`CullBackFaces` is now unused by these passes. If nothing else calls it (`grep -n CullBackFaces src/runner/Renderer.cpp`), delete it.

- [ ] **Step 5: geometry.vert reads the instance slots**

Replace `resources/shaders/pipeline/geometry.vert` with:

```glsl
#version 330 core
// An AMESH vertex (amesh.hpp's GpuMesh locations) at its instance's world
// matrix, passed on in view space. The G-buffer pass and the transparency
// pass both draw with it (the legacy deferred.vert and forward.vert). Each
// instance's matrices and tint come from runner::InstanceBuffer, slots 7 to 14.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 4) in vec4 aColor;
layout (location = 7) in mat4 aModel;
// The inverse transpose of aModel's 3 by 3, made once per instance on the CPU,
// so normals stay square to the surface under a non-uniform scale.
layout (location = 11) in mat3 aNormalMatrix;
// The GameObject's Color, already linear, so it multiplies like a vertex color.
layout (location = 14) in vec3 aTint;

uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vViewPosition;
out vec3 vViewNormal;
out vec2 vUv;
out vec4 vColor;

void main() {
    vec4 viewPosition = uView * (aModel * vec4(aPosition, 1.0));
    // The view has no scale, so its rotation carries normals on.
    vViewNormal = mat3(uView) * (aNormalMatrix * aNormal);
    vViewPosition = viewPosition.xyz;
    vUv = aUv;
    vColor = aColor * vec4(aTint, 1.0);
    gl_Position = uProjection * viewPosition;
}
```

The studio reads shaders from the bundle, so run `cmake --build build --target bundle-resources` before any studio check. `scene-render-check` reads `resources/shaders` from the repository root.

- [ ] **Step 6: Run the checks**

```bash
cmake --build build --parallel
./build/scene-render-check --compare build/regression; echo "exit $?"
./build/sandbox
```

Expected: `scene render checks passed` and exit 0, with IN1–IN4, CL1–CL4 and the regression frames passing.

If the regression frames differ by 2 or more, find out why before loosening anything. Write both frames to PNG and look at where they differ. Likely causes:
- a tint not made linear;
- a normal matrix in the wrong order (columns, not rows);
- cull face swapped for mirrored runs.

- [ ] **Step 7: Commit**

```bash
git add src/runner/Renderer.hpp src/runner/Renderer.cpp resources/shaders/pipeline/geometry.vert tests/SceneRenderCheck.cpp
git commit -m "Draw opaque and see-through meshes as instanced runs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: GameView numbers each Prefab Model's slot and keeps the tint apart

**Files:**
- Modify: `src/runner/GameView.cpp` (`collectMeshes`: the `prefabMeshes_` loop and the per-row loop)

**Interfaces:**
- Consumes: `MeshDraw::slot`, `MeshDraw::tint` (Task 7).

- [ ] **Step 1: Number the slots**

In `collectMeshes`, just before `for (std::size_t index = 0; index < snapshot.prefabs.size(); ++index) {`, add:

```cpp
    // Each Prefab Model its own slot, from 1, so every GameObject drawing it
    // shares one instanced call. Numbered again each frame, as the list is made again.
    std::uint32_t nextSlot = 1;
```

Before `loaded.push_back(draw);`, add `draw.slot = nextSlot++;`.

- [ ] **Step 2: Keep the GameObject's Color as tint**

In the per-row loop, replace

```cpp
            draw.color[0] *= row.color.r;
            draw.color[1] *= row.color.g;
            draw.color[2] *= row.color.b;
```

with

```cpp
            draw.tint[0] = row.color.r;
            draw.tint[1] = row.color.g;
            draw.tint[2] = row.color.b;
```

Update the comment above: `// The GameObject's Color tints each Material's, per instance, and its opacity multiplies each Material's.`

- [ ] **Step 3: Build and run every check**

```bash
cmake --build build --target bundle-resources --parallel && cmake --build build --parallel
./build/sandbox && ./build/scene-render-check --compare build/regression && ./build/amesh-gl-check
(cd build && ctest --output-on-failure)
```

Expected: all pass. `ctest` runs `studio-tests`, which includes `BillboardLayerTest`: it draws a GameView from a test snapshot.

- [ ] **Step 4: See it in the studio**

Open a fresh scratch copy of Mitsuba (as in Task 1 Step 3) with this build. Run the stress script. Screenshot the Overview pose. Then check through `run_lua` that GameObjects of one Prefab show different Colors: set one GameObject's Color to bright red and screenshot again. Send both screenshots to the user. Quit that studio by pid.

- [ ] **Step 5: Commit**

```bash
git add src/runner/GameView.cpp
git commit -m "Give each Prefab Model a slot, so its GameObjects draw in one instanced call

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Shadow casters draw as instanced runs

**Files:**
- Modify: `src/runner/ShadowRenderer.hpp`, `src/runner/ShadowRenderer.cpp`
- Modify: `resources/shaders/pipeline/shadow.vert`
- Modify: `src/runner/Renderer.cpp` (`shadowPass` adds the shadow calls to the stats)
- Test: `tests/SceneRenderCheck.cpp` (check SH1)

**Interfaces:**
- Consumes: `InstanceBuffer`, `InstanceData` (Tasks 5–6); `GpuMesh::draw_instanced`.
- Produces: `int ShadowRenderer::calls() const`, the instanced calls this frame's `draw` and `drawSun` made.

- [ ] **Step 1: Write the failing check**

In the spot shadow checks of `tests/SceneRenderCheck.cpp`, just after `spotShadowed` is measured and checked, add:

```cpp
            // SH1: the spot's one tile draws its casters, all one mesh, in one call,
            // and a frame that reuses the tile draws none. Visible: the cube and the
            // floor, two runs of slot 0.
            runner::LightDraw cachedSpot = spotLight;
            cachedSpot.id = 31;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2, &cachedSpot, 1);
            const int firstCalls = renderer.stats().instancedCalls;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2, &cachedSpot, 1);
            const int reusedCalls = renderer.stats().instancedCalls;
            Expect(firstCalls == 3 && reusedCalls == 2,
                   "SH1 one shadow call for the tile, none when it is reused (" + std::to_string(firstCalls) +
                       " then " + std::to_string(reusedCalls) + ")");
```

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build --target scene-render-check --parallel && ./build/scene-render-check; echo "exit $?"
```

Expected: `FAIL SH1 … (2 then 2)`, because shadow draws are not counted yet, and exit 1.

- [ ] **Step 3: Group casters into runs**

`ShadowRenderer.hpp`: add `#include "InstanceBuffer.hpp"` and `#include <utility>`, and beside `struct MeshDraw;` forward-declare `namespace anarchy::amesh { class GpuMesh; }` (outside `namespace runner`). In the public section, after `atlasPages()`:

```cpp
    // Instanced calls this frame's draw and drawSun made; draw starts the count over.
    int calls() const { return calls_; }
```

Remove `int model = -1;` from `DepthProgram`. Replace `drawCasters`'s declaration with:

```cpp
    // Adds casters' runs, grouped by mesh, to casterRuns_ and their rows to
    // casterRows_; returns the first run's index. Their end is casterRuns_.size().
    int addCasterRuns(const std::vector<int>& casters, const MeshDraw* meshes);
    // Draws runs first to end (indices into casterRuns_) seen through viewProjection.
    bool drawCasters(const engine_core::Matrix4& viewProjection, int first, int end, bool& asked);
```

Add members:

```cpp
    // The casters of every tile or cascade drawn this frame, as instanced runs.
    struct CasterRun {
        const anarchy::amesh::GpuMesh* mesh = nullptr;
        int first = 0;
        int count = 0;
    };
    std::vector<CasterRun> casterRuns_;
    std::vector<InstanceData> casterRows_;
    std::vector<int> casterOrder_;
    // Each tile's or cascade's runs, first to end in casterRuns_.
    std::vector<std::pair<int, int>> tileRuns_;
    InstanceBuffer instances_;
    int calls_ = 0;
```

`ShadowRenderer.cpp`: in `initialize`, delete the line that reads `uModel` (`depth_.model = glGetUniformLocation(depth_.id, "uModel");`). In `shutdown`, add `instances_.destroy();`.

Replace `drawCasters` with:

```cpp
int ShadowRenderer::addCasterRuns(const std::vector<int>& casters, const MeshDraw* meshes) {
    const int begin = static_cast<int>(casterRuns_.size());
    casterOrder_.assign(casters.begin(), casters.end());
    const auto meshOf = [&](int caster) { return meshes[casterMeshes_[static_cast<std::size_t>(caster)]].mesh; };
    std::stable_sort(casterOrder_.begin(), casterOrder_.end(),
                     [&](int a, int b) { return std::less<const void*>()(meshOf(a), meshOf(b)); });
    for (const int caster : casterOrder_) {
        const MeshDraw& draw = meshes[casterMeshes_[static_cast<std::size_t>(caster)]];
        if (static_cast<int>(casterRuns_.size()) == begin || casterRuns_.back().mesh != draw.mesh) {
            casterRuns_.push_back(CasterRun{draw.mesh, static_cast<int>(casterRows_.size()), 0});
        }
        ++casterRuns_.back().count;
        // Depth only reads the world matrix.
        InstanceData row{};
        std::copy(draw.model.m, draw.model.m + 16, row.model);
        casterRows_.push_back(row);
    }
    return begin;
}

bool ShadowRenderer::drawCasters(const Matrix4& viewProjection, int first, int end, bool& asked) {
    glUseProgram(depth_.id);
    glUniformMatrix4fv(depth_.viewProjection, 1, GL_FALSE, viewProjection.m);
    for (int index = first; index < end; ++index) {
        const CasterRun& run = casterRuns_[static_cast<std::size_t>(index)];
        run.mesh->bind();
        instances_.attach(run.first);
        if (!asked && !CanDraw(depth_.id)) {
            return false;
        }
        asked = true;
        run.mesh->draw_instanced(0, run.count);
        ++calls_;
    }
    return true;
}
```

Add `#include <functional>` for `std::less`.

In `draw`, add `calls_ = 0;` as its first line, before the `refused_` check. Replace the tile loop's body so that all tiles' runs are built and uploaded first:

```cpp
    if (!plan.draws.empty()) {
        // Each page's tiles together, so each page is attached once.
        std::vector<const TileDraw*> byPage;
        byPage.reserve(plan.draws.size());
        for (const TileDraw& tile : plan.draws) {
            byPage.push_back(&tile);
        }
        std::stable_sort(byPage.begin(), byPage.end(),
                         [](const TileDraw* a, const TileDraw* b) { return a->tile.page < b->tile.page; });
        // Every tile's casters as runs, uploaded once.
        casterRuns_.clear();
        casterRows_.clear();
        tileRuns_.clear();
        for (const TileDraw* draw : byPage) {
            const int first = addCasterRuns(draw->casters, meshes);
            tileRuns_.emplace_back(first, static_cast<int>(casterRuns_.size()));
        }
        instances_.upload(casterRows_.data(), static_cast<int>(casterRows_.size()));
        glBindFramebuffer(RT_GL_FRAMEBUFFER, atlasFbo_);
        begin();
        bool asked = false;
        int attached = -1;
        for (std::size_t t = 0; t < byPage.size(); ++t) {
            const TileDraw& tile = *byPage[t];
            if (tile.tile.page != attached) {
                glFramebufferTextureLayer(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, atlas_, 0, tile.tile.page);
                attached = tile.tile.page;
            }
            glViewport(tile.tile.x, tile.tile.y, tile.tile.size, tile.tile.size);
            glScissor(tile.tile.x, tile.tile.y, tile.tile.size, tile.tile.size);
            glClear(GL_DEPTH_BUFFER_BIT);
            if (!drawCasters(tile.viewProjection, tileRuns_[t].first, tileRuns_[t].second, asked)) {
                end();
                return false;
            }
        }
        end();
    }
```

In `drawSun`, after `planner_.forgetCascades();` and before binding `cascadeFbo_`, build its runs the same way:

```cpp
    casterRuns_.clear();
    casterRows_.clear();
    tileRuns_.clear();
    for (const CascadeDraw& draw : draws) {
        const int first = addCasterRuns(draw.casters, meshes);
        tileRuns_.emplace_back(first, static_cast<int>(casterRuns_.size()));
    }
    instances_.upload(casterRows_.data(), static_cast<int>(casterRows_.size()));
```

Change its loop to index the cascades (`for (std::size_t c = 0; c < draws.size(); ++c) { const CascadeDraw& draw = draws[c]; …`), and call `drawCasters(draw.viewProjection, tileRuns_[c].first, tileRuns_[c].second, asked)`.

- [ ] **Step 4: shadow.vert reads the instance's matrix**

Replace `resources/shaders/pipeline/shadow.vert` with:

```glsl
#version 330 core
// A caster in a shadow map: a SpotLight's tile, a PointLight's cube-face
// tile, or a DirectionalLight's cascade. Only depth is drawn. Each caster's
// world matrix comes from runner::InstanceBuffer, slots 7 to 10.
layout (location = 0) in vec3 aPosition;
layout (location = 7) in mat4 aModel;

uniform mat4 uViewProjection;

void main() {
    gl_Position = uViewProjection * (aModel * vec4(aPosition, 1.0));
}
```

- [ ] **Step 5: Count shadow calls in the stats**

In `Renderer::shadowPass`, after the `drawSun` call succeeds, add `stats_.instancedCalls += shadows_.calls();`.

- [ ] **Step 6: Run the checks**

```bash
cmake --build build --target bundle-resources --parallel && cmake --build build --parallel
./build/scene-render-check --compare build/regression; echo "exit $?"
./build/sandbox && (cd build && ctest --output-on-failure)
```

Expected: `scene render checks passed` and exit 0, with SH1 and every existing shadow check passing (spot, point, paged atlas, cascades, caching). The regression frames match within 1. `ctest` passes.

- [ ] **Step 7: Commit**

```bash
git add src/runner/ShadowRenderer.hpp src/runner/ShadowRenderer.cpp src/runner/Renderer.cpp resources/shaders/pipeline/shadow.vert tests/SceneRenderCheck.cpp
git commit -m "Draw shadow casters as instanced runs, one upload per pass

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Measure the stress place, and write the results into the spec

**Files:**
- Modify: `docs/superpowers/specs/2026-10-05-culling-instancing-design.md` (a "Results" section at the end)
- Modify: `src/runner/GameView.cpp` (stats printed when `ANARCHY_RENDER_STATS` is set)

- [ ] **Step 1: Build and open the stress place**

```bash
cmake --build build --target bundle-resources --parallel && cmake --build build --parallel
```

Open a fresh scratch copy of Mitsuba with this build, select it by pid, and run `scripts/stress_place.lua`, exactly as in Task 1 Step 3.

- [ ] **Step 2: Measure at the two poses**

Use the Overview and Corner poses from Task 1 Step 4, and record the same scopes with GPU detail on and off.

Also record `Renderer::stats()`. The profiler has no counters, so `GameView` prints them when asked. In `GameView::renderContent`, just after the `renderer_.draw(…)` call, add:

```cpp
        // ANARCHY_RENDER_STATS set prints the draw's counts once a second, for measuring culling and instancing.
        static const bool printStats = std::getenv("ANARCHY_RENDER_STATS") != nullptr;
        if (printStats) {
            static auto printed = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - printed >= std::chrono::seconds(1)) {
                printed = now;
                const RenderStats& stats = renderer_.stats();
                std::fprintf(stderr, "render stats: %d draws, %d visible, %d culled, %d runs, %d instanced calls\n",
                             stats.draws, stats.visible, stats.culled, stats.runs, stats.instancedCalls);
            }
        }
```

Add `<chrono>`, `<cstdio>` and `<cstdlib>` to GameView.cpp's includes if they are missing. Build, then open the copy with `open -n --env ANARCHY_RENDER_STATS=1 --stderr <scratchpad>/stats.log build/AnarchyStudio.app --args <copy>`, and read the log at each pose. The baseline's draw calls are its `draws`, since it drew every one.

Targets:
- **Overview**: runs in the dozens (about 3 shared slots × Models per Prefab, plus 50 one-off slots, plus the floor and the place's own objects). The GPU Geometry and the CPU Scene View times fall well below the baseline.
- **Corner**: `culled` above 80% of `draws`. The CPU `Visibility` scope well under 1 ms.

If a target is missed, report the numbers as they are. Do not tune anything in this task.

- [ ] **Step 3: Screenshots**

Screenshot both poses. Compare them by eye with Task 1's baseline screenshots: the same scene, with no missing objects, no wrong colors and no missing shadows. Send all four screenshots and the before-and-after table to the user.

- [ ] **Step 4: Write the results into the spec**

Append a section to the spec:

```markdown
## Results

Measured on <machine>, <date>, with the stress place (`scripts/stress_place.lua`) in a copy of Mitsuba.

| Pose | Measure | Before | After |
| --- | --- | --- | --- |
| Overview | mesh draw calls | … | … |
| Overview | GPU Geometry (ms) | … | … |
| Overview | GPU Shadows (ms) | … | … |
| Overview | CPU Scene View (ms) | … | … |
| Corner | visible / culled of draws | all / 0 | … / … |
| Corner | CPU Visibility (ms) | — | … |
```

Fill in every cell from Steps 2 and 3 and Task 1 Step 4. Also correct anything in the spec that shipped differently (for example, the attach call count or the shadow upload count).

- [ ] **Step 5: Commit, and quit the studio**

```bash
git add docs/superpowers/specs/2026-10-05-culling-instancing-design.md src/runner/GameView.cpp
git commit -m "Record the culling and instancing measurements, and print render stats on request

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Quit the scratch studio by pid.
