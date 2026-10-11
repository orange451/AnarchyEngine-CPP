# Joint Constraints Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Five constraints (`RevoluteConstraint`, `PrismaticConstraint`, `DistanceConstraint`, `SphericalConstraint`, `WheelConstraint`) that link two assemblies with a moving Box3D joint, so a user can build a drivable car in Anarchy Studio.

**Architecture:** A pure frame module (`JointFrame`) turns the two ends' pivots into Box3D joint frames. A new abstract `JointConstraint` (under the existing `Constraint`) holds every property in one table-driven array of `JointParam` slots. `Assemblies` lists the active joints (`joints()`); `PhysicsWorld` keeps one Box3D joint per entry in play, capturing frames relative to each end's node, applying settings when a revision moves, and writing one read-only reading back after each step. The Scene View draws a selected joint green with its axes.

**Tech Stack:** C++17, MSVC 14.23 (/W4), Box3D (`b3*Joint*`), Catch2 sandbox tests, the engine's Luau bindings (`LuaApi`).

**Spec:** `docs/superpowers/specs/2026-10-11-joint-constraints-design.md`

## Global Constraints

- Engine distances are "units"; never write "studs" anywhere.
- Names are Box3D's (classes and properties), not Roblox's.
- Angles are degrees, angular speeds degrees per second, lengths units (Box3D takes radians: convert at the Box3D boundary only).
- PVInstance1 is the base (Box3D body A), PVInstance2 the moving part (body B). The joint's axis is its frame's Y.
- Joints exist only in play. `Active` reads the same in the editor and in play.
- A place with no JointConstraint in Workspace runs no new per-step work; a sync where nothing changed allocates nothing.
- Every setter returns why it refused (`std::optional<std::string>`), changing nothing; runs on SimulationThread.
- Build Release: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target <target> -- /m /v:minimal`. Test exes are not in ALL_BUILD: build `sandbox` and `engine-tests` by name.
- Work and commit on `main` (the user's standing choice). Do not push.
- Python is `py` (no Pillow): scripts that write files open them with `newline=''`.

## Review Focus

1. **An end destroyed mid-play** (a wheel blown off by a script): the joint goes with it, no crash, no dangling Box3D id. → Task 5, JP4.
2. **A weld made mid-play between a joint's two ends** (both now one assembly): the joint goes, nothing explodes. → Task 5, JP5.
3. **A script teleports a jointed part** (writes its Transform mid-play): positions stay finite and the joint pulls it back. → Task 5, JP7.
4. **The base's Anchored toggled mid-play**: the joint survives the body's type change and keeps working. → Task 6, JB7.
5. **A script writes the same setting every frame** (car throttle in Heartbeat): settings apply in place; the Box3D joint is never remade. → Task 6, JB8.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `src/engine_core/JointFrame.{hpp,cpp}` (new) | `JointKind`; pivots → world joint frames; axes to draw. Pure math. |
| `src/engine_instances/JointConstraint.{hpp,cpp}` (new) | `JointParam` table, `JointConstraint` and its five classes, validation, Lua properties. |
| `src/engine_instances/Constraint.{hpp,cpp}` | `joint()` and the `link_changed()` hook. |
| `src/engine_core/Assemblies.{hpp,cpp}` | `JointEdge`, `joints()`, the activity rules. |
| `src/engine_core/PhysicsWorld.{hpp,cpp}` | Box3D joints: capture, make, remake, apply, read, drop; test accessors. |
| `src/engine_core/ConstraintLines.{hpp,cpp}`, `src/runner/GameView.cpp` | Drawing a selected joint. |
| `src/engine_core/Project.cpp`, `src/engine_core/ScriptBindings.cpp`, `src/ide/ClassOrder.hpp`, `src/ide/InsertPopup.cpp`, `src/engine_core/LuaApi.cpp`, `src/engine_instances/README.md`, `resources/icons/*` | Registration, Insert list, docs, icons. |
| `sandbox/joint_frame_tests.cpp`, `sandbox/joint_constraint_tests.cpp`, `sandbox/joint_tests.cpp` (new) | Tests. |
| `CMakeLists.txt` | The new sources and test files. |

---

### Task 1: Joint frames

**Files:**
- Create: `src/engine_core/JointFrame.hpp`, `src/engine_core/JointFrame.cpp`
- Create: `sandbox/joint_frame_tests.cpp`
- Modify: `CMakeLists.txt` (engine sources beside `src/engine_core/ConstraintLines.cpp`; sandbox list beside `sandbox/constraint_lines_tests.cpp`)

**Interfaces:**
- Produces: `enum class JointKind { Revolute, Prismatic, Distance, Spherical, Wheel };`
  `void joint_frames(JointKind kind, const Matrix4& pivot1, const Matrix4& pivot2, Matrix4& frame1, Matrix4& frame2);`
  `int joint_axes(JointKind kind, const Matrix4& frame1, Vec3 axes[2]);`

- [ ] **Step 1: Write the failing tests**

`sandbox/joint_frame_tests.cpp`:

```cpp
// Where a joint's Box3D frames go, from its ends' pivots
// (docs/superpowers/specs/2026-10-11-joint-constraints-design.md §2, The frame).

#include "JointFrame.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace {

using engine_core::JointKind;
using engine_core::Matrix4;
using engine_core::Vec3;

constexpr double kPi = 3.14159265358979323846;

Vec3 column(const Matrix4& m, int c) { return Vec3{m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }

bool same(Vec3 a, Vec3 b) {
    return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
}

Matrix4 placed(float x, float y, float z, Vec3 axis, double degrees) {
    return engine_core::matrix4_multiply(engine_core::matrix4_translation(x, y, z),
                                         engine_core::matrix4_axis_angle(axis, degrees * kPi / 180.0));
}

}  // namespace

TEST_CASE("JF1 a Revolute's and a Spherical's frame turns about the moving end's Y, at its pivot",
          "[joint-frame]") {
    const Matrix4 base = placed(0.f, 0.f, 0.f, Vec3{0.f, 1.f, 0.f}, 0.0);
    const Matrix4 moving = placed(1.f, 2.f, 3.f, Vec3{1.f, 0.f, 0.f}, 30.0);
    for (const JointKind kind : {JointKind::Revolute, JointKind::Spherical}) {
        Matrix4 frame1{};
        Matrix4 frame2{};
        engine_core::joint_frames(kind, base, moving, frame1, frame2);
        // Box3D turns about Z: the frame's Z is the pivot's Y.
        REQUIRE(same(column(frame1, 2), column(moving, 1)));
        REQUIRE(same(column(frame1, 0), column(moving, 0)));
        REQUIRE(same(engine_core::matrix4_position(frame1), Vec3{1.f, 2.f, 3.f}));
        REQUIRE(engine_core::same_matrix4(frame1, frame2));
    }
}

TEST_CASE("JF2 a Prismatic's frame slides along the moving end's Y", "[joint-frame]") {
    const Matrix4 moving = placed(4.f, 0.f, 0.f, Vec3{0.f, 0.f, 1.f}, 90.0);
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Prismatic, engine_core::matrix4_identity(), moving, frame1, frame2);
    // Box3D slides along X: the frame's X is the pivot's Y.
    REQUIRE(same(column(frame1, 0), column(moving, 1)));
    REQUIRE(same(engine_core::matrix4_position(frame1), Vec3{4.f, 0.f, 0.f}));
    REQUIRE(engine_core::same_matrix4(frame1, frame2));
}

TEST_CASE("JF3 a Distance's frames are each end's position, unrotated", "[joint-frame]") {
    const Matrix4 base = placed(0.f, 5.f, 0.f, Vec3{0.f, 1.f, 0.f}, 40.0);
    const Matrix4 moving = placed(0.f, 3.f, 0.f, Vec3{1.f, 0.f, 0.f}, 70.0);
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Distance, base, moving, frame1, frame2);
    REQUIRE(engine_core::same_matrix4(frame1, engine_core::matrix4_translation(0.f, 5.f, 0.f)));
    REQUIRE(engine_core::same_matrix4(frame2, engine_core::matrix4_translation(0.f, 3.f, 0.f)));
}

TEST_CASE("JF4 a Wheel spins about its own Y and travels along the chassis's up", "[joint-frame]") {
    // A cylinder wheel on its side: its Y points along world -X.
    const Matrix4 wheel = placed(2.f, 0.5f, 1.f, Vec3{0.f, 0.f, 1.f}, 90.0);
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Wheel, engine_core::matrix4_identity(), wheel, frame1, frame2);
    REQUIRE(same(column(frame1, 2), Vec3{-1.f, 0.f, 0.f}));  // spin: the wheel's Y
    REQUIRE(same(column(frame1, 0), Vec3{0.f, 1.f, 0.f}));   // travel and steering: the chassis's up
    REQUIRE(same(column(frame1, 1), Vec3{0.f, 0.f, -1.f}));  // Z × X
    REQUIRE(same(engine_core::matrix4_position(frame1), Vec3{2.f, 0.5f, 1.f}));
    REQUIRE(engine_core::same_matrix4(frame1, frame2));
}

TEST_CASE("JF5 a Wheel lying flat travels along the chassis's X instead", "[joint-frame]") {
    // Its axle points up, along the chassis's up: that cannot be the travel axis.
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Wheel, engine_core::matrix4_identity(),
                              engine_core::matrix4_translation(0.f, 1.f, 0.f), frame1, frame2);
    REQUIRE(same(column(frame1, 2), Vec3{0.f, 1.f, 0.f}));
    REQUIRE(same(column(frame1, 0), Vec3{1.f, 0.f, 0.f}));
}

TEST_CASE("JF6 the axes drawn: one for a hinge, slider, or ball; two for a wheel; none for a distance",
          "[joint-frame]") {
    const Matrix4 wheel = placed(0.f, 0.f, 0.f, Vec3{0.f, 0.f, 1.f}, 90.0);
    Vec3 axes[2]{};
    Matrix4 frame1{};
    Matrix4 frame2{};
    engine_core::joint_frames(JointKind::Revolute, engine_core::matrix4_identity(), wheel, frame1, frame2);
    REQUIRE(engine_core::joint_axes(JointKind::Revolute, frame1, axes) == 1);
    REQUIRE(same(axes[0], column(wheel, 1)));
    engine_core::joint_frames(JointKind::Prismatic, engine_core::matrix4_identity(), wheel, frame1, frame2);
    REQUIRE(engine_core::joint_axes(JointKind::Prismatic, frame1, axes) == 1);
    REQUIRE(same(axes[0], column(wheel, 1)));
    engine_core::joint_frames(JointKind::Wheel, engine_core::matrix4_identity(), wheel, frame1, frame2);
    REQUIRE(engine_core::joint_axes(JointKind::Wheel, frame1, axes) == 2);
    REQUIRE(same(axes[0], Vec3{-1.f, 0.f, 0.f}));
    REQUIRE(same(axes[1], Vec3{0.f, 1.f, 0.f}));
    REQUIRE(engine_core::joint_axes(JointKind::Distance, frame1, axes) == 0);
}
```

Add `sandbox/joint_frame_tests.cpp` to the `add_executable(sandbox ...)` list after `sandbox/constraint_lines_tests.cpp`. Create `src/engine_core/JointFrame.hpp` with the declarations below and an empty `JointFrame.cpp` holding definitions that return identity / 0, and add `src/engine_core/JointFrame.cpp` to the engine sources after `src/engine_core/ConstraintLines.cpp`, so the tests compile.

- [ ] **Step 2: Run them to see them fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[joint-frame]"`
Expected: FAIL (the stubs give identity frames and 0 axes).

- [ ] **Step 3: Implement**

`src/engine_core/JointFrame.hpp`:

```cpp
#pragma once

#include "types.hpp"

namespace engine_core {

// The Box3D joint a JointConstraint makes
// (docs/superpowers/specs/2026-10-11-joint-constraints-design.md).
enum class JointKind { Revolute, Prismatic, Distance, Spherical, Wheel };

// Where a joint's frame is on each side when it starts, in world space, as
// Box3D's joint of kind wants it (spec §2, The frame). pivot1 and pivot2 are
// the ends' pivots (Assemblies::end_pivot), scale removed. The engine's axis
// is a pivot's Y: a Revolute's and a Spherical's frame turns Box3D's Z onto
// pivot2's Y, a Prismatic's turns Box3D's X onto it, and a Wheel's spins
// about pivot2's Y and travels along pivot1's Y made perpendicular to that
// (pivot1's X when the two are parallel). Those put both frames at one
// frame W at pivot2. A Distance puts each at its own end's position,
// unrotated. Pure.
void joint_frames(JointKind kind, const Matrix4& pivot1, const Matrix4& pivot2, Matrix4& frame1, Matrix4& frame2);

// The axes to draw at a joint's frame1, as unit vectors: the turning or
// sliding axis for a Revolute, Spherical, or Prismatic; the spin axis, then
// the suspension and steering axis, for a Wheel; none for a Distance.
// Returns how many it wrote.
int joint_axes(JointKind kind, const Matrix4& frame1, Vec3 axes[2]);

}  // namespace engine_core
```

`src/engine_core/JointFrame.cpp`:

```cpp
#include "JointFrame.hpp"

#include <cmath>

namespace engine_core {
namespace {

constexpr double kHalfPi = 1.57079632679489661923;
// Shorter than this, a direction made perpendicular is too close to the axis to use.
constexpr float kShortest = 0.001f;

Vec3 column(const Matrix4& m, int c) { return Vec3{m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 scaled(Vec3 v, float s) { return Vec3{v.x * s, v.y * s, v.z * s}; }
Vec3 minus(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
float length(Vec3 v) { return std::sqrt(dot(v, v)); }

Vec3 unit(Vec3 v) {
    const float l = length(v);
    return l > 0.f ? scaled(v, 1.f / l) : v;
}

// v with its part along the unit axis taken out, normalized. False when what
// is left is shorter than kShortest.
bool perpendicular(Vec3 v, Vec3 axis, Vec3& out) {
    const Vec3 rest = minus(v, scaled(axis, dot(v, axis)));
    const float l = length(rest);
    if (!(l >= kShortest)) {
        return false;
    }
    out = scaled(rest, 1.f / l);
    return true;
}

Matrix4 from_columns(Vec3 x, Vec3 y, Vec3 z, Vec3 at) {
    Matrix4 out = matrix4_identity();
    const Vec3 columns[3] = {x, y, z};
    for (int c = 0; c < 3; ++c) {
        out.m[c * 4] = columns[c].x;
        out.m[c * 4 + 1] = columns[c].y;
        out.m[c * 4 + 2] = columns[c].z;
    }
    out.m[12] = at.x;
    out.m[13] = at.y;
    out.m[14] = at.z;
    return out;
}

}  // namespace

void joint_frames(JointKind kind, const Matrix4& pivot1, const Matrix4& pivot2, Matrix4& frame1, Matrix4& frame2) {
    switch (kind) {
        case JointKind::Distance: {
            const Vec3 a = matrix4_position(pivot1);
            const Vec3 b = matrix4_position(pivot2);
            frame1 = matrix4_translation(a.x, a.y, a.z);
            frame2 = matrix4_translation(b.x, b.y, b.z);
            return;
        }
        case JointKind::Revolute:
        case JointKind::Spherical:
            // -90° about X turns Z onto Y.
            frame1 = matrix4_multiply(pivot2, matrix4_axis_angle(Vec3{1.f, 0.f, 0.f}, -kHalfPi));
            break;
        case JointKind::Prismatic:
            // +90° about Z turns X onto Y.
            frame1 = matrix4_multiply(pivot2, matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, kHalfPi));
            break;
        case JointKind::Wheel: {
            const Vec3 spin = unit(column(pivot2, 1));
            Vec3 travel{1.f, 0.f, 0.f};
            if (!perpendicular(column(pivot1, 1), spin, travel) && !perpendicular(column(pivot1, 0), spin, travel)) {
                // A rotation's X and Y are never both along one axis; a degenerate pivot1 lands here.
                perpendicular(column(pivot2, 0), spin, travel);
            }
            frame1 = from_columns(travel, cross(spin, travel), spin, matrix4_position(pivot2));
            break;
        }
    }
    frame2 = frame1;
}

int joint_axes(JointKind kind, const Matrix4& frame1, Vec3 axes[2]) {
    switch (kind) {
        case JointKind::Revolute:
        case JointKind::Spherical:
            axes[0] = unit(column(frame1, 2));
            return 1;
        case JointKind::Prismatic:
            axes[0] = unit(column(frame1, 0));
            return 1;
        case JointKind::Wheel:
            axes[0] = unit(column(frame1, 2));
            axes[1] = unit(column(frame1, 0));
            return 2;
        case JointKind::Distance:
            return 0;
    }
    return 0;
}

}  // namespace engine_core
```

- [ ] **Step 4: Run them to see them pass**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[joint-frame]"`
Expected: PASS (6 test cases). If JF1's X check fails because `matrix4_axis_angle` turns the other way, the Z check must still hold; flip the sign of the angle (both Revolute and Prismatic) and ledger the ruling.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/JointFrame.hpp src/engine_core/JointFrame.cpp sandbox/joint_frame_tests.cpp CMakeLists.txt
git commit -m "Joints: frames from the ends' pivots, as Box3D's joints want them

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: JointConstraint and its five classes

**Files:**
- Create: `src/engine_instances/JointConstraint.hpp`, `src/engine_instances/JointConstraint.cpp`
- Create: `sandbox/joint_constraint_tests.cpp`
- Modify: `src/engine_instances/Constraint.hpp`, `src/engine_instances/Constraint.cpp`
- Modify: `src/engine_core/Project.cpp:164` (class registry), `src/engine_core/ScriptBindings.cpp:211,264` (creatable)
- Modify: `CMakeLists.txt` (engine sources after `src/engine_instances/Constraint.cpp`; sandbox list)

**Interfaces:**
- Consumes: `JointKind` (Task 1).
- Produces:
  - `Constraint::joint() const` (virtual, false), protected `virtual void Constraint::link_changed()`.
  - `enum class JointParam` (40 slots + `Count`), `kJointParamCount`, `enum class JointRule { Flag, Any, NonNegative, Hinge, Cone }`, `struct JointParamInfo { const char* name; JointRule rule; double default_value; }`, `const JointParamInfo& joint_param_info(JointParam)`.
  - `class JointConstraint : public Constraint` with `kind()`, `param(JointParam)`, `flag(JointParam)`, `set_param(JointParam, double)`, `motor_velocity()`, `set_motor_velocity(Vec3)`, `settings_revision()`, `capture_revision()`, `reading()`, `set_reading(double)`.
  - `RevoluteConstraint`, `PrismaticConstraint`, `DistanceConstraint`, `SphericalConstraint`, `WheelConstraint` (final).

- [ ] **Step 1: Write the failing tests**

`sandbox/joint_constraint_tests.cpp`:

```cpp
// JointConstraint's values (docs/superpowers/specs/2026-10-11-joint-constraints-design.md
// §1, §4): defaults, what a setter refuses or clamps, from C++ and Luau, and saving.

#include "support.hpp"

#include "Assemblies.hpp"
#include "Folder.hpp"
#include "InstanceFile.hpp"
#include "JointConstraint.hpp"
#include "PhysicsObject.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using engine_core::DataModel;
using engine_core::InstanceId;
using engine_core::JointConstraint;
using engine_core::JointKind;
using engine_core::JointParam;
using engine_core::PhysicsObject;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

bool has(const std::string& output, const std::string& text) { return output.find(text) != std::string::npos; }

std::string run(ScriptRig& rig, const char* source) {
    rig.runtime.drain_output();
    rig.runtime.run_chunk(source);
    rig.frames(1);
    std::string out;
    for (const auto& line : rig.runtime.drain_output().lines) {
        out += line.text;
        out += '\n';
    }
    return out;
}

PhysicsObject& part(DataModel& game, const char* name) {
    PhysicsObject& made = game.create<PhysicsObject>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), workspace_of(game));
    return made;
}

template <class T>
T& joint(DataModel& game, InstanceId base, InstanceId moving, InstanceId parent) {
    T& made = game.create<T>();
    REQUIRE_FALSE(made.set_pv_instance1(instance_slot(base)));
    REQUIRE_FALSE(made.set_pv_instance2(instance_slot(moving)));
    game.set_parent(made.id(), parent);
    return made;
}

}  // namespace

TEST_CASE("JC1 the five joints have their kinds, and every property starts at its default", "[joint]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(game.create<engine_core::RevoluteConstraint>().kind() == JointKind::Revolute);
    REQUIRE(game.create<engine_core::PrismaticConstraint>().kind() == JointKind::Prismatic);
    REQUIRE(game.create<engine_core::DistanceConstraint>().kind() == JointKind::Distance);
    REQUIRE(game.create<engine_core::SphericalConstraint>().kind() == JointKind::Spherical);
    engine_core::WheelConstraint& wheel = game.create<engine_core::WheelConstraint>();
    REQUIRE(wheel.kind() == JointKind::Wheel);
    REQUIRE(std::string(wheel.class_name()) == "WheelConstraint");
    REQUIRE_FALSE(wheel.rigid());
    REQUIRE_FALSE(wheel.joins());
    REQUIRE(wheel.joint());
    for (int i = 0; i < engine_core::kJointParamCount; ++i) {
        const auto param = static_cast<JointParam>(i);
        REQUIRE(wheel.param(param) == engine_core::joint_param_info(param).default_value);
    }
    // The table lines up with the enum at its start, middle, and end.
    REQUIRE(std::string(engine_core::joint_param_info(JointParam::CollideConnected).name) == "CollideConnected");
    REQUIRE(std::string(engine_core::joint_param_info(JointParam::MaxLength).name) == "MaxLength");
    REQUIRE(std::string(engine_core::joint_param_info(JointParam::UpperSteeringLimit).name) ==
            "UpperSteeringLimit");
    REQUIRE(wheel.flag(JointParam::EnableSuspension));
    REQUIRE_FALSE(wheel.flag(JointParam::CollideConnected));
    REQUIRE(wheel.param(JointParam::SuspensionHertz) == 4.0);
    REQUIRE(wheel.param(JointParam::MaxSpinTorque) == 10000.0);
    REQUIRE(wheel.param(JointParam::LowerSteeringLimit) == -30.0);
    REQUIRE(wheel.reading() == 0.0);
    REQUIRE(wheel.motor_velocity().y == 0.f);
}

TEST_CASE("JC2 a value that is not finite, or a negative one where none is allowed, is refused", "[joint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::RevoluteConstraint& hinge = game.create<engine_core::RevoluteConstraint>();
    REQUIRE(*hinge.set_param(JointParam::MotorSpeed, std::nan("")) == "MotorSpeed must be a finite number");
    REQUIRE(*hinge.set_param(JointParam::TargetAngle, std::numeric_limits<double>::infinity()) ==
            "TargetAngle must be a finite number");
    for (const JointParam param :
         {JointParam::Hertz, JointParam::DampingRatio, JointParam::MaxMotorTorque, JointParam::MaxMotorForce,
          JointParam::Length, JointParam::MinLength, JointParam::MaxLength, JointParam::SuspensionHertz,
          JointParam::SuspensionDampingRatio, JointParam::SteeringHertz, JointParam::SteeringDampingRatio,
          JointParam::MaxSpinTorque, JointParam::MaxSteeringTorque}) {
        const double before = hinge.param(param);
        const std::optional<std::string> refused = hinge.set_param(param, -1.0);
        REQUIRE(refused);
        REQUIRE(*refused == std::string(engine_core::joint_param_info(param).name) + " must not be negative");
        REQUIRE(hinge.param(param) == before);
    }
    // A speed may be negative: it turns the other way.
    REQUIRE_FALSE(hinge.set_param(JointParam::MotorSpeed, -90.0));
    REQUIRE(hinge.param(JointParam::MotorSpeed) == -90.0);
    engine_core::SphericalConstraint& ball = game.create<engine_core::SphericalConstraint>();
    REQUIRE(*ball.set_motor_velocity(engine_core::Vec3{std::nanf(""), 0.f, 0.f}) == "MotorVelocity must be finite");
    REQUIRE_FALSE(ball.set_motor_velocity(engine_core::Vec3{0.f, 90.f, 0.f}));
    REQUIRE(ball.motor_velocity().y == 90.f);
}

TEST_CASE("JC3 angle limits are clamped to Box3D's ranges", "[joint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::RevoluteConstraint& hinge = game.create<engine_core::RevoluteConstraint>();
    REQUIRE_FALSE(hinge.set_param(JointParam::LowerAngle, -500.0));
    REQUIRE(hinge.param(JointParam::LowerAngle) == -178.2);
    REQUIRE_FALSE(hinge.set_param(JointParam::UpperAngle, 179.0));
    REQUIRE(hinge.param(JointParam::UpperAngle) == 178.2);
    engine_core::SphericalConstraint& ball = game.create<engine_core::SphericalConstraint>();
    REQUIRE_FALSE(ball.set_param(JointParam::UpperTwistAngle, 200.0));
    REQUIRE(ball.param(JointParam::UpperTwistAngle) == 178.2);
    REQUIRE_FALSE(ball.set_param(JointParam::ConeAngle, 120.0));
    REQUIRE(ball.param(JointParam::ConeAngle) == 90.0);
    REQUIRE_FALSE(ball.set_param(JointParam::ConeAngle, -5.0));
    REQUIRE(ball.param(JointParam::ConeAngle) == 0.0);
    // A flag takes any non-zero as on.
    REQUIRE_FALSE(ball.set_param(JointParam::EnableConeLimit, 7.0));
    REQUIRE(ball.param(JointParam::EnableConeLimit) == 1.0);
}

TEST_CASE("JC4 a lower limit above its upper is kept as written", "[joint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::PrismaticConstraint& slider = game.create<engine_core::PrismaticConstraint>();
    REQUIRE_FALSE(slider.set_param(JointParam::LowerTranslation, 3.0));
    REQUIRE(slider.param(JointParam::LowerTranslation) == 3.0);
    REQUIRE(slider.param(JointParam::UpperTranslation) == 1.0);
}

TEST_CASE("JC5 a setting moves the settings revision; an end or Enabled, the capture revision", "[joint]") {
    SimRole role;
    engine_core::Game game;
    PhysicsObject& a = part(game, "A");
    engine_core::RevoluteConstraint& hinge = game.create<engine_core::RevoluteConstraint>();
    const std::uint32_t settings = hinge.settings_revision();
    REQUIRE_FALSE(hinge.set_param(JointParam::MotorSpeed, 30.0));
    REQUIRE(hinge.settings_revision() != settings);
    const std::uint32_t again = hinge.settings_revision();
    REQUIRE_FALSE(hinge.set_param(JointParam::MotorSpeed, 30.0));
    REQUIRE(hinge.settings_revision() == again);
    const std::uint32_t capture = hinge.capture_revision();
    REQUIRE_FALSE(hinge.set_pv_instance1(instance_slot(a.id())));
    REQUIRE(hinge.capture_revision() != capture);
    const std::uint32_t enabled = hinge.capture_revision();
    REQUIRE_FALSE(hinge.set_enabled(false));
    REQUIRE(hinge.capture_revision() != enabled);
    const std::uint32_t same = hinge.capture_revision();
    REQUIRE_FALSE(hinge.set_enabled(false));
    REQUIRE(hinge.capture_revision() == same);
    REQUIRE(hinge.settings_revision() == again);
}

TEST_CASE("JC6 Luau reads and writes a joint's properties, and is refused as C++ is", "[joint][lua]") {
    ScriptRig rig;
    const std::string out = run(rig, R"(
        local w = Instance.new("WheelConstraint")
        w.SpinMotorSpeed = -360
        w.EnableSteering = true
        print("spin=" .. tostring(w.SpinMotorSpeed))
        print("steer=" .. tostring(w.EnableSteering))
        print("hertz=" .. tostring(w.SuspensionHertz))
        print("reading=" .. tostring(w.SpinSpeed))
        print("collide=" .. tostring(w.CollideConnected))
        local ok, err = pcall(function() w.SteeringHertz = -1 end)
        print("refused=" .. tostring(ok) .. " " .. tostring(err))
        local b = Instance.new("SphericalConstraint")
        b.MotorVelocity = Vector3.new(0, 90, 0)
        print("velocity=" .. tostring(b.MotorVelocity.Y))
        print("isa=" .. tostring(b:IsA("JointConstraint")) .. tostring(b:IsA("Constraint")) .. tostring(b.Enabled))
        print("foreign=" .. tostring(pcall(function() w.LowerAngle = 1 end)))
    )");
    INFO(out);
    REQUIRE(has(out, "spin=-360"));
    REQUIRE(has(out, "steer=true"));
    REQUIRE(has(out, "hertz=4"));
    REQUIRE(has(out, "reading=0"));
    REQUIRE(has(out, "collide=false"));
    REQUIRE(has(out, "refused=false"));
    REQUIRE(has(out, "SteeringHertz must not be negative"));
    REQUIRE(has(out, "velocity=90"));
    REQUIRE(has(out, "isa=truetruetrue"));
    REQUIRE(has(out, "foreign=false"));
}

TEST_CASE("JC7 a saved joint loads with every value it had", "[joint]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        DataModel& game = project.datamodel();
        PhysicsObject& chassis = part(game, "Chassis");
        PhysicsObject& wheel = part(game, "Wheel");
        engine_core::WheelConstraint& axle =
            joint<engine_core::WheelConstraint>(game, chassis.id(), wheel.id(), workspace_of(game));
        game.set_name(axle.id(), "Axle");
        REQUIRE_FALSE(axle.set_param(JointParam::SpinMotorSpeed, -360.0));
        REQUIRE_FALSE(axle.set_param(JointParam::SuspensionHertz, 2.0));
        REQUIRE_FALSE(axle.set_param(JointParam::EnableSteering, 1.0));
        REQUIRE_FALSE(axle.set_param(JointParam::EnableSuspension, 0.0));
        REQUIRE_FALSE(axle.set_param(JointParam::CollideConnected, 1.0));
        engine_core::SphericalConstraint& ball =
            joint<engine_core::SphericalConstraint>(game, chassis.id(), wheel.id(), workspace_of(game));
        game.set_name(ball.id(), "Ball");
        REQUIRE_FALSE(ball.set_motor_velocity(engine_core::Vec3{0.f, 90.f, 0.f}));
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    auto* axle = dynamic_cast<engine_core::WheelConstraint*>(
        game.instance(game.find_first_child(workspace_of(game), "Axle")));
    REQUIRE(axle != nullptr);
    REQUIRE(axle->pv_instance1_id() == game.find_first_child(workspace_of(game), "Chassis"));
    REQUIRE(axle->pv_instance2_id() == game.find_first_child(workspace_of(game), "Wheel"));
    REQUIRE(axle->param(JointParam::SpinMotorSpeed) == -360.0);
    REQUIRE(axle->param(JointParam::SuspensionHertz) == 2.0);
    REQUIRE(axle->flag(JointParam::EnableSteering));
    REQUIRE_FALSE(axle->flag(JointParam::EnableSuspension));
    REQUIRE(axle->flag(JointParam::CollideConnected));
    auto* ball = dynamic_cast<engine_core::SphericalConstraint*>(
        game.instance(game.find_first_child(workspace_of(game), "Ball")));
    REQUIRE(ball != nullptr);
    REQUIRE(ball->motor_velocity().y == 90.f);
}

TEST_CASE("JC8 duplicating a jointed pair joins the copies, with the joint's values", "[joint]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Folder& car = game.create<engine_core::Folder>();
    game.set_parent(car.id(), workspace_of(game));
    PhysicsObject& chassis = game.create<PhysicsObject>();
    game.set_parent(chassis.id(), car.id());
    PhysicsObject& wheel = game.create<PhysicsObject>();
    game.set_parent(wheel.id(), car.id());
    engine_core::WheelConstraint& axle =
        joint<engine_core::WheelConstraint>(game, chassis.id(), wheel.id(), car.id());
    REQUIRE_FALSE(axle.set_param(JointParam::SpinMotorSpeed, 45.0));
    std::vector<engine_core::CopiedNode> group{engine_core::copy_tree(game, car.id())};
    engine_core::CopyMap map;
    std::vector<InstanceId> made;
    {
        engine_core::AssemblySuspend suspend(game);
        REQUIRE(engine_core::paste_copies(game, group, workspace_of(game), &made, nullptr, &map));
        engine_core::remap_references(game, map);
    }
    REQUIRE(made.size() == 1);
    InstanceId copy_axle = 0;
    std::vector<InstanceId> parts;
    for (InstanceId child = game.first_child(made[0]); child != 0; child = game.next_sibling(child)) {
        if (dynamic_cast<engine_core::WheelConstraint*>(game.instance(child)) != nullptr) {
            copy_axle = child;
        } else {
            parts.push_back(child);
        }
    }
    auto* copied = dynamic_cast<engine_core::WheelConstraint*>(game.instance(copy_axle));
    REQUIRE(copied != nullptr);
    REQUIRE(copied->param(JointParam::SpinMotorSpeed) == 45.0);
    REQUIRE(parts.size() == 2);
    REQUIRE(copied->pv_instance1_id() == parts[0]);
    REQUIRE(copied->pv_instance2_id() == parts[1]);
}
```

Add `sandbox/joint_constraint_tests.cpp` to the sandbox list. (`CopiedNode`, `CopyMap`, `copy_tree`, `paste_copies`, and `remap_references` are in `InstanceFile.hpp`; `AssemblySuspend` in `Assemblies.hpp`. `Model` is an asset reference here, not a group: the car is a Folder.)

For the RED run, create `JointConstraint.hpp` exactly as in Step 3 and a `JointConstraint.cpp` whose `set_param` and `set_motor_velocity` return `std::nullopt` and change nothing, whose constructor leaves `params_` zero, and with no Lua registration, so the file compiles and links.

- [ ] **Step 2: Run them to see them fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[joint]"`
Expected: FAIL — JC1 (defaults 0), JC2 (nothing refused), JC3, JC5, JC6 (`Instance.new("WheelConstraint")` unknown), JC7, JC8.

- [ ] **Step 3: Implement**

`src/engine_instances/Constraint.hpp` — in `class Constraint`, after `joins()`:

```cpp
    // A JointConstraint: it joins nothing, and links two assemblies with a
    // Box3D joint in play.
    virtual bool joint() const { return false; }
```

and in its `protected:` section, after `on_reuse`:

```cpp
    // An end or Enabled changed (the setters below, after they took the
    // value). A JointConstraint takes its frame again from where things are.
    virtual void link_changed() {}
```

Update the class comment's second sentence to: `NoCollisionConstraint keeps them from colliding with each other; a JointConstraint (JointConstraint.hpp) links two assemblies with a moving joint.`

`src/engine_instances/Constraint.cpp` — in `set_pv_instance1` and `set_pv_instance2`, inside `if (!refused) {`, after `assemblies().constraint_changed(*this, id());`, add `link_changed();`. In `set_enabled`, after `assemblies().constraint_changed(*this, id());`, add `link_changed();`.

`src/engine_instances/JointConstraint.hpp`:

```cpp
#pragma once

#include "Constraint.hpp"
#include "JointFrame.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace engine_core {

// Every property a joint may have (spec
// docs/superpowers/specs/2026-10-11-joint-constraints-design.md §1). A name is
// one slot whichever class shows it: Hertz is a Revolute's and a Prismatic's,
// with one rule and one default. Each class registers only its own. Keep in
// step with kParams in JointConstraint.cpp.
enum class JointParam : int {
    CollideConnected,
    EnableSpring,
    Hertz,
    DampingRatio,
    TargetAngle,
    TargetTranslation,
    EnableLimit,
    LowerAngle,
    UpperAngle,
    LowerTranslation,
    UpperTranslation,
    EnableMotor,
    MotorSpeed,
    MaxMotorTorque,
    MaxMotorForce,
    Length,
    MinLength,
    MaxLength,
    EnableConeLimit,
    ConeAngle,
    EnableTwistLimit,
    LowerTwistAngle,
    UpperTwistAngle,
    EnableSuspension,
    SuspensionHertz,
    SuspensionDampingRatio,
    EnableSuspensionLimit,
    LowerSuspensionLimit,
    UpperSuspensionLimit,
    EnableSpinMotor,
    SpinMotorSpeed,
    MaxSpinTorque,
    EnableSteering,
    SteeringHertz,
    SteeringDampingRatio,
    TargetSteeringAngle,
    MaxSteeringTorque,
    EnableSteeringLimit,
    LowerSteeringLimit,
    UpperSteeringLimit,
    Count
};
constexpr int kJointParamCount = static_cast<int>(JointParam::Count);

// What a value may be (spec §4).
enum class JointRule {
    Flag,         // a boolean, kept as 0 or 1
    Any,          // any finite number
    NonNegative,  // refused below 0
    Hinge,        // degrees, clamped to ±178.2 (Box3D's ±0.99π)
    Cone,         // degrees, clamped to 0..90 (Box3D's cone is at most π/2)
};

struct JointParamInfo {
    const char* name;
    JointRule rule;
    double default_value;
};

const JointParamInfo& joint_param_info(JointParam param);

// A Constraint that links two assemblies with a moving Box3D joint (spec §1,
// §2). Abstract: the Lua class JointConstraint cannot be made; its five
// subclasses can. It joins nothing, so each end stays in its own assembly.
// PVInstance1 is the base (Box3D's body A), PVInstance2 the moving part; the
// joint sits at PVInstance2's pivot and turns or slides along its Y.
//
// CollideConnected  boolean  false. Saved. Whether the two ends collide.
//
// Each subclass shows its own saved JointParams, and every one but a
// SphericalConstraint one read-only reading, which PhysicsWorld sets after
// each step in play and which reads 0 while the place is stopped.
class JointConstraint : public Constraint {
public:
    JointConstraint(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    bool rigid() const override { return false; }
    bool joins() const override { return false; }
    bool joint() const override { return true; }
    virtual JointKind kind() const = 0;

    double param(JointParam p) const { return params_[static_cast<std::size_t>(p)]; }
    bool flag(JointParam p) const { return param(p) != 0.0; }
    // A SphericalConstraint's MotorVelocity: degrees per second about each world axis.
    Vec3 motor_velocity() const { return motor_velocity_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing (spec §4). A Flag takes any non-zero as 1.
    std::optional<std::string> set_param(JointParam p, double value);
    std::optional<std::string> set_motor_velocity(Vec3 velocity);

    // Moves on every change to a value above: PhysicsWorld gives Box3D the
    // settings again.
    std::uint32_t settings_revision() const { return settings_revision_; }
    // Moves on every change to an end or Enabled: PhysicsWorld takes the
    // joint's frame again from where things are then (spec §2, Capture).
    std::uint32_t capture_revision() const { return capture_revision_; }

    // The read-only value (Angle, Translation, CurrentLength, or SpinSpeed), in
    // its property's units. PhysicsWorld sets it after each step, as a plain
    // field: no Changed, no history.
    double reading() const { return reading_; }
    void set_reading(double value) { reading_ = value; }

protected:
    void on_reuse() override;
    void link_changed() override { ++capture_revision_; }

private:
    void reset_values();

    std::array<double, kJointParamCount> params_{};
    Vec3 motor_velocity_{};
    double reading_ = 0.0;
    std::uint32_t settings_revision_ = 0;
    std::uint32_t capture_revision_ = 0;
};

// Body B turns about the axis (b3RevoluteJoint).
class RevoluteConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "RevoluteConstraint"; }
    JointKind kind() const override { return JointKind::Revolute; }
};

// Body B slides along the axis and does not turn (b3PrismaticJoint).
class PrismaticConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "PrismaticConstraint"; }
    JointKind kind() const override { return JointKind::Prismatic; }
};

// Holds the distance between the two ends' pivots: a rod, a spring, or a
// rope (b3DistanceJoint).
class DistanceConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "DistanceConstraint"; }
    JointKind kind() const override { return JointKind::Distance; }
};

// Body B turns freely about the pivot (b3SphericalJoint).
class SphericalConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "SphericalConstraint"; }
    JointKind kind() const override { return JointKind::Spherical; }
};

// A car wheel: PVInstance1 the chassis, PVInstance2 the wheel, spinning about
// its own Y, on a suspension along the chassis's up, steering about it
// (b3WheelJoint).
class WheelConstraint final : public JointConstraint {
public:
    using JointConstraint::JointConstraint;
    const char* class_name() const override { return "WheelConstraint"; }
    JointKind kind() const override { return JointKind::Wheel; }
};

}  // namespace engine_core
```

`src/engine_instances/JointConstraint.cpp`:

```cpp
#include "JointConstraint.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>

namespace engine_core {
namespace {

constexpr double kHingeLimit = 178.2;
constexpr double kConeLimit = 90.0;

using R = JointRule;

// By JointParam, in its order.
const JointParamInfo kParams[] = {
    {"CollideConnected", R::Flag, 0.0},
    {"EnableSpring", R::Flag, 0.0},
    {"Hertz", R::NonNegative, 1.0},
    {"DampingRatio", R::NonNegative, 0.7},
    {"TargetAngle", R::Any, 0.0},
    {"TargetTranslation", R::Any, 0.0},
    {"EnableLimit", R::Flag, 0.0},
    {"LowerAngle", R::Hinge, -45.0},
    {"UpperAngle", R::Hinge, 45.0},
    {"LowerTranslation", R::Any, -1.0},
    {"UpperTranslation", R::Any, 1.0},
    {"EnableMotor", R::Flag, 0.0},
    {"MotorSpeed", R::Any, 0.0},
    {"MaxMotorTorque", R::NonNegative, 10000.0},
    {"MaxMotorForce", R::NonNegative, 10000.0},
    {"Length", R::NonNegative, 0.0},
    {"MinLength", R::NonNegative, 0.0},
    {"MaxLength", R::NonNegative, 10.0},
    {"EnableConeLimit", R::Flag, 0.0},
    {"ConeAngle", R::Cone, 45.0},
    {"EnableTwistLimit", R::Flag, 0.0},
    {"LowerTwistAngle", R::Hinge, -45.0},
    {"UpperTwistAngle", R::Hinge, 45.0},
    {"EnableSuspension", R::Flag, 1.0},
    {"SuspensionHertz", R::NonNegative, 4.0},
    {"SuspensionDampingRatio", R::NonNegative, 0.7},
    {"EnableSuspensionLimit", R::Flag, 0.0},
    {"LowerSuspensionLimit", R::Any, -0.5},
    {"UpperSuspensionLimit", R::Any, 0.5},
    {"EnableSpinMotor", R::Flag, 0.0},
    {"SpinMotorSpeed", R::Any, 0.0},
    {"MaxSpinTorque", R::NonNegative, 10000.0},
    {"EnableSteering", R::Flag, 0.0},
    {"SteeringHertz", R::NonNegative, 5.0},
    {"SteeringDampingRatio", R::NonNegative, 0.7},
    {"TargetSteeringAngle", R::Any, 0.0},
    {"MaxSteeringTorque", R::NonNegative, 10000.0},
    {"EnableSteeringLimit", R::Flag, 0.0},
    {"LowerSteeringLimit", R::Any, -30.0},
    {"UpperSteeringLimit", R::Any, 30.0},
};
static_assert(std::size(kParams) == static_cast<std::size_t>(kJointParamCount), "kParams lists every JointParam");

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot vec3_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

LuaSlot param_slot(const JointParamInfo& info, double value) {
    return info.rule == R::Flag ? bool_slot(value != 0.0) : number_slot(value);
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("JointConstraint setters run on SimulationThread");
    }
}

}  // namespace

const JointParamInfo& joint_param_info(JointParam param) { return kParams[static_cast<std::size_t>(param)]; }

JointConstraint::JointConstraint(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
    : Constraint(tag, state, id) {
    reset_values();
}

void JointConstraint::reset_values() {
    for (int i = 0; i < kJointParamCount; ++i) {
        params_[static_cast<std::size_t>(i)] = kParams[i].default_value;
    }
    motor_velocity_ = Vec3{};
    reading_ = 0.0;
    ++settings_revision_;
    ++capture_revision_;
}

void JointConstraint::on_reuse() {
    Constraint::on_reuse();
    reset_values();
}

std::optional<std::string> JointConstraint::set_param(JointParam p, double value) {
    require_thread(*this);
    const JointParamInfo& info = joint_param_info(p);
    if (!std::isfinite(value)) {
        return std::string(info.name) + " must be a finite number";
    }
    switch (info.rule) {
        case R::Flag:
            value = value != 0.0 ? 1.0 : 0.0;
            break;
        case R::NonNegative:
            if (value < 0.0) {
                return std::string(info.name) + " must not be negative";
            }
            break;
        case R::Hinge:
            value = std::clamp(value, -kHingeLimit, kHingeLimit);
            break;
        case R::Cone:
            value = std::clamp(value, 0.0, kConeLimit);
            break;
        case R::Any:
            break;
    }
    double& slot = params_[static_cast<std::size_t>(p)];
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    ++settings_revision_;
    note_property_change(info.name, param_slot(info, previous), param_slot(info, value));
    return std::nullopt;
}

std::optional<std::string> JointConstraint::set_motor_velocity(Vec3 velocity) {
    require_thread(*this);
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z)) {
        return std::string("MotorVelocity must be finite");
    }
    if (velocity.x == motor_velocity_.x && velocity.y == motor_velocity_.y && velocity.z == motor_velocity_.z) {
        return std::nullopt;
    }
    const Vec3 previous = motor_velocity_;
    motor_velocity_ = velocity;
    ++settings_revision_;
    note_property_change("MotorVelocity", vec3_slot(previous), vec3_slot(velocity));
    return std::nullopt;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

template <JointParam P>
bool read_param(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* joint = dynamic_cast<const JointConstraint*>(&object);
    if (joint == nullptr) {
        return false;
    }
    out = param_slot(joint_param_info(P), joint->param(P));
    return true;
}

template <JointParam P>
bool write_param(DataModel&, DataModel& object, LuaSlot& in) {
    auto* joint = dynamic_cast<JointConstraint*>(&object);
    if (joint == nullptr) {
        return false;
    }
    const double value = joint_param_info(P).rule == R::Flag ? (in.flag ? 1.0 : 0.0) : in.number;
    return refuse(in, joint->set_param(P, value));
}

bool read_reading(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* joint = dynamic_cast<const JointConstraint*>(&object);
    if (joint == nullptr) {
        return false;
    }
    out = number_slot(object.simulation_running() ? joint->reading() : 0.0);
    return true;
}

bool read_motor_velocity(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* joint = dynamic_cast<const JointConstraint*>(&object);
    if (joint == nullptr) {
        return false;
    }
    out = vec3_slot(joint->motor_velocity());
    return true;
}

bool write_motor_velocity(DataModel&, DataModel& object, LuaSlot& in) {
    auto* joint = dynamic_cast<JointConstraint*>(&object);
    return joint != nullptr && refuse(in, joint->set_motor_velocity(in.vec));
}

// Each default as a save would write it.
const char* default_json(JointParam p) {
    static const std::array<std::string, kJointParamCount> json = [] {
        std::array<std::string, kJointParamCount> out;
        for (int i = 0; i < kJointParamCount; ++i) {
            const JointParamInfo& info = kParams[i];
            out[static_cast<std::size_t>(i)] = info.rule == R::Flag ? (info.default_value != 0.0 ? "true" : "false")
                                                                    : write_json(JsonValue::number(info.default_value));
        }
        return out;
    }();
    return json[static_cast<std::size_t>(p)].c_str();
}

template <JointParam P>
LuaField param_field() {
    const JointParamInfo& info = joint_param_info(P);
    return lua_saved_property(info.name, info.rule == R::Flag ? "boolean" : "number", read_param<P>, write_param<P>,
                              default_json(P));
}

LuaField reading_field(const char* name) { return lua_property(name, "number", false, read_reading, nullptr); }

template <std::size_t N>
void register_joint(const char* name, const LuaField (&fields)[N]) {
    register_lua_class(name, "JointConstraint", fields, static_cast<int>(N));
    register_suited_parents(name, {"Workspace", "PVInstance"});
}

ANARCHY_LUA_REGISTER(register_joint_constraint_lua) {
    using P = JointParam;
    static const std::string zero_vector = [] {
        const float zeros[3] = {0.f, 0.f, 0.f};
        return write_json(json_floats(zeros, 3));
    }();
    const LuaField joint_fields[] = {
        lua_group("Joint"),
        param_field<P::CollideConnected>(),
    };
    // Abstract: no factory registers it.
    register_lua_class("JointConstraint", "Constraint", joint_fields, static_cast<int>(std::size(joint_fields)));
    const LuaField revolute[] = {
        lua_group("Spring"), param_field<P::EnableSpring>(), param_field<P::Hertz>(),
        param_field<P::DampingRatio>(), param_field<P::TargetAngle>(),
        lua_group("Limit"), param_field<P::EnableLimit>(), param_field<P::LowerAngle>(),
        param_field<P::UpperAngle>(),
        lua_group("Motor"), param_field<P::EnableMotor>(), param_field<P::MotorSpeed>(),
        param_field<P::MaxMotorTorque>(),
        lua_group("State"), reading_field("Angle"),
    };
    register_joint("RevoluteConstraint", revolute);
    const LuaField prismatic[] = {
        lua_group("Spring"), param_field<P::EnableSpring>(), param_field<P::Hertz>(),
        param_field<P::DampingRatio>(), param_field<P::TargetTranslation>(),
        lua_group("Limit"), param_field<P::EnableLimit>(), param_field<P::LowerTranslation>(),
        param_field<P::UpperTranslation>(),
        lua_group("Motor"), param_field<P::EnableMotor>(), param_field<P::MotorSpeed>(),
        param_field<P::MaxMotorForce>(),
        lua_group("State"), reading_field("Translation"),
    };
    register_joint("PrismaticConstraint", prismatic);
    const LuaField distance[] = {
        lua_group("Distance"), param_field<P::Length>(),
        lua_group("Spring"), param_field<P::EnableSpring>(), param_field<P::Hertz>(),
        param_field<P::DampingRatio>(),
        lua_group("Limit"), param_field<P::EnableLimit>(), param_field<P::MinLength>(),
        param_field<P::MaxLength>(),
        lua_group("Motor"), param_field<P::EnableMotor>(), param_field<P::MotorSpeed>(),
        param_field<P::MaxMotorForce>(),
        lua_group("State"), reading_field("CurrentLength"),
    };
    register_joint("DistanceConstraint", distance);
    const LuaField spherical[] = {
        lua_group("Spring"), param_field<P::EnableSpring>(), param_field<P::Hertz>(),
        param_field<P::DampingRatio>(),
        lua_group("Limit"), param_field<P::EnableConeLimit>(), param_field<P::ConeAngle>(),
        param_field<P::EnableTwistLimit>(), param_field<P::LowerTwistAngle>(), param_field<P::UpperTwistAngle>(),
        lua_group("Motor"), param_field<P::EnableMotor>(),
        lua_saved_property("MotorVelocity", "Vector3", read_motor_velocity, write_motor_velocity,
                           zero_vector.c_str()),
        param_field<P::MaxMotorTorque>(),
    };
    register_joint("SphericalConstraint", spherical);
    const LuaField wheel[] = {
        lua_group("Suspension"), param_field<P::EnableSuspension>(), param_field<P::SuspensionHertz>(),
        param_field<P::SuspensionDampingRatio>(), param_field<P::EnableSuspensionLimit>(),
        param_field<P::LowerSuspensionLimit>(), param_field<P::UpperSuspensionLimit>(),
        lua_group("Spin"), param_field<P::EnableSpinMotor>(), param_field<P::SpinMotorSpeed>(),
        param_field<P::MaxSpinTorque>(),
        lua_group("Steering"), param_field<P::EnableSteering>(), param_field<P::SteeringHertz>(),
        param_field<P::SteeringDampingRatio>(), param_field<P::TargetSteeringAngle>(),
        param_field<P::MaxSteeringTorque>(), param_field<P::EnableSteeringLimit>(),
        param_field<P::LowerSteeringLimit>(), param_field<P::UpperSteeringLimit>(),
        lua_group("State"), reading_field("SpinSpeed"),
    };
    register_joint("WheelConstraint", wheel);
}

}  // namespace

}  // namespace engine_core
```

`src/engine_core/Project.cpp` — add `#include "JointConstraint.hpp"` beside `#include "Constraint.hpp"`, and after the `NoCollisionConstraint` entry (line 164):

```cpp
        out.push_back({"RevoluteConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<RevoluteConstraint>(); }});
        out.push_back({"PrismaticConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<PrismaticConstraint>(); }});
        out.push_back({"DistanceConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<DistanceConstraint>(); }});
        out.push_back({"SphericalConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<SphericalConstraint>(); }});
        out.push_back({"WheelConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<WheelConstraint>(); }});
```

`src/engine_core/ScriptBindings.cpp` — add `#include "JointConstraint.hpp"` beside `#include "Constraint.hpp"`; after `create_no_collision_constraint` (line 211):

```cpp
DataModel& create_revolute_constraint(DataModel& world) { return world.create<RevoluteConstraint>(); }
DataModel& create_prismatic_constraint(DataModel& world) { return world.create<PrismaticConstraint>(); }
DataModel& create_distance_constraint(DataModel& world) { return world.create<DistanceConstraint>(); }
DataModel& create_spherical_constraint(DataModel& world) { return world.create<SphericalConstraint>(); }
DataModel& create_wheel_constraint(DataModel& world) { return world.create<WheelConstraint>(); }
```

and after `register_lua_creatable("NoCollisionConstraint", ...)` (line 264):

```cpp
    register_lua_creatable("RevoluteConstraint", create_revolute_constraint);
    register_lua_creatable("PrismaticConstraint", create_prismatic_constraint);
    register_lua_creatable("DistanceConstraint", create_distance_constraint);
    register_lua_creatable("SphericalConstraint", create_spherical_constraint);
    register_lua_creatable("WheelConstraint", create_wheel_constraint);
```

`CMakeLists.txt` — add `src/engine_instances/JointConstraint.cpp` after `src/engine_instances/Constraint.cpp`.

- [ ] **Step 4: Run them to see them pass**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[joint],[no-collision],[assembly]"`
Expected: PASS. (`[no-collision]` and `[assembly]` guard the Constraint changes.) If JC6's `foreign=` line shows `true` (the bindings allow unknown members), ledger it as a finding about the binding layer and drop that one line: it tests existing behavior, not this task.

- [ ] **Step 5: Commit**

```bash
git add src/engine_instances/JointConstraint.hpp src/engine_instances/JointConstraint.cpp src/engine_instances/Constraint.hpp src/engine_instances/Constraint.cpp src/engine_core/Project.cpp src/engine_core/ScriptBindings.cpp sandbox/joint_constraint_tests.cpp CMakeLists.txt
git commit -m "Joints: RevoluteConstraint, PrismaticConstraint, DistanceConstraint, SphericalConstraint, WheelConstraint

Five constraints under a new abstract JointConstraint, with Box3D-named,
saved properties in one table: checked, clamped, scriptable, saved, and
copied with their ends.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Insert list, icons, docs

**Files:**
- Modify: `src/ide/ClassOrder.hpp:46`, `src/ide/InsertPopup.cpp:59`, `src/engine_core/LuaApi.cpp:1060-1072`, `src/engine_instances/README.md:15`, `resources/icons/ATTRIBUTION.md`
- Create: `resources/icons/{Revolute,Prismatic,Distance,Spherical,Wheel}Constraint.png` (drawn by a scratchpad script, not committed)
- Test: `tests/LuauCompleteTest.cpp` (`testInsertFilter`, `testVector3`)

**Interfaces:**
- Consumes: the five Lua classes (Task 2).

- [ ] **Step 1: Write the failing tests**

`tests/LuauCompleteTest.cpp`, `testInsertFilter` — replace the line

```cpp
        "Animator", "Attachment", "Bone", "NoCollisionConstraint", "RigidConstraint", "WeldConstraint",
```

with

```cpp
        "Animator", "Attachment", "Bone", "DistanceConstraint", "NoCollisionConstraint", "PrismaticConstraint",
        "RevoluteConstraint", "RigidConstraint", "SphericalConstraint", "WeldConstraint", "WheelConstraint",
```

and in `testVector3`, after the `touch_ended` checks:

```cpp
    const ide::CompletionList wheel = at_end("local wheel = Instance.new(\"WheelConstraint\")\nwheel.");
    expect_has(wheel, "SpinMotorSpeed", "WheelConstraint.SpinMotorSpeed");
    expect_has(wheel, "SpinSpeed", "WheelConstraint.SpinSpeed");
    expect_has(wheel, "CollideConnected", "WheelConstraint inherits JointConstraint.CollideConnected");
    expect_has(wheel, "Enabled", "WheelConstraint inherits Constraint.Enabled");
    expect_missing(wheel, "LowerAngle", "a WheelConstraint has no LowerAngle");
    const ide::CompletionList hinge = at_end("local hinge = Instance.new(\"RevoluteConstraint\")\nhinge.");
    expect_has(hinge, "MaxMotorTorque", "RevoluteConstraint.MaxMotorTorque");
    expect_has(hinge, "Angle", "RevoluteConstraint.Angle");
```

- [ ] **Step 2: Run them to see them fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target engine-tests -- /m /v:minimal && build/Release/engine-tests.exe`
Expected: FAIL in `testInsertFilter` (the five are not in cluster 10, so they sort elsewhere). The completion checks may already pass (Task 2 registered the classes); that is fine — they guard this task's doc edits.

- [ ] **Step 3: Implement**

`src/ide/ClassOrder.hpp` — after `{"NoCollisionConstraint", 10},`:

```cpp
        {"RevoluteConstraint", 10},
        {"PrismaticConstraint", 10},
        {"DistanceConstraint", 10},
        {"SphericalConstraint", 10},
        {"WheelConstraint", 10},
```

`src/ide/InsertPopup.cpp` — after the `NoCollisionConstraint` row:

```cpp
        {"RevoluteConstraint", "Lets one object turn about an axis on another: a hinge, an axle."},
        {"PrismaticConstraint", "Lets one object slide along an axis on another."},
        {"DistanceConstraint", "Holds two objects a distance apart: a rod, a rope, or a spring."},
        {"SphericalConstraint", "Lets one object turn freely about a point on another: a ball joint."},
        {"WheelConstraint", "A car wheel: it spins, rides on a suspension, and can steer."},
```

`src/engine_core/LuaApi.cpp` — replace the `Constraint` `PVInstance1` and `PVInstance2` docs with:

```cpp
    add("Constraint", "PVInstance1",
        "One end. For a weld, rigid, or no-collision constraint, which end is which does not matter: the end on "
        "the assembly root's side stays, and a RigidConstraint snaps the other so its pivot is on this one's. For "
        "a joint (RevoluteConstraint and the others), this is the base.",
        "PVInstance?", false, {});
    add("Constraint", "PVInstance2",
        "The other end. For a joint (RevoluteConstraint and the others), this is the moving part: the joint sits "
        "at its pivot and turns or slides along its Y axis.",
        "PVInstance?", false, {});
```

and after the `Constraint` `Active` doc:

```cpp
    // Joint constraints (docs/superpowers/specs/2026-10-11-joint-constraints-design.md).
    {
        struct JointDoc {
            const char* owner;
            const char* name;
            const char* summary;
            const char* type;
        };
        const char* const hertz = "The spring's stiffness, in cycles per second.";
        const char* const damping = "How fast the spring settles: 1 settles without overshooting.";
        const char* const torque = "The most torque the motor uses.";
        const char* const force = "The most force the motor uses.";
        const JointDoc joint_docs[] = {
            {"JointConstraint", "CollideConnected",
             "When true, the two ends collide with each other. Off by default, so a wheel does not rub its wheel "
             "well.",
             "boolean"},
            {"RevoluteConstraint", "EnableSpring", "Pulls the angle toward TargetAngle, like a spring.", "boolean"},
            {"RevoluteConstraint", "Hertz", hertz, "number"},
            {"RevoluteConstraint", "DampingRatio", damping, "number"},
            {"RevoluteConstraint", "TargetAngle", "The angle the spring pulls toward, in degrees. 0 is as placed.",
             "number"},
            {"RevoluteConstraint", "EnableLimit", "Keeps the angle between LowerAngle and UpperAngle.", "boolean"},
            {"RevoluteConstraint", "LowerAngle", "The lowest angle, in degrees, down to -178.2.", "number"},
            {"RevoluteConstraint", "UpperAngle", "The highest angle, in degrees, up to 178.2.", "number"},
            {"RevoluteConstraint", "EnableMotor", "Turns it at MotorSpeed.", "boolean"},
            {"RevoluteConstraint", "MotorSpeed", "The motor's speed, in degrees per second.", "number"},
            {"RevoluteConstraint", "MaxMotorTorque", torque, "number"},
            {"RevoluteConstraint", "Angle",
             "How far it has turned from as placed, in degrees, while the place runs. Read-only.", "number"},
            {"PrismaticConstraint", "EnableSpring", "Pulls the slide toward TargetTranslation, like a spring.",
             "boolean"},
            {"PrismaticConstraint", "Hertz", hertz, "number"},
            {"PrismaticConstraint", "DampingRatio", damping, "number"},
            {"PrismaticConstraint", "TargetTranslation",
             "Where along the axis the spring pulls toward, in units. 0 is as placed.", "number"},
            {"PrismaticConstraint", "EnableLimit", "Keeps the slide between LowerTranslation and UpperTranslation.",
             "boolean"},
            {"PrismaticConstraint", "LowerTranslation", "The lowest place along the axis, in units.", "number"},
            {"PrismaticConstraint", "UpperTranslation", "The highest place along the axis, in units.", "number"},
            {"PrismaticConstraint", "EnableMotor", "Slides it at MotorSpeed.", "boolean"},
            {"PrismaticConstraint", "MotorSpeed", "The motor's speed, in units per second.", "number"},
            {"PrismaticConstraint", "MaxMotorForce", force, "number"},
            {"PrismaticConstraint", "Translation",
             "How far it has slid from as placed, in units, while the place runs. Read-only.", "number"},
            {"DistanceConstraint", "Length",
             "The distance it holds, in units. 0 holds the distance the ends were at when the joint started.",
             "number"},
            {"DistanceConstraint", "EnableSpring", "Makes the distance springy instead of rigid.", "boolean"},
            {"DistanceConstraint", "Hertz", hertz, "number"},
            {"DistanceConstraint", "DampingRatio", damping, "number"},
            {"DistanceConstraint", "EnableLimit", "Keeps the distance between MinLength and MaxLength, as a rope.",
             "boolean"},
            {"DistanceConstraint", "MinLength", "The shortest distance, in units.", "number"},
            {"DistanceConstraint", "MaxLength", "The longest distance, in units.", "number"},
            {"DistanceConstraint", "EnableMotor", "Lengthens or shortens it at MotorSpeed.", "boolean"},
            {"DistanceConstraint", "MotorSpeed", "The motor's speed, in units per second.", "number"},
            {"DistanceConstraint", "MaxMotorForce", force, "number"},
            {"DistanceConstraint", "CurrentLength",
             "The distance between the ends now, in units, while the place runs. Read-only.", "number"},
            {"SphericalConstraint", "EnableSpring", "Pulls it back toward its rotation as placed, like a spring.",
             "boolean"},
            {"SphericalConstraint", "Hertz", hertz, "number"},
            {"SphericalConstraint", "DampingRatio", damping, "number"},
            {"SphericalConstraint", "EnableConeLimit", "Keeps its axis within ConeAngle of where it was placed.",
             "boolean"},
            {"SphericalConstraint", "ConeAngle", "The cone's half-angle, in degrees, from 0 to 90.", "number"},
            {"SphericalConstraint", "EnableTwistLimit",
             "Keeps the twist about its axis between LowerTwistAngle and UpperTwistAngle.", "boolean"},
            {"SphericalConstraint", "LowerTwistAngle", "The lowest twist, in degrees, down to -178.2.", "number"},
            {"SphericalConstraint", "UpperTwistAngle", "The highest twist, in degrees, up to 178.2.", "number"},
            {"SphericalConstraint", "EnableMotor", "Turns it at MotorVelocity.", "boolean"},
            {"SphericalConstraint", "MotorVelocity",
             "The motor's turning speed about each world axis, in degrees per second.", "Vector3"},
            {"SphericalConstraint", "MaxMotorTorque", torque, "number"},
            {"WheelConstraint", "EnableSuspension", "Rides the wheel on a spring along the chassis's up.", "boolean"},
            {"WheelConstraint", "SuspensionHertz", "The suspension's stiffness, in cycles per second.", "number"},
            {"WheelConstraint", "SuspensionDampingRatio", damping, "number"},
            {"WheelConstraint", "EnableSuspensionLimit",
             "Keeps the suspension's travel between LowerSuspensionLimit and UpperSuspensionLimit.", "boolean"},
            {"WheelConstraint", "LowerSuspensionLimit", "The lowest travel, in units.", "number"},
            {"WheelConstraint", "UpperSuspensionLimit", "The highest travel, in units.", "number"},
            {"WheelConstraint", "EnableSpinMotor", "Spins the wheel at SpinMotorSpeed.", "boolean"},
            {"WheelConstraint", "SpinMotorSpeed",
             "The spin motor's speed about the wheel's Y axis, in degrees per second.", "number"},
            {"WheelConstraint", "MaxSpinTorque", torque, "number"},
            {"WheelConstraint", "EnableSteering",
             "Turns the wheel toward TargetSteeringAngle. Off, it is held facing forward.", "boolean"},
            {"WheelConstraint", "SteeringHertz", "The steering's stiffness, in cycles per second.", "number"},
            {"WheelConstraint", "SteeringDampingRatio", damping, "number"},
            {"WheelConstraint", "TargetSteeringAngle",
             "The steering angle about the chassis's up, in degrees. 0 is as placed.", "number"},
            {"WheelConstraint", "MaxSteeringTorque", "The most torque the steering uses.", "number"},
            {"WheelConstraint", "EnableSteeringLimit",
             "Keeps the steering between LowerSteeringLimit and UpperSteeringLimit.", "boolean"},
            {"WheelConstraint", "LowerSteeringLimit", "The lowest steering angle, in degrees.", "number"},
            {"WheelConstraint", "UpperSteeringLimit", "The highest steering angle, in degrees.", "number"},
            {"WheelConstraint", "SpinSpeed",
             "How fast the wheel spins now, in degrees per second, while the place runs. Read-only.", "number"},
        };
        for (const JointDoc& doc : joint_docs) {
            add(doc.owner, doc.name, doc.summary, doc.type, false, {});
        }
    }
```

`src/engine_instances/README.md` — append to the constraints paragraph (line 15), after the NoCollisionConstraint sentence and before "See `docs/...assemblies-design.md`":

```markdown
The joint constraints, `RevoluteConstraint`, `PrismaticConstraint`, `DistanceConstraint`, `SphericalConstraint`, and `WheelConstraint` (`JointConstraint.{hpp,cpp}`, under the abstract `JointConstraint`), also join nothing: each links its two ends' assemblies with a moving Box3D joint in play. PVInstance1 is the base and PVInstance2 the moving part; the joint sits at PVInstance2's pivot, held where placed, and turns or slides along its Y. Their properties are Box3D's, in degrees and units, held in one table (`JointParam`); `Assemblies::joints()` lists the active ones and PhysicsWorld makes and drives them (`docs/superpowers/specs/2026-10-11-joint-constraints-design.md`).
```

Icons — write `jc_icons.py` in the scratchpad (stdlib only, the same renderer as the NoCollisionConstraint icon) and run `py <scratchpad>/jc_icons.py resources/icons`:

```python
"""Draws the joint constraint icons: 32x32 RGBA, Fugue style, like
WeldConstraint.png and NoCollisionConstraint.png. Anti-aliased by 4x4
supersampling of signed distances. Stdlib only.
Usage: py jc_icons.py <resources/icons>
"""
import math
import struct
import sys
import zlib

SIZE = 32
SS = 4
OUTLINE = (58, 66, 74)
LIGHT = (232, 236, 240)
DARK = (128, 138, 148)
BLUE = (66, 133, 214)
BLUE_OUT = (28, 64, 120)
TIRE = (70, 76, 84)


def segment(px, py, ax, ay, bx, by, width):
    vx, vy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - ax - vx * t, py - ay - vy * t) - width / 2


def line(ax, ay, bx, by, width):
    return lambda px, py: segment(px, py, ax, ay, bx, by, width)


def polyline(points, width):
    def d(px, py):
        return min(segment(px, py, *points[i], *points[i + 1], width) for i in range(len(points) - 1))
    return d


def circle(cx, cy, r):
    return lambda px, py: math.hypot(px - cx, py - cy) - r


def ring(cx, cy, r, width):
    return lambda px, py: abs(math.hypot(px - cx, py - cy) - r) - width / 2


def box(cx, cy, hx, hy, radius=1.5):
    def d(px, py):
        dx = abs(px - cx) - (hx - radius)
        dy = abs(py - cy) - (hy - radius)
        return math.hypot(max(dx, 0.0), max(dy, 0.0)) + min(max(dx, dy), 0.0) - radius
    return d


def clip(sdf, keep):
    return lambda px, py: sdf(px, py) if keep(px, py) else 1.0


def zigzag(ax, ay, bx, by, teeth, amplitude):
    length = math.hypot(bx - ax, by - ay)
    ux, uy = (bx - ax) / length, (by - ay) / length
    nx, ny = -uy, ux
    points = [(ax, ay)]
    for i in range(1, 2 * teeth):
        t = i / (2 * teeth)
        side = amplitude if i % 2 else -amplitude
        points.append((ax + ux * length * t + nx * side, ay + uy * length * t + ny * side))
    points.append((bx, by))
    return points


def layer(sdf, fill, outline, shade=False, top=0.0, height=32.0):
    def f(px, py):
        d = sdf(px, py)
        if d > 0.0:
            return None
        if d > -1.0:
            return outline + (1.0,)
        if shade:
            t = max(0.0, min(1.0, (py - top) / height))
            return tuple(LIGHT[i] + (DARK[i] - LIGHT[i]) * t for i in range(3)) + (1.0,)
        return fill + (1.0,)
    return f


def grey(sdf, top=0.0, height=32.0):
    return layer(sdf, LIGHT, OUTLINE, True, top, height)


def blue(sdf):
    return layer(sdf, BLUE, BLUE_OUT)


def render(layers):
    pixels = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS
                    py = y + (sy + 0.5) / SS
                    color = (0.0, 0.0, 0.0, 0.0)
                    for draw in layers:
                        top = draw(px, py)
                        if top is not None:
                            a = top[3]
                            color = (top[0] * a + color[0] * (1 - a), top[1] * a + color[1] * (1 - a),
                                     top[2] * a + color[2] * (1 - a), a + color[3] * (1 - a))
                    acc[0] += color[0] * color[3]
                    acc[1] += color[1] * color[3]
                    acc[2] += color[2] * color[3]
                    acc[3] += color[3]
            n = SS * SS
            alpha = acc[3] / n
            rgb = [acc[i] / acc[3] for i in range(3)] if alpha > 0 else [0, 0, 0]
            row.append((int(round(rgb[0])), int(round(rgb[1])), int(round(rgb[2])), int(round(alpha * 255))))
        pixels.append(row)
    return pixels


def write_png(path, pixels):
    raw = b''.join(b'\x00' + bytes(c for p in row for c in p) for row in pixels)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


ICONS = {
    # Two grey arms joined by a blue pin.
    'RevoluteConstraint': [grey(line(16, 16, 27, 6, 6)), grey(line(16, 16, 5, 26, 6)), blue(circle(16, 16, 4.5))],
    # A grey block on a grey rail, under a blue arrow along it.
    'PrismaticConstraint': [grey(line(3, 25, 29, 25, 3)), grey(box(16, 16, 7, 6), 10, 12),
                            blue(polyline([(6, 5), (26, 5)], 2.4)), blue(polyline([(9.5, 2), (6, 5), (9.5, 8)], 2.4)),
                            blue(polyline([(22.5, 2), (26, 5), (22.5, 8)], 2.4))],
    # Two grey balls joined by a blue spring.
    'DistanceConstraint': [blue(polyline(zigzag(9, 23, 23, 9, 4, 3.0), 2.2)), grey(circle(7, 25, 5), 20, 10),
                           grey(circle(25, 7, 5), 2, 10)],
    # A blue ball in a grey socket, on a grey stem.
    'SphericalConstraint': [grey(line(16, 13, 16, 2, 4)), blue(circle(16, 18, 6.5)),
                            grey(clip(ring(16, 18, 10, 3.5), lambda px, py: py >= 17), 17, 12)],
    # A dark tire with a grey hub, on a blue spring.
    'WheelConstraint': [blue(polyline(zigzag(16, 1, 16, 10, 3, 3.0), 2.2)), layer(ring(16, 20, 8.5, 5), TIRE, OUTLINE),
                        grey(circle(16, 20, 3.5), 16, 8)],
}

out = sys.argv[1]
for name, layers in ICONS.items():
    write_png(out + '/' + name + '.png', render(layers))
print('ok')
```

Open each PNG with `Start-Process` (memory: never VS Code) to check it reads at 32 and 16 pixels; adjust the shapes if one is unreadable.

`resources/icons/ATTRIBUTION.md` — append:

```markdown

RevoluteConstraint.png, PrismaticConstraint.png, DistanceConstraint.png,
SphericalConstraint.png, and WheelConstraint.png are our own, in the same
style: two grey arms on a blue pin; a grey block on a rail under a blue arrow;
two grey balls joined by a blue spring; a blue ball in a grey socket; and a
dark tire with a grey hub on a blue spring.
```

- [ ] **Step 4: Run them to see them pass**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target engine-tests -- /m /v:minimal && build/Release/engine-tests.exe`
Expected: PASS (`testInsertFilter`, `testVector3`). Then `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target AnarchyStudio -- /m /v:minimal` builds clean.

- [ ] **Step 5: Commit**

```bash
git add src/ide/ClassOrder.hpp src/ide/InsertPopup.cpp src/engine_core/LuaApi.cpp src/engine_instances/README.md resources/icons tests/LuauCompleteTest.cpp
git commit -m "Joints: in the Insert list with icons, documented for completion

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Which joints are active (Assemblies)

**Files:**
- Modify: `src/engine_core/Assemblies.hpp`, `src/engine_core/Assemblies.cpp:160-200` (reserve), `:530-720` (rebuild)
- Create: `sandbox/joint_tests.cpp`
- Modify: `CMakeLists.txt` (sandbox list)

**Interfaces:**
- Consumes: `Constraint::joint()` (Task 2).
- Produces: `struct Assemblies::JointEdge { InstanceId constraint; InstanceId node1; InstanceId node2; };` and `const std::vector<JointEdge>& Assemblies::joints() const` — active joints, by constraint id ascending, node1 = PVInstance1's node.

- [ ] **Step 1: Write the failing tests**

`sandbox/joint_tests.cpp` (Tasks 5 to 7 append to it):

```cpp
// Joint constraints in Assemblies and in play
// (docs/superpowers/specs/2026-10-11-joint-constraints-design.md §2, §3, §6).

#include "physics_rig.hpp"
#include "support.hpp"

#include "alloc_counter.hpp"

#include "Assemblies.hpp"
#include "Attachment.hpp"
#include "Constraint.hpp"
#include "GameObject.hpp"
#include "JointConstraint.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

namespace {

using engine_core::DataModel;
using engine_core::InstanceId;
using engine_core::JointConstraint;
using engine_core::JointParam;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

constexpr double kPi = 3.14159265358979323846;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

template <class T>
T& joint(DataModel& game, InstanceId base, InstanceId moving) {
    T& made = game.create<T>();
    REQUIRE_FALSE(made.set_pv_instance1(instance_slot(base)));
    REQUIRE_FALSE(made.set_pv_instance2(instance_slot(moving)));
    game.set_parent(made.id(), workspace_of(game));
    return made;
}

engine_core::WeldConstraint& weld(DataModel& game, InstanceId a, InstanceId b) {
    engine_core::WeldConstraint& made = game.create<engine_core::WeldConstraint>();
    REQUIRE_FALSE(made.set_pv_instance1(instance_slot(a)));
    REQUIRE_FALSE(made.set_pv_instance2(instance_slot(b)));
    game.set_parent(made.id(), workspace_of(game));
    return made;
}

bool active(DataModel& game, const engine_core::Constraint& constraint) {
    game.assemblies().refresh(game);
    return game.assemblies().active(constraint.id());
}

void set(JointConstraint& joint, JointParam param, double value) { REQUIRE_FALSE(joint.set_param(param, value)); }

void no_gravity(DataModel& game) {
    auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    REQUIRE_FALSE(workspace->set_gravity(0.0));
}

// where, turned by degrees about axis.
Matrix4 turned(const Matrix4& where, Vec3 axis, double degrees) {
    return engine_core::matrix4_multiply(where, engine_core::matrix4_axis_angle(axis, degrees * kPi / 180.0));
}

Vec3 column(const Matrix4& m, int c) { return Vec3{m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }

float distance(const Matrix4& a, const Matrix4& b) {
    const float dx = a.m[12] - b.m[12];
    const float dy = a.m[13] - b.m[13];
    const float dz = a.m[14] - b.m[14];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float degrees_between(Vec3 a, Vec3 b) {
    const float la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const float lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    const float cosine = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
    return static_cast<float>(std::acos(std::max(-1.f, std::min(1.f, cosine))) * 180.0 / kPi);
}

bool finite(const Matrix4& m) {
    for (const float v : m.m) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("JA1 a joint between two bodies is Active, listed base first, and joins nothing", "[joint][assembly]") {
    PhysicsRig rig;
    PhysicsObject& chassis = rig.body(at(0.f, 2.f, 0.f), Vec3{2.f, 1.f, 2.f}, false);
    PhysicsObject& wheel = rig.body(at(2.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, chassis.id(), wheel.id());
    REQUIRE(active(rig.game, hinge));
    const engine_core::Assemblies& assemblies = rig.game.assemblies();
    REQUIRE(assemblies.joints().size() == 1);
    REQUIRE(assemblies.joints()[0].constraint == hinge.id());
    REQUIRE(assemblies.joints()[0].node1 == chassis.id());
    REQUIRE(assemblies.joints()[0].node2 == wheel.id());
    REQUIRE(assemblies.group_of(chassis.id()) == -1);
    REQUIRE(assemblies.root_of(rig.game, wheel.id()) == wheel.id());
    // Order matters for a joint: swapping the ends swaps the nodes.
    REQUIRE_FALSE(hinge.set_pv_instance1(instance_slot(wheel.id())));
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(chassis.id())));
    REQUIRE(active(rig.game, hinge));
    REQUIRE(rig.game.assemblies().joints()[0].node1 == wheel.id());
}

TEST_CASE("JA2 a joint is inactive disabled, outside Workspace, inside one assembly, between two anchored "
          "assemblies, or at a controller, a plain object, or Terrain",
          "[joint][assembly]") {
    PhysicsRig rig;
    PhysicsObject& a = rig.body(at(0.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& b = rig.body(at(2.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, a.id(), b.id());
    REQUIRE(active(rig.game, hinge));
    REQUIRE_FALSE(hinge.set_enabled(false));
    REQUIRE_FALSE(active(rig.game, hinge));
    REQUIRE_FALSE(hinge.set_enabled(true));
    rig.game.set_parent(hinge.id(), rig.game.scene_service("Storage"));
    REQUIRE_FALSE(active(rig.game, hinge));
    rig.game.set_parent(hinge.id(), workspace_of(rig.game));
    REQUIRE(active(rig.game, hinge));
    engine_core::WeldConstraint& welded = weld(rig.game, a.id(), b.id());
    REQUIRE_FALSE(active(rig.game, hinge));
    rig.game.destroy(welded.id());
    REQUIRE(active(rig.game, hinge));
    a.set_anchored(true);
    REQUIRE(active(rig.game, hinge));
    b.set_anchored(true);
    REQUIRE_FALSE(active(rig.game, hinge));
    b.set_anchored(false);
    REQUIRE(active(rig.game, hinge));
    engine_core::PlayerController& player = rig.controller(at(5.f, 0.f, 0.f));
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(player.id())));
    REQUIRE_FALSE(active(rig.game, hinge));
    engine_core::GameObject& plain = create_part(rig.game);
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(plain.id())));
    REQUIRE_FALSE(active(rig.game, hinge));
    engine_core::Terrain& terrain = rig.game.create<engine_core::Terrain>();
    rig.game.set_parent(terrain.id(), workspace_of(rig.game));
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(terrain.id())));
    REQUIRE_FALSE(active(rig.game, hinge));
    REQUIRE_FALSE(hinge.set_pv_instance2(instance_slot(b.id())));
    REQUIRE(active(rig.game, hinge));
}

TEST_CASE("JA3 an Attachment end stands for its holder; a welded assembly's anchoring and controller count",
          "[joint][assembly]") {
    PhysicsRig rig;
    PhysicsObject& chassis = rig.body(at(0.f, 2.f, 0.f), Vec3{2.f, 1.f, 2.f}, false);
    PhysicsObject& wheel = rig.body(at(2.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    engine_core::Attachment& axle = rig.game.create<engine_core::Attachment>();
    rig.game.set_parent(axle.id(), wheel.id());
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, chassis.id(), axle.id());
    REQUIRE(active(rig.game, hinge));
    REQUIRE(rig.game.assemblies().joints()[0].node2 == wheel.id());
    // The wheel welded to an anchored stand: its assembly is anchored, the chassis's is not.
    PhysicsObject& stand = rig.body(at(4.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    weld(rig.game, wheel.id(), stand.id());
    REQUIRE(active(rig.game, hinge));
    chassis.set_anchored(true);
    REQUIRE_FALSE(active(rig.game, hinge));
    chassis.set_anchored(false);
    // The chassis welded to a PlayerController: the controller is its root.
    engine_core::PlayerController& player = rig.controller(at(-3.f, 0.f, 0.f));
    weld(rig.game, chassis.id(), player.id());
    REQUIRE_FALSE(active(rig.game, hinge));
}

TEST_CASE("JA4 joints are listed by constraint id, and read the same in play", "[joint][assembly]") {
    PhysicsRig rig;
    PhysicsObject& a = rig.body(at(0.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& b = rig.body(at(2.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& c = rig.body(at(4.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& first = joint<engine_core::WheelConstraint>(rig.game, b.id(), c.id());
    auto& second = joint<engine_core::DistanceConstraint>(rig.game, a.id(), b.id());
    rig.game.assemblies().refresh(rig.game);
    const auto& joints = rig.game.assemblies().joints();
    REQUIRE(joints.size() == 2);
    REQUIRE(joints[0].constraint == std::min(first.id(), second.id()));
    REQUIRE(joints[1].constraint == std::max(first.id(), second.id()));
    rig.play();
    REQUIRE(active(rig.game, first));
    REQUIRE(active(rig.game, second));
}
```

Add `sandbox/joint_tests.cpp` to the sandbox list. Declare `JointEdge` and `joints()` (returning an always-empty `joints_`) in `Assemblies.hpp` so the file compiles.

- [ ] **Step 2: Run them to see them fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[joint][assembly]"`
Expected: FAIL — `active` is false and `joints()` empty for every joint.

- [ ] **Step 3: Implement**

`src/engine_core/Assemblies.hpp` — in `public:`, after `RigidEdge`:

```cpp
    // An active JointConstraint (joint spec §2): node1 is PVInstance1's node
    // (the base), node2 PVInstance2's. It joins nothing.
    struct JointEdge {
        InstanceId constraint = 0;
        InstanceId node1 = 0;
        InstanceId node2 = 0;
    };
```

after `no_collide_pairs()`:

```cpp
    // Active JointConstraints, by constraint id: two different assemblies,
    // each with a body, not both anchored, and no PlayerController at either
    // end or root. They join nothing.
    const std::vector<JointEdge>& joints() const { return joints_; }
```

in `private:`, after `orient_rigid`:

```cpp
    // Whether a joint between node1 and node2 holds, once groups_ is built.
    bool joint_holds(const DataModel& game, InstanceId node1, InstanceId node2) const;
```

and after `no_collide_;`: `std::vector<JointEdge> joints_;`

`src/engine_core/Assemblies.cpp` — in the constructor, after `no_collide_.reserve(slots);`: `joints_.reserve(slots);`. In `rebuild`, after `no_collide_.clear();`: `joints_.clear();`. In the constraint loop, replace the `if (!constraint->joins()) {` block's first line so it reads:

```cpp
        if (!constraint->joins()) {
            if (constraint->joint()) {
                // A joint joins nothing; whether it holds needs the assemblies (below).
                active_[id_slot(id)] = 0;
                if (constraint->enabled() && node1 != 0 && node2 != 0 && node1 != node2) {
                    joints_.push_back(JointEdge{id, node1, node2});
                }
                continue;
            }
            // A NoCollisionConstraint joins nothing: it only keeps two bodies apart.
```

(the rest of the no-collide block unchanged). After the loop that sets every `group.root` (just before `orient_rigid(game);`):

```cpp
    // Joints, now that assemblies are known (joint spec §2).
    joints_.erase(std::remove_if(joints_.begin(), joints_.end(),
                                 [&](const JointEdge& joint) { return !joint_holds(game, joint.node1, joint.node2); }),
                  joints_.end());
    std::sort(joints_.begin(), joints_.end(),
              [](const JointEdge& a, const JointEdge& b) { return a.constraint < b.constraint; });
    for (const JointEdge& joint : joints_) {
        active_[id_slot(joint.constraint)] = 1;
    }
```

and the helper, beside `root_before`:

```cpp
bool Assemblies::joint_holds(const DataModel& game, InstanceId node1, InstanceId node2) const {
    const std::int32_t group1 = group_[id_slot(node1)];
    const std::int32_t group2 = group_[id_slot(node2)];
    if (group1 >= 0 && group1 == group2) {
        return false;
    }
    // One end's assembly: it needs a body, and no PlayerController at the end or as its root.
    const auto side = [&](InstanceId node, std::int32_t group, bool& anchored) {
        const PhysicsBase* body = body_of(game, node);
        InstanceId root = node;
        bool physics = body != nullptr;
        anchored = body != nullptr && body->anchored();
        if (group >= 0) {
            const Group& assembly = groups_[static_cast<std::size_t>(group)];
            root = assembly.root;
            physics = assembly.has_physics;
            anchored = assembly.anchored;
        }
        return physics && dynamic_cast<const PlayerController*>(body) == nullptr &&
               dynamic_cast<const PlayerController*>(body_of(game, root)) == nullptr;
    };
    bool anchored1 = false;
    bool anchored2 = false;
    if (!side(node1, group1, anchored1) || !side(node2, group2, anchored2)) {
        return false;
    }
    return !(anchored1 && anchored2);
}
```

- [ ] **Step 4: Run them to see them pass**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[joint],[assembly],[no-collision],[constraint]"`
Expected: PASS. If JA2's anchored step fails because `set_anchored` did not move the revision for a joint end, the end is not `kLinked`: check `resolve_end(game, end, true)` runs for joint ends (it does, above the `joins()` branch) before changing anything else.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/Assemblies.hpp src/engine_core/Assemblies.cpp sandbox/joint_tests.cpp CMakeLists.txt
git commit -m "Joints: Assemblies lists the active ones

A joint holds between two different assemblies, each with a body, not both
anchored, with no PlayerController at either end. Active reads the same in
the editor and in play.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Box3D joints in play

**Files:**
- Modify: `src/engine_core/PhysicsWorld.hpp` (test accessors), `src/engine_core/PhysicsWorld.cpp` (Impl state, reset, sync, joint functions)
- Test: `sandbox/joint_tests.cpp`

**Interfaces:**
- Consumes: `Assemblies::joints()`, `JointEdge` (Task 4); `JointConstraint` (Task 2); `joint_frames` (Task 1); `Assemblies::end_pivot`, `node_frame`, `play_of`, `play()`.
- Produces: `std::size_t PhysicsWorld::joint_count() const;` `std::uint64_t PhysicsWorld::joint_key(InstanceId constraint) const;` (b3StoreJointId, 0 when none). Impl: `struct Joint`, `joints`, `update_joints`, `capture_joint`, `make_joint`, `apply_joint` (Task 6 fills its body), `drop_joints`, `body_for_node`.

- [ ] **Step 1: Write the failing tests** (append to `sandbox/joint_tests.cpp`)

```cpp
TEST_CASE("JP1 a jointed part hangs where it was placed from an anchored base", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    // A Revolute and a Spherical hold their pivot (the part's centre); a
    // Prismatic slides across gravity; a Distance hangs straight down.
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& balled = rig.body(at(-3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& slid = rig.body(turned(at(0.f, 5.f, 3.f), Vec3{0.f, 0.f, 1.f}, 90.0), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& hung = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    joint<engine_core::SphericalConstraint>(rig.game, base.id(), balled.id());
    joint<engine_core::PrismaticConstraint>(rig.game, base.id(), slid.id());
    joint<engine_core::DistanceConstraint>(rig.game, base.id(), hung.id());
    const Matrix4 placed[4] = {hinged.transform(), balled.transform(), slid.transform(), hung.transform()};
    rig.play();
    rig.seconds(1.0);
    REQUIRE(rig.physics.joint_count() == 4);
    REQUIRE(distance(hinged.transform(), placed[0]) < 0.02f);
    REQUIRE(distance(balled.transform(), placed[1]) < 0.02f);
    REQUIRE(distance(slid.transform(), placed[2]) < 0.02f);
    REQUIRE(distance(hung.transform(), placed[3]) < 0.02f);
}

TEST_CASE("JP2 with gravity off, nothing jointed moves at Play, however its parts are turned", "[joint][physics]") {
    PhysicsRig rig;
    no_gravity(rig.game);
    PhysicsObject& base = rig.body(turned(at(0.f, 2.f, 0.f), Vec3{0.f, 1.f, 0.f}, 25.0), Vec3{2.f, 1.f, 2.f}, false);
    std::vector<PhysicsObject*> parts;
    for (int i = 0; i < 5; ++i) {
        parts.push_back(&rig.body(turned(at(3.f * static_cast<float>(i) - 6.f, 4.f, 2.f), Vec3{1.f, 0.f, 0.f},
                                         30.0 + 10.0 * i),
                                  Vec3{1.f, 1.f, 1.f}, false));
    }
    joint<engine_core::RevoluteConstraint>(rig.game, base.id(), parts[0]->id());
    joint<engine_core::PrismaticConstraint>(rig.game, base.id(), parts[1]->id());
    joint<engine_core::DistanceConstraint>(rig.game, base.id(), parts[2]->id());
    joint<engine_core::SphericalConstraint>(rig.game, base.id(), parts[3]->id());
    joint<engine_core::WheelConstraint>(rig.game, base.id(), parts[4]->id());
    std::vector<Matrix4> placed;
    for (PhysicsObject* part : parts) {
        placed.push_back(part->transform());
    }
    const Matrix4 base_placed = base.transform();
    rig.play();
    rig.seconds(1.0);
    REQUIRE(rig.physics.joint_count() == 5);
    REQUIRE(distance(base.transform(), base_placed) < 1e-3f);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        INFO(i);
        REQUIRE(distance(parts[i]->transform(), placed[i]) < 1e-3f);
        REQUIRE(degrees_between(column(parts[i]->transform(), 1), column(placed[i], 1)) < 0.1f);
    }
}

TEST_CASE("JP3 a weld made in play remakes the joint on the new body, without a jump", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& extra = rig.body(at(3.f, 6.f, 0.f), Vec3{0.5f, 0.5f, 0.5f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    const Matrix4 placed = hinged.transform();
    rig.play();
    rig.seconds(0.5);
    const std::uint64_t before = rig.physics.joint_key(hinge.id());
    REQUIRE(before != 0);
    weld(rig.game, hinged.id(), extra.id());
    rig.seconds(1.0);
    REQUIRE(rig.physics.joint_count() == 1);
    REQUIRE(rig.physics.joint_key(hinge.id()) != before);
    REQUIRE(distance(hinged.transform(), placed) < 0.02f);
}

TEST_CASE("JP4 an end destroyed in play takes its joint with it", "[joint][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    rig.play();
    rig.seconds(0.5);
    REQUIRE(rig.physics.joint_count() == 1);
    rig.game.destroy(hinged.id());
    rig.seconds(0.5);
    REQUIRE(rig.physics.joint_count() == 0);
    REQUIRE(rig.physics.joint_key(hinge.id()) == 0);
    REQUIRE_FALSE(active(rig.game, hinge));
}

TEST_CASE("JP5 a weld made in play between a joint's two ends ends the joint", "[joint][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& a = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& b = rig.body(at(2.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, a.id(), b.id());
    rig.play();
    rig.seconds(0.25);
    REQUIRE(rig.physics.joint_count() == 1);
    weld(rig.game, a.id(), b.id());
    rig.seconds(1.0);
    REQUIRE(rig.physics.joint_count() == 0);
    REQUIRE_FALSE(active(rig.game, hinge));
    REQUIRE(finite(a.transform()));
    REQUIRE(near(distance(a.transform(), b.transform()), 2.f, 0.02f));
}

TEST_CASE("JP6 Stop takes every joint away, and the next Play makes them again", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    // Nothing is made while stopped.
    rig.sync_steps(2);
    REQUIRE(rig.physics.joint_count() == 0);
    rig.play();
    rig.steps(1);
    REQUIRE(rig.physics.joint_count() == 1);
    rig.game.stop_simulation();
    rig.sync_steps(1);
    REQUIRE(rig.physics.joint_count() == 0);
    rig.game.start_simulation();
    rig.steps(1);
    REQUIRE(rig.physics.joint_count() == 1);
}

TEST_CASE("JP7 a jointed part a script moves away is pulled back, and stays finite", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hinged = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    joint<engine_core::RevoluteConstraint>(rig.game, base.id(), hinged.id());
    const Matrix4 placed = hinged.transform();
    rig.play();
    rig.seconds(0.25);
    REQUIRE_FALSE(hinged.set_transform(at(6.f, 7.f, 1.f)));
    for (int i = 0; i < 480; ++i) {
        rig.steps(1);
        REQUIRE(finite(hinged.transform()));
    }
    REQUIRE(rig.physics.joint_count() == 1);
    REQUIRE(distance(hinged.transform(), placed) < 0.05f);
}
```

Declare `joint_count()` and `joint_key()` in `PhysicsWorld.hpp` returning 0 (stubs in PhysicsWorld.cpp) so the file compiles.

- [ ] **Step 2: Run them to see them fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "JP*"`
Expected: FAIL — JP1 (parts fall), JP2/JP3/JP6/JP7 (`joint_count` 0); JP4 and JP5 may pass on their count checks (nothing was made) — they guard the teardown paths.

- [ ] **Step 3: Implement**

`src/engine_core/PhysicsWorld.hpp` — after `contact_events_enabled`:

```cpp
    // For tests: how many Box3D joints JointConstraints have now, and one's
    // Box3D joint id (b3StoreJointId), 0 when it has none.
    std::size_t joint_count() const;
    std::uint64_t joint_key(InstanceId constraint) const;
```

`src/engine_core/PhysicsWorld.cpp`:

Includes: add `#include "JointConstraint.hpp"` and `#include "JointFrame.hpp"` beside the other engine includes.

In `struct PhysicsWorld::Impl`, after the `no_collide_changed` member:

```cpp
    // Joint constraints (docs/superpowers/specs/2026-10-11-joint-constraints-design.md
    // §3): per active JointConstraint, its Box3D joint and what it was made from.
    struct Joint {
        b3JointId joint = b3_nullJointId;
        JointKind kind = JointKind::Revolute;
        InstanceId node1 = 0;
        InstanceId node2 = 0;
        // The bodies it was made between: another body under a node remakes it.
        b3BodyId body1 = b3_nullBodyId;
        b3BodyId body2 = b3_nullBodyId;
        // Each end's joint frame in its node's frame, taken when it started
        // (spec §2, Capture), so a body made again places it where it was.
        b3Transform local1 = b3Transform_identity;
        b3Transform local2 = b3Transform_identity;
        // A Distance's length as placed, for a Length of 0.
        float placed_length = 0.f;
        std::uint32_t capture_revision = 0;
        std::uint32_t settings_revision = 0;
        std::uint64_t seen = 0;
    };
    std::unordered_map<InstanceId, Joint> joints;
    std::uint64_t joint_pass = 0;
```

In `Impl()`: `joints.reserve(1024);`. In `reset()`, after `no_collide_ids.clear();`: `joints.clear();` with the comment `// The world took the joints with it.`

In `sync`, after the `Physics reconcile` scope (bodies exist and have their types):

```cpp
        {
            PROFILE_SCOPE("Physics joints", profiler::Group::Physics);
            update_joints(game, running);
        }
```

New Impl functions, after `update_no_collide`:

```cpp
    static float radians(double degrees) { return static_cast<float>(degrees) * (kPi / 180.f); }

    static b3Transform pose_of_body(b3BodyId body) {
        b3Transform pose = b3Transform_identity;
        pose.p = b3Body_GetPosition(body);
        pose.q = b3Body_GetRotation(body);
        return pose;
    }

    // The Box3D body a node moves with in play: its own, its assembly's (a
    // compound), or, for a plain node, its play assembly's. Null for none.
    b3BodyId body_for_node(const Assemblies& assemblies, InstanceId node) const {
        std::uint32_t stamp = 0;
        if (const auto found = bodies.find(node); found != bodies.end()) {
            if (found->second.compound == 0) {
                return found->second.body;
            }
            stamp = found->second.compound;
        } else if (const int play = assemblies.play_of(node); play >= 0) {
            stamp = assemblies.play()[static_cast<std::size_t>(play)].stamp;
        }
        const auto compound = stamp != 0 ? compounds.find(stamp) : compounds.end();
        return compound != compounds.end() ? compound->second.body : b3_nullBodyId;
    }

    // Takes a joint's frames from where its ends are now (spec §2, Capture).
    // False when an end does not resolve.
    bool capture_joint(DataModel& game, const JointConstraint& constraint, const Assemblies::JointEdge& edge,
                       Joint& joint) const {
        const Assemblies& assemblies = game.assemblies();
        Matrix4 pivot1{};
        Matrix4 pivot2{};
        if (!assemblies.end_pivot(game, edge.constraint, 1, pivot1) ||
            !assemblies.end_pivot(game, edge.constraint, 2, pivot2)) {
            return false;
        }
        Matrix4 frame1{};
        Matrix4 frame2{};
        joint_frames(constraint.kind(), pivot1, pivot2, frame1, frame2);
        joint.kind = constraint.kind();
        joint.node1 = edge.node1;
        joint.node2 = edge.node2;
        joint.local1 = b3InvMulTransforms(transform_of(assemblies.node_frame(game, edge.node1)), transform_of(frame1));
        joint.local2 = b3InvMulTransforms(transform_of(assemblies.node_frame(game, edge.node2)), transform_of(frame2));
        const Vec3 a = matrix4_position(pivot1);
        const Vec3 b = matrix4_position(pivot2);
        joint.placed_length = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        joint.capture_revision = constraint.capture_revision();
        return true;
    }

    // Makes the Box3D joint between body1 and body2 from the captured frames,
    // as the nodes are now, and gives it every setting.
    void make_joint(DataModel& game, const JointConstraint& constraint, Joint& joint, b3BodyId body1,
                    b3BodyId body2) {
        const Assemblies& assemblies = game.assemblies();
        const b3Transform frame1 = b3MulTransforms(transform_of(assemblies.node_frame(game, joint.node1)), joint.local1);
        const b3Transform frame2 = b3MulTransforms(transform_of(assemblies.node_frame(game, joint.node2)), joint.local2);
        const auto place = [&](b3JointDef& def) {
            def.bodyIdA = body1;
            def.bodyIdB = body2;
            def.localFrameA = b3InvMulTransforms(pose_of_body(body1), frame1);
            def.localFrameB = b3InvMulTransforms(pose_of_body(body2), frame2);
            def.collideConnected = constraint.flag(JointParam::CollideConnected);
        };
        switch (joint.kind) {
            case JointKind::Revolute: {
                b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
                place(def.base);
                joint.joint = b3CreateRevoluteJoint(world, &def);
                break;
            }
            case JointKind::Prismatic: {
                b3PrismaticJointDef def = b3DefaultPrismaticJointDef();
                place(def.base);
                joint.joint = b3CreatePrismaticJoint(world, &def);
                break;
            }
            case JointKind::Distance: {
                b3DistanceJointDef def = b3DefaultDistanceJointDef();
                place(def.base);
                def.length = std::max(joint.placed_length, 0.001f);
                joint.joint = b3CreateDistanceJoint(world, &def);
                break;
            }
            case JointKind::Spherical: {
                b3SphericalJointDef def = b3DefaultSphericalJointDef();
                place(def.base);
                joint.joint = b3CreateSphericalJoint(world, &def);
                break;
            }
            case JointKind::Wheel: {
                b3WheelJointDef def = b3DefaultWheelJointDef();
                place(def.base);
                joint.joint = b3CreateWheelJoint(world, &def);
                break;
            }
        }
        joint.body1 = body1;
        joint.body2 = body2;
        apply_joint(constraint, joint);
        joint.settings_revision = constraint.settings_revision();
    }

    // Gives Box3D every setting the constraint holds (spec §1), and wakes
    // both bodies. Task 6 fills in the kinds.
    void apply_joint(const JointConstraint& constraint, const Joint& joint) {
        b3Joint_SetCollideConnected(joint.joint, constraint.flag(JointParam::CollideConnected));
        b3Joint_WakeBodies(joint.joint);
    }

    static void destroy_joint(Joint& joint) {
        if (b3Joint_IsValid(joint.joint)) {
            b3DestroyJoint(joint.joint, true);
        }
        joint.joint = b3_nullJointId;
    }

    void drop_joint(DataModel& game, InstanceId constraint) {
        const auto found = joints.find(constraint);
        if (found == joints.end()) {
            return;
        }
        destroy_joint(found->second);
        if (auto* made = dynamic_cast<JointConstraint*>(game.instance(constraint))) {
            made->set_reading(0.0);
        }
        joints.erase(found);
    }

    void drop_joints(DataModel& game) {
        for (auto& [id, joint] : joints) {
            destroy_joint(joint);
            if (auto* made = dynamic_cast<JointConstraint*>(game.instance(id))) {
                made->set_reading(0.0);
            }
        }
        joints.clear();
    }

    // Each sync: one Box3D joint per active JointConstraint in play, made,
    // remade, given its settings, or dropped (spec §3). Stopped, none.
    void update_joints(DataModel& game, bool running) {
        if (!running) {
            if (!joints.empty()) {
                drop_joints(game);
            }
            return;
        }
        const Assemblies& assemblies = game.assemblies();
        const std::vector<Assemblies::JointEdge>& list = assemblies.joints();
        if (list.empty() && joints.empty()) {
            return;
        }
        ++joint_pass;
        for (const Assemblies::JointEdge& edge : list) {
            auto* constraint = dynamic_cast<JointConstraint*>(game.instance(edge.constraint));
            if (constraint == nullptr) {
                continue;
            }
            const b3BodyId body1 = body_for_node(assemblies, edge.node1);
            const b3BodyId body2 = body_for_node(assemblies, edge.node2);
            if (!b3Body_IsValid(body1) || !b3Body_IsValid(body2) || B3_ID_EQUALS(body1, body2)) {
                drop_joint(game, edge.constraint);
                continue;
            }
            auto [found, made] = joints.try_emplace(edge.constraint);
            Joint& joint = found->second;
            joint.seen = joint_pass;
            const bool recapture = made || joint.capture_revision != constraint->capture_revision() ||
                                   joint.node1 != edge.node1 || joint.node2 != edge.node2;
            if (recapture && !capture_joint(game, *constraint, edge, joint)) {
                destroy_joint(joint);
                joints.erase(found);
                continue;
            }
            if (recapture || !b3Joint_IsValid(joint.joint) || !B3_ID_EQUALS(joint.body1, body1) ||
                !B3_ID_EQUALS(joint.body2, body2)) {
                destroy_joint(joint);
                make_joint(game, *constraint, joint, body1, body2);
            } else if (joint.settings_revision != constraint->settings_revision()) {
                apply_joint(*constraint, joint);
                joint.settings_revision = constraint->settings_revision();
            }
        }
        for (auto it = joints.begin(); it != joints.end();) {
            if (it->second.seen == joint_pass) {
                ++it;
                continue;
            }
            destroy_joint(it->second);
            if (auto* gone = dynamic_cast<JointConstraint*>(game.instance(it->first))) {
                gone->set_reading(0.0);
            }
            it = joints.erase(it);
        }
    }
```

Out of the Impl, beside `contact_events_enabled`:

```cpp
std::size_t PhysicsWorld::joint_count() const {
    std::size_t count = 0;
    for (const auto& [id, joint] : impl_->joints) {
        count += b3Joint_IsValid(joint.joint) ? 1 : 0;
    }
    return count;
}

std::uint64_t PhysicsWorld::joint_key(InstanceId constraint) const {
    const auto found = impl_->joints.find(constraint);
    return found != impl_->joints.end() && b3Joint_IsValid(found->second.joint) ? b3StoreJointId(found->second.joint)
                                                                                : 0;
}
```

Notes for the implementer:
- `transform_of` and `compounds` are Impl members defined further down the struct; C++ member functions see them.
- A body Box3D destroys (a member dropped, a compound remade) takes its joints with it; `b3Joint_IsValid` then fails and the joint is remade on the new body from `local1`/`local2`, which is JP3.
- A plain node's play assembly is only in `play()` when it has more than one node; a lone plain node has no body and `Assemblies` already made its joint inactive.

- [ ] **Step 4: Run them to see them pass**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[joint]"`
Expected: PASS. JP1's Prismatic and JP7 depend on Task 6 only for settings, not for holding; they pass with defaults. If JP1's Distance part drifts, check `placed_length` is the pivot distance (2), not 0.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.hpp src/engine_core/PhysicsWorld.cpp sandbox/joint_tests.cpp
git commit -m "Joints: one Box3D joint per active joint constraint in play

Held where placed: each end's frame is taken in its node's frame when the
joint starts, so a body made again in play (a weld, a compound) gets the
joint back where it was. Ends that go, a weld that joins them, and Stop take
it away.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Settings and readings

**Files:**
- Modify: `src/engine_core/PhysicsWorld.cpp` (`apply_joint`, new `read_joints`, `simulate`)
- Test: `sandbox/joint_tests.cpp`

**Interfaces:**
- Consumes: Task 5's `Joint`, `joints`, `apply_joint`.
- Produces: `read_joints(DataModel&)` after each Box3D step.

- [ ] **Step 1: Write the failing tests** (append)

```cpp
TEST_CASE("JB1 a Revolute's motor turns it, its limit stops it, and its spring brings it to TargetAngle",
          "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    // Its Y is up: it turns about a vertical axis, so gravity does not turn it.
    PhysicsObject& turner = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), turner.id());
    set(hinge, JointParam::EnableMotor, 1.0);
    set(hinge, JointParam::MotorSpeed, 90.0);
    rig.play();
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(hinge.reading()), 90.f, 5.f));
    set(hinge, JointParam::EnableLimit, 1.0);
    set(hinge, JointParam::LowerAngle, -45.0);
    set(hinge, JointParam::UpperAngle, 120.0);
    rig.seconds(1.5);
    REQUIRE(near(static_cast<float>(hinge.reading()), 120.f, 3.f));
    set(hinge, JointParam::EnableMotor, 0.0);
    set(hinge, JointParam::EnableSpring, 1.0);
    set(hinge, JointParam::Hertz, 4.0);
    set(hinge, JointParam::DampingRatio, 1.0);
    set(hinge, JointParam::TargetAngle, -30.0);
    rig.seconds(3.0);
    REQUIRE(near(static_cast<float>(hinge.reading()), -30.f, 3.f));
}

TEST_CASE("JB2 a Prismatic's motor slides it, and its limit stops it", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    // Its Y lies along -X: it slides across gravity.
    PhysicsObject& slider = rig.body(turned(at(0.f, 5.f, 3.f), Vec3{0.f, 0.f, 1.f}, 90.0), Vec3{1.f, 1.f, 1.f}, false);
    auto& slide = joint<engine_core::PrismaticConstraint>(rig.game, base.id(), slider.id());
    set(slide, JointParam::EnableMotor, 1.0);
    set(slide, JointParam::MotorSpeed, 1.0);
    rig.play();
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(slide.reading()), 1.f, 0.05f));
    REQUIRE(near(x_of(slider.transform()), -1.f, 0.05f));
    set(slide, JointParam::EnableLimit, 1.0);
    set(slide, JointParam::UpperTranslation, 1.5);
    rig.seconds(1.5);
    REQUIRE(near(static_cast<float>(slide.reading()), 1.5f, 0.05f));
}

TEST_CASE("JB3 a Distance holds its length as a rod, sags as a spring, and takes a Length", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& weight = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& rod = joint<engine_core::DistanceConstraint>(rig.game, base.id(), weight.id());
    rig.play();
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(rod.reading()), 2.f, 0.01f));
    set(rod, JointParam::EnableSpring, 1.0);
    set(rod, JointParam::Hertz, 1.0);
    set(rod, JointParam::DampingRatio, 1.0);
    rig.seconds(3.0);
    // g / (2πf)² ≈ 0.25 below the rest length.
    REQUIRE(rod.reading() > 2.1);
    REQUIRE(rod.reading() < 2.4);
    set(rod, JointParam::EnableSpring, 0.0);
    set(rod, JointParam::Length, 3.0);
    rig.seconds(2.0);
    REQUIRE(near(static_cast<float>(rod.reading()), 3.f, 0.02f));
}

TEST_CASE("JB4 a Spherical's cone limit holds a swinging weight", "[joint][physics]") {
    const auto widest_swing = [](bool limited) {
        PhysicsRig rig;
        PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{0.5f, 0.5f, 0.5f}, true);
        PhysicsObject& weight = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
        // The ball is 2 above the weight, at the base.
        engine_core::Attachment& top = rig.game.create<engine_core::Attachment>();
        REQUIRE_FALSE(top.set_offset(at(0.f, 2.f, 0.f)));
        rig.game.set_parent(top.id(), weight.id());
        auto& ball = joint<engine_core::SphericalConstraint>(rig.game, base.id(), top.id());
        if (limited) {
            set(ball, JointParam::EnableConeLimit, 1.0);
            set(ball, JointParam::ConeAngle, 20.0);
        }
        REQUIRE_FALSE(weight.set_velocity(Vec3{5.f, 0.f, 0.f}));
        rig.play();
        float widest = 0.f;
        for (int i = 0; i < 480; ++i) {
            rig.steps(1);
            const Matrix4 at_now = weight.transform();
            widest = std::max(widest, degrees_between(Vec3{x_of(at_now), y_of(at_now) - 5.f, z_of(at_now)},
                                                      Vec3{0.f, -1.f, 0.f}));
        }
        return widest;
    };
    REQUIRE(widest_swing(false) > 45.f);
    REQUIRE(widest_swing(true) < 22.f);
}

TEST_CASE("JB5 a Wheel rides its suspension, spins at its motor's speed, and steers", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& chassis = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 0.5f, 2.f}, true);
    // A cylinder on its side: its Y, the axle, lies along -X.
    PhysicsObject& wheel = rig.body(turned(at(1.5f, 3.f, 0.f), Vec3{0.f, 0.f, 1.f}, 90.0), Vec3{1.f, 0.4f, 1.f}, false);
    REQUIRE_FALSE(wheel.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    auto& axle = joint<engine_core::WheelConstraint>(rig.game, chassis.id(), wheel.id());
    set(axle, JointParam::SuspensionHertz, 1.0);
    set(axle, JointParam::SuspensionDampingRatio, 1.0);
    rig.play();
    rig.seconds(3.0);
    // g / (2πf)² ≈ 0.25 below where it was placed, and nowhere else.
    REQUIRE(near(y_of(wheel.transform()), 2.75f, 0.05f));
    REQUIRE(near(x_of(wheel.transform()), 1.5f, 0.01f));
    REQUIRE(near(z_of(wheel.transform()), 0.f, 0.01f));
    set(axle, JointParam::EnableSpinMotor, 1.0);
    set(axle, JointParam::SpinMotorSpeed, 180.0);
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(axle.reading()), 180.f, 5.f));
    set(axle, JointParam::EnableSpinMotor, 0.0);
    set(axle, JointParam::EnableSteering, 1.0);
    set(axle, JointParam::TargetSteeringAngle, 20.0);
    rig.seconds(2.0);
    const Vec3 spin = column(wheel.transform(), 1);
    REQUIRE(near(degrees_between(Vec3{spin.x, 0.f, spin.z}, Vec3{-1.f, 0.f, 0.f}), 20.f, 3.f));
}

TEST_CASE("JB6 a setting written in play wakes a sleeping body, and Stop puts the settings back",
          "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& turner = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), turner.id());
    rig.play();
    rig.seconds(3.0);  // long enough for Box3D to put it to sleep
    set(hinge, JointParam::EnableMotor, 1.0);
    set(hinge, JointParam::MotorSpeed, 90.0);
    rig.seconds(1.0);
    REQUIRE(near(static_cast<float>(hinge.reading()), 90.f, 5.f));
    rig.game.stop_simulation();
    REQUIRE_FALSE(hinge.flag(JointParam::EnableMotor));
    REQUIRE(hinge.param(JointParam::MotorSpeed) == 0.0);
}

TEST_CASE("JB7 a base unanchored in play keeps its joint", "[joint][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& base = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& turner = rig.body(at(2.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), turner.id());
    set(hinge, JointParam::EnableMotor, 1.0);
    set(hinge, JointParam::MotorSpeed, 45.0);
    rig.play();
    rig.seconds(0.5);
    base.set_anchored(false);
    rig.seconds(2.0);
    REQUIRE(rig.physics.joint_count() == 1);
    REQUIRE(finite(turner.transform()));
    REQUIRE(near(distance(base.transform(), turner.transform()), 2.f, 0.05f));
}

TEST_CASE("JB8 a setting written every step changes the joint in place", "[joint][physics]") {
    PhysicsRig rig;
    PhysicsObject& base = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& turner = rig.body(at(3.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    auto& hinge = joint<engine_core::RevoluteConstraint>(rig.game, base.id(), turner.id());
    set(hinge, JointParam::EnableMotor, 1.0);
    rig.play();
    rig.steps(1);
    const std::uint64_t made = rig.physics.joint_key(hinge.id());
    for (int i = 0; i < 240; ++i) {
        set(hinge, JointParam::MotorSpeed, 30.0 + 0.25 * i);
        rig.steps(1);
    }
    REQUIRE(rig.physics.joint_key(hinge.id()) == made);
    REQUIRE(hinge.reading() > 20.0);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "JB*"`
Expected: FAIL — readings stay 0 and motors, limits, and springs do nothing.

- [ ] **Step 3: Implement**

Replace `apply_joint`'s body:

```cpp
    void apply_joint(const JointConstraint& c, const Joint& joint) {
        using P = JointParam;
        const b3JointId id = joint.joint;
        const auto f = [&](P p) { return static_cast<float>(c.param(p)); };
        const auto low = [&](P a, P b) { return std::min(f(a), f(b)); };
        const auto high = [&](P a, P b) { return std::max(f(a), f(b)); };
        b3Joint_SetCollideConnected(id, c.flag(P::CollideConnected));
        switch (joint.kind) {
            case JointKind::Revolute:
                b3RevoluteJoint_EnableSpring(id, c.flag(P::EnableSpring));
                b3RevoluteJoint_SetSpringHertz(id, f(P::Hertz));
                b3RevoluteJoint_SetSpringDampingRatio(id, f(P::DampingRatio));
                b3RevoluteJoint_SetTargetAngle(id, radians(c.param(P::TargetAngle)));
                b3RevoluteJoint_EnableLimit(id, c.flag(P::EnableLimit));
                b3RevoluteJoint_SetLimits(id, radians(c.param(P::LowerAngle)), radians(c.param(P::UpperAngle)));
                b3RevoluteJoint_EnableMotor(id, c.flag(P::EnableMotor));
                b3RevoluteJoint_SetMotorSpeed(id, radians(c.param(P::MotorSpeed)));
                b3RevoluteJoint_SetMaxMotorTorque(id, f(P::MaxMotorTorque));
                break;
            case JointKind::Prismatic:
                b3PrismaticJoint_EnableSpring(id, c.flag(P::EnableSpring));
                b3PrismaticJoint_SetSpringHertz(id, f(P::Hertz));
                b3PrismaticJoint_SetSpringDampingRatio(id, f(P::DampingRatio));
                b3PrismaticJoint_SetTargetTranslation(id, f(P::TargetTranslation));
                b3PrismaticJoint_EnableLimit(id, c.flag(P::EnableLimit));
                b3PrismaticJoint_SetLimits(id, f(P::LowerTranslation), f(P::UpperTranslation));
                b3PrismaticJoint_EnableMotor(id, c.flag(P::EnableMotor));
                b3PrismaticJoint_SetMotorSpeed(id, f(P::MotorSpeed));
                b3PrismaticJoint_SetMaxMotorForce(id, f(P::MaxMotorForce));
                break;
            case JointKind::Distance:
                b3DistanceJoint_SetLength(id, c.param(P::Length) > 0.0 ? f(P::Length) : joint.placed_length);
                b3DistanceJoint_EnableSpring(id, c.flag(P::EnableSpring));
                b3DistanceJoint_SetSpringHertz(id, f(P::Hertz));
                b3DistanceJoint_SetSpringDampingRatio(id, f(P::DampingRatio));
                b3DistanceJoint_EnableLimit(id, c.flag(P::EnableLimit));
                b3DistanceJoint_SetLengthRange(id, f(P::MinLength), f(P::MaxLength));
                b3DistanceJoint_EnableMotor(id, c.flag(P::EnableMotor));
                b3DistanceJoint_SetMotorSpeed(id, f(P::MotorSpeed));
                b3DistanceJoint_SetMaxMotorForce(id, f(P::MaxMotorForce));
                break;
            case JointKind::Spherical: {
                const Vec3 velocity = c.motor_velocity();
                b3SphericalJoint_EnableSpring(id, c.flag(P::EnableSpring));
                b3SphericalJoint_SetSpringHertz(id, f(P::Hertz));
                b3SphericalJoint_SetSpringDampingRatio(id, f(P::DampingRatio));
                b3SphericalJoint_EnableConeLimit(id, c.flag(P::EnableConeLimit));
                b3SphericalJoint_SetConeLimit(id, radians(c.param(P::ConeAngle)));
                b3SphericalJoint_EnableTwistLimit(id, c.flag(P::EnableTwistLimit));
                b3SphericalJoint_SetTwistLimits(id, radians(c.param(P::LowerTwistAngle)),
                                                radians(c.param(P::UpperTwistAngle)));
                b3SphericalJoint_EnableMotor(id, c.flag(P::EnableMotor));
                b3SphericalJoint_SetMotorVelocity(id, b3Vec3{radians(velocity.x), radians(velocity.y),
                                                             radians(velocity.z)});
                b3SphericalJoint_SetMaxMotorTorque(id, f(P::MaxMotorTorque));
                break;
            }
            case JointKind::Wheel:
                b3WheelJoint_EnableSuspension(id, c.flag(P::EnableSuspension));
                b3WheelJoint_SetSuspensionHertz(id, f(P::SuspensionHertz));
                b3WheelJoint_SetSuspensionDampingRatio(id, f(P::SuspensionDampingRatio));
                b3WheelJoint_EnableSuspensionLimit(id, c.flag(P::EnableSuspensionLimit));
                // Box3D asserts lower <= upper here, so the smaller goes first (spec §4).
                b3WheelJoint_SetSuspensionLimits(id, low(P::LowerSuspensionLimit, P::UpperSuspensionLimit),
                                                 high(P::LowerSuspensionLimit, P::UpperSuspensionLimit));
                b3WheelJoint_EnableSpinMotor(id, c.flag(P::EnableSpinMotor));
                b3WheelJoint_SetSpinMotorSpeed(id, radians(c.param(P::SpinMotorSpeed)));
                b3WheelJoint_SetMaxSpinTorque(id, f(P::MaxSpinTorque));
                b3WheelJoint_EnableSteering(id, c.flag(P::EnableSteering));
                b3WheelJoint_SetSteeringHertz(id, f(P::SteeringHertz));
                b3WheelJoint_SetSteeringDampingRatio(id, f(P::SteeringDampingRatio));
                b3WheelJoint_SetTargetSteeringAngle(id, radians(c.param(P::TargetSteeringAngle)));
                b3WheelJoint_SetMaxSteeringTorque(id, f(P::MaxSteeringTorque));
                b3WheelJoint_EnableSteeringLimit(id, c.flag(P::EnableSteeringLimit));
                b3WheelJoint_SetSteeringLimits(id,
                                               radians(std::min(c.param(P::LowerSteeringLimit),
                                                                c.param(P::UpperSteeringLimit))),
                                               radians(std::max(c.param(P::LowerSteeringLimit),
                                                                c.param(P::UpperSteeringLimit))));
                break;
        }
        b3Joint_WakeBodies(id);
    }
```

Add, after `update_joints`:

```cpp
    // After a step: each joint's read-only value into its constraint, in the
    // property's units.
    void read_joints(DataModel& game) {
        for (auto& [id, joint] : joints) {
            auto* constraint = dynamic_cast<JointConstraint*>(game.instance(id));
            if (constraint == nullptr || !b3Joint_IsValid(joint.joint)) {
                continue;
            }
            switch (joint.kind) {
                case JointKind::Revolute:
                    constraint->set_reading(b3RevoluteJoint_GetAngle(joint.joint) * (180.0 / kPi));
                    break;
                case JointKind::Prismatic:
                    constraint->set_reading(b3PrismaticJoint_GetTranslation(joint.joint));
                    break;
                case JointKind::Distance:
                    constraint->set_reading(b3DistanceJoint_GetCurrentLength(joint.joint));
                    break;
                case JointKind::Wheel:
                    constraint->set_reading(b3WheelJoint_GetSpinSpeed(joint.joint) * (180.0 / kPi));
                    break;
                case JointKind::Spherical:
                    break;
            }
        }
    }
```

In `simulate`, after `play_seconds += dt;`:

```cpp
        if (!joints.empty()) {
            read_joints(game);
        }
```

- [ ] **Step 4: Run them to see them pass**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[joint]"`
Expected: PASS. Where a sign is the only thing off (JB1's angle, JB2's x, JB5's spin), the frame mapping in Task 1 turned the axis the other way: fix it there (both frames move together), not in the test, and ledger the ruling. Revolute angles read in (-180, 180]: JB1 keeps every target inside that range.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.cpp sandbox/joint_tests.cpp
git commit -m "Joints: Box3D gets every setting as it changes, and each joint's reading comes back after a step

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: The car

**Files:**
- Test: `sandbox/joint_tests.cpp`
- Modify (only if the test shows a default does not work): `src/engine_instances/JointConstraint.cpp` (`kParams`), `docs/superpowers/specs/2026-10-11-joint-constraints-design.md` (§1 tables)

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Write the tests** (append)

```cpp
namespace {

struct Car {
    PhysicsObject* chassis = nullptr;
    // Front left, front right, back left, back right.
    std::vector<engine_core::WheelConstraint*> axles;
};

// A chassis on four cylinder wheels, resting on the rig's floor, facing +Z.
// Every value is a default but the shape and size.
Car build_car(PhysicsRig& rig) {
    Car car;
    car.chassis = &rig.body(at(0.f, 1.f, 0.f), Vec3{2.f, 0.5f, 4.f}, false);
    const float xs[2] = {-1.3f, 1.3f};
    const float zs[2] = {1.5f, -1.5f};
    for (const float z : zs) {
        for (const float x : xs) {
            // On its side: its Y, the axle, lies along -X.
            PhysicsObject& wheel =
                rig.body(turned(at(x, 0.5f, z), Vec3{0.f, 0.f, 1.f}, 90.0), Vec3{1.f, 0.4f, 1.f}, false);
            REQUIRE_FALSE(wheel.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
            car.axles.push_back(&joint<engine_core::WheelConstraint>(rig.game, car.chassis->id(), wheel.id()));
        }
    }
    return car;
}

void throttle(Car& car, double degrees_per_second) {
    for (engine_core::WheelConstraint* axle : car.axles) {
        set(*axle, JointParam::EnableSpinMotor, 1.0);
        set(*axle, JointParam::SpinMotorSpeed, degrees_per_second);
    }
}

}  // namespace

TEST_CASE("JK1 a car of default parts drives forward on its motors, upright, and turns when it steers",
          "[joint][physics][car]") {
    PhysicsRig rig;
    rig.floor();
    Car car = build_car(rig);
    rig.play();
    rig.seconds(1.0);
    // Resting on its suspension: upright, a little below where it was placed.
    REQUIRE(column(car.chassis->transform(), 1).y > 0.99f);
    REQUIRE(y_of(car.chassis->transform()) < 1.f);
    REQUIRE(y_of(car.chassis->transform()) > 0.8f);
    // The axle is -X: spinning negative about it rolls the wheel toward +Z.
    throttle(car, -360.0);
    rig.seconds(3.0);
    REQUIRE(z_of(car.chassis->transform()) > 4.f);
    REQUIRE(std::fabs(x_of(car.chassis->transform())) < 0.5f);
    REQUIRE(column(car.chassis->transform(), 1).y > 0.9f);
    const Vec3 heading = column(car.chassis->transform(), 2);
    for (int front = 0; front < 2; ++front) {
        set(*car.axles[static_cast<std::size_t>(front)], JointParam::EnableSteering, 1.0);
        set(*car.axles[static_cast<std::size_t>(front)], JointParam::TargetSteeringAngle, 25.0);
    }
    rig.seconds(3.0);
    const Vec3 turned_to = column(car.chassis->transform(), 2);
    REQUIRE(degrees_between(Vec3{turned_to.x, 0.f, turned_to.z}, Vec3{heading.x, 0.f, heading.z}) > 15.f);
    REQUIRE(column(car.chassis->transform(), 1).y > 0.9f);
}

TEST_CASE("JK2 a step of a driving car allocates nothing", "[joint][physics][alloc]") {
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    SKIP("Debug containers allocate proxies; run in Release");
#endif
    PhysicsRig rig;
    rig.floor();
    Car car = build_car(rig);
    throttle(car, -360.0);
    rig.play();
    rig.seconds(1.0);
    alloc_counter::arm();
    rig.seconds(0.5);
    alloc_counter::disarm();
    REQUIRE(alloc_counter::count() == 0u);
}
```

- [ ] **Step 2: Run them**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[car],JK2*"`
Expected: PASS when the defaults are right (Tasks 5 and 6 built everything these tests use). This task's RED is the defaults: if JK1 fails, the failure says which (it sags too far or bounces: `SuspensionHertz`/`SuspensionDampingRatio`; wheels slip or stall: `MaxSpinTorque`; the front does not steer: `SteeringHertz`/`MaxSteeringTorque`). If JK2 fails, find the allocation (a `joints` map insert every step means a joint is remade every step: check `B3_ID_EQUALS` on the bodies) — do not loosen the test.

- [ ] **Step 3: Retune only if Step 2 failed**

Change the failing default in `kParams` (`src/engine_instances/JointConstraint.cpp`) and the same row in the spec's §1 table, keeping names. Re-run Step 2 and `build/Release/sandbox.exe "[joint]"` (JC1 reads the table, so it follows). Ledger: `Task 7: Ruling: <param> default <old> → <new> — <what the car did>`.

- [ ] **Step 4: Run the joint suite**

Run: `build/Release/sandbox.exe "[joint],[joint-frame]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add sandbox/joint_tests.cpp src/engine_instances/JointConstraint.cpp docs/superpowers/specs/2026-10-11-joint-constraints-design.md
git commit -m "Joints: a car of default parts drives, stays upright, and steers

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Drawing a selected joint

**Files:**
- Modify: `src/engine_core/ConstraintLines.hpp`, `src/engine_core/ConstraintLines.cpp`, `src/runner/GameView.cpp:715-745`
- Test: `sandbox/constraint_lines_tests.cpp`

**Interfaces:**
- Consumes: `JointConstraint::kind()`, `joint_frames`, `joint_axes` (Task 1).
- Produces: `ConstraintPivots::joint`, `axis_count`, `axis_origin`, `axes[2]`.

- [ ] **Step 1: Write the failing test** (append to `sandbox/constraint_lines_tests.cpp`)

```cpp
TEST_CASE("VZ5 an active joint is green, with a line along each of its axes", "[assembly][viz][joint]") {
    ConstraintPivots wheel = weld();
    wheel.joint = true;
    wheel.axis_count = 2;
    wheel.axis_origin = engine_core::Vec3{2.f, 0.f, -10.f};
    wheel.axes[0] = engine_core::Vec3{-1.f, 0.f, 0.f};
    wheel.axes[1] = engine_core::Vec3{0.f, 1.f, 0.f};
    std::vector<float> lines;
    engine_core::constraint_lines(wheel, view(), lines);
    // The segment, two crosses, and one line per axis.
    REQUIRE(lines.size() == kPoint * 2 * (1 + 3 + 3 + 2));
    REQUIRE(colored(lines, 0.4f, 0.9f, 0.45f));
    // The last line runs along the second axis, through the frame's origin.
    const std::size_t last = lines.size() - 2 * kPoint;
    REQUIRE(lines[last] == 2.f);
    REQUIRE(lines[last + kPoint] == 2.f);
    REQUIRE(lines[last + 1] < 0.f);
    REQUIRE(lines[last + kPoint + 1] > 0.f);
    ConstraintPivots idle = wheel;
    idle.active = false;
    lines.clear();
    engine_core::constraint_lines(idle, view(), lines);
    REQUIRE(colored(lines, 0.55f, 0.55f, 0.55f));
}
```

Add the four `ConstraintPivots` fields (Step 3) so it compiles.

- [ ] **Step 2: Run it to see it fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "VZ5*"`
Expected: FAIL (no axis lines; drawn light blue).

- [ ] **Step 3: Implement**

`src/engine_core/ConstraintLines.hpp` — in `ConstraintPivots`, after `no_collide`:

```cpp
    // A JointConstraint: drawn green, with a line along each of its axes
    // (joint_axes) through axis_origin, its frame's position.
    bool joint = false;
    int axis_count = 0;
    Vec3 axis_origin{};
    Vec3 axes[2]{};
```

and in the `constraint_lines` comment: `a NoCollisionConstraint red, a joint green with its axes, and an inactive one gray.`

`src/engine_core/ConstraintLines.cpp` — add `constexpr float kAxisPixels = 24.f;` beside `kCrossPixels`; the color becomes:

```cpp
    const Rgb color = !constraint.active ? Rgb{0.55f, 0.55f, 0.55f}
                      : constraint.joint ? Rgb{0.4f, 0.9f, 0.45f}
                      : constraint.no_collide ? Rgb{1.f, 0.35f, 0.35f}
                      : constraint.rigid ? Rgb{1.f, 0.7f, 0.2f}
                                         : Rgb{0.35f, 0.75f, 1.f};
```

and at the end of `constraint_lines`:

```cpp
    if (constraint.axis_count > 0) {
        const Vec3 at = constraint.axis_origin;
        const float reach = kAxisPixels * handle_scale(view, at);
        for (int i = 0; i < constraint.axis_count && i < 2; ++i) {
            const Vec3 along = constraint.axes[i];
            line(out, Vec3{at.x - along.x * reach, at.y - along.y * reach, at.z - along.z * reach},
                 Vec3{at.x + along.x * reach, at.y + along.y * reach, at.z + along.z * reach}, color);
        }
    }
```

`src/runner/GameView.cpp` — add `#include "JointConstraint.hpp"` and `#include "JointFrame.hpp"` beside `#include "Constraint.hpp"`. In `readSelectedBodies`, replace `pivots.no_collide = !constraint->joins();` with:

```cpp
        pivots.joint = constraint->joint();
        pivots.no_collide = !constraint->joins() && !pivots.joint;
```

and after the two `end_pivot` lines:

```cpp
        // A joint's axes, at the frame it would take if it started now.
        if (const auto* joint = dynamic_cast<const engine_core::JointConstraint*>(constraint);
            joint != nullptr && pivots.has1 && pivots.has2) {
            engine_core::Matrix4 frame1{};
            engine_core::Matrix4 frame2{};
            engine_core::joint_frames(joint->kind(), pivots.pivot1, pivots.pivot2, frame1, frame2);
            pivots.axis_origin = engine_core::matrix4_position(frame1);
            pivots.axis_count = engine_core::joint_axes(joint->kind(), frame1, pivots.axes);
        }
```

- [ ] **Step 4: Run it to see it pass**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox -- /m /v:minimal && build/Release/sandbox.exe "[viz]"`
Expected: PASS (VZ1 to VZ5). Then build `AnarchyStudio` clean.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/ConstraintLines.hpp src/engine_core/ConstraintLines.cpp src/runner/GameView.cpp sandbox/constraint_lines_tests.cpp
git commit -m "Joints: a selected joint is drawn green, with its axes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Finish

- [ ] Full suites: `build/Release/sandbox.exe` (all), `build/Release/engine-tests.exe`, then `ctest -C Release` from `build/` (explorer-tests has 6 known failures from before this work; any other failure is this plan's).
- [ ] Build Release `AnarchyStudio` and `AnarchyPlayer` (`--target AnarchyStudio`, `--target AnarchyPlayer`).
