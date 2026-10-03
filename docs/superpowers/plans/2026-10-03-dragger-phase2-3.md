# Dragger Phases 2–3: Viewport, Drag Math, and the Dragger

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `Dragger` that scripts can make and that drags its PVInstance from mouse input, with events and undo, fully tested. Nothing is drawn yet (phase 4).

**Architecture:** `Camera.ViewportSize` gives the simulation thread the view's size. `DraggerMath` is pure geometry: rays, picking, and drag offsets. `Dragger` is an instance with `Adornee`, `Space`, `Increment`, `Dragging`, and three events. `DraggerWorld`, owned by the DataModel state, is UserInputService's dispatch filter: once per dispatch it closes finished drags' undo steps, then reads each record to hover, begin, move, and end drags, marking what it uses processed.

**Tech Stack:** C++17, flecs, Luau, Catch2.

**Spec:** `docs/superpowers/specs/2026-10-03-dragger-design.md` (sections 2–5)

## Global Constraints

- Build `cmake --build build --parallel`; sandbox `./build/sandbox "[TAG]"`; all `./build/sandbox` and `(cd build && ctest -C Release)`.
- Geometry matches `runner::Perspective`: column-major Matrix4, right-handed, Y up, the camera looks down its -Z (forward = -column 2), vertical field of view, near 0.1, far 1000.
- Screen points are in points from the view's top-left, y down.
- A drag's offset is always total since the drag began.
- No new warnings in touched files.

## Review Focus

- A drag whose target is destroyed mid-drag, or whose Dragger is: the drag ends with DragEnded, the undo step closes, nothing writes to a dead id (DR14).
- The mouse ray going parallel to the axis or plane mid-drag: the target stays where the last good offset put it (DR8, and in the world through `drag_offset` nullopt).
- An undo step opened by a drag in edit mode when the place starts playing before the next dispatch: `start_simulation` seals edit recordings, so the drag's recording must not be finished twice or leak into play.
- Two Draggers bound to the same target: only one drag at a time; the other Dragger stays inactive for input during it.
- `ViewportSize` of 0 (no view yet): input passes through, no division by zero.

---

## Phase 2

### Task 1: Camera.ViewportSize

**Files:** `src/engine_instances/Camera.{hpp,cpp}`, `src/runner/GameView.cpp` (writes it), `sandbox/dragger_tests.cpp` (new; add to CMake sandbox sources).

**Produces:** `Vec2 Camera::viewport_size() const;`, `void Camera::set_viewport_size(Vec2);`.

- [ ] **Test VP1** in `sandbox/dragger_tests.cpp`:

```cpp
TEST_CASE("VP1 Camera.ViewportSize is read-only to scripts, unsaved, and not undone", "[VP1]") {
    ScriptRig rig;
    engine_core::Camera& camera = rig.game.create<engine_core::Camera>();
    rig.game.set_parent(camera.id(), rig.game.scene_service("Workspace"));
    rig.game.history().reset_waypoints();
    camera.set_viewport_size(engine_core::Vec2{800.f, 600.f});
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    rig.runtime.run_chunk(R"(
        local camera
        for _, child in workspace:GetChildren() do
            if child.ClassName == "Camera" then camera = child end
        end
        print("size", camera.ViewportSize.X, camera.ViewportSize.Y)
        print("refused", not pcall(function() camera.ViewportSize = Vector2.new(1, 1) end))
    )");
    rig.frames(1);
    const auto output = rig.runtime.drain_output();
    REQUIRE(has_line(output, "size\t800\t600\n"));
    REQUIRE(has_line(output, "refused\ttrue\n"));
}
```

- [ ] **Implement:** Camera gains `Vec2 viewport_size_{}` and the accessors (SimulationThread; no history, no Changed beyond `emit_property("ViewportSize")`, no authored dirty). Register `lua_property("ViewportSize", "Vector2", false, read_viewport_size, nullptr)` beside FieldOfView. Reset to (0,0) in the Camera's reuse path. In `GameView`, when the view's size changes and in `noteCurrentCamera`, post through `engine_->on_simulation` a write of `(width, height)` in points to the configured camera, if it is a live Camera.
- [ ] Run `[VP1]`, the suite; commit `Give Camera a ViewportSize, written by the scene view`.

### Task 2: DraggerMath

**Files:** `src/engine_core/DraggerMath.{hpp,cpp}` (new; add to the `studio_core` sources beside PhysicsWorld), `sandbox/dragger_tests.cpp`.

**Produces** (namespace `engine_core`):

```cpp
enum class DraggerHandle { None = -1, X = 0, Y = 1, Z = 2, XY = 3, YZ = 4, XZ = 5 };
struct DraggerFrame { Vec3 origin; Vec3 axes[3]; };
struct DraggerView { Matrix4 camera; float fov_degrees = 70.f; Vec2 size; };
struct DraggerRay { Vec3 origin; Vec3 direction; };
struct DragStart { DraggerFrame frame; DraggerHandle handle = DraggerHandle::None; Vec3 hit; float along = 0.f; };

constexpr float kArrowPixels = 100.f;
constexpr float kPickPixels = 8.f;
constexpr float kPlaneNear = 0.25f;  // plane squares span these fractions of the arrow
constexpr float kPlaneFar = 0.40f;

DraggerRay viewport_ray(const DraggerView& view, Vec2 point);
// The screen point of a world point; false when it is behind the near plane.
bool project_point(const DraggerView& view, Vec3 point, Vec2& out);
float handle_scale(const DraggerView& view, Vec3 origin);  // world length of one pixel at origin
bool handle_visible(const DraggerFrame& frame, const DraggerView& view, DraggerHandle handle);
DraggerHandle pick_handle(const DraggerFrame& frame, const DraggerView& view, Vec2 point, float* depth);
bool begin_drag(const DraggerFrame& frame, const DraggerView& view, Vec2 point, DraggerHandle handle, DragStart& out);
std::optional<Vec3> drag_offset(const DragStart& start, const DraggerView& view, Vec2 point, double increment);
// The frame a Dragger draws at: world axes, or the target's rotation columns normalized.
DraggerFrame dragger_frame(const Matrix4& target, bool local);
```

Math (all vectors normalized where a direction is meant):
- Camera axes from `view.camera`: right = column 0, up = column 1, forward = -column 2, eye = column 3. `tan_half = tan(fov/2)`, `aspect = size.x / size.y`.
- `viewport_ray`: `ndc = (2 px / size.x - 1, 1 - 2 py / size.y)`; direction = normalize(right·ndc.x·tan_half·aspect + up·ndc.y·tan_half + forward).
- `project_point`: v = p - eye; z = dot(v, forward); z ≤ 0.1 → false; ndc.x = dot(v,right)/(z·tan_half·aspect), ndc.y = dot(v,up)/(z·tan_half); px = (ndc.x+1)/2·size.x, py = (1-ndc.y)/2·size.y.
- `handle_scale`: z = max(dot(origin-eye, forward), 0.1); return 2·z·tan_half / size.y. Arrow length L = kArrowPixels × that.
- `handle_visible`: d = normalize(origin - eye). Arrow i: |dot(axis i, d)| ≤ 0.98. Plane: XY normal = axis 2, YZ normal = axis 0, XZ normal = axis 1; visible when |dot(normal, d)| ≥ 0.1. Plane spans: XY (0,1), YZ (1,2), XZ (0,2).
- `pick_handle`: planes first: for each visible plane, ray–plane (through origin, its normal): denom = dot(dir, n); skip |denom| < 1e-6; t = dot(origin - ray.origin, n)/denom; skip t ≤ 0; hit; u = dot(hit - origin, a1)/L, v = dot(hit - origin, a2)/L; inside when both in [kPlaneNear, kPlaneFar]; keep the smallest t. If any, return it with depth t. Otherwise arrows: for each visible axis project origin and tip (origin + axis·L); both must project; distance from point to the segment in pixels; within kPickPixels keeps the smallest distance; depth = |closest world point - eye|. None → None.
- `begin_drag`: arrow: closest points between the ray and the axis line through origin: w0 = origin - ray.origin; b = dot(axis, dir); dd = dot(axis, w0); e = dot(dir, w0); denom = 1 - b²; |denom| < 1e-6 → false; s = (b·e - dd)/denom; along = s; hit = origin + axis·s. Plane: the ray–plane hit (as in pick; false when parallel or behind). Returns true with `out` filled.
- `drag_offset`: arrow: the same closest-point solve with `start.frame.origin`; also ray t = (e - b·dd)/denom must be > 0 and ≤ 1000, else nullopt; delta = s - along. Plane: hit as above, nullopt when parallel, t ≤ 0, or t > 1000; raw = hit - start.hit; components c1 = dot(raw, a1), c2 = dot(raw, a2). Snap: `increment > 0` rounds each component (delta, or c1 and c2) to a multiple of increment. Arrow offset = axis·delta; plane offset = a1·c1 + a2·c2.

- [ ] **Tests DR1–DR8** (camera at the identity looking down -Z, fov 90 so tan_half = 1):

```cpp
namespace {
engine_core::DraggerView view_of(float width, float height) {
    engine_core::DraggerView view;
    view.camera = engine_core::matrix4_identity();
    view.fov_degrees = 90.f;
    view.size = engine_core::Vec2{width, height};
    return view;
}
engine_core::DraggerFrame world_frame(engine_core::Vec3 origin) {
    return engine_core::dragger_frame(engine_core::matrix4_translation(origin.x, origin.y, origin.z), false);
}
bool close(float a, float b, float tolerance = 1e-3f) { return std::abs(a - b) <= tolerance; }
}  // namespace

TEST_CASE("DR1 the center ray is the look, and a corner ray is the frustum's corner", "[DR1]") {
    const auto view = view_of(200, 100);
    const auto center = engine_core::viewport_ray(view, {100, 50});
    REQUIRE(close(center.direction.x, 0) && close(center.direction.y, 0) && close(center.direction.z, -1));
    // aspect 2, tan_half 1: the top-left corner is (-2, 1, -1), normalized.
    const auto corner = engine_core::viewport_ray(view, {0, 0});
    const float n = std::sqrt(6.f);
    REQUIRE(close(corner.direction.x, -2 / n) && close(corner.direction.y, 1 / n) && close(corner.direction.z, -1 / n));
}

TEST_CASE("DR2 an arrow is the same number of pixels long near and far", "[DR2]") {
    const auto view = view_of(200, 200);
    for (float depth : {5.f, 500.f}) {
        const engine_core::Vec3 origin{0, 0, -depth};
        const float length = engine_core::kArrowPixels * engine_core::handle_scale(view, origin);
        engine_core::Vec2 a, b;
        REQUIRE(engine_core::project_point(view, origin, a));
        REQUIRE(engine_core::project_point(view, {length, 0, -depth}, b));
        REQUIRE(close(b.x - a.x, engine_core::kArrowPixels, 1e-2f));
    }
}

// At (0, 0, -10) in a 200 x 200 view, one pixel is 0.1 studs: the X arrow runs from
// screen (100, 100) to (200, 100), and the XY square covers x 125..140, y 60..75.
TEST_CASE("DR3 an arrow picks within 8 pixels, and a plane square picks inside itself", "[DR3]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    REQUIRE(engine_core::pick_handle(frame, view, {150, 108}, nullptr) == engine_core::DraggerHandle::X);
    REQUIRE(engine_core::pick_handle(frame, view, {150, 109}, nullptr) == engine_core::DraggerHandle::None);
    REQUIRE(engine_core::pick_handle(frame, view, {100, 50}, nullptr) == engine_core::DraggerHandle::Y);
    REQUIRE(engine_core::pick_handle(frame, view, {130, 70}, nullptr) == engine_core::DraggerHandle::XY);
}

TEST_CASE("DR4 an arrow at the camera and a plane seen edge-on are hidden", "[DR4]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    REQUIRE_FALSE(engine_core::handle_visible(frame, view, engine_core::DraggerHandle::Z));
    REQUIRE_FALSE(engine_core::handle_visible(frame, view, engine_core::DraggerHandle::YZ));
    REQUIRE_FALSE(engine_core::handle_visible(frame, view, engine_core::DraggerHandle::XZ));
    REQUIRE(engine_core::handle_visible(frame, view, engine_core::DraggerHandle::XY));
    REQUIRE(engine_core::pick_handle(frame, view, {100, 100}, nullptr) != engine_core::DraggerHandle::Z);
}

TEST_CASE("DR5 an axis drag stays on its axis, wherever the arrow was grabbed", "[DR5]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    engine_core::DragStart start;
    REQUIRE(engine_core::begin_drag(frame, view, {150, 100}, engine_core::DraggerHandle::X, start));
    auto offset = engine_core::drag_offset(start, view, {170, 90}, 0);
    REQUIRE(offset);
    REQUIRE(close(offset->x, 2) && close(offset->y, 0) && close(offset->z, 0));
    REQUIRE(engine_core::begin_drag(frame, view, {120, 100}, engine_core::DraggerHandle::X, start));
    offset = engine_core::drag_offset(start, view, {140, 100}, 0);
    REQUIRE((offset && close(offset->x, 2)));
}

TEST_CASE("DR6 a plane drag stays in its plane", "[DR6]") {
    const auto view = view_of(200, 200);
    const auto frame = world_frame({0, 0, -10});
    engine_core::DragStart start;
    REQUIRE(engine_core::begin_drag(frame, view, {130, 70}, engine_core::DraggerHandle::XY, start));
    const auto offset = engine_core::drag_offset(start, view, {150, 50}, 0);
    REQUIRE(offset);
    REQUIRE(close(offset->x, 2) && close(offset->y, 2) && close(offset->z, 0));
}

TEST_CASE("DR7 snapping rounds each component along the frame's own axes", "[DR7]") {
    const auto view = view_of(200, 200);
    engine_core::DragStart start;
    REQUIRE(engine_core::begin_drag(world_frame({0, 0, -10}), view, {150, 100}, engine_core::DraggerHandle::X, start));
    auto offset = engine_core::drag_offset(start, view, {174, 100}, 1.0);
    REQUIRE((offset && close(offset->x, 2)));
    // Local: turned 45 degrees about Z. The offset is a whole number of studs along X', none along Y'.
    const engine_core::Matrix4 turned = engine_core::matrix4_multiply(
        engine_core::matrix4_translation(0, 0, -10), engine_core::matrix4_axis_angle({0, 0, 1}, 3.14159265 / 4));
    const auto frame = engine_core::dragger_frame(turned, true);
    engine_core::Vec2 tip;
    REQUIRE(engine_core::project_point(view, {frame.axes[0].x * 5, frame.axes[0].y * 5, -10}, tip));
    REQUIRE(engine_core::begin_drag(frame, view, tip, engine_core::DraggerHandle::X, start));
    offset = engine_core::drag_offset(start, view, {tip.x + 23, tip.y - 23}, 1.0);
    REQUIRE(offset);
    const float along = offset->x * frame.axes[0].x + offset->y * frame.axes[0].y;
    const float across = offset->x * frame.axes[1].x + offset->y * frame.axes[1].y;
    REQUIRE(close(along, std::round(along)));
    REQUIRE(close(across, 0));
}

TEST_CASE("DR8 a ray along the axis gives no offset, and nothing is NaN", "[DR8]") {
    const auto view = view_of(200, 200);
    engine_core::DragStart start;
    start.frame = world_frame({0, 0, -10});
    start.handle = engine_core::DraggerHandle::Z;
    REQUIRE_FALSE(engine_core::drag_offset(start, view, {100, 100}, 0).has_value());
    start.handle = engine_core::DraggerHandle::YZ;
    start.hit = {0, 3, -10};
    REQUIRE_FALSE(engine_core::drag_offset(start, view, {100, 50}, 0).has_value());
}
```

- [ ] Implement, run `[DR1]`–`[DR8]`, the suite; commit `Add DraggerMath: rays, picking, and drag offsets for translate handles`.

## Phase 3

### Task 3: The Dragger instance

**Files:** `src/engine_datatypes/Enum.{hpp,cpp}` (DraggerSpace, DraggerHandle), `src/engine_core/PVInstance.hpp` (`set_pv_transform`), `src/engine_instances/GameObject.hpp`, `PhysicsObject.hpp` (overrides), `src/engine_instances/Dragger.{hpp,cpp}` (new), `src/engine_core/DataModel.hpp/.cpp` + `Ecs` (`dragger()` tag, `draggers(out)`), `src/engine_core/ScriptBindings.cpp` (factory), `sandbox/dragger_tests.cpp`.

**Produces:** `Enum.DraggerSpace` {World 0, Local 1}, `Enum.DraggerHandle` {X 0, Y 1, Z 2, XY 3, YZ 4, XZ 5} (values match `DraggerHandle`). `class Dragger : public DataModel` with `adornee()`/`set_adornee(LuaSlot)` (InstanceRef, class PVInstance), `space()`/`set_space(int)`, `increment()`/`set_increment(double)` (refuses negative or non-finite), `dragging()`, `hovered()`, `active_handle()`, and `InstanceId target() const` (Adornee if set, else the parent, only when a live PVInstance under game; 0 otherwise). Events `DragBegan(handle: EnumItem)`, `Dragged(handle: EnumItem, offset: Vector3)`, `DragEnded(handle: EnumItem)`. `void DataModel::draggers(std::vector<InstanceId>&) const`. `virtual std::optional<std::string> PVInstance::set_pv_transform(const Matrix4&)`.

- [ ] **Tests:** DR9 (target with a PVInstance parent, with Adornee, inactive under a Folder, unparented, or bound to a DirectionalLight parent), DR18 (Space and Increment refuse bad values from Lua; `Dragging` is read-only), DR21 (a PointLight and a SpotLight parent are targets), and `Instance.new("Dragger")` from a script.
- [ ] **Implement** following SoundEmitter's pattern: saved enum and number properties, `lua_saved_property("Adornee", "PVInstance?", ...)` through `set_instance_reference`, `lua_event` with `LuaParam` lists. GameObject's override calls `set_transform` and returns nullopt; PhysicsObject's returns its own `set_transform`'s result. The ECS tag follows `SoundSource`.
- [ ] Run the tests and the suite; commit `Add the Dragger instance: Adornee, Space, Increment, and drag events`.

### Task 4: DraggerWorld: hover, drag, and events

**Files:** `src/engine_services/UserInputService.{hpp,cpp}` (`set_filter`), `src/engine_core/DraggerWorld.{hpp,cpp}` (new), `src/engine_core/DataModelState.hpp` (owns it), `src/engine_core/DataModel.cpp` (installs the filter in the root constructor), `sandbox/dragger_tests.cpp`.

**UserInputService:** `using DispatchFilter = std::function<void(std::vector<InputRecord>&)>; void set_filter(DispatchFilter);`. `dispatch` calls it once, with every record taken from the queue (possibly none), before the loop that applies and fires them.

**DraggerWorld** (`void dispatch(DataModel& game, std::vector<InputRecord>& records)`):
1. Close the previous drag's undo step if one is pending (Task 5).
2. If a drag runs and its Dragger or target is gone, or the Dragger's target changed, end it (DragEnded with the active handle).
3. The view: Workspace's CurrentCamera, its Transform, FieldOfView, ViewportSize. No camera, a zero size, or `MouseBehavior` not Default: a running drag ends; records pass untouched.
4. For each record in order:
   - MouseMovement: if dragging, `drag_offset`; when it has a value, move the target to `start_transform` with offset added to its translation (rotation and scale kept), zero a PhysicsObject target's velocities while the place runs, fire `Dragged(handle, offset)`, mark processed. Not dragging: update hover across every active Dragger (nearest by depth).
   - MouseButton1 Begin: pick across active Draggers, nearest wins; on a hit start a drag (`begin_drag`; keep the target's start Transform, the handle, the Dragger), set `Dragging`, fire `DragBegan`, mark processed.
   - MouseButton1 End or Cancel while dragging: end the drag (`Dragging` false, `DragEnded`), mark processed.
5. When `set_pv_transform` refuses the write, the drag ends there (DragEnded fires) and the target stays where it was.

- [ ] **Tests** (drive the camera and Dragger in C++, post input with `game.input().post_mouse_button(0, down, x, y)` and `post_mouse_move(x, y)`, then `game.input().dispatch(game.events())` and `game.events().drain()`): DR10 (a press on the X arrow, moves, release move the target, and a script sees DragBegan, Dragged with the right offset, DragEnded in order; `Dragging` true between), DR11 (used records are processed, a miss is not — read through a C++ InputBegan listener's `current_args`), DR13b (a PhysicsObject target in play ends with zero velocity), DR14 (destroying the Dragger or the target mid-drag ends it with DragEnded), DR15 (two Draggers overlapping: the nearer drags), DR16 (rotation and scale survive), DR17 (no CurrentCamera, zero ViewportSize, or a locked pointer: nothing drags and nothing is processed), DR19 (offset stays total when Increment changes mid-drag).
- [ ] Implement; run; commit `Add DraggerWorld: hover, drags, and their events from UserInputService's dispatch`.

### Task 5: Undo

**Files:** `src/engine_core/DraggerWorld.cpp`, `sandbox/dragger_tests.cpp`.

1. Drag begins in edit mode (`!game.simulation_running()`): `try_begin_recording("Move")`; keep the id (empty when one was already open: the drag joins it and does not finish it).
2. Drag ends: remember the id and whether the target moved; at the next `dispatch`, before anything else, `finish_recording(id, moved ? Commit : Cancel)`.
3. `start_simulation` seals edit recordings: if the place is running at that next dispatch, the pending id is dropped without finishing.

- [ ] **Tests:** DR12 (edit: one drag is one undo step; undo restores the start; a press and release with no motion leaves no step), DR13 (play: no history), DR20 (a Lua `Dragged` handler that moves a second part: one undo step restores both).
- [ ] Implement; run all; commit `Make a drag one undo step, closed the step after it ends`.
