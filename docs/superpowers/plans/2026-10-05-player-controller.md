# PlayerController Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `PlayerController` instance: a dynamic Box3D body shaped as an upright cylinder that hovers `StepHeight` above the ground, never tilts, slides along walls, and slows by its own ground `Friction`, under a new abstract `PhysicsBase` it shares with `PhysicsObject`.

**Architecture:** The shared rigid-body state moves out of `PhysicsObject` into a new C++ and Lua class `PhysicsBase`. `PlayerController` is a second `PhysicsBase`. `PhysicsWorld` handles any `PhysicsBase` and branches on `PlayerController` for its shape, its locks, and a per-step controller pass (ground probe, hover spring, friction) that runs just before `b3World_Step`.

**Tech Stack:** C++17 (the engine also builds on MSVC 19.23 and Xcode 13 / libc++ 13), Box3D at commit `9f998c86`, Catch2 tests in the `sandbox` target, Luau.

**Spec:** `docs/superpowers/specs/2026-10-05-player-controller-design.md`

## Global Constraints

- C++17 only: no designated initializers, no `std::numbers`, no ranges, nothing newer than libc++ 13 ships.
- Only `src/engine_core/PhysicsWorld.cpp` includes Box3D.
- PhysicsObject's saved properties keep their names, types, defaults, and saved form; existing places load unchanged.
- Each property is clamped only against its own limits, never against another property.
- PlayerController defaults: Friction 8, Radius 0.5, Height 2, StepHeight 0.4, MaxSlope 45 (degrees, clamped to [0, 89]).
- The hover gap is `min(StepHeight, Height - kMinSize)`, never below 0. StepHeight and Height read back what was written.
- Collider: 16-sided cylinder hull, radius Radius, from the hover gap above the feet to Height; friction 0, restitution 0, all rotation locked, sleep off.
- Transform's position is the feet. Rotation keeps only yaw about Y. Radius and Height are not scaled by the GameObject's Scale; no Prefab recentering.
- OnGround and IsSliding are read-only, not saved, not in history, and false when not playing.
- Probe constants: kProbeSides 16, kProbeWidth 0.95, kProbeNarrowest 0.6, kProbeSkin 0.01, kSnapMin 0.1, kHoverFrequency 6 Hz (damping ratio 1), kRisingSpeed 0.1.
- Physics writes fire no Changed and record no history.
- Comments and names follow the surrounding code: plain sentences, the codebase's terms (body, driven GameObject, Workspace, play, Stop).
- Work only in the worktree `/Users/yaoli/Documents/AnarchyEngine-CPP-player-controller` on branch `player-controller`. Never commit on main. Before each commit, `git show --stat` must list only this task's files.

## Review Focus

1. **A PlayerController that moves a GameObject.** The GameObject sits at the feet, upright, and a script moving that GameObject (even tilted) teleports the controller upright. Pinned in Task 3 (C3, "it moves its GameObject parent").
2. **Changing Radius, Height, or StepHeight during play.** The collider is rebuilt in place without launching the controller. Pinned in Task 4 (C9, "raising StepHeight during play").
3. **OnGround after Stop.** It reads false once the place stops, and is never saved. Pinned in Task 2 (C1) and Task 4 (C9, "Stop clears OnGround").
4. **One controller standing on another.** Each is the other's dynamic ground and gets the other's impulses, and the stack settles instead of bouncing or exploding. Pinned in Task 5 (C15).
5. **StepHeight larger than Height.** The collider becomes a kMinSize slab at the top and the controller still stands with its feet on the floor. Pinned in Task 2 (C1, values read back) and Task 4 (C9, "StepHeight above Height").

---

## File Structure

| File | Responsibility |
| --- | --- |
| `src/engine_instances/PhysicsBase.hpp/.cpp` (new) | Shared rigid-body state, its setters, dirty bits, Lua fields for Transform, Velocity, Anchored, Mass, LinearDamping, GameObject; the Lua slot helpers both subclasses use. |
| `src/engine_instances/PhysicsObject.hpp/.cpp` | Only PhysicsObject's own state and fields. |
| `src/engine_instances/PlayerController.hpp/.cpp` (new) | PlayerController's state, clamps, yaw-only Transform, read-only ground flags, Lua fields. |
| `src/engine_core/PhysicsWorld.hpp/.cpp` | Bodies for any PhysicsBase; the controller's shape and per-step pass; outlines. |
| `src/engine_core/ScriptBindings.cpp`, `src/engine_core/Project.cpp` | Factories for PlayerController. |
| `src/runner/GameView.cpp`, `src/ide/IdeIcons.cpp`, `src/ide/ClassOrder.hpp` | Studio: selection outline, icon, Explorer order. |
| `sandbox/physics_rig.hpp` (new) | The physics test rig, shared by both physics test files. |
| `sandbox/physics_tests.cpp` | Existing PhysicsObject tests, plus P23. |
| `sandbox/player_controller_tests.cpp` (new) | PlayerController tests, tag `[player]`. |
| `CMakeLists.txt` | New sources and test file. |
| `README.md`, `src/engine_instances/README.md` | Docs. |

## Build and test commands

Configure once (Task 1, Step 1). Then:

- Build tests: `cmake --build build --target sandbox -j8`
- Run a tag: `./build/sandbox "[physics]"` or `./build/sandbox "[player]"` from the worktree root.
- If `./build/sandbox` is not there, find it with `find build -maxdepth 2 -name sandbox -type f -perm +111`.

---

### Task 1: Extract PhysicsBase from PhysicsObject

**Files:**
- Create: `src/engine_instances/PhysicsBase.hpp`, `src/engine_instances/PhysicsBase.cpp`
- Modify: `src/engine_instances/PhysicsObject.hpp`, `src/engine_instances/PhysicsObject.cpp`, `src/engine_core/PhysicsWorld.cpp` (three `store_simulated` calls), `CMakeLists.txt` (engine_instances source list near line 508)
- Test: `sandbox/physics_tests.cpp`

**Interfaces:**
- Produces: `class PhysicsBase : public PVInstance` with
  - `Matrix4 transform() const`, `Vec3 velocity() const`, `bool anchored() const`, `double mass() const`, `double linear_damping() const`
  - `LuaSlot game_object() const`, `InstanceId game_object_id() const`, `InstanceId driven_game_object() const`
  - `virtual std::optional<std::string> set_transform(const Matrix4&)`, `set_velocity(Vec3)`, `void set_anchored(bool)`, `set_mass(double)`, `set_linear_damping(double)`, `set_game_object(const LuaSlot&)`
  - `std::uint32_t take_dirty()`, `void store_simulated(const Matrix4& transform, Vec3 velocity)`
  - `enum Dirty` (kDirtyPose, kDirtyVelocity, kDirtyMaterial, kDirtyShape, kDirtyMass, kDirtyType, kDirtyDamping, kDirtyAll), `kDefaultMass`, `kMinMass`, `kMinSize`
  - `bool warned_shared`
  - protected: `set_number`, `set_vec`, `mark_dirty(std::uint32_t)`, `on_reuse()`
  - `namespace physics_detail`: `number_slot`, `vec3_slot`, `bool_slot`, `matrix_slot`, `finite`, `refuse`, `require_simulation_thread`, and the `read_number`/`write_number`/`read_vec`/`write_vec` templates over a class `T`
- Produces: `PhysicsObject::store_angular_velocity(Vec3)`

- [ ] **Step 1: Configure the worktree build**

Point FetchContent at the main checkout's already-downloaded sources. Never pass `FETCHCONTENT_BASE_DIR`, because that overwrites the main build's `_deps/*-build`.

```bash
cd /Users/yaoli/Documents/AnarchyEngine-CPP-player-controller
deps=/Users/yaoli/Documents/AnarchyEngine-CPP/build/_deps
args=""
for d in assimp box3d catch2 flecs flecs_header flecs_source glfw httplib libvterm luau miniaudio stb_image stb_image_write stb_truetype; do
  u=$(echo "$d" | tr '[:lower:]' '[:upper:]')
  args="$args -DFETCHCONTENT_SOURCE_DIR_$u=$deps/$d-src"
done
cmake -S . -B build -DJADEFX_CPP_DIR=/Users/yaoli/Documents/JadeFX_CPP $args
cmake --build build --target sandbox -j8
./build/sandbox "[physics]"
```

Expected: the build succeeds and every `[physics]` test passes. This is the baseline.

- [ ] **Step 2: Write the failing test**

Append to `sandbox/physics_tests.cpp`:

```cpp
TEST_CASE("P23 PhysicsObject is a PhysicsBase, which is never made itself", "[physics]") {
    REQUIRE(engine_core::lua_class_inherits("PhysicsObject", "PhysicsBase"));
    REQUIRE(engine_core::lua_class_inherits("PhysicsBase", "PVInstance"));
    REQUIRE_FALSE(engine_core::project_class_known("PhysicsBase"));
    for (const char* shared : {"Transform", "Velocity", "Anchored", "Mass", "LinearDamping", "GameObject"}) {
        INFO(shared);
        REQUIRE(engine_core::lua_class_find("PhysicsBase", shared) != nullptr);
    }
    for (const char* own : {"AngularVelocity", "AngularDamping", "Friction", "Bounciness", "Shape", "Size", "Mesh"}) {
        INFO(own);
        REQUIRE(engine_core::lua_class_find("PhysicsBase", own) == nullptr);
        REQUIRE(engine_core::lua_class_find("PhysicsObject", own) != nullptr);
    }

    // Every saved property is still saved, by the same name.
    SimRole role;
    engine_core::Game game;
    PhysicsObject& body = game.create<PhysicsObject>();
    engine_core::PropertyBag defaults;
    body.default_properties(defaults);
    for (const char* name : {"Transform", "Velocity", "AngularVelocity", "Anchored", "Mass", "Friction", "Bounciness",
                             "LinearDamping", "AngularDamping", "Shape", "Size", "Mesh", "GameObject"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(defaults, name) != nullptr);
    }
}
```

- [ ] **Step 3: Run it to see it fail**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "P23*"`
Expected: FAIL at `lua_class_inherits("PhysicsObject", "PhysicsBase")`.

- [ ] **Step 4: Create `src/engine_instances/PhysicsBase.hpp`**

```cpp
#pragma once

#include "PVInstance.hpp"
#include "InstanceRef.hpp"
#include "Matrix4.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace engine_core {

// What every rigid body shares: PhysicsObject and PlayerController. While it
// is in Workspace, at any depth, and the place is playing, the physics world
// (PhysicsWorld) simulates it; anywhere else it is only data. It is not a
// GameObject: it has a Transform but draws nothing. When GameObject names
// one, or else when its parent is a GameObject, the body starts at that
// GameObject's Transform and moves it. Of several bodies moving one
// GameObject, only the first in tree order gets one. Abstract: the Lua class
// PhysicsBase cannot be made, and IsA("PhysicsBase") is true of both.
//
// Transform        Matrix4   where the body is. Identity.
// Velocity         Vector3   world units per second. (0, 0, 0).
// Anchored         boolean   a static body, which nothing moves. false.
// Mass             number    1. Below kMinMass, as 0 is, is taken as kMinMass.
// LinearDamping    number    0, not below 0.
// GameObject       GameObject?  what the body moves. Nil: the parent, if it
//                               is a GameObject.
//
// Each is a saved registry property, so DataModel saves, loads, undoes, and
// restores it at Stop. A value that is not finite is refused. Writes made by
// the physics world itself go through store_simulated and are none of those.
class PhysicsBase : public PVInstance {
public:
    // What a write changed, for the physics world to push into the body.
    enum Dirty : std::uint32_t {
        kDirtyPose = 1u << 0,
        kDirtyVelocity = 1u << 1,
        kDirtyMaterial = 1u << 2,
        kDirtyShape = 1u << 3,
        kDirtyMass = 1u << 4,
        kDirtyType = 1u << 5,
        kDirtyDamping = 1u << 6,
        kDirtyAll = 0x7fu,
    };

    static constexpr double kDefaultMass = 1.0;
    static constexpr double kMinMass = 0.001;
    static constexpr float kMinSize = 0.01f;

    PhysicsBase(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PVInstance(tag, state, id) {}

    bool physics_body() const override { return true; }

    Matrix4 transform() const override { return transform_; }
    Vec3 velocity() const { return velocity_; }
    bool anchored() const { return anchored_; }
    double mass() const { return mass_; }
    double linear_damping() const { return linear_damping_; }

    LuaSlot game_object() const;
    // The live target, or 0.
    InstanceId game_object_id() const;
    // The GameObject the body moves: GameObject when it names one, else the
    // parent when that is a GameObject, else 0. The parent link is never
    // written to GameObject, so a body moved out from under it stops moving it.
    InstanceId driven_game_object() const;

    // SimulationThread. Each returns why it refused the value, changing nothing.
    virtual std::optional<std::string> set_transform(const Matrix4& transform);
    std::optional<std::string> set_pv_transform(const Matrix4& transform) override { return set_transform(transform); }
    std::optional<std::string> set_velocity(Vec3 velocity);
    void set_anchored(bool anchored);
    std::optional<std::string> set_mass(double mass);
    std::optional<std::string> set_linear_damping(double damping);
    std::optional<std::string> set_game_object(const LuaSlot& value);

    // The physics world's side. take_dirty returns what writes changed since
    // the last call and clears it. store_simulated keeps what the body did,
    // with no Changed, no history, and no dirty mark.
    std::uint32_t take_dirty() {
        const std::uint32_t out = dirty_;
        dirty_ = 0;
        return out;
    }
    void store_simulated(const Matrix4& transform, Vec3 velocity);

    // One warning, until the condition clears: this one lost its GameObject
    // to an earlier body.
    bool warned_shared = false;

protected:
    void on_reuse() override;

    std::optional<std::string> set_number(const char* property, double& slot, double value, std::uint32_t dirty);
    std::optional<std::string> set_vec(const char* property, Vec3& slot, Vec3 value, std::uint32_t dirty);
    void mark_dirty(std::uint32_t dirty) { dirty_ |= dirty; }

private:
    Matrix4 transform_ = matrix4_identity();
    Vec3 velocity_{};
    bool anchored_ = false;
    double mass_ = kDefaultMass;
    double linear_damping_ = 0.0;
    InstanceRef game_object_ref_;
    std::uint32_t dirty_ = kDirtyAll;
};

// The Lua slot helpers PhysicsBase, PhysicsObject, and PlayerController
// register their fields with.
namespace physics_detail {

LuaSlot number_slot(double value);
LuaSlot vec3_slot(Vec3 value);
LuaSlot bool_slot(bool value);
LuaSlot matrix_slot(const Matrix4& value);
inline bool finite(Vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
// Contract-fails off SimulationThread.
void require_simulation_thread(const DataModel& object);
// Puts error into in and returns false, or returns true when there is none.
bool refuse(LuaSlot& in, std::optional<std::string> error);

template <class T, double (T::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const T* body = dynamic_cast<const T*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = number_slot((body->*Get)());
    return true;
}

template <class T, std::optional<std::string> (T::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    T* body = dynamic_cast<T*>(&object);
    return body != nullptr && refuse(in, (body->*Set)(in.number));
}

template <class T, Vec3 (T::*Get)() const>
bool read_vec(DataModel&, DataModel& object, LuaSlot& out) {
    const T* body = dynamic_cast<const T*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = vec3_slot((body->*Get)());
    return true;
}

template <class T, std::optional<std::string> (T::*Set)(Vec3)>
bool write_vec(DataModel&, DataModel& object, LuaSlot& in) {
    T* body = dynamic_cast<T*>(&object);
    return body != nullptr && refuse(in, (body->*Set)(in.vec));
}

}  // namespace physics_detail

}  // namespace engine_core
```

- [ ] **Step 5: Create `src/engine_instances/PhysicsBase.cpp`**

Move these bodies from `PhysicsObject.cpp` unchanged except for the class name: `game_object`, `game_object_id`, `driven_game_object`, `set_transform`, `set_vec`, `set_velocity`, `set_anchored`, `set_number`, `set_mass`, `set_linear_damping`, `set_game_object`. Also move the Lua readers and writers for Transform, Anchored, and GameObject.

```cpp
#include "PhysicsBase.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace engine_core {

namespace physics_detail {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot vec3_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot matrix_slot(const Matrix4& value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Matrix4;
    slot.transform = value;
    return slot;
}

void require_simulation_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("Physics setters run on SimulationThread");
    }
}

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

}  // namespace physics_detail

using namespace physics_detail;

namespace {

bool same_vec(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

}  // namespace

LuaSlot PhysicsBase::game_object() const { return instance_reference_slot(game_object_ref_, "GameObject"); }

InstanceId PhysicsBase::game_object_id() const {
    const LuaSlot slot = game_object();
    return slot.kind == LuaSlot::Kind::Instance ? slot.id : 0;
}

InstanceId PhysicsBase::driven_game_object() const {
    if (const InstanceId linked = game_object_id(); linked != 0) {
        return linked;
    }
    const InstanceId above = parent(id());
    return above != kNoParent && above != 0 && DataModel::game_object(above) != nullptr ? above : 0;
}

std::optional<std::string> PhysicsBase::set_transform(const Matrix4& transform) {
    require_simulation_thread(*this);
    for (float value : transform.m) {
        if (!std::isfinite(value)) {
            return std::string("Transform must be finite");
        }
    }
    if (same_matrix4(transform_, transform)) {
        return std::nullopt;
    }
    const Matrix4 previous = transform_;
    transform_ = transform;
    dirty_ |= kDirtyPose;
    note_property_change("Transform", matrix_slot(previous), matrix_slot(transform));
    return std::nullopt;
}

std::optional<std::string> PhysicsBase::set_vec(const char* property, Vec3& slot, Vec3 value, std::uint32_t dirty) {
    require_simulation_thread(*this);
    if (!finite(value)) {
        return std::string(property) + " must be finite";
    }
    if (same_vec(slot, value)) {
        return std::nullopt;
    }
    const Vec3 previous = slot;
    slot = value;
    dirty_ |= dirty;
    note_property_change(property, vec3_slot(previous), vec3_slot(value));
    return std::nullopt;
}

std::optional<std::string> PhysicsBase::set_velocity(Vec3 velocity) {
    return set_vec("Velocity", velocity_, velocity, kDirtyVelocity);
}

void PhysicsBase::set_anchored(bool anchored) {
    require_simulation_thread(*this);
    if (anchored_ == anchored) {
        return;
    }
    anchored_ = anchored;
    dirty_ |= kDirtyType;
    note_property_change("Anchored", bool_slot(!anchored), bool_slot(anchored));
}

std::optional<std::string> PhysicsBase::set_number(const char* property, double& slot, double value,
                                                   std::uint32_t dirty) {
    require_simulation_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    dirty_ |= dirty;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> PhysicsBase::set_mass(double mass) {
    return set_number("Mass", mass_, std::isfinite(mass) ? std::max(mass, kMinMass) : mass, kDirtyMass);
}

std::optional<std::string> PhysicsBase::set_linear_damping(double damping) {
    return set_number("LinearDamping", linear_damping_, std::isfinite(damping) ? std::max(damping, 0.0) : damping,
                      kDirtyDamping);
}

std::optional<std::string> PhysicsBase::set_game_object(const LuaSlot& value) {
    require_simulation_thread(*this);
    // The physics world sees a new target by comparing it with the one its body drives.
    return set_instance_reference("GameObject", "GameObject", game_object_ref_, value);
}

void PhysicsBase::store_simulated(const Matrix4& transform, Vec3 velocity) {
    transform_ = transform;
    velocity_ = velocity;
}

void PhysicsBase::on_reuse() {
    transform_ = matrix4_identity();
    velocity_ = {};
    anchored_ = false;
    mass_ = kDefaultMass;
    linear_damping_ = 0.0;
    game_object_ref_.set_guid(std::string());
    dirty_ = kDirtyAll;
    warned_shared = false;
}

namespace {

PhysicsBase* base_of(DataModel& object) { return dynamic_cast<PhysicsBase*>(&object); }

bool read_transform(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    out = matrix_slot(body->transform());
    return true;
}

bool write_transform(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsBase* body = base_of(object);
    return body != nullptr && refuse(in, body->set_transform(in.transform));
}

bool read_anchored(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    out = bool_slot(body->anchored());
    return true;
}

bool write_anchored(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    body->set_anchored(in.flag);
    return true;
}

bool read_game_object(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsBase* body = base_of(object);
    if (body == nullptr) {
        return false;
    }
    out = body->game_object();
    return true;
}

bool write_game_object(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsBase* body = base_of(object);
    return body != nullptr && refuse(in, body->set_game_object(in));
}

ANARCHY_LUA_REGISTER(register_physics_base_lua) {
    static const std::string mass = write_json(JsonValue::number(PhysicsBase::kDefaultMass));
    static const std::string identity = [] {
        const Matrix4 value = matrix4_identity();
        return write_json(json_floats(value.m, 16));
    }();
    const LuaField fields[] = {
        lua_saved_property("Transform", "Matrix4", read_transform, write_transform, identity.c_str()),
        lua_saved_property("Velocity", "Vector3", read_vec<PhysicsBase, &PhysicsBase::velocity>,
                           write_vec<PhysicsBase, &PhysicsBase::set_velocity>, "[0,0,0]"),
        lua_saved_property("Anchored", "boolean", read_anchored, write_anchored, "false"),
        lua_saved_property("Mass", "number", read_number<PhysicsBase, &PhysicsBase::mass>,
                           write_number<PhysicsBase, &PhysicsBase::set_mass>, mass.c_str()),
        lua_saved_property("LinearDamping", "number", read_number<PhysicsBase, &PhysicsBase::linear_damping>,
                           write_number<PhysicsBase, &PhysicsBase::set_linear_damping>, "0"),
        lua_saved_property("GameObject", "GameObject?", read_game_object, write_game_object, "null"),
    };
    // Abstract: no factory registers it.
    register_lua_class("PhysicsBase", "PVInstance", fields, static_cast<int>(std::size(fields)));
}

}  // namespace

}  // namespace engine_core
```

- [ ] **Step 6: Rewrite `src/engine_instances/PhysicsObject.hpp`**

```cpp
#pragma once

#include "PhysicsBase.hpp"

namespace engine_core {

// A rigid body of any Shape: a PhysicsBase (which see for Transform,
// Velocity, Anchored, Mass, LinearDamping, and GameObject) with these too.
//
// AngularVelocity  Vector3   world space, radians per second. (0, 0, 0).
// Friction         number    0.6, not below 0.
// Bounciness       number    restitution: 0, not below 0; the slider runs to 1.
// AngularDamping   number    0, not below 0.
// Shape            Enum.PhysicsShape  Box.
// Size             Vector3   (1, 1, 1); each axis at least kMinSize. Box: its
//                            extents. Sphere: diameter X. Capsule: diameter X,
//                            height Y, along Y. Cylinder: diameter X,
//                            height Y, along Y. Cone: base diameter X at
//                            the bottom, height Y to its tip. Wedge: Size's
//                            box halved by a slope from its bottom front
//                            (-Z) edge up to its top back (+Z) edge. Hull
//                            and Custom: the mesh fits it.
// Mesh             Mesh?     a Hull's points, or a Custom's triangles: the
//                            whole mesh, which collides only while Anchored
//                            (Box3D gives a mesh contacts only on a static
//                            body); unanchored, a Custom is a Hull of it.
//                            Shown only for a Hull or a Custom.
class PhysicsObject : public PhysicsBase {
public:
    enum class Shape { Box = 0, Sphere = 1, Capsule = 2, Hull = 3, Custom = 4, Cylinder = 5, Cone = 6, Wedge = 7 };

    static constexpr double kDefaultFriction = 0.6;

    PhysicsObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PhysicsBase(tag, state, id) {}

    const char* class_name() const override { return "PhysicsObject"; }

    Vec3 angular_velocity() const { return angular_velocity_; }
    double friction() const { return friction_; }
    double bounciness() const { return bounciness_; }
    double angular_damping() const { return angular_damping_; }
    Shape shape() const { return shape_; }
    Vec3 size() const { return size_; }

    LuaSlot mesh() const;
    // The live target, or 0.
    InstanceId mesh_id() const;

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_angular_velocity(Vec3 velocity);
    std::optional<std::string> set_friction(double friction);
    std::optional<std::string> set_bounciness(double bounciness);
    std::optional<std::string> set_angular_damping(double damping);
    std::optional<std::string> set_shape(int shape);
    std::optional<std::string> set_size(Vec3 size);
    std::optional<std::string> set_mesh(const LuaSlot& value);

    // The physics world's side, beside store_simulated: what the body's spin
    // is now, with no Changed, no history, and no dirty mark.
    void store_angular_velocity(Vec3 velocity) { angular_velocity_ = velocity; }

    // One warning each, until the condition clears: its Hull fell back to a Box.
    bool warned_hull = false;
    // Once, until it is reused: this unanchored Custom collides as a Hull.
    bool warned_custom = false;

protected:
    void on_reuse() override;

private:
    Vec3 angular_velocity_{};
    double friction_ = kDefaultFriction;
    double bounciness_ = 0.0;
    double angular_damping_ = 0.0;
    Shape shape_ = Shape::Box;
    Vec3 size_{1.f, 1.f, 1.f};
    InstanceRef mesh_ref_;
};

}  // namespace engine_core
```

- [ ] **Step 7: Trim `src/engine_instances/PhysicsObject.cpp`**

Make these edits:
1. Delete the anonymous-namespace helpers `number_slot`, `vec3_slot`, `bool_slot`, `matrix_slot`, `finite`, `same_vec`, and `require_thread`. Keep `shape_slot`. Add `using namespace physics_detail;` after `namespace engine_core {`.
2. Delete the definitions now in PhysicsBase.cpp: `game_object`, `game_object_id`, `driven_game_object`, `set_transform`, `set_vec`, `set_velocity`, `set_anchored`, `set_number`, `set_mass`, `set_linear_damping`, `set_game_object`, and `store_simulated`.
3. Replace every `require_thread(*this)` with `require_simulation_thread(*this)`.
4. Make `set_angular_velocity` use the base's `set_vec`. It is protected and inherited, so the body stays `return set_vec("AngularVelocity", angular_velocity_, velocity, kDirtyVelocity);`. Do the same for `set_size`, `set_friction`, `set_bounciness`, `set_angular_damping` (`set_number`), and `set_mesh`, `set_shape`. Where they did `dirty_ |= X;`, write `mark_dirty(X);`.
5. Replace `on_reuse` with:

```cpp
void PhysicsObject::on_reuse() {
    PhysicsBase::on_reuse();
    angular_velocity_ = {};
    friction_ = kDefaultFriction;
    bounciness_ = 0.0;
    angular_damping_ = 0.0;
    shape_ = Shape::Box;
    size_ = {1.f, 1.f, 1.f};
    mesh_ref_.set_guid(std::string());
    warned_hull = false;
    warned_custom = false;
}
```

6. In the Lua section, delete `refuse`, `body_of`, `read_number`, `write_number`, `read_vec`, `write_vec`, `read_transform`, `write_transform`, `read_anchored`, and `write_anchored`. Keep a reference reader for Mesh:

```cpp
bool read_mesh(DataModel&, DataModel& object, LuaSlot& out) {
    const PhysicsObject* body = dynamic_cast<const PhysicsObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = body->mesh();
    return true;
}

bool write_mesh(DataModel&, DataModel& object, LuaSlot& in) {
    PhysicsObject* body = dynamic_cast<PhysicsObject*>(&object);
    return body != nullptr && refuse(in, body->set_mesh(in));
}
```

   In `read_shape` and `write_shape`, replace `body_of(object)` with `dynamic_cast<PhysicsObject*>(&object)`, and the `const` cast in `read_shape`.
7. Replace the registration with only PhysicsObject's own fields, under PhysicsBase:

```cpp
ANARCHY_LUA_REGISTER(register_physics_object_lua) {
    static const std::string friction = number_json(PhysicsObject::kDefaultFriction);
    const LuaField fields[] = {
        lua_saved_property("AngularVelocity", "Vector3", read_vec<PhysicsObject, &PhysicsObject::angular_velocity>,
                           write_vec<PhysicsObject, &PhysicsObject::set_angular_velocity>, "[0,0,0]"),
        lua_saved_property("Friction", "number", read_number<PhysicsObject, &PhysicsObject::friction>,
                           write_number<PhysicsObject, &PhysicsObject::set_friction>, friction.c_str()),
        lua_slider(lua_saved_property("Bounciness", "number", read_number<PhysicsObject, &PhysicsObject::bounciness>,
                                      write_number<PhysicsObject, &PhysicsObject::set_bounciness>, "0"),
                   0.0, 1.0),
        lua_saved_property("AngularDamping", "number", read_number<PhysicsObject, &PhysicsObject::angular_damping>,
                           write_number<PhysicsObject, &PhysicsObject::set_angular_damping>, "0"),
        lua_saved_enum("Shape", physics_shape_enum(), read_shape, write_shape, "\"Box\""),
        lua_saved_property("Size", "Vector3", read_vec<PhysicsObject, &PhysicsObject::size>,
                           write_vec<PhysicsObject, &PhysicsObject::set_size>, "[1,1,1]"),
        lua_shown_when(lua_saved_property("Mesh", "Mesh?", read_mesh, write_mesh, "null"), "Shape",
                       {static_cast<int>(PhysicsObject::Shape::Hull), static_cast<int>(PhysicsObject::Shape::Custom)}),
    };
    register_lua_class("PhysicsObject", "PhysicsBase", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("PhysicsObject", {"Workspace", "PVInstance"});
}
```

- [ ] **Step 8: Update the physics world's store calls**

Find every caller: `grep -rn "store_simulated" src tests sandbox`. Each call in `src/engine_core/PhysicsWorld.cpp` (in `create`, `follow_driven`, and `pull`) passes three arguments today. Split each one, for example in `pull`:

```cpp
object->store_simulated(matrix_of(position, rotation, object->transform()),
                        from_b3(b3Body_GetLinearVelocity(record.body)));
object->store_angular_velocity(from_b3(b3Body_GetAngularVelocity(record.body)));
```

In `create` and `follow_driven`, use `object.store_simulated(<transform>, object.velocity());` and then `object.store_angular_velocity(object.angular_velocity());`. That second call keeps the value it already has, so it can be left out; leave it out.

- [ ] **Step 9: Add the source to CMake**

In `CMakeLists.txt`, in the engine_instances list, add above `src/engine_instances/PhysicsObject.cpp`:

```cmake
    src/engine_instances/PhysicsBase.cpp
```

- [ ] **Step 10: Build and run every physics test**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[physics]"`
Expected: P1–P23 all pass.

Also build what else uses PhysicsObject: `cmake --build build --target properties-tests engine-tests -j8 && ./build/properties-tests && ./build/engine-tests`.
Expected: PASS.

- [ ] **Step 11: Commit**

```bash
git add src/engine_instances/PhysicsBase.hpp src/engine_instances/PhysicsBase.cpp \
  src/engine_instances/PhysicsObject.hpp src/engine_instances/PhysicsObject.cpp \
  src/engine_core/PhysicsWorld.cpp CMakeLists.txt sandbox/physics_tests.cpp
git diff --cached --stat
git commit -m "Move what every rigid body shares into PhysicsBase

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The PlayerController instance and its properties

**Files:**
- Create: `src/engine_instances/PlayerController.hpp`, `src/engine_instances/PlayerController.cpp`, `sandbox/physics_rig.hpp`, `sandbox/player_controller_tests.cpp`
- Modify: `sandbox/physics_tests.cpp` (use the rig header), `src/engine_core/ScriptBindings.cpp:192,232`, `src/engine_core/Project.cpp:~141`, `CMakeLists.txt` (engine_instances list; sandbox list near line 857)

**Interfaces:**
- Consumes: `PhysicsBase`, `physics_detail::*` from Task 1.
- Produces: `class PlayerController : public PhysicsBase` with
  - `kDefaultFriction` 8.0, `kDefaultRadius` 0.5, `kDefaultHeight` 2.0, `kDefaultStepHeight` 0.4, `kDefaultMaxSlope` 45.0, `kMaxSlopeLimit` 89.0
  - `double friction() const`, `radius()`, `height()`, `step_height()`, `max_slope()`, `double hover_gap() const`
  - `bool on_ground() const`, `bool is_sliding() const`
  - setters returning `std::optional<std::string>`: `set_friction(double)`, `set_radius(double)`, `set_height(double)`, `set_step_height(double)`, `set_max_slope(double)`; `set_transform` override
  - `void store_ground(bool on_ground, bool sliding)`
  - free function `Matrix4 upright_transform(const Matrix4& transform)`
- Produces (tests): `sandbox/physics_rig.hpp` with `kStep`, `at`, `x_of`, `y_of`, `z_of`, `near`, `column_length`, `PhysicsRig` (with `body`, `floor`, `controller`, `play`, `steps`, `seconds`).

- [ ] **Step 1: Move the rig into `sandbox/physics_rig.hpp`**

Cut `kStep`, `at`, `y_of`, `x_of`, `near`, `column_length`, and `struct PhysicsRig` out of the anonymous namespace in `sandbox/physics_tests.cpp`, and put them here with two additions: `z_of`, and `PhysicsRig::controller`.

```cpp
#pragma once

// The physics test rig: a Game, a physics world stepped by hand at the
// engine's 240 Hz, and the warnings it gave.

#include "support.hpp"

#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "PlayerController.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace physics_rig {

using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::PlayerController;
using engine_core::Vec3;

constexpr double kStep = 1.0 / 240.0;

inline Matrix4 at(float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); }

inline float x_of(const Matrix4& m) { return m.m[12]; }
inline float y_of(const Matrix4& m) { return m.m[13]; }
inline float z_of(const Matrix4& m) { return m.m[14]; }

inline bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

inline float column_length(const Matrix4& m, int column) {
    const float* axis = m.m + column * 4;
    return std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
}

struct PhysicsRig {
    SimRole role;
    engine_core::Game game;
    engine_core::PhysicsWorld physics;
    std::vector<std::string> warnings;

    PhysicsRig() {
        physics.set_warning_sink([this](const std::string& text) { warnings.push_back(text); });
    }

    PhysicsObject& body(Matrix4 where, Vec3 size, bool anchored, InstanceId parent = 0) {
        PhysicsObject& object = game.create<PhysicsObject>();
        REQUIRE_FALSE(object.set_transform(where));
        REQUIRE_FALSE(object.set_size(size));
        object.set_anchored(anchored);
        game.set_parent(object.id(), parent != 0 ? parent : workspace_of(game));
        return object;
    }

    // A wide anchored floor whose top is at y = 0.
    PhysicsObject& floor() { return body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, true); }

    // A PlayerController with its defaults, its feet at where.
    PlayerController& controller(Matrix4 where, InstanceId parent = 0) {
        PlayerController& object = game.create<PlayerController>();
        REQUIRE_FALSE(object.set_transform(where));
        game.set_parent(object.id(), parent != 0 ? parent : workspace_of(game));
        return object;
    }

    void play() {
        game.capture_place();
        game.start_simulation();
    }

    void steps(int count) {
        for (int i = 0; i < count; ++i) {
            physics.step(game, kStep);
        }
    }

    void seconds(double time) { steps(static_cast<int>(time / kStep + 0.5)); }
};

}  // namespace physics_rig
```

In `sandbox/physics_tests.cpp`, replace the moved code with `#include "physics_rig.hpp"`, and inside the remaining anonymous namespace add `using namespace physics_rig;`. Keep `instance_slot` and the other test-only helpers where they are.

Run: `cmake --build build --target sandbox -j8`
Expected: it fails only because `PlayerController.hpp` does not exist yet. That is fine; the next steps create it.

- [ ] **Step 2: Write the failing tests**

Create `sandbox/player_controller_tests.cpp`:

```cpp
// PlayerController: a PhysicsBase that is an upright cylinder hovering
// StepHeight above the ground, slowed there by its own Friction.

#include "physics_rig.hpp"

#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace physics_rig;

TEST_CASE("C1 PlayerController properties are checked, saved, and come back at Stop", "[player]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("PlayerController"));
    REQUIRE(engine_core::lua_class_inherits("PlayerController", "PhysicsBase"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("PlayerController", "PhysicsObject"));
    for (const char* gone : {"Shape", "Size", "Mesh", "Bounciness", "AngularVelocity", "AngularDamping"}) {
        INFO(gone);
        REQUIRE(engine_core::lua_class_find("PlayerController", gone) == nullptr);
    }

    PlayerController& c = game.create<PlayerController>();
    game.set_parent(c.id(), workspace_of(game));
    REQUIRE(c.friction() == 8.0);
    REQUIRE(c.radius() == 0.5);
    REQUIRE(c.height() == 2.0);
    REQUIRE(c.step_height() == 0.4);
    REQUIRE(c.max_slope() == 45.0);
    REQUIRE_FALSE(c.on_ground());
    REQUIRE_FALSE(c.is_sliding());

    engine_core::PropertyBag saved;
    c.save_properties(saved);
    REQUIRE(saved.empty());

    // Each clamps against its own limits only.
    REQUIRE_FALSE(c.set_friction(-1.0));
    REQUIRE(c.friction() == 0.0);
    REQUIRE_FALSE(c.set_radius(0.0));
    REQUIRE(c.radius() == PlayerController::kMinSize);
    REQUIRE_FALSE(c.set_height(0.0));
    REQUIRE(c.height() == PlayerController::kMinSize);
    REQUIRE_FALSE(c.set_height(2.0));
    REQUIRE_FALSE(c.set_step_height(-1.0));
    REQUIRE(c.step_height() == 0.0);
    REQUIRE_FALSE(c.set_max_slope(120.0));
    REQUIRE(c.max_slope() == 89.0);
    REQUIRE_FALSE(c.set_max_slope(-5.0));
    REQUIRE(c.max_slope() == 0.0);
    REQUIRE(*c.set_radius(std::nan("")) == "Radius must be a finite number");

    // StepHeight above Height changes neither; only the hover gap is capped.
    REQUIRE_FALSE(c.set_step_height(3.0));
    REQUIRE(c.step_height() == 3.0);
    REQUIRE(c.height() == 2.0);
    REQUIRE(near(static_cast<float>(c.hover_gap()), 2.f - PlayerController::kMinSize, 1e-6f));
    REQUIRE_FALSE(c.set_step_height(0.4));
    REQUIRE(near(static_cast<float>(c.hover_gap()), 0.4f, 1e-6f));

    // OnGround and IsSliding are not saved, and false when not playing.
    engine_core::PropertyBag changed;
    c.save_properties(changed);
    REQUIRE(engine_core::bag_find(changed, "OnGround") == nullptr);
    REQUIRE(engine_core::bag_find(changed, "IsSliding") == nullptr);
    REQUIRE(engine_core::bag_find(changed, "MaxSlope") != nullptr);
    c.store_ground(true, false);
    REQUIRE_FALSE(c.on_ground());

    REQUIRE_FALSE(c.set_radius(0.5));
    game.capture_place();
    game.start_simulation();
    c.store_ground(true, false);
    REQUIRE(c.on_ground());
    REQUIRE_FALSE(c.set_radius(2.0));
    game.stop_simulation();
    REQUIRE(c.radius() == 0.5);
    REQUIRE_FALSE(c.on_ground());

    const engine_core::LuaField* slope = engine_core::lua_class_find("PlayerController", "MaxSlope");
    REQUIRE(slope != nullptr);
    REQUIRE(slope->slider());
}

TEST_CASE("C2 scripts make a PlayerController and cannot write its ground flags", "[player]") {
    ScriptRig rig;
    add_script(rig.game, "Controller", R"(
        local c = Instance.new("PlayerController", workspace)
        _G.default = c.Friction == 8 and c.Radius == 0.5 and c.Height == 2 and c.StepHeight == 0.4
            and c.MaxSlope == 45 and c.OnGround == false and c.IsSliding == false and c.Mass == 1
        _G.isa = c:IsA("PhysicsBase") and c:IsA("PVInstance") and not c:IsA("PhysicsObject")
        _G.readonly = not pcall(function() c.OnGround = true end)
            and not pcall(function() c.IsSliding = true end)
        _G.noshape = not pcall(function() return c.Shape end)
        _G.abstract = not pcall(function() Instance.new("PhysicsBase") end)
        c.StepHeight = 5
        _G.kept = c.StepHeight == 5 and c.Height == 2
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"default", "isa", "readonly", "noshape", "abstract", "kept"}) {
        INFO(name);
        bool value = false;
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}
```

Add `sandbox/player_controller_tests.cpp` to the sandbox list in `CMakeLists.txt`, after `sandbox/physics_tests.cpp`.

- [ ] **Step 3: Create `src/engine_instances/PlayerController.hpp`**

```cpp
#pragma once

#include "PhysicsBase.hpp"

#include <algorithm>

namespace engine_core {

// A body for a walking character: a PhysicsBase (which see for Transform,
// Velocity, Anchored, Mass, LinearDamping, and GameObject) that is an upright
// cylinder of Radius, standing from the hover gap above its feet up to
// Height. Its contact friction and bounciness are 0, so it slides along
// walls, and it never turns but about Y. While the place plays, PhysicsWorld
// holds its cylinder hover_gap() above the ground under it, so it walks up
// anything that tall, and slows it there by Friction.
//
// Transform's position is the feet: the ground point under the cylinder's
// middle. Any Transform written keeps only its position and its turn about Y.
// The Scale of the GameObject it moves does not size it.
//
// Friction    number   8, not below 0. Per second, only while OnGround: the
//                      speed across the ground, relative to the ground's
//                      own, decays by exp(-Friction * dt).
// Radius      number   0.5, at least kMinSize.
// Height      number   2, at least kMinSize. Ground to top of head.
// StepHeight  number   0.4, not below 0. How far the cylinder hovers, and so
//                      the tallest edge it walks up. Read back as written;
//                      hover_gap() caps it.
// MaxSlope    number   45, degrees in [0, 89]. Steeper ground slides.
// OnGround    boolean  read only: on ground no steeper than MaxSlope.
// IsSliding   boolean  read only: on ground steeper than MaxSlope.
//
// The first five are saved registry properties. OnGround and IsSliding are
// not saved, record no history, and read false while the place is not playing.
class PlayerController : public PhysicsBase {
public:
    static constexpr double kDefaultFriction = 8.0;
    static constexpr double kDefaultRadius = 0.5;
    static constexpr double kDefaultHeight = 2.0;
    static constexpr double kDefaultStepHeight = 0.4;
    static constexpr double kDefaultMaxSlope = 45.0;
    static constexpr double kMaxSlopeLimit = 89.0;

    PlayerController(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PhysicsBase(tag, state, id) {}

    const char* class_name() const override { return "PlayerController"; }

    double friction() const { return friction_; }
    double radius() const { return radius_; }
    double height() const { return height_; }
    double step_height() const { return step_height_; }
    double max_slope() const { return max_slope_; }
    bool on_ground() const { return on_ground_ && simulation_running(); }
    bool is_sliding() const { return sliding_ && simulation_running(); }

    // How far the cylinder's bottom stands above the feet: StepHeight, capped
    // so the cylinder is at least kMinSize tall.
    double hover_gap() const { return std::max(0.0, std::min(step_height_, height_ - kMinSize)); }

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_transform(const Matrix4& transform) override;
    std::optional<std::string> set_friction(double friction);
    std::optional<std::string> set_radius(double radius);
    std::optional<std::string> set_height(double height);
    std::optional<std::string> set_step_height(double step_height);
    std::optional<std::string> set_max_slope(double degrees);

    // The physics world's side: what its ground probe found this step, with
    // no Changed and no history.
    void store_ground(bool on_ground, bool sliding) {
        on_ground_ = on_ground;
        sliding_ = sliding;
    }

protected:
    void on_reuse() override;

private:
    double friction_ = kDefaultFriction;
    double radius_ = kDefaultRadius;
    double height_ = kDefaultHeight;
    double step_height_ = kDefaultStepHeight;
    double max_slope_ = kDefaultMaxSlope;
    bool on_ground_ = false;
    bool sliding_ = false;
};

// transform's position, turned only as it turns about Y: its tilt and scale
// taken out. Its +Z axis, laid flat, gives the turn; when that points up or
// down, its +X axis does.
Matrix4 upright_transform(const Matrix4& transform);

}  // namespace engine_core
```

Check `simulation_running()` is callable on an instance: `grep -n "bool simulation_running" src/engine_core/DataModel.hpp`. If it is a member of DataModel, the header compiles as written. If it is only on the root `Game`, call the root's (`root().simulation_running()` or whatever the root accessor is named; find it with `grep -n "root()" src/engine_core/DataModel.hpp`).

- [ ] **Step 4: Create `src/engine_instances/PlayerController.cpp`**

```cpp
#include "PlayerController.hpp"

#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <cmath>
#include <iterator>

namespace engine_core {

using namespace physics_detail;

Matrix4 upright_transform(const Matrix4& transform) {
    const float* x = transform.m;
    const float* z = transform.m + 8;
    double yaw = 0.0;
    if (std::hypot(z[0], z[2]) > 1e-6) {
        // A turn of yaw about Y takes +Z to (sin yaw, 0, cos yaw).
        yaw = std::atan2(z[0], z[2]);
    } else if (std::hypot(x[0], x[2]) > 1e-6) {
        // And +X to (cos yaw, 0, -sin yaw).
        yaw = std::atan2(-x[2], x[0]);
    }
    Matrix4 out = matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, yaw);
    out.m[12] = transform.m[12];
    out.m[13] = transform.m[13];
    out.m[14] = transform.m[14];
    return out;
}

std::optional<std::string> PlayerController::set_transform(const Matrix4& transform) {
    for (float value : transform.m) {
        if (!std::isfinite(value)) {
            return std::string("Transform must be finite");
        }
    }
    return PhysicsBase::set_transform(upright_transform(transform));
}

std::optional<std::string> PlayerController::set_friction(double friction) {
    return set_number("Friction", friction_, std::isfinite(friction) ? std::max(friction, 0.0) : friction, 0);
}

std::optional<std::string> PlayerController::set_radius(double radius) {
    return set_number("Radius", radius_, std::isfinite(radius) ? std::max(radius, double(kMinSize)) : radius,
                      kDirtyShape);
}

std::optional<std::string> PlayerController::set_height(double height) {
    return set_number("Height", height_, std::isfinite(height) ? std::max(height, double(kMinSize)) : height,
                      kDirtyShape);
}

std::optional<std::string> PlayerController::set_step_height(double step_height) {
    return set_number("StepHeight", step_height_,
                      std::isfinite(step_height) ? std::max(step_height, 0.0) : step_height, kDirtyShape);
}

std::optional<std::string> PlayerController::set_max_slope(double degrees) {
    return set_number("MaxSlope", max_slope_,
                      std::isfinite(degrees) ? std::min(std::max(degrees, 0.0), kMaxSlopeLimit) : degrees, 0);
}

void PlayerController::on_reuse() {
    PhysicsBase::on_reuse();
    friction_ = kDefaultFriction;
    radius_ = kDefaultRadius;
    height_ = kDefaultHeight;
    step_height_ = kDefaultStepHeight;
    max_slope_ = kDefaultMaxSlope;
    on_ground_ = false;
    sliding_ = false;
}

namespace {

template <bool (PlayerController::*Get)() const>
bool read_flag(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* body = dynamic_cast<const PlayerController*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = bool_slot((body->*Get)());
    return true;
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_player_controller_lua) {
    using C = PlayerController;
    static const std::string friction = number_json(C::kDefaultFriction);
    static const std::string radius = number_json(C::kDefaultRadius);
    static const std::string height = number_json(C::kDefaultHeight);
    static const std::string step = number_json(C::kDefaultStepHeight);
    static const std::string slope = number_json(C::kDefaultMaxSlope);
    const LuaField fields[] = {
        lua_saved_property("Friction", "number", read_number<C, &C::friction>, write_number<C, &C::set_friction>,
                           friction.c_str()),
        lua_saved_property("Radius", "number", read_number<C, &C::radius>, write_number<C, &C::set_radius>,
                           radius.c_str()),
        lua_saved_property("Height", "number", read_number<C, &C::height>, write_number<C, &C::set_height>,
                           height.c_str()),
        lua_saved_property("StepHeight", "number", read_number<C, &C::step_height>,
                           write_number<C, &C::set_step_height>, step.c_str()),
        lua_slider(lua_saved_property("MaxSlope", "number", read_number<C, &C::max_slope>,
                                      write_number<C, &C::set_max_slope>, slope.c_str()),
                   0.0, C::kMaxSlopeLimit),
        lua_property("OnGround", "boolean", false, read_flag<&C::on_ground>, nullptr),
        lua_property("IsSliding", "boolean", false, read_flag<&C::is_sliding>, nullptr),
    };
    register_lua_class("PlayerController", "PhysicsBase", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("PlayerController", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
```

- [ ] **Step 5: Register the factories**

In `src/engine_core/ScriptBindings.cpp`, add the include `#include "PlayerController.hpp"` beside `PhysicsObject.hpp`. Under `create_physics_object` add:

```cpp
DataModel& create_player_controller(DataModel& world) { return world.create<PlayerController>(); }
```

and after `register_lua_creatable("PhysicsObject", create_physics_object);`:

```cpp
    register_lua_creatable("PlayerController", create_player_controller);
```

In `src/engine_core/Project.cpp`, include `PlayerController.hpp`, and after the PhysicsObject entry:

```cpp
        out.push_back(
            {"PlayerController", [](DataModel& world) -> DataModel& { return world.create<PlayerController>(); }});
```

In `CMakeLists.txt`, add `src/engine_instances/PlayerController.cpp` after `src/engine_instances/PhysicsObject.cpp`.

- [ ] **Step 6: Build and run**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[player]" && ./build/sandbox "[physics]"`
Expected: C1 and C2 pass, and every `[physics]` test still passes. PhysicsWorld already gives a PlayerController a body, because `physics_body()` is true. It casts to PhysicsObject and skips anything else, so nothing simulates a controller yet. That is Task 3.

If C2's `readonly` check fails because a `nullptr` write crashes instead of raising, find how an instance's read-only property refuses writes (`grep -rn "writable" src/engine_core/ScriptRuntime*.cpp`) and make the controller's flags follow that pattern.

- [ ] **Step 7: Commit**

```bash
git add src/engine_instances/PlayerController.hpp src/engine_instances/PlayerController.cpp \
  src/engine_core/ScriptBindings.cpp src/engine_core/Project.cpp CMakeLists.txt \
  sandbox/physics_rig.hpp sandbox/physics_tests.cpp sandbox/player_controller_tests.cpp
git diff --cached --stat
git commit -m "Add a PlayerController instance with its properties

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Give a PlayerController its body: an upright, unturning cylinder

**Files:**
- Modify: `src/engine_core/PhysicsWorld.hpp`, `src/engine_core/PhysicsWorld.cpp`
- Test: `sandbox/player_controller_tests.cpp`

**Interfaces:**
- Consumes: `PlayerController`, `upright_transform`, `PhysicsBase::store_simulated(const Matrix4&, Vec3)`, `PhysicsObject::store_angular_velocity(Vec3)`.
- Produces:
  - `static Vec3 PhysicsWorld::shape_center(const DataModel&, const PhysicsBase&)` and `static float shape_scale(const DataModel&, const PhysicsBase&)`: the origin and 1 for a PlayerController.
  - `static void PhysicsWorld::collision_outline(const PlayerController& controller, std::vector<Vec3>& lines)`.
  - `PhysicsWorld::Impl::Body::controller` (bool), and `make_controller_shape(PlayerController&, Body&)`, used by Tasks 4–5.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/player_controller_tests.cpp`:

```cpp
namespace {

// Its +Y axis, which an upright Transform keeps straight up.
bool upright(const Matrix4& m) { return near(m.m[4], 0.f, 1e-4f) && near(m.m[5], 1.f, 1e-4f) && near(m.m[6], 0.f, 1e-4f); }

}  // namespace

TEST_CASE("C3 a PlayerController is an upright cylinder that never turns", "[player]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("with StepHeight 0 it falls and rests with its feet on the floor") {
        PlayerController& c = rig.controller(at(0.f, 3.f, 0.f));
        REQUIRE_FALSE(c.set_step_height(0.0));
        rig.play();
        rig.seconds(2.0);
        INFO(y_of(c.transform()));
        REQUIRE(near(y_of(c.transform()), 0.f, 0.02f));
        REQUIRE(rig.physics.has_body(c.id()));
    }

    SECTION("a spinning box knocks it aside but does not turn it") {
        PlayerController& c = rig.controller(at(0.f, 0.f, 0.f));
        REQUIRE_FALSE(c.set_step_height(0.0));
        PhysicsObject& box = rig.body(at(-3.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
        REQUIRE_FALSE(box.set_velocity(Vec3{8.f, 0.f, 0.f}));
        REQUIRE_FALSE(box.set_angular_velocity(Vec3{3.f, 5.f, 20.f}));
        rig.play();
        rig.seconds(2.0);
        REQUIRE(x_of(c.transform()) > 0.05f);
        REQUIRE(upright(c.transform()));
        REQUIRE(near(c.transform().m[0], 1.f, 1e-4f));
    }

    SECTION("a written Transform keeps its position and its turn about Y only") {
        PlayerController& c = rig.controller(at(0.f, 0.f, 0.f));
        Matrix4 turned = engine_core::matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, 0.7);
        turned.m[12] = 1.f;
        turned.m[13] = 2.f;
        turned.m[14] = 3.f;
        REQUIRE_FALSE(c.set_transform(turned));
        REQUIRE(near(c.transform().m[0], std::cos(0.7f), 1e-4f));
        Matrix4 tilted = engine_core::matrix4_axis_angle(Vec3{1.f, 0.f, 0.f}, 0.5);
        tilted.m[12] = 4.f;
        REQUIRE_FALSE(c.set_transform(tilted));
        REQUIRE(upright(c.transform()));
        REQUIRE(x_of(c.transform()) == 4.f);
    }

    SECTION("anchored, it is a static cylinder a box lands on") {
        PlayerController& c = rig.controller(at(0.f, 0.f, 0.f));
        c.set_anchored(true);
        PhysicsObject& box = rig.body(at(0.f, 4.f, 0.f), Vec3{0.5f, 0.5f, 0.5f}, false);
        rig.play();
        rig.seconds(2.0);
        INFO(y_of(box.transform()));
        REQUIRE(near(y_of(box.transform()), 2.25f, 0.03f));
        REQUIRE(y_of(c.transform()) == 0.f);
    }

    SECTION("it moves its GameObject parent, upright, from its feet") {
        engine_core::GameObject& part = create_part(rig.game);
        part.set_transform(at(0.f, 3.f, 0.f));
        PlayerController& c = rig.controller(at(0.f, 0.f, 0.f), part.id());
        REQUIRE_FALSE(c.set_step_height(0.0));
        rig.play();
        rig.seconds(2.0);
        REQUIRE(near(y_of(part.transform()), 0.f, 0.02f));
        // A script moving the GameObject, tilted, moves the body upright.
        Matrix4 tilted = engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 0.6);
        tilted.m[12] = 5.f;
        tilted.m[13] = 1.f;
        part.set_transform(tilted);
        rig.seconds(2.0);
        INFO(x_of(part.transform()) << " " << y_of(part.transform()));
        REQUIRE(near(x_of(part.transform()), 5.f, 0.05f));
        REQUIRE(near(y_of(part.transform()), 0.f, 0.02f));
        REQUIRE(upright(part.transform()));
    }
    REQUIRE(rig.warnings.empty());
}

TEST_CASE("C4 a PlayerController's outline is its cylinder and a line up from its feet", "[player]") {
    PhysicsRig rig;
    PlayerController& c = rig.controller(at(0.f, 0.f, 0.f));
    REQUIRE_FALSE(c.set_radius(0.5));
    REQUIRE_FALSE(c.set_height(2.0));
    REQUIRE_FALSE(c.set_step_height(0.4));
    std::vector<Vec3> lines;
    engine_core::PhysicsWorld::collision_outline(c, lines);
    REQUIRE(lines.size() >= 2 * 16 * 3);
    REQUIRE(lines.size() % 2 == 0);
    bool feet = false;
    for (const Vec3& p : lines) {
        const float out = std::sqrt(p.x * p.x + p.z * p.z);
        REQUIRE(out <= 0.5f + 1e-4f);
        REQUIRE(p.y >= -1e-4f);
        REQUIRE(p.y <= 2.f + 1e-4f);
        // Only the line from the feet comes below the cylinder.
        if (p.y < 0.4f - 1e-4f) {
            REQUIRE(out < 1e-4f);
        }
        feet = feet || (out < 1e-4f && p.y < 1e-4f);
    }
    REQUIRE(feet);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "C3*,C4*"`
Expected: a compile error, because `collision_outline(const PlayerController&, ...)` doesn't exist yet. Once Step 3's header change is in, C3 fails: the controller doesn't fall, since it has no body.

- [ ] **Step 3: Generalize the header**

In `src/engine_core/PhysicsWorld.hpp`, change the forward declarations to:

```cpp
class DataModel;
class PhysicsBase;
class PhysicsObject;
class PlayerController;
```

Change `shape_center` and `shape_scale` to take `const PhysicsBase& object`, and add to their comments: "The origin, and 1, for a PlayerController, which is neither recentered nor scaled." After the existing `collision_outline`, add:

```cpp
    // A PlayerController's edges, as collision_outline gives a PhysicsObject's:
    // its cylinder, from hover_gap() above its feet up to Height, and one
    // line from its feet up to the cylinder's bottom, in the body's space.
    static void collision_outline(const PlayerController& controller, std::vector<Vec3>& lines);
```

In the class comment's step list, change "Every PhysicsObject in Workspace" to "Every PhysicsBase in Workspace".

- [ ] **Step 4: Generalize `PhysicsWorld.cpp` to PhysicsBase**

1. `#include "PlayerController.hpp"`.
2. The cylinder uses the existing `kRoundSides` (16); no new constant.
3. In `Body`, add after `volume`:

```cpp
        // A PlayerController's: an upright cylinder, never recentered or scaled.
        bool controller = false;
```

4. Change these signatures from `PhysicsObject&` to `PhysicsBase&`: `keep`, `recenter`, `create`, `make_shape`, `density`, `push`, `follow_driven`. In `reconcile`, `first_in_tree_order`, and `pull`, change `dynamic_cast<PhysicsObject*>` and `dynamic_cast<const PhysicsObject*>` to `PhysicsBase`. In the shared-GameObject warning, write `std::string(object->class_name()) + " " + game.name(id) + " has no body: an earlier body already moves " + game.name(target)`.
5. Rename the current `make_shape` to `make_object_shape(DataModel& game, PhysicsObject& object, Body& record)`, and add a dispatcher above it:

```cpp
    // Puts the body's shape on it, replacing any it had: a PlayerController's
    // cylinder, or a PhysicsObject's Shape.
    void make_shape(DataModel& game, PhysicsBase& object, Body& record) {
        if (auto* controller = dynamic_cast<PlayerController*>(&object)) {
            make_controller_shape(*controller, record);
        } else if (auto* body = dynamic_cast<PhysicsObject*>(&object)) {
            make_object_shape(game, *body, record);
        }
    }

    // An upright cylinder of Radius from hover_gap() above the feet (the
    // body's origin) up to Height, with no friction and no bounce.
    void make_controller_shape(PlayerController& controller, Body& record) {
        drop_shape(record);
        record.controller = true;
        record.center = Vec3{};
        record.scale = 1.f;
        record.prefab.clear();
        const float radius = static_cast<float>(controller.radius());
        const float gap = static_cast<float>(controller.hover_gap());
        const float tall = static_cast<float>(controller.height()) - gap;
        primitive_points(PhysicsObject::Shape::Cylinder, Vec3{2.f * radius, tall, 2.f * radius},
                         Vec3{0.f, gap + tall * 0.5f, 0.f}, points);
        b3ShapeDef def = b3DefaultShapeDef();
        def.baseMaterial.friction = 0.f;
        def.baseMaterial.restitution = 0.f;
        def.userData = user_data(controller.id());
        def.updateBodyMass = false;
        if (b3HullData* hull = build_hull(points)) {
            record.volume = b3ComputeHullMass(hull, 1.f).mass;
            def.density = density(controller, record.volume);
            record.shape = b3CreateHullShape(record.body, &def, hull);
            b3DestroyHull(hull);
        }
        if (!b3Shape_IsValid(record.shape)) {
            const b3BoxHull box = b3MakeOffsetBoxHull(radius, tall * 0.5f, radius, b3Vec3{0.f, gap + tall * 0.5f, 0.f});
            record.volume = 4.f * radius * radius * tall;
            def.density = density(controller, record.volume);
            record.shape = b3CreateHullShape(record.body, &def, &box.base);
        }
        b3Body_ApplyMassFromShapes(record.body);
    }
```

6. In `create`, after `b3BodyDef def = b3DefaultBodyDef();`:

```cpp
        auto* controller = dynamic_cast<PlayerController*>(&object);
        auto* rigid = dynamic_cast<PhysicsObject*>(&object);
        if (controller != nullptr) {
            start = upright_transform(start);
            def.motionLocks.angularX = true;
            def.motionLocks.angularY = true;
            def.motionLocks.angularZ = true;
            def.enableSleep = false;
        }
```

   Move the `start` computation above this block, so the upright pose is the one `pose_of` reads. Then set velocities and damping:

```cpp
        def.linearVelocity = to_b3(object.velocity());
        def.linearDamping = static_cast<float>(object.linear_damping());
        if (rigid != nullptr) {
            def.angularVelocity = to_b3(rigid->angular_velocity());
            def.angularDamping = static_cast<float>(rigid->angular_damping());
        }
```

   The end-of-create store becomes `object.store_simulated(matrix_of(def.position, def.rotation, object.transform()), object.velocity());`.
7. In `recenter`, return at once for a controller: `if (record.controller) { return; }`.
8. In `push`:
   - The Custom-anchoring check reads `auto* rigid = dynamic_cast<PhysicsObject*>(&object); const bool custom_type = rigid != nullptr && (dirty & PhysicsBase::kDirtyType) != 0 && rigid->shape() == PhysicsObject::Shape::Custom;`.
   - The Material branch runs only `if (rigid != nullptr && ...)` and uses `rigid->friction()` and `rigid->bounciness()`.
   - Damping sets the angular damping only when `rigid != nullptr`.
   - Velocity sets the angular velocity only when `rigid != nullptr`.
   - Rename `PhysicsObject::kDirty*` to `PhysicsBase::kDirty*` throughout.
9. In `follow_driven`, read the GameObject's pose upright for a controller:

```cpp
        const Matrix4 now = driven->transform();
        if (same_matrix4(now, record.driven_pose)) {
            return false;
        }
        b3Vec3 position{};
        b3Quat rotation{};
        pose_of(record.controller ? upright_transform(now) : now, position, rotation);
```

   Its store becomes `object.store_simulated(matrix_of(position, rotation, object.transform()), object.velocity());`.
10. In `pull`, after `store_simulated`, `if (auto* rigid = dynamic_cast<PhysicsObject*>(object)) { rigid->store_angular_velocity(...); }`.
11. Replace `shape_center` and `shape_scale`:

```cpp
Vec3 PhysicsWorld::shape_center(const DataModel& game, const PhysicsBase& object) {
    if (dynamic_cast<const PlayerController*>(&object) != nullptr) {
        return Vec3{};
    }
    return center_for(game, object.driven_game_object());
}

float PhysicsWorld::shape_scale(const DataModel& game, const PhysicsBase& object) {
    if (dynamic_cast<const PlayerController*>(&object) != nullptr) {
        return 1.f;
    }
    return scale_for(game, object.driven_game_object());
}
```

12. Add the outline:

```cpp
void PhysicsWorld::collision_outline(const PlayerController& controller, std::vector<Vec3>& lines) {
    lines.clear();
    const float radius = static_cast<float>(controller.radius());
    const float gap = static_cast<float>(controller.hover_gap());
    const float tall = static_cast<float>(controller.height()) - gap;
    std::vector<b3Vec3> points;
    primitive_points(PhysicsObject::Shape::Cylinder, Vec3{2.f * radius, tall, 2.f * radius},
                     Vec3{0.f, gap + tall * 0.5f, 0.f}, points);
    if (b3HullData* hull = build_hull(points)) {
        outline_hull(*hull, lines);
        b3DestroyHull(hull);
    }
    if (gap > 0.f) {
        add_line(lines, Vec3{0.f, 0.f, 0.f}, Vec3{0.f, gap, 0.f});
    }
}
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[player]" && ./build/sandbox "[physics]"`
Expected: C1–C4 and all `[physics]` pass.

Then build the Studio, because GameView calls `shape_center`/`shape_scale`: `cmake --build build --target AnarchyStudio -j8`. Expected: it compiles, since `PhysicsObject&` binds to `const PhysicsBase&`.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/PhysicsWorld.hpp src/engine_core/PhysicsWorld.cpp sandbox/player_controller_tests.cpp
git diff --cached --stat
git commit -m "Simulate a PlayerController as an upright cylinder that never turns

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Probe for ground, hover over it, and report OnGround and IsSliding

**Files:**
- Modify: `src/engine_core/PhysicsWorld.cpp`, `src/engine_core/PhysicsWorld.hpp` (class comment)
- Test: `sandbox/player_controller_tests.cpp`

**Interfaces:**
- Consumes: `Body::controller`, `PlayerController::hover_gap()`, `max_slope()`, `mass()`, `store_ground(bool, bool)`.
- Produces: `Impl::control(DataModel& game, double dt)`, called in `step` between `reconcile` and `b3World_Step`. Inside it, `Impl::Ground probe(const PlayerController&, const Body&)`. Task 5 adds friction to `control`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/player_controller_tests.cpp`:

```cpp
namespace {

// Each step, sets the controller's speed across the ground to (vx, vz),
// keeping what physics gave it up and down, then steps.
void walk(PhysicsRig& rig, PlayerController& c, float vx, float vz, double time) {
    const int count = static_cast<int>(time / kStep + 0.5);
    for (int i = 0; i < count; ++i) {
        REQUIRE_FALSE(c.set_velocity(Vec3{vx, c.velocity().y, vz}));
        rig.steps(1);
    }
}

// An anchored ramp rising toward +X at degrees, its top through the origin.
PhysicsObject& ramp(PhysicsRig& rig, double degrees) {
    const double angle = degrees * 3.14159265358979 / 180.0;
    Matrix4 where = engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, angle);
    // The box's top face passes through the origin: its center sits half
    // its thickness below, along the face's normal (-sin, cos, 0).
    where.m[12] = static_cast<float>(0.5 * std::sin(angle));
    where.m[13] = static_cast<float>(-0.5 * std::cos(angle));
    PhysicsObject& slope = rig.body(where, Vec3{30.f, 1.f, 8.f}, true);
    return slope;
}

}  // namespace

TEST_CASE("C5 a PlayerController hovers StepHeight above the floor, on ground", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PlayerController& c = rig.controller(at(0.f, 3.f, 0.f));
    rig.play();
    rig.seconds(3.0);
    INFO(y_of(c.transform()));
    // Its feet are on the floor: the cylinder floats 0.4 above them, within 1%.
    REQUIRE(near(y_of(c.transform()), 0.f, 0.004f));
    REQUIRE(near(c.velocity().y, 0.f, 0.01f));
    REQUIRE(c.on_ground());
    REQUIRE_FALSE(c.is_sliding());
}

TEST_CASE("C6 it walks up an edge as tall as StepHeight, and no taller", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));

    SECTION("an edge 0.9 of StepHeight is walked up") {
        rig.body(at(4.f, 0.18f, 0.f), Vec3{4.f, 0.36f, 4.f}, true);
        rig.play();
        rig.seconds(1.0);
        walk(rig, c, 2.f, 0.f, 2.5);
        INFO(x_of(c.transform()) << " " << y_of(c.transform()));
        REQUIRE(x_of(c.transform()) > 3.f);
        REQUIRE(near(y_of(c.transform()), 0.36f, 0.01f));
        REQUIRE(c.on_ground());
    }

    SECTION("an edge 1.1 of StepHeight blocks it") {
        rig.body(at(4.f, 0.22f, 0.f), Vec3{4.f, 0.44f, 4.f}, true);
        rig.play();
        rig.seconds(1.0);
        walk(rig, c, 2.f, 0.f, 2.5);
        INFO(x_of(c.transform()));
        // The ledge's face is at x = 2, and the cylinder's radius 0.5.
        REQUIRE(x_of(c.transform()) <= 1.51f);
        REQUIRE(near(y_of(c.transform()), 0.f, 0.01f));
    }
}

TEST_CASE("C7 an upward Velocity jumps, and the hover lets go", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
    rig.play();
    rig.seconds(1.0);
    REQUIRE(c.on_ground());
    REQUIRE_FALSE(c.set_velocity(Vec3{0.f, 5.f, 0.f}));
    float top = 0.f;
    bool left = false;
    for (int i = 0; i < 240; ++i) {
        rig.steps(1);
        top = std::max(top, y_of(c.transform()));
        left = left || !c.on_ground();
    }
    INFO(top);
    // 5 up under 9.81 rises 1.27; the hover pulling down would cut that short.
    REQUIRE(top > 1.15f);
    REQUIRE(left);
    rig.seconds(2.0);
    REQUIRE(c.on_ground());
}

TEST_CASE("C8 it stands still on a walkable slope and slides down a steep one", "[player]") {
    PhysicsRig rig;

    SECTION("30 degrees: on ground, and it does not creep") {
        ramp(rig, 30.0);
        PlayerController& c = rig.controller(at(0.f, 1.f, 0.f));
        rig.play();
        rig.seconds(1.0);
        const float x = x_of(c.transform());
        rig.seconds(2.0);
        INFO(x << " " << x_of(c.transform()));
        REQUIRE(near(x_of(c.transform()), x, 0.02f));
        REQUIRE(c.on_ground());
        REQUIRE_FALSE(c.is_sliding());
    }

    SECTION("60 degrees: sliding, and it goes down") {
        ramp(rig, 60.0);
        PlayerController& c = rig.controller(at(0.f, 1.f, 0.f));
        rig.play();
        bool slid = false;
        for (int i = 0; i < 480; ++i) {
            rig.steps(1);
            slid = slid || c.is_sliding();
            REQUIRE_FALSE(c.on_ground());
        }
        INFO(x_of(c.transform()));
        REQUIRE(slid);
        REQUIRE(x_of(c.transform()) < -1.f);
    }
}

TEST_CASE("C9 ground in odd places: none, anchored, StepHeight over Height, changes in play, Stop", "[player]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("in the air it is on nothing") {
        PlayerController& c = rig.controller(at(0.f, 20.f, 0.f));
        rig.play();
        rig.seconds(0.1);
        REQUIRE_FALSE(c.on_ground());
        REQUIRE_FALSE(c.is_sliding());
    }

    SECTION("anchored, it is never on ground") {
        PlayerController& c = rig.controller(at(0.f, 0.4f, 0.f));
        c.set_anchored(true);
        rig.play();
        rig.seconds(0.5);
        REQUIRE_FALSE(c.on_ground());
    }

    SECTION("StepHeight above Height: a thin slab at the top, its feet still on the floor") {
        PlayerController& c = rig.controller(at(0.f, 3.f, 0.f));
        REQUIRE_FALSE(c.set_step_height(3.0));
        rig.play();
        rig.seconds(3.0);
        INFO(y_of(c.transform()));
        REQUIRE(near(y_of(c.transform()), 0.f, 0.03f));
        REQUIRE(c.on_ground());
        REQUIRE(c.step_height() == 3.0);
    }

    SECTION("raising StepHeight during play lifts the cylinder, not the feet, and launches nothing") {
        PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
        rig.play();
        rig.seconds(1.0);
        REQUIRE_FALSE(c.set_step_height(0.8));
        float fastest = 0.f;
        for (int i = 0; i < 240; ++i) {
            rig.steps(1);
            fastest = std::max(fastest, std::fabs(c.velocity().y));
        }
        INFO(fastest << " " << y_of(c.transform()));
        REQUIRE(fastest < 5.f);
        REQUIRE(near(y_of(c.transform()), 0.f, 0.01f));
        REQUIRE(c.on_ground());
    }

    SECTION("Stop clears OnGround") {
        PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
        rig.play();
        rig.seconds(1.0);
        REQUIRE(c.on_ground());
        rig.game.stop_simulation();
        REQUIRE_FALSE(c.on_ground());
    }
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "C5*,C6*,C7*,C8*,C9*"`
Expected: FAIL. C5 finds the feet at about -0.4 (the cylinder sits on the floor), and on_ground is false.

- [ ] **Step 3: Add the constants**

At the top of `PhysicsWorld.cpp`, after `kRoundSides`:

```cpp
// A PlayerController's ground probe: a flat ring of points kProbeWidth of
// Radius across, cast down from kProbeSkin above the cylinder's bottom. A
// probe that starts inside something is cast again narrower, down to
// kProbeNarrowest. It reaches the hover gap again below the feet, at least
// kSnapMin, so the controller keeps to stairs and slopes going down.
constexpr int kProbeSides = 16;
constexpr float kProbeWidth = 0.95f;
constexpr float kProbeNarrowest = 0.6f;
constexpr float kProbeSkin = 0.01f;
constexpr float kSnapMin = 0.1f;
// The hover: a spring at this many hertz, critically damped.
constexpr float kHoverFrequency = 6.f;
// Faster than this up, relative to the ground, and above the hover gap, a
// controller is leaving the ground: a jump.
constexpr float kRisingSpeed = 0.1f;
constexpr float kPi = 3.14159265359f;
```

If `collision_outline` already declares a local `kPi`, remove that local so there is one.

- [ ] **Step 4: Add the probe**

In the anonymous namespace (before `struct PhysicsWorld::Impl`):

```cpp
// The closest shape a probe's cast hits, skipping its own body's. One it
// starts inside it skips too, and notes.
struct ProbeHits {
    b3BodyId self = b3_nullBodyId;
    float closest = 1.f;
    bool hit = false;
    bool started_inside = false;
    b3ShapeId shape = b3_nullShapeId;
    b3Vec3 point{};
    b3Vec3 normal{};
};

float probe_hit(b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t, int, int, void* context) {
    auto* hits = static_cast<ProbeHits*>(context);
    if (B3_ID_EQUALS(b3Shape_GetBody(shape), hits->self)) {
        return -1.f;
    }
    if (fraction == 0.f) {
        hits->started_inside = true;
        return -1.f;
    }
    if (fraction < hits->closest) {
        hits->closest = fraction;
        hits->hit = true;
        hits->shape = shape;
        hits->point = point;
        hits->normal = normal;
    }
    return hits->closest;
}
```

In `Impl`, add:

```cpp
    // What a PlayerController's probe found under it.
    struct Ground {
        bool hit = false;
        // How far below the cylinder's bottom the ground is: hover_gap() at rest.
        float gap = 0.f;
        b3Vec3 normal{0.f, 1.f, 0.f};
        b3Vec3 point{};
        b3BodyId body = b3_nullBodyId;
        // The ground's own velocity at point.
        b3Vec3 velocity{};
    };

    Ground probe(const PlayerController& controller, const Body& record) {
        const float radius = static_cast<float>(controller.radius());
        const float gap = static_cast<float>(controller.hover_gap());
        const b3Vec3 feet = b3Body_GetPosition(record.body);
        const float start = gap + kProbeSkin;
        const float reach = start + std::max(gap, kSnapMin);
        const b3Vec3 origin{feet.x, feet.y + start, feet.z};
        b3Vec3 ring[kProbeSides];
        for (float width = kProbeWidth; width > kProbeNarrowest - 1e-4f; width -= 0.1f) {
            for (int side = 0; side < kProbeSides; ++side) {
                const float angle = 2.f * kPi * static_cast<float>(side) / static_cast<float>(kProbeSides);
                ring[side] = b3Vec3{radius * width * std::cos(angle), 0.f, radius * width * std::sin(angle)};
            }
            b3ShapeProxy proxy;
            proxy.points = ring;
            proxy.count = kProbeSides;
            proxy.radius = 0.f;
            ProbeHits hits;
            hits.self = record.body;
            b3World_CastShape(world, origin, &proxy, b3Vec3{0.f, -reach, 0.f}, b3DefaultQueryFilter(), probe_hit,
                              &hits);
            if (hits.started_inside) {
                continue;
            }
            Ground ground;
            if (!hits.hit) {
                return ground;
            }
            ground.hit = true;
            ground.gap = hits.closest * reach - kProbeSkin;
            ground.normal = hits.normal;
            ground.point = hits.point;
            ground.body = b3Shape_GetBody(hits.shape);
            ground.velocity = b3Body_GetWorldPointVelocity(ground.body, hits.point);
            return ground;
        }
        return Ground{};
    }
```

- [ ] **Step 5: Add the controller pass**

In `Impl`:

```cpp
    // ---- PlayerControllers ---------------------------------------------

    // Before Box3D steps: each controller probes for ground, says whether it
    // is on it or sliding, and on ground hovers hover_gap() above it. What
    // it does to its own velocity, it does the opposite of to a dynamic ground.
    void control(DataModel& game, double dt) {
        const float step = static_cast<float>(dt);
        for (auto& [id, record] : bodies) {
            if (!record.controller) {
                continue;
            }
            auto* controller = dynamic_cast<PlayerController*>(game.instance(id));
            if (controller == nullptr || controller->anchored()) {
                if (controller != nullptr) {
                    controller->store_ground(false, false);
                }
                continue;
            }
            const Ground ground = probe(*controller, record);
            const b3Vec3 before = b3Body_GetLinearVelocity(record.body);
            const float gap = static_cast<float>(controller->hover_gap());
            const bool rising = ground.hit && before.y - ground.velocity.y > kRisingSpeed &&
                                ground.gap > gap + kProbeSkin;
            const float slope =
                std::acos(std::min(std::max(ground.normal.y, -1.f), 1.f)) * 180.f / kPi;
            const bool steep = slope > static_cast<float>(controller->max_slope());
            const bool on_ground = ground.hit && !rising && !steep;
            controller->store_ground(on_ground, ground.hit && !rising && steep);
            if (!on_ground) {
                continue;
            }
            b3Vec3 velocity = before;
            // The hover, implicit as Box3D's mover sample's pogo, on velocity
            // up and down relative to the ground's. Box3D adds this step's
            // gravity after, so it is taken out first.
            const float omega = 2.f * kPi * kHoverFrequency;
            const float relative = velocity.y - ground.velocity.y;
            const float settled = (relative - omega * omega * step * (ground.gap - gap)) /
                                  (1.f + 2.f * omega * step + omega * omega * step * step);
            velocity.y = ground.velocity.y + settled - kGravity * step;
            b3Body_SetLinearVelocity(record.body, velocity);
            push_ground(ground, controller->mass(), b3Sub(velocity, before));
        }
    }

    // The opposite of change, times mass, into ground's body at the probe's
    // hit, when that body is dynamic.
    static void push_ground(const Ground& ground, double mass, b3Vec3 change) {
        if (!b3Body_IsValid(ground.body) || b3Body_GetType(ground.body) != b3_dynamicBody) {
            return;
        }
        const float scale = -static_cast<float>(mass);
        b3Body_ApplyLinearImpulse(ground.body, b3Vec3{change.x * scale, change.y * scale, change.z * scale},
                                  ground.point, true);
    }
```

In `step`, call it:

```cpp
        reconcile(game);
        control(game, dt);
        b3World_Step(world, static_cast<float>(dt), 1);
        pull(game);
```

In `destroy`, before erasing, clear a controller's flags so one that leaves Workspace is on nothing. `destroy` has no `game`, so instead do this in `reconcile`'s loop over `gone`, before `destroy(id)`:

```cpp
            if (auto* controller = dynamic_cast<PlayerController*>(game.instance(id))) {
                controller->store_ground(false, false);
            }
```

Check `b3Sub` exists (`grep -n "b3Sub" build/_deps/box3d-src/include/box3d/math_functions.h`); if not, subtract component-wise.

Add step 4b to the class comment in `PhysicsWorld.hpp`: "Each PlayerController probes for the ground under it, says whether it is OnGround or IsSliding, and on ground hovers its hover gap above it (PlayerController)."

- [ ] **Step 6: Run the tests**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[player]" && ./build/sandbox "[physics]"`
Expected: C1–C9 and every `[physics]` test pass.

If C5's settle misses the 0.004 tolerance, print `y_of` each 0.25 s, and check the gravity term's sign. Box3D adds `kGravity * dt` (negative) during the step, so the pass must leave `velocity.y` at `target - kGravity * dt`. If C6's blocked case climbs, check that the probe ignores the ledge top only because the puck never reaches it before the cylinder's side hits the face.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/PhysicsWorld.hpp src/engine_core/PhysicsWorld.cpp sandbox/player_controller_tests.cpp
git diff --cached --stat
git commit -m "Hover a PlayerController over the ground its probe finds

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Ground friction and pushing back on the ground

**Files:**
- Modify: `src/engine_core/PhysicsWorld.cpp` (`control`)
- Test: `sandbox/player_controller_tests.cpp`

**Interfaces:**
- Consumes: `Impl::control`, `Ground`, `push_ground` from Task 4; `PlayerController::friction()`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/player_controller_tests.cpp`:

```cpp
TEST_CASE("C10 Friction slows it across the ground as exp(-Friction t), and only there", "[player]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("on the ground, Friction 8") {
        PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
        rig.play();
        rig.seconds(1.0);
        REQUIRE_FALSE(c.set_velocity(Vec3{4.f, 0.f, 0.f}));
        rig.seconds(0.25);
        INFO(c.velocity().x);
        REQUIRE(near(c.velocity().x, 4.f * std::exp(-8.f * 0.25f), 0.05f));
    }

    SECTION("on the ground, Friction 0") {
        PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
        REQUIRE_FALSE(c.set_friction(0.0));
        rig.play();
        rig.seconds(1.0);
        REQUIRE_FALSE(c.set_velocity(Vec3{4.f, 0.f, 0.f}));
        rig.seconds(0.5);
        REQUIRE(near(c.velocity().x, 4.f, 0.01f));
    }

    SECTION("in the air, nothing") {
        PlayerController& c = rig.controller(at(0.f, 30.f, 0.f));
        REQUIRE_FALSE(c.set_velocity(Vec3{4.f, 0.f, 0.f}));
        rig.play();
        rig.seconds(0.5);
        REQUIRE(near(c.velocity().x, 4.f, 0.01f));
    }
}

TEST_CASE("C11 a moving platform carries it", "[player]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    REQUIRE_FALSE(floor.set_friction(0.0));
    PhysicsObject& platform = rig.body(at(0.f, 0.25f, 0.f), Vec3{6.f, 0.5f, 6.f}, false);
    REQUIRE_FALSE(platform.set_friction(0.0));
    REQUIRE_FALSE(platform.set_mass(10000.0));
    PlayerController& c = rig.controller(at(0.f, 1.f, 0.f));
    rig.play();
    rig.seconds(1.0);
    REQUIRE_FALSE(platform.set_velocity(Vec3{2.f, 0.f, 0.f}));
    rig.seconds(1.5);
    INFO(c.velocity().x << " " << x_of(c.transform()) << " " << x_of(platform.transform()));
    REQUIRE(near(c.velocity().x, 2.f, 0.05f));
    REQUIRE(near(x_of(c.transform()) - x_of(platform.transform()), 0.f, 0.2f));
    REQUIRE(c.on_ground());
}

TEST_CASE("C12 it slides along a wall it is pushed into", "[player]") {
    PhysicsRig rig;
    rig.floor();
    // A wall whose face is at x = 1.
    rig.body(at(1.5f, 2.f, 0.f), Vec3{1.f, 4.f, 40.f}, true);
    PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
    rig.play();
    rig.seconds(1.0);
    walk(rig, c, 3.f, 3.f, 1.0);
    INFO(x_of(c.transform()) << " " << z_of(c.transform()));
    REQUIRE(x_of(c.transform()) <= 0.51f);
    REQUIRE(z_of(c.transform()) > 2.5f);
}

TEST_CASE("C13 it pushes down on what it stands on", "[player]") {
    PhysicsRig rig;
    rig.floor();
    // A plank balanced on a ridge along Z.
    rig.body(at(0.f, 0.25f, 0.f), Vec3{0.2f, 0.5f, 4.f}, true);
    PhysicsObject& plank = rig.body(at(0.f, 0.6f, 0.f), Vec3{6.f, 0.2f, 2.f}, false);
    REQUIRE_FALSE(plank.set_mass(10.0));
    PlayerController& c = rig.controller(at(2.5f, 1.2f, 0.f));
    REQUIRE_FALSE(c.set_mass(50.0));
    rig.play();
    rig.seconds(2.0);
    // The plank's +X axis tips down toward the controller's end.
    INFO(plank.transform().m[1]);
    REQUIRE(plank.transform().m[1] < -0.05f);
}

TEST_CASE("C15 one PlayerController stands on another and the stack settles", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PlayerController& below = rig.controller(at(0.f, 0.5f, 0.f));
    PlayerController& above = rig.controller(at(0.f, 4.f, 0.f));
    rig.play();
    rig.seconds(4.0);
    INFO(y_of(below.transform()) << " " << y_of(above.transform()));
    REQUIRE(std::isfinite(y_of(above.transform())));
    REQUIRE(near(y_of(below.transform()), 0.f, 0.03f));
    // The lower one's top is at 2; the upper one's feet hover on it.
    REQUIRE(near(y_of(above.transform()), 2.f, 0.05f));
    REQUIRE(above.on_ground());
    REQUIRE(near(above.velocity().y, 0.f, 0.05f));
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "C10*,C11*,C12*,C13*,C15*"`
Expected: C10 "on the ground, Friction 8" fails with the speed still about 4, and C11 fails because the platform slides out from under the controller. C12, C13, and C15 may already pass; that's fine, they guard behavior.

- [ ] **Step 3: Add friction to `control`**

In `control`, between computing the hover's `velocity.y` and `b3Body_SetLinearVelocity`:

```cpp
            // Friction, across the ground only: speed relative to the
            // ground's own decays by exp(-Friction dt).
            const float keep = std::exp(-static_cast<float>(controller->friction()) * step);
            velocity.x = ground.velocity.x + (velocity.x - ground.velocity.x) * keep;
            velocity.z = ground.velocity.z + (velocity.z - ground.velocity.z) * keep;
```

`push_ground` already takes the whole change, so the platform and plank feel the friction too.

Update the comment above `control` to say that it also slows the controller across the ground by Friction.

- [ ] **Step 4: Run every test**

Run: `cmake --build build --target sandbox -j8 && ./build/sandbox "[player]" && ./build/sandbox "[physics]"`
Expected: every test passes.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.cpp sandbox/player_controller_tests.cpp
git diff --cached --stat
git commit -m "Slow a PlayerController by its Friction while it is on ground

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Studio and docs

**Files:**
- Modify: `src/runner/GameView.cpp` (`readSelectedBodies`), `src/ide/IdeIcons.cpp:~76`, `src/ide/ClassOrder.hpp:~24`, `README.md` (the PhysicsObject paragraph near line 43 and the PVInstance one), `src/engine_instances/README.md`, `tests/LuauCompleteTest.cpp:~1200` if it lists creatable classes exactly
- Test: build + existing studio tests + one MCP playtest

**Interfaces:**
- Consumes: `PhysicsWorld::collision_outline(const PlayerController&, std::vector<Vec3>&)`, `PlayerController`.

- [ ] **Step 1: Outline a selected PlayerController**

In `src/runner/GameView.cpp`, include `PlayerController.hpp`. At the top of the loop body in `readSelectedBodies`, before the `PhysicsObject` cast:

```cpp
        if (const auto* controller = dynamic_cast<const engine_core::PlayerController*>(game_->instance(id))) {
            if (!game_->in_workspace(id)) {
                continue;
            }
            BodyOutline& outline = outlineScratch_.emplace_back();
            for (BodyOutline& kept : outlines_) {
                if (kept.id == id) {
                    outline = std::move(kept);
                    kept.id = 0;
                    break;
                }
            }
            // Its outline is made from Radius, Height, and the hover gap, kept in size.
            const engine_core::Vec3 size{static_cast<float>(controller->radius()),
                                         static_cast<float>(controller->height()),
                                         static_cast<float>(controller->hover_gap())};
            const bool made = outline.id == id;
            if (!made || outline.size.x != size.x || outline.size.y != size.y || outline.size.z != size.z) {
                outline.size = size;
                engine_core::PhysicsWorld::collision_outline(*controller, outline.lines);
            }
            outline.id = id;
            outline.shape = -1;
            outline.driven = controller->driven_game_object();
            const engine_core::GameObject* driven = outline.driven != 0 ? game_->game_object(outline.driven) : nullptr;
            outline.transform = driven != nullptr ? driven->transform() : controller->transform();
            continue;
        }
```

- [ ] **Step 2: Icon, order, and docs**

`src/ide/IdeIcons.cpp`, beside the PhysicsObject case:

```cpp
    // The default icon, named so no PhysicsObject.png or PlayerController.png is looked for first.
    if (class_name == "PhysicsObject" || class_name == "PlayerController") {
        return "wat.gif";
    }
```

`src/ide/ClassOrder.hpp`, after `{"PhysicsObject", 3},`:

```cpp
        {"PlayerController", 3},
```

`src/engine_instances/README.md`:
- Add `PlayerController` to the list of instance classes.
- Rewrite the PhysicsObject paragraph to start: "`PhysicsBase` (`PhysicsBase.hpp`) is the abstract base of every rigid body, `PhysicsObject` and `PlayerController`: it holds Transform, Velocity, Anchored, Mass, LinearDamping, and GameObject, says `physics_body()`, and keeps the dirty mask." Keep the rest of the paragraph about PhysicsObject.
- Add: "`PlayerController` is an upright cylinder with no contact friction that `PhysicsWorld` hovers its hover gap above the ground and slows by its own `Friction` there; its `OnGround` and `IsSliding` are read-only, unsaved fields the world writes through `store_ground`."

`README.md`:
- After the PhysicsObject paragraph, add one for PlayerController, covering:
  - what it is for: a character's body, which scripts move by writing `Velocity`;
  - Radius, Height (ground to head), StepHeight (the hover gap, and the tallest edge it walks up), MaxSlope in degrees, and Friction (per second, only on ground, relative to the ground's motion, so platforms carry it);
  - OnGround and IsSliding;
  - that it turns only about Y and its Transform's position is its feet;
  - that a jump is an upward `Velocity.Y`.
- Say that `IsA("PhysicsBase")` is true of both PhysicsObject and PlayerController.
- In the PVInstance sentence, add PlayerController to the list.

If `tests/LuauCompleteTest.cpp:~1200` lists class names that completion must offer and fails without the new one, add `"PlayerController"` where the list is sorted.

- [ ] **Step 3: Build and run the suites**

```bash
cmake --build build -j8
./build/sandbox
./build/properties-tests
./build/studio-tests
./build/engine-tests
```

Run `ctest` if you prefer: `cd build && ctest --output-on-failure`.
Expected: everything passes.

- [ ] **Step 4: Playtest in the studio**

Rules: run one studio at a time, and quit it right after measuring. A subagent can't quit it, so the session owner runs this step. Build the bundle with its resources (`cmake --build build --target bundle-resources AnarchyStudio`), or icons and themes go missing. Launch `build/AnarchyStudio.app`, then use the anarchy MCP:

1. `select_studio` for the worktree's studio.
2. `run_lua`: create an anchored floor PhysicsObject (Size 40×1×40 at y −0.5), a step PhysicsObject 0.3 tall at x 3, a PlayerController at y 1 with a GameObject child or parent, and a Script that each Heartbeat sets the controller's `Velocity = Vector3.new(2, c.Velocity.Y, 0)`.
3. `playtest` for 3 seconds. Then `run_lua` to read `c.Transform.Position` (or the Transform), `c.OnGround`, and `c.IsSliding`. Expect x > 3, feet y ≈ 0.3, and OnGround true.
4. `screenshot` with the controller selected. Expect a cylinder outline and a short line under it.
5. Stop, check OnGround reads false, and quit the studio.

Report what each step showed.

- [ ] **Step 5: Commit**

```bash
git add src/runner/GameView.cpp src/ide/IdeIcons.cpp src/ide/ClassOrder.hpp README.md \
  src/engine_instances/README.md tests/LuauCompleteTest.cpp
git diff --cached --stat
git commit -m "Show a PlayerController in the Studio and describe it

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Only `git add tests/LuauCompleteTest.cpp` if it changed.
