# Edit-Mode Physics and Raycast Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the Box3D world alive and in step with the tree while the place is stopped (paused, never simulated), and give scripts `workspace:Raycast` against it in edit and play mode.

**Architecture:** `PhysicsWorld::step` splits into `sync` (bodies match the tree) and `simulate` (probe, step, write back). The Engine's stopped tick calls `sync`. While stopped, `sync` never writes to instances and never decomposes synchronously. `PhysicsWorld::raycast` wraps `b3World_CastRay`. Lua reaches the world through a `PhysicsWorld*` the Engine registers on the `DataModel`. `RaycastParams` / `RaycastResult` are new datatypes, and `Enum.RaycastFilterType` a new enum.

**Tech Stack:** C++20, Box3D, Luau, Catch2 (`sandbox` target), CMake with the Visual Studio 2019 generator (multi-config).

**Spec:** `docs/superpowers/specs/2026-10-06-edit-mode-physics-raycast-design.md`

> **Amendment (2026-10-06, after this plan was written):** main now makes a stopped body's Transform follow the GameObject it moves. `PhysicsWorld::follow_game_objects(game)` (static, `PhysicsWorld.cpp`) copies each driven GameObject's position and rotation onto its body through `store_simulated`, keeping the body's scale and a PlayerController upright; the Engine's stopped tick calls it beside `decomposer_.update`, and `P35` in `sandbox/physics_tests.cpp` pins it. This was asked for so a dragger over a GameObject with a body as a child finds the body where the GameObject is. It reverses this plan's premise that nothing writes a Transform while stopped, so when implementing: (1) drop the "no `store_simulated` while stopped" constraint and Review Focus 1; (2) flip E3 so `body.transform()` follows the GameObject (as P35 asserts) instead of staying identity; (3) the `seeded` flag and E5 are unnecessary, since the body's Transform is already the GameObject's when Play starts, so `follow_driven` keeps storing unconditionally as it does today; (4) fold `follow_game_objects` into `sync` (`follow_driven` already does the same for a body that exists) and delete the static helper and the Engine call once `sync` runs from the stopped tick. `store_ground` while stopped stays unwritten.

> **Merged (2026-10-07, into `terrain`):** the Amendment is applied: `sync` does what `follow_game_objects` did (the helper and its Engine call are gone), `seeded` and E5 are gone, E3 asserts the Transform follows. The final review's C1 guard stays: stopped, a driven body ignores a write to its own Transform and stores its body's pose back (E11, E12), and it does the same on the first played `sync` so a write just before Play cannot move the GameObject (E13). Task code below that seeds or skips `store_simulated` while stopped is superseded.

## Global Constraints

- Only `src/engine_core/PhysicsWorld.cpp` includes Box3D. Nothing else may.
- `PhysicsWorld::step` keeps doing nothing while the place is stopped; the stopped path is `sync` only.
- While stopped, nothing in `sync` writes to an instance (`store_simulated`, `store_ground`, `store_angular_velocity`) and nothing decomposes synchronously.
- Pressing Play does not remake bodies that exist. Stop drops every body (a new world_generation), as today.
- `workspace:Raycast(origin: Vector3, direction: Vector3, params: RaycastParams?) -> RaycastResult?`; the direction's length is the ray's length; `nil` on a miss or a zero direction.
- `Enum.RaycastFilterType { Exclude = 0, Include = 1 }`, Exclude the default.
- `RaycastResult` fields: `Instance`, `Position`, `Normal`, `Distance`, `Material` (always `nil` in this sub-project).
- Comments follow the codebase's style: plain English, say which thread and lock.
- Build: `cmake --build build --config Debug --target sandbox`. Run: `build/Debug/sandbox.exe "[tag]"`.

## Review Focus

1. **Editing a scene while stopped silently rewrites saved data.** A PhysicsObject driving a GameObject must keep its own authored Transform while stopped, even when the GameObject moves; otherwise saves and undo drift. Pinned in Task 1 (E3).
2. **Play after a long edit session.** An anchored PhysicsObject that drives a GameObject must still have its Transform seeded from the GameObject on the first played tick, as it was when bodies were born at Play. Pinned in Task 1 (E5).
3. **A ray that starts inside a part.** A reasonable user expects it to see what is beyond, not report a hit at distance 0. Pinned in Task 3 (R4).
4. **Filtering by a Model or Folder.** Listing a container in `FilterDescendantsInstances` must filter everything under it, at any depth. Pinned in Task 3 (R3) and Task 5 (L3).
5. **A Raycast in a script without an Engine (tests, tools, a Game without physics).** It must raise a clear error, not crash. Pinned in Task 5 (L5).

---

### Task 1: Split `step` into `sync` and `simulate`; sync while stopped without writing

**Files:**
- Modify: `src/engine_core/PhysicsWorld.hpp` (class comment lines 20-44, public API around line 53)
- Modify: `src/engine_core/PhysicsWorld.cpp` (`Impl::step` at 474-487, `create` around 1030-1062, `keep` 579-593, `follow_driven` 1383-1402, `Body` struct 383-415, public wrappers at 1435-1457)
- Modify: `sandbox/physics_rig.hpp` (add `sync_steps`)
- Create: `sandbox/edit_physics_tests.cpp`
- Modify: `CMakeLists.txt` (add the test file to `add_executable(sandbox ...)` near line 883)

**Interfaces:**
- Produces:
  - `void PhysicsWorld::sync(DataModel& game);`: bodies match the tree, playing or stopped. Creates the world when there is none or the world_generation changed. Pulls gravity.
  - `void PhysicsWorld::simulate(DataModel& game, double dt);`: does nothing while stopped; otherwise ground probes, `b3World_Step`, unclimb, write-back.
  - `void PhysicsWorld::step(DataModel& game, double dt);`: unchanged contract: nothing while stopped, else `sync` then `simulate`.
  - For tests: `std::uint64_t PhysicsWorld::body_key(InstanceId id) const;` (0 when none; `b3StoreBodyId`), `std::optional<Vec3> PhysicsWorld::body_position(InstanceId id) const;`.

- [ ] **Step 1: Write the failing tests**

Add to `sandbox/physics_rig.hpp`, inside `PhysicsRig`:

```cpp
    // What the Engine's stopped tick does: bodies follow the tree, nothing moves.
    void sync_steps(int count) {
        for (int i = 0; i < count; ++i) {
            physics.sync(game);
        }
    }
```

Create `sandbox/edit_physics_tests.cpp`:

```cpp
// PhysicsWorld while the place is stopped: bodies exist and follow the tree,
// but nothing is simulated and nothing is written back to instances.

#include "physics_rig.hpp"
#include "support.hpp"

#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using engine_core::GameObject;
using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

}  // namespace

TEST_CASE("E1 stopped, a PhysicsObject in Workspace has a body", "[physics][edit]") {
    PhysicsRig rig;
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.sync_steps(1);
    REQUIRE(rig.physics.has_body(box.id()));
    rig.game.set_parent(box.id(), rig.game.scene_service("Storage"));
    rig.sync_steps(1);
    REQUIRE_FALSE(rig.physics.has_body(box.id()));
}

TEST_CASE("E2 stopped, nothing falls however many ticks pass", "[physics][edit]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.sync_steps(600);
    REQUIRE(y_of(box.transform()) == 5.f);
    REQUIRE(rig.physics.body_position(box.id())->y == 5.f);
}

TEST_CASE("E3 stopped, a body follows its GameObject without rewriting its own Transform", "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 8.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->y == 8.f);
    part.set_transform(at(5.f, 8.f, 0.f));
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->x == 5.f);
    REQUIRE(engine_core::same_matrix4(body.transform(), engine_core::matrix4_identity()));
}

TEST_CASE("E4 Play keeps the bodies made while stopped, and Stop remakes them", "[physics][edit]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.sync_steps(1);
    const std::uint64_t before = rig.physics.body_key(box.id());
    REQUIRE(before != 0);
    rig.play();
    rig.steps(1);
    REQUIRE(rig.physics.body_key(box.id()) == before);
    rig.seconds(0.5);
    REQUIRE(y_of(box.transform()) < 5.f);
    rig.game.stop_simulation();
    rig.sync_steps(1);
    REQUIRE(rig.physics.has_body(box.id()));
    REQUIRE(rig.physics.body_position(box.id())->y == 5.f);
}

TEST_CASE("E5 an anchored body made while stopped seeds its Transform on the first played tick", "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(2.f, 3.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, true);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.sync_steps(5);
    REQUIRE(engine_core::same_matrix4(body.transform(), engine_core::matrix4_identity()));
    rig.play();
    rig.steps(1);
    REQUIRE(x_of(body.transform()) == 2.f);
    REQUIRE(y_of(body.transform()) == 3.f);
}
```

Add `sandbox/edit_physics_tests.cpp` to the `add_executable(sandbox ...)` list in `CMakeLists.txt`, right after `sandbox/physics_tests.cpp`.

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build --config Debug --target sandbox`
Expected: compile errors: `sync`, `body_key`, `body_position` are not members of `PhysicsWorld`.

- [ ] **Step 3: Implement**

In `PhysicsWorld.hpp`, replace the class comment's pipeline paragraph with:

```cpp
// The Box3D world behind PhysicsObject. It lives while the place is stopped
// too: then the Engine's stopped tick calls sync, and bodies follow the tree
// but are never simulated, as if everything were anchored. While the place
// plays, Engine::step_physics calls step once per physics substep, before
// Heartbeat. Only this class's source includes Box3D.
//
// sync, on SimulationThread under the write lock, playing or stopped:
//   1. A Stop since the last sync (a new world_generation) drops every body.
//   2. Every PhysicsBase in Workspace, at any depth, gets a body; one that
//      left Workspace or was destroyed loses its body. Of several naming one
//      GameObject, the first in tree order wins, and each other warns once.
//   3. What scripts and Properties changed since the last sync goes into the
//      bodies (PhysicsObject::take_dirty). A driven GameObject whose
//      Transform is not what physics last wrote was moved by someone else,
//      and its body jumps there. A body whose shape_center moved since its
//      shape was made is made again.
//   Stopped, sync writes nothing to any instance and never decomposes a
//   Mesh: an unanchored Custom without known pieces is a Hull until its
//   pieces are stored, or until Play, when it is made again.
//
// simulate, only while playing:
//   4. Each PlayerController probes for the ground under it (PlayerController).
//   5. Box3D steps by dt, in one substep: the engine's own substeps are
//      already 240 Hz.
//   6. Every body Box3D moved writes its Transform and Velocity (and a
//      PhysicsObject's AngularVelocity), and its GameObject's Transform with that
//      GameObject's scale kept. These writes fire no Changed and record no
//      history, as a GameObject's own velocity integration does.
//
// Gravity is (0, -Workspace.Gravity, 0), read again at each sync.
```

In the public section, replace the `step` declaration with:

```cpp
    // Bodies match the tree. Playing or stopped.
    void sync(DataModel& game);
    // Probes, steps, and writes back. Does nothing while game is not playing.
    void simulate(DataModel& game, double dt);
    // Does nothing while game is not playing; else sync, then simulate.
    void step(DataModel& game, double dt);
```

and after `shape_frictions`:

```cpp
    // For tests: a key naming the body's Box3D handle, the same while the
    // body lives; 0 when the PhysicsBase has none. And where the body is.
    std::uint64_t body_key(InstanceId id) const;
    std::optional<Vec3> body_position(InstanceId id) const;
```

(add `#include <optional>`).

In `PhysicsWorld.cpp`, add to `Impl::Body`:

```cpp
        // Its Transform was seeded from its GameObject in this play session.
        // Bodies made while stopped are seeded on the first played sync.
        bool seeded = false;
```

Replace `Impl::step` with:

```cpp
    void sync(DataModel& game) {
        if (!b3World_IsValid(world) || generation != game.world_generation()) {
            begin(game.world_generation());
        }
        pull_gravity(game);
        reconcile(game);
    }

    void simulate(DataModel& game, double dt) {
        if (!game.simulation_running()) {
            return;
        }
        control(game, dt);
        b3World_Step(world, static_cast<float>(dt), 1);
        unclimb(game);
        pull(game);
    }

    void step(DataModel& game, double dt) {
        if (!game.simulation_running()) {
            return;
        }
        sync(game);
        simulate(game, dt);
    }
```

In `create`, replace the block that seeds the Transform:

```cpp
        // Its Transform says where the body is from the start. Stopped, the
        // authored Transform is left alone; the first played sync seeds it.
        if (target != 0 && game.simulation_running()) {
            object.store_simulated(matrix_of(def.position, def.rotation, object.transform()), object.velocity());
            record.seeded = true;
        }
```

(Set `record.seeded` on the local `record` before it is moved into `bodies`.)

In `keep`, after `recenter(...)`:

```cpp
        if (game.simulation_running() && !body.seeded) {
            body.seeded = true;
            if (target != 0) {
                b3Vec3 position = b3Body_GetPosition(body.body);
                b3Quat rotation = b3Body_GetRotation(body.body);
                object.store_simulated(matrix_of(position, rotation, object.transform()), object.velocity());
            }
        }
```

In `follow_driven`, guard the write:

```cpp
        record.driven_pose = now;
        if (game.simulation_running()) {
            object.store_simulated(matrix_of(position, rotation, object.transform()), object.velocity());
        }
        return true;
```

In `reconcile`, guard the `store_ground(false, false)` for gone controllers with `if (game.simulation_running())`.

Public wrappers (replace the `step` wrapper at line 1435):

```cpp
void PhysicsWorld::sync(DataModel& game) { impl_->sync(game); }

void PhysicsWorld::simulate(DataModel& game, double dt) { impl_->simulate(game, dt); }

void PhysicsWorld::step(DataModel& game, double dt) { impl_->step(game, dt); }

std::uint64_t PhysicsWorld::body_key(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    return found == impl_->bodies.end() ? 0 : b3StoreBodyId(found->second.body);
}

std::optional<Vec3> PhysicsWorld::body_position(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    if (found == impl_->bodies.end()) {
        return std::nullopt;
    }
    return from_b3(b3Body_GetPosition(found->second.body));
}
```

- [ ] **Step 4: Run the new and the existing physics tests**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[physics]"`
Expected: E1–E5 and every P and player-controller test pass. If a P test fails, it relied on bodies not existing before Play; read it, and fix the code rather than the test unless the test asserted stopped-mode absence of bodies.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.hpp src/engine_core/PhysicsWorld.cpp sandbox/physics_rig.hpp sandbox/edit_physics_tests.cpp CMakeLists.txt
git commit -m "Keep physics bodies in step with the tree while stopped, without simulating"
```

---

### Task 2: An unanchored Custom never decomposes while stopped

**Files:**
- Modify: `src/engine_core/PhysicsWorld.cpp` (`Body` struct, `make_pieces` ~1256-1286, `keep` 579-593)
- Test: `sandbox/edit_physics_tests.cpp`

**Interfaces:**
- Consumes: `sync`, `body_key` (Task 1); `known_pieces`, `pieces_for`, `decompose_count`, `clear_piece_cache`, `remember_pieces` (`ConvexDecomposition.hpp`); `Mesh::file_stamp()`, `Mesh::store_pieces(kRecipe, pieces)`.
- Produces: `Body::made_without_pieces` (bool) and `Body::pieces_stamp` (std::string), internal only.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/edit_physics_tests.cpp` (add `#include "AssetInstances.hpp"`, `#include "ConvexDecomposition.hpp"`, `#include "MeshShapes.hpp"`, `#include "amesh.hpp"`):

```cpp
namespace {

// An unanchored Custom 2x2x2 at height 3, with a cube Mesh whose pieces are not known.
PhysicsObject& custom_cube(PhysicsRig& rig, engine_core::Mesh*& mesh_out) {
    engine_core::clear_piece_cache();
    engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
    PhysicsObject& body = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(body.set_mesh(instance_slot(mesh.id())));
    anarchy::amesh::Data cube;
    engine_core::add_box(cube, Vec3{1.f, 1.f, 1.f}, Vec3{0.f, 0.f, 0.f});
    REQUIRE_FALSE(mesh.edit_geometry([&cube](anarchy::amesh::Data& data) { data = cube; }));
    mesh_out = &mesh;
    return body;
}

}  // namespace

TEST_CASE("E6 stopped, an unanchored Custom without pieces is a Hull and decomposes nothing", "[physics][edit]") {
    PhysicsRig rig;
    engine_core::Mesh* mesh = nullptr;
    PhysicsObject& body = custom_cube(rig, mesh);
    const std::uint64_t decomposed = engine_core::decompose_count();
    rig.sync_steps(3);
    REQUIRE(rig.physics.has_body(body.id()));
    REQUIRE(engine_core::decompose_count() == decomposed);
    REQUIRE(rig.warnings.empty());
}

TEST_CASE("E7 a body made without pieces is made again at Play, with pieces", "[physics][edit]") {
    PhysicsRig rig;
    engine_core::Mesh* mesh = nullptr;
    PhysicsObject& body = custom_cube(rig, mesh);
    rig.sync_steps(1);
    const std::uint64_t decomposed = engine_core::decompose_count();
    rig.play();
    rig.steps(1);
    REQUIRE(engine_core::decompose_count() == decomposed + 1);
    engine_core::clear_piece_cache();
}

TEST_CASE("E8 stopped, a body made without pieces is made again once its Mesh stores them", "[physics][edit]") {
    PhysicsRig rig;
    engine_core::Mesh* mesh = nullptr;
    PhysicsObject& body = custom_cube(rig, mesh);
    rig.sync_steps(1);
    const std::size_t hull_shapes = rig.physics.shape_frictions(body.id()).size();
    // What the studio's decomposer does when it finishes: two pieces into the file.
    std::vector<anarchy::amesh::ConvexPiece> pieces(2);
    for (auto& piece : pieces) {
        piece.points = {{-1.f, -1.f, -1.f}, {1.f, -1.f, -1.f}, {-1.f, 1.f, -1.f}, {-1.f, -1.f, 1.f}};
    }
    REQUIRE_FALSE(mesh->store_pieces(engine_core::kRecipe, pieces));
    rig.sync_steps(1);
    REQUIRE(rig.physics.shape_frictions(body.id()).size() == 2);
    REQUIRE(hull_shapes == 1);
}
```

Check `anarchy::amesh::ConvexPiece`'s point type in `src/amesh/amesh.hpp` and adjust the initializer if it is not a list of 3-float arrays; `store_pieces` needs an open project in some paths — if it returns a reason in the rig, use the `Project`-backed setup that `P32` in `sandbox/physics_tests.cpp:1024` uses.

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[edit]"`
Expected: E6 fails (`decompose_count` went up: `pieces_for` decomposed while stopped). E8 fails (still 1 shape).

- [ ] **Step 3: Implement**

Add to `Impl::Body`:

```cpp
        // An unanchored Custom made while stopped, when its Mesh's pieces were
        // not known: it is a Hull, made again when the Mesh's file changes
        // (its stamp then) or when play starts.
        bool made_without_pieces = false;
        std::string pieces_stamp;
```

In `make_pieces`, replace the `pieces_for` line:

```cpp
        const auto* mesh = dynamic_cast<const Mesh*>(game.instance(object.mesh_id()));
        std::vector<anarchy::amesh::ConvexPiece> pieces;
        record.made_without_pieces = false;
        if (game.simulation_running()) {
            pieces = pieces_for(*mesh, mesh_points, triangles);
        } else if (!known_pieces(*mesh, mesh_points, triangles, pieces)) {
            // Stopped, never decompose here: the studio's decomposer will.
            record.made_without_pieces = true;
            record.pieces_stamp = mesh->file_stamp();
            return false;
        }
```

Then in the Custom case of `make_object_shape`, the fall-through to Hull must not warn when `record.made_without_pieces` is true. Read `make_hull`'s warning path: if it warns only on failure, nothing to change; if it warns that Custom fell back, skip that warning when `record.made_without_pieces`.

In `keep`, after `recenter(...)` and before the seeding block from Task 1:

```cpp
        if (body.made_without_pieces && !object.anchored()) {
            const auto* rigid = dynamic_cast<PhysicsObject*>(&object);
            const auto* mesh = rigid != nullptr ? dynamic_cast<const Mesh*>(game.instance(rigid->mesh_id())) : nullptr;
            if (game.simulation_running() || (mesh != nullptr && mesh->file_stamp() != body.pieces_stamp)) {
                make_shape(game, object, body);
            }
        }
```

- [ ] **Step 4: Run tests**

Run: `build/Debug/sandbox.exe "[physics]"` and `build/Debug/sandbox.exe "[convex]"` (the decomposition tests' tag; check `sandbox/convex_decomposition_tests.cpp` for the exact tag)
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.cpp sandbox/edit_physics_tests.cpp
git commit -m "Make an unanchored Custom a Hull while stopped until its pieces are stored"
```

---

### Task 3: `PhysicsWorld::raycast`

**Files:**
- Modify: `src/engine_core/PhysicsWorld.hpp`, `src/engine_core/PhysicsWorld.cpp`
- Create: `sandbox/raycast_tests.cpp`; add it to `CMakeLists.txt`'s sandbox list

**Interfaces:**
- Consumes: `sync` (Task 1); `DataModel::parent(InstanceId)`.
- Produces (in `PhysicsWorld.hpp`, namespace `engine_core`):

```cpp
// Which instances a ray sees: with include, only those that are, or are under,
// one of instances; else every instance but those.
struct RayFilter {
    bool include = false;
    std::vector<InstanceId> instances;
};

// What a ray hit first.
struct RayHit {
    InstanceId instance = 0;   // The PhysicsBase (later, Terrain) whose body was hit.
    Vec3 position{};
    Vec3 normal{};
    float distance = 0.f;
    // Box3D's surface material for the hit triangle or shape: a Terrain's
    // material Id. False for everything but Terrain.
    bool has_material = false;
    std::uint8_t material = 0;
};
```

and on `PhysicsWorld`:

```cpp
    // The first body a ray from origin along direction hits, within direction's
    // length, that filter lets it see. Syncs first, so it sees the tree as it
    // is now. A ray that starts inside a body does not hit it. Nothing for a
    // zero direction. SimulationThread, under the write lock.
    std::optional<RayHit> raycast(DataModel& game, Vec3 origin, Vec3 direction, const RayFilter& filter);
```

- [ ] **Step 1: Write the failing tests**

Create `sandbox/raycast_tests.cpp`:

```cpp
// PhysicsWorld::raycast, stopped and playing.

#include "physics_rig.hpp"
#include "support.hpp"

#include "Folder.hpp"
#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using engine_core::PhysicsObject;
using engine_core::RayFilter;
using engine_core::Vec3;

using namespace physics_rig;

}  // namespace

TEST_CASE("R1 a ray down hits the floor's top, stopped", "[physics][raycast]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    const auto hit = rig.physics.raycast(rig.game, Vec3{1.f, 10.f, 2.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{});
    REQUIRE(hit.has_value());
    REQUIRE(hit->instance == floor.id());
    REQUIRE(near(hit->position.y, 0.f, 1e-3f));
    REQUIRE(near(hit->normal.y, 1.f, 1e-3f));
    REQUIRE(near(hit->distance, 10.f, 1e-3f));
    REQUIRE_FALSE(hit->has_material);
}

TEST_CASE("R2 a ray too short, or with no length, hits nothing", "[physics][raycast]") {
    PhysicsRig rig;
    rig.floor();
    REQUIRE_FALSE(rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -5.f, 0.f}, RayFilter{}));
    REQUIRE_FALSE(rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{}, RayFilter{}));
}

TEST_CASE("R3 Exclude and Include filter whole subtrees", "[physics][raycast]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(folder.id(), workspace_of(rig.game));
    engine_core::Folder& inner = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(inner.id(), folder.id());
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f}, true, inner.id());
    const Vec3 origin{0.f, 10.f, 0.f};
    const Vec3 down{0.f, -20.f, 0.f};

    REQUIRE(rig.physics.raycast(rig.game, origin, down, RayFilter{})->instance == box.id());

    RayFilter exclude;
    exclude.instances = {folder.id()};
    REQUIRE(rig.physics.raycast(rig.game, origin, down, exclude)->instance == floor.id());

    RayFilter include;
    include.include = true;
    include.instances = {floor.id()};
    REQUIRE(rig.physics.raycast(rig.game, origin, down, include)->instance == floor.id());

    include.instances = {};
    REQUIRE_FALSE(rig.physics.raycast(rig.game, origin, down, include));
}

TEST_CASE("R4 a ray that starts inside a box sees past it", "[physics][raycast]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    rig.body(at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f}, true);
    const auto hit = rig.physics.raycast(rig.game, Vec3{0.f, 5.f, 0.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{});
    REQUIRE(hit.has_value());
    REQUIRE(hit->instance == floor.id());
}

TEST_CASE("R5 a ray sees a Transform set just before it, and works while playing", "[physics][raycast]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f}, true);
    rig.physics.sync(rig.game);
    REQUIRE_FALSE(box.set_transform(at(10.f, 5.f, 0.f)));
    auto hit = rig.physics.raycast(rig.game, Vec3{10.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{});
    REQUIRE(hit->instance == box.id());

    rig.play();
    rig.steps(1);
    hit = rig.physics.raycast(rig.game, Vec3{10.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{});
    REQUIRE(hit->instance == box.id());
    REQUIRE(near(hit->position.y, 6.f, 1e-3f));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build --config Debug --target sandbox`
Expected: compile errors: `RayFilter` / `raycast` undefined.

- [ ] **Step 3: Implement**

Add the declarations above to `PhysicsWorld.hpp` (before the class; `raycast` as a public member).

In `PhysicsWorld.cpp`'s anonymous namespace, after `probe_hit`:

```cpp
// The closest hit a script's ray keeps, skipping what the filter hides and
// bodies it starts inside.
struct RayHits {
    const DataModel* game = nullptr;
    const RayFilter* filter = nullptr;
    float closest = 1.f;
    bool hit = false;
    InstanceId instance = 0;
    b3Vec3 point{};
    b3Vec3 normal{};
    uint64_t material = 0;
};

// Whether id is one of instances or under one of them.
bool under_any(const DataModel& game, InstanceId id, const std::vector<InstanceId>& instances) {
    for (InstanceId at = id; at != 0; at = game.parent(at)) {
        if (std::find(instances.begin(), instances.end(), at) != instances.end()) {
            return true;
        }
    }
    return false;
}

float ray_hit(b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t material, int, int,
              void* context) {
    auto* hits = static_cast<RayHits*>(context);
    if (fraction == 0.f) {
        return -1.f;
    }
    const InstanceId id = id_of(b3Body_GetUserData(b3Shape_GetBody(shape)));
    if (under_any(*hits->game, id, hits->filter->instances) != hits->filter->include) {
        return -1.f;
    }
    if (fraction < hits->closest) {
        hits->closest = fraction;
        hits->hit = true;
        hits->instance = id;
        hits->point = point;
        hits->normal = normal;
        hits->material = material;
    }
    return hits->closest;
}
```

(`id_of` already exists in this file for `userData`; `kNoParent` is 0 — confirm in `DataModel.hpp` and use the constant if it is not.)

On `Impl`:

```cpp
    std::optional<RayHit> raycast(DataModel& game, Vec3 origin, Vec3 direction, const RayFilter& filter) {
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (!(length > 0.f) || !std::isfinite(length)) {
            return std::nullopt;
        }
        sync(game);
        RayHits hits;
        hits.game = &game;
        hits.filter = &filter;
        b3World_CastRay(world, to_b3(origin), to_b3(direction), b3DefaultQueryFilter(), ray_hit, &hits);
        if (!hits.hit) {
            return std::nullopt;
        }
        RayHit out;
        out.instance = hits.instance;
        out.position = from_b3(hits.point);
        out.normal = from_b3(hits.normal);
        out.distance = hits.closest * length;
        return out;
    }
```

(`has_material` stays false; Terrain sets it in sub-project 1.) Public wrapper:

```cpp
std::optional<RayHit> PhysicsWorld::raycast(DataModel& game, Vec3 origin, Vec3 direction, const RayFilter& filter) {
    return impl_->raycast(game, origin, direction, filter);
}
```

If `b3Pos` is not `b3Vec3` in this Box3D build, convert the way `probe` does (`hits.point = point` is used there with `b3Vec3`; follow it).

- [ ] **Step 4: Run tests**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[physics]"`
Expected: R1–R5 pass, everything else still passes.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/PhysicsWorld.hpp src/engine_core/PhysicsWorld.cpp sandbox/raycast_tests.cpp CMakeLists.txt
git commit -m "Cast rays against the physics world, filtered by subtree"
```

---

### Task 4: The Engine syncs while stopped and registers its world on the DataModel

**Files:**
- Modify: `src/engine_core/DataModelState.hpp` (hooks near line 216)
- Modify: `src/engine_core/DataModel.hpp` (near `set_script_analysis`, line 261), `src/engine_core/DataModel.cpp` (near line 1867)
- Modify: `src/engine_core/Engine.cpp` (constructor near line 54-73; stopped tick near line 287-296)
- Test: `sandbox/edit_physics_tests.cpp`

**Interfaces:**
- Produces: `void DataModel::set_physics(PhysicsWorld* physics);` and `PhysicsWorld* DataModel::physics() const;` (null when no Engine registered one).

- [ ] **Step 1: Write the failing test**

Append to `sandbox/edit_physics_tests.cpp` (add `#include "Engine.hpp"`, `<chrono>`, `<thread>`). It follows Q5 in `sandbox/convex_decomposition_tests.cpp:241`:

```cpp
TEST_CASE("E9 a stopped Engine keeps bodies and registers its world", "[physics][edit][engine]") {
    engine_core::Engine engine;
    engine.start();
    // The studio's Engine is paused whenever the place is stopped.
    REQUIRE(engine.paused());
    InstanceId id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        auto& object = game.create<PhysicsObject>();
        object.set_anchored(true);
        game.set_parent(object.id(), workspace_of(game));
        id = object.id();
    });
    bool synced = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!synced && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        engine.on_simulation([&](engine_core::DataModel& game) {
            synced = game.physics() != nullptr && game.physics()->has_body(id);
        });
    }
    engine.stop();
    REQUIRE(synced);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Debug --target sandbox`
Expected: compile error: `DataModel` has no `physics()`.

- [ ] **Step 3: Implement**

`DataModelState.hpp`, beside `ScriptAnalysis* script_analysis`: forward-declare `class PhysicsWorld;` at the top and add

```cpp
    // The Engine's physics world, for scripts' queries. Null without an Engine.
    PhysicsWorld* physics = nullptr;
```

`DataModel.hpp`, after `script_analysis()` (forward-declare `class PhysicsWorld;`):

```cpp
    // The physics world scripts' queries (Workspace:Raycast) use. The Engine
    // sets it; null without one.
    void set_physics(PhysicsWorld* physics);
    PhysicsWorld* physics() const;
```

`DataModel.cpp`, after `script_analysis()`:

```cpp
void DataModel::set_physics(PhysicsWorld* physics) { state_->physics = physics; }

PhysicsWorld* DataModel::physics() const { return state_->physics; }
```

`Engine.cpp` constructor, after `physics_.set_warning_sink(...)`:

```cpp
    game_.set_physics(&physics_);
```

`Engine.cpp` stopped tick: rename the scope and add the sync after the decomposer:

```cpp
                    // Stopped, Custom PhysicsObjects' Meshes are split into convex
                    // pieces, and bodies follow the tree without being simulated.
                    // A paused test writes no files.
                    guarded_step(
                        [&] {
                            PROFILE_SCOPE("Stopped physics", profiler::Group::Physics);
                            DataModelLock lock(game_, DataModelLock::Write);
                            if (!game_.simulation_running()) {
                                decomposer_.update(game_);
                                physics_.sync(game_);
                            }
                        },
                        [&] { contract_count_.fetch_add(1); });
```

In the Engine destructor (or wherever `physics_` goes away), call `game_.set_physics(nullptr)` first if `game_` can outlive `physics_`; check member order in `Engine.hpp:128-132`.

- [ ] **Step 4: Run tests**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[physics]"`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/DataModelState.hpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/Engine.cpp sandbox/edit_physics_tests.cpp
git commit -m "Sync physics on the Engine's stopped tick and register the world for scripts"
```

---

### Task 5: `workspace:Raycast`, `RaycastParams`, `RaycastResult`, `Enum.RaycastFilterType`

**Files:**
- Modify: `src/engine_datatypes/Enum.hpp` (near line 42-45), `src/engine_datatypes/Enum.cpp` (near 192, 221, 232, 312)
- Create: `src/engine_core/RaycastBindings.cpp`
- Modify: `src/engine_core/ScriptBindings.hpp` (static declarations near line 316-320; a new `open_raycast` declaration)
- Modify: `src/engine_core/ScriptRuntime.cpp` (the `open_*` calls near line 762-766)
- Modify: `CMakeLists.txt` (add `src/engine_core/RaycastBindings.cpp` after `src/engine_core/ScriptBindings.cpp`, line 567)
- Modify: `src/engine_instances/README.md` or `src/engine_services/README.md`, whichever documents Workspace
- Create: `sandbox/raycast_lua_tests.cpp`; add it to `CMakeLists.txt`'s sandbox list

**Interfaces:**
- Consumes: `PhysicsWorld::raycast`, `RayFilter`, `RayHit` (Task 3); `DataModel::physics()` (Task 4); `ScriptRuntime::push_instance`, `runtime_from`, `lua_guard`, `InstanceUd`, `kInstanceMeta`, `test_userdata` / `check_userdata` (`LuaUserdata.hpp`), `check_enum_arg`, `push_enum_item`.
- Produces:
  - `const EnumType& raycast_filter_type_enum();` and `enum class RaycastFilterType { Exclude = 0, Include = 1 };` in `Enum.hpp`.
  - Luau globals `RaycastParams.new()`; userdata types `RaycastParams` (metatable `"AE.RaycastParams"`), `RaycastResult` (metatable `"AE.RaycastResult"`).
  - `static int ScriptBindings::workspace_raycast(lua_State*)`, `void open_raycast(lua_State*)`.

- [ ] **Step 1: Write the failing tests**

Create `sandbox/raycast_lua_tests.cpp`:

```cpp
// workspace:Raycast from Luau, with RaycastParams and RaycastResult.

#include "physics_rig.hpp"
#include "support.hpp"

#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

bool has_line(const std::string& output, const std::string& line) { return output.find(line) != std::string::npos; }

// A ScriptRig whose Game has a physics world, as an Engine's does.
struct RaycastRig : ScriptRig {
    engine_core::PhysicsWorld physics;
    RaycastRig() { game.set_physics(&physics); }
    ~RaycastRig() { game.set_physics(nullptr); }

    PhysicsObject& body(engine_core::Matrix4 where, Vec3 size, const char* name) {
        PhysicsObject& object = game.create<PhysicsObject>();
        REQUIRE_FALSE(object.set_transform(where));
        REQUIRE_FALSE(object.set_size(size));
        object.set_anchored(true);
        game.set_name(object.id(), name);
        game.set_parent(object.id(), workspace_of(game));
        return object;
    }

    std::string run(const char* source) {
        runtime.run_chunk(source);
        frames(1);
        return runtime.drain_output();
    }
};

}  // namespace

TEST_CASE("L1 Raycast returns what it hit", "[raycast][lua]") {
    RaycastRig rig;
    rig.body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, "Floor");
    const std::string out = rig.run(R"(
        local r = workspace:Raycast(Vector3.new(1, 10, 2), Vector3.new(0, -20, 0))
        print(typeof(r), r.Instance.Name, r.Position.Y, r.Normal.Y, r.Distance, r.Material)
    )");
    INFO(out);
    REQUIRE(has_line(out, "RaycastResult\tFloor\t0\t1\t10\tnil\n"));
}

TEST_CASE("L2 a miss is nil", "[raycast][lua]") {
    RaycastRig rig;
    rig.body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, "Floor");
    const std::string out = rig.run(R"(print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, 5, 0))))");
    REQUIRE(has_line(out, "nil\n"));
}

TEST_CASE("L3 RaycastParams filters", "[raycast][lua]") {
    RaycastRig rig;
    rig.body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, "Floor");
    rig.body(at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f}, "Box");
    const std::string out = rig.run(R"(
        local p = RaycastParams.new()
        print(typeof(p), p.FilterType == Enum.RaycastFilterType.Exclude, #p.FilterDescendantsInstances)
        p.FilterDescendantsInstances = { workspace.Box }
        print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0), p).Instance.Name)
        p.FilterType = Enum.RaycastFilterType.Include
        print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0), p).Instance.Name)
    )");
    INFO(out);
    REQUIRE(has_line(out, "RaycastParams\ttrue\t0\n"));
    REQUIRE(has_line(out, "Floor\n"));
    REQUIRE(has_line(out, "Box\n"));
}

TEST_CASE("L4 bad arguments and read-only results raise", "[raycast][lua]") {
    RaycastRig rig;
    rig.body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, "Floor");
    const std::string out = rig.run(R"(
        print(pcall(function() return workspace:Raycast(1, Vector3.new(0, -1, 0)) end))
        print(pcall(function() return workspace:Raycast(Vector3.new(0, 1, 0), Vector3.new(0, -2, 0), 5) end))
        local r = workspace:Raycast(Vector3.new(0, 1, 0), Vector3.new(0, -2, 0))
        print(pcall(function() r.Distance = 3 end))
        local p = RaycastParams.new()
        print(pcall(function() p.FilterDescendantsInstances = { 5 } end))
    )");
    INFO(out);
    REQUIRE(has_line(out, "false\t") );
    REQUIRE(out.find("origin must be a Vector3") != std::string::npos);
    REQUIRE(out.find("params must be a RaycastParams") != std::string::npos);
    REQUIRE(out.find("Distance cannot be assigned to") != std::string::npos);
    REQUIRE(out.find("FilterDescendantsInstances must hold only Instances") != std::string::npos);
}

TEST_CASE("L5 Raycast without a physics world raises", "[raycast][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(print(pcall(function() return workspace:Raycast(Vector3.new(), Vector3.new(0, -1, 0)) end)))");
    rig.frames(1);
    REQUIRE(rig.runtime.drain_output().find("Raycast needs a running engine") != std::string::npos);
}
```

Use the game's actual name setter if it is not `set_name` (check `DataModel.hpp`). If `print` formats numbers like `10.0`, adjust L1's expected text to what `print` produces for other numbers in existing tests.

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[lua]"`
Expected: L1–L5 fail (Raycast is nil, RaycastParams is nil).

- [ ] **Step 3: Add the enum**

`Enum.hpp`, after the TransformSpace lines:

```cpp
// Exclude 0, Include 1. RaycastParams.FilterType.
const EnumType& raycast_filter_type_enum();
enum class RaycastFilterType { Exclude = 0, Include = 1 };
```

`Enum.cpp`: after `kTransformSpaces`:

```cpp
// Whether a ray sees only what RaycastParams lists (Include) or all but it.
const EnumEntry kRaycastFilterTypes[] = {
    {"Exclude", 0},
    {"Include", 1},
};
```

after `kTransformSpaceType`:

```cpp
const EnumType kRaycastFilterTypeType{"RaycastFilterType", kRaycastFilterTypes, count_of(kRaycastFilterTypes)};
```

add `&kRaycastFilterTypeType` to the end of `kTypes[]`, and after `transform_space_enum()`:

```cpp
const EnumType& raycast_filter_type_enum() { return kRaycastFilterTypeType; }
```

- [ ] **Step 4: Write the bindings**

`ScriptBindings.hpp`, in `struct ScriptBindings` beside `prefab_get_bounding_box`:

```cpp
    static int workspace_raycast(lua_State* state);
```

and at namespace scope:

```cpp
// RaycastParams and RaycastResult: their metatables and the RaycastParams global.
void open_raycast(lua_State* state);
```

Create `src/engine_core/RaycastBindings.cpp`:

```cpp
// Workspace:Raycast and the datatypes it takes and gives: RaycastParams, which
// a script fills in, and RaycastResult, which it only reads.

#include "Enum.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "PhysicsWorld.hpp"
#include "SceneService.hpp"
#include "ScriptBindings.hpp"
#include "ScriptRuntime.hpp"

#include "lua.h"
#include "lualib.h"

#include <cstring>
#include <new>
#include <vector>

namespace engine_core {
namespace {

const char* kRaycastParamsMeta = "AE.RaycastParams";
const char* kRaycastResultMeta = "AE.RaycastResult";

// Lives in a Luau userdata with a destructor, so its vector is freed with it.
struct RaycastParamsUd {
    int filter_type = 0;
    std::vector<InstanceId> instances;
};

struct RaycastResultUd {
    RayHit hit;
};

void destroy_params(void* data) { static_cast<RaycastParamsUd*>(data)->~RaycastParamsUd(); }

RaycastParamsUd* push_params(lua_State* state) {
    void* data = lua_newuserdatadtor(state, sizeof(RaycastParamsUd), destroy_params);
    auto* params = new (data) RaycastParamsUd();
    luaL_getmetatable(state, kRaycastParamsMeta);
    lua_setmetatable(state, -2);
    return params;
}

RaycastParamsUd* check_params(lua_State* state, int index) {
    return static_cast<RaycastParamsUd*>(luaL_checkudata(state, index, kRaycastParamsMeta));
}

int params_new(lua_State* state) {
    push_params(state);
    return 1;
}

int params_index(lua_State* state) {
    RaycastParamsUd* params = check_params(state, 1);
    const char* key = luaL_checkstring(state, 2);
    if (std::strcmp(key, "FilterType") == 0) {
        push_enum_item(state, raycast_filter_type_enum(), params->filter_type);
        return 1;
    }
    if (std::strcmp(key, "FilterDescendantsInstances") == 0) {
        ScriptRuntime* runtime = runtime_from(state);
        lua_createtable(state, static_cast<int>(params->instances.size()), 0);
        int slot = 1;
        for (InstanceId id : params->instances) {
            if (runtime != nullptr && runtime->game_ != nullptr && runtime->game_->instance(id) != nullptr) {
                runtime->push_instance(state, id);
                lua_rawseti(state, -2, slot++);
            }
        }
        return 1;
    }
    luaL_error(state, "%s is not a valid member of RaycastParams", key);
}

int params_newindex(lua_State* state) {
    RaycastParamsUd* params = check_params(state, 1);
    const char* key = luaL_checkstring(state, 2);
    if (std::strcmp(key, "FilterType") == 0) {
        params->filter_type = check_enum_arg(state, 3, raycast_filter_type_enum());
        return 0;
    }
    if (std::strcmp(key, "FilterDescendantsInstances") == 0) {
        luaL_checktype(state, 3, LUA_TTABLE);
        std::vector<InstanceId> instances;
        const int count = lua_objlen(state, 3);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(state, 3, i);
            auto* ud = static_cast<InstanceUd*>(test_userdata(state, -1, kInstanceMeta));
            if (ud == nullptr) {
                luaL_error(state, "FilterDescendantsInstances must hold only Instances");
            }
            instances.push_back(ud->id);
            lua_pop(state, 1);
        }
        params->instances = std::move(instances);
        return 0;
    }
    luaL_error(state, "%s is not a valid member of RaycastParams", key);
}

void push_result(lua_State* state, const RayHit& hit) {
    auto* result = static_cast<RaycastResultUd*>(lua_newuserdata(state, sizeof(RaycastResultUd)));
    new (result) RaycastResultUd{hit};
    luaL_getmetatable(state, kRaycastResultMeta);
    lua_setmetatable(state, -2);
}

int result_index(lua_State* state) {
    auto* result = static_cast<RaycastResultUd*>(luaL_checkudata(state, 1, kRaycastResultMeta));
    const char* key = luaL_checkstring(state, 2);
    const RayHit& hit = result->hit;
    if (std::strcmp(key, "Instance") == 0) {
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr || runtime->game_->instance(hit.instance) == nullptr) {
            lua_pushnil(state);
        } else {
            runtime->push_instance(state, hit.instance);
        }
    } else if (std::strcmp(key, "Position") == 0) {
        lua_pushvector(state, hit.position.x, hit.position.y, hit.position.z);
    } else if (std::strcmp(key, "Normal") == 0) {
        lua_pushvector(state, hit.normal.x, hit.normal.y, hit.normal.z);
    } else if (std::strcmp(key, "Distance") == 0) {
        lua_pushnumber(state, hit.distance);
    } else if (std::strcmp(key, "Material") == 0) {
        // Terrain fills this in (sub-project 1); nothing else has a material yet.
        lua_pushnil(state);
    } else {
        luaL_error(state, "%s is not a valid member of RaycastResult", key);
    }
    return 1;
}

int result_newindex(lua_State* state) {
    luaL_checkudata(state, 1, kRaycastResultMeta);
    luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2));
}

int type_tostring(lua_State* state) {
    lua_pushstring(state, luaL_typename(state, 1));
    return 1;
}

void install(lua_State* state, const char* meta, const char* type_name, lua_CFunction index, lua_CFunction newindex) {
    luaL_newmetatable(state, meta);
    lua_pushcfunction(state, index, "index");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, newindex, "newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushstring(state, type_name);
    lua_setfield(state, -2, "__type");
    lua_pushcfunction(state, type_tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
}

Vec3 check_vector(lua_State* state, int index, const char* name) {
    const float* v = lua_tovector(state, index);
    if (v == nullptr) {
        luaL_error(state, "%s must be a Vector3", name);
    }
    return Vec3{v[0], v[1], v[2]};
}

}  // namespace

void open_raycast(lua_State* state) {
    install(state, kRaycastParamsMeta, "RaycastParams", params_index, params_newindex);
    install(state, kRaycastResultMeta, "RaycastResult", result_index, result_newindex);
    lua_createtable(state, 0, 1);
    lua_pushcfunction(state, params_new, "new");
    lua_setfield(state, -2, "new");
    lua_setreadonly(state, -1, true);
    lua_setglobal(state, "RaycastParams");
}

int ScriptBindings::workspace_raycast(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        ScriptRuntime* runtime = runtime_from(state);
        auto* workspace = runtime == nullptr ? nullptr
                                             : dynamic_cast<Workspace*>(runtime->resolve_id(ud->id, ud->world));
        if (workspace == nullptr) {
            luaL_error(state, "Raycast must be called on Workspace");
        }
        const Vec3 origin = check_vector(state, 2, "origin");
        const Vec3 direction = check_vector(state, 3, "direction");
        RayFilter filter;
        if (!lua_isnoneornil(state, 4)) {
            auto* params = static_cast<RaycastParamsUd*>(test_userdata(state, 4, kRaycastParamsMeta));
            if (params == nullptr) {
                luaL_error(state, "params must be a RaycastParams");
            }
            filter.include = params->filter_type == static_cast<int>(RaycastFilterType::Include);
            filter.instances = params->instances;
        }
        PhysicsWorld* physics = runtime->game_->physics();
        if (physics == nullptr) {
            luaL_error(state, "Raycast needs a running engine");
        }
        const std::optional<RayHit> hit = physics->raycast(*runtime->game_, origin, direction, filter);
        if (!hit) {
            lua_pushnil(state);
        } else {
            push_result(state, *hit);
        }
        return 1;
    });
}

ANARCHY_LUA_REGISTER(register_raycast_lua) {
    const LuaField workspace = lua_method("Raycast", "RaycastResult?", reinterpret_cast<void*>(&ScriptBindings::workspace_raycast));
    register_lua_class("Workspace", nullptr, &workspace, 1);

    const LuaField params[] = {
        lua_property("FilterType", "Enum.RaycastFilterType", true, nullptr, nullptr),
        lua_property("FilterDescendantsInstances", "{Instance}", true, nullptr, nullptr),
    };
    register_lua_class("RaycastParams", nullptr, params, static_cast<int>(sizeof(params) / sizeof(params[0])));
    lua_note_result("RaycastParams", "new", "RaycastParams", false);

    const LuaField result[] = {
        lua_property("Instance", "Instance", false, nullptr, nullptr),
        lua_property("Position", "Vector3", false, nullptr, nullptr),
        lua_property("Normal", "Vector3", false, nullptr, nullptr),
        lua_property("Distance", "number", false, nullptr, nullptr),
        lua_property("Material", "Material?", false, nullptr, nullptr),
    };
    register_lua_class("RaycastResult", nullptr, result, static_cast<int>(sizeof(result) / sizeof(result[0])));
}

}  // namespace engine_core
```

Check these against the codebase while writing, and match it where it differs:
- the exact header names for `test_userdata`, `push_enum_item`, `Workspace` (`SceneService.hpp`), and the Luau includes other bindings use;
- `lua_guard`'s lambda must return an `int`; `luaL_error` does not return, so the compiler may need `return 0;` after each `luaL_error` in the lambdas and in `params_index` / `params_newindex` / `result_newindex`. Follow what `color3_index` / `color3_newindex` do (`Color3.cpp:81-130`);
- `kInstanceMeta`'s declaration;
- how `lua_property`'s `writable` flag and `"{Instance}"` / `"Enum.X"` type names are spelled elsewhere in `LuaApi.cpp` / `SceneService.cpp`.

`ScriptRuntime.cpp`, after `open_matrix4(state);` (near line 766):

```cpp
    open_raycast(state);
```

Add property and method docs where `LuaApi.cpp` keeps them (near line 1292, where `add("Workspace", "Gravity", ...)` is):

```cpp
    add("Workspace", "Raycast",
        "Casts a ray from origin along direction (its length is how far) and returns the first thing it hits, as a "
        "RaycastResult, or nil. params (a RaycastParams) chooses what it sees. Works stopped and playing. A ray "
        "that starts inside something does not hit it.");
```

Follow the `add` call's actual signature there, and add entries for `RaycastParams.FilterType`, `RaycastParams.FilterDescendantsInstances`, and each `RaycastResult` field the same way if that file documents datatype members.

Add `src/engine_core/RaycastBindings.cpp` to `CMakeLists.txt` after `src/engine_core/ScriptBindings.cpp`, and `sandbox/raycast_lua_tests.cpp` to the sandbox list.

- [ ] **Step 5: Run tests**

Run: `cmake --build build --config Debug --target sandbox` then `build/Debug/sandbox.exe "[raycast]"` and `build/Debug/sandbox.exe`
Expected: L1–L5, R1–R5 pass, and the whole suite passes (the analyzer and docs tests check every registered member is documented; fix any they report).

- [ ] **Step 6: Release build**

Run: `cmake --build build --config Release --target AnarchyStudio`
Expected: builds. In the studio's command bar, with a part in Workspace: `print(workspace:Raycast(Vector3.new(0, 50, 0), Vector3.new(0, -100, 0)))` prints a RaycastResult while stopped.

- [ ] **Step 7: Commit**

```bash
git add src/engine_datatypes/Enum.hpp src/engine_datatypes/Enum.cpp src/engine_core/RaycastBindings.cpp src/engine_core/ScriptBindings.hpp src/engine_core/ScriptRuntime.cpp src/engine_core/LuaApi.cpp CMakeLists.txt sandbox/raycast_lua_tests.cpp
git commit -m "Give scripts workspace:Raycast with RaycastParams and RaycastResult"
```
