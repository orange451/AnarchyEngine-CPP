#pragma once

#include "amesh.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {

class DataModel;
class PhysicsBase;
class PhysicsObject;
class PlayerController;

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
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Bodies match the tree. Playing or stopped.
    void sync(DataModel& game);
    // Probes, steps, and writes back. Does nothing while game is not playing.
    void simulate(DataModel& game, double dt);
    // Does nothing while game is not playing; else sync, then simulate.
    void step(DataModel& game, double dt);

    // The first body a ray from origin along direction hits, within direction's
    // length, that filter lets it see. Syncs first, so it sees the tree as it
    // is now. A ray that starts inside a body does not hit it. Nothing for a
    // zero direction or a non-finite origin. Any thread, under the write lock.
    std::optional<RayHit> raycast(DataModel& game, Vec3 origin, Vec3 direction, const RayFilter& filter);

    // How many bodies the world holds, and whether this PhysicsObject has one.
    std::size_t body_count() const;
    bool has_body(InstanceId id) const;
    // For tests: the body's mass, and each of its shapes' friction. 0 and
    // empty when the PhysicsObject has no body.
    float body_mass(InstanceId id) const;
    std::vector<float> shape_frictions(InstanceId id) const;
    // For tests: a key naming the body's Box3D handle, the same while the
    // body lives; 0 when the PhysicsBase has none. And where the body is.
    std::uint64_t body_key(InstanceId id) const;
    std::optional<Vec3> body_position(InstanceId id) const;
    // For tests: a key naming its first shape's Box3D handle, changing
    // whenever the shape is remade; 0 when it has none.
    std::uint64_t shape_key(InstanceId id) const;

    // Where a warning goes, such as a Hull that fell back to a Box. Unset, it
    // goes nowhere.
    void set_warning_sink(std::function<void(const std::string&)> sink);

    // Where object's shape is centered in its body's space: the middle of the
    // Prefab of the GameObject it moves (Prefab::origin_offset), scaled as
    // that GameObject's Transform and its Scale scale it, so the shape sits
    // where the Prefab is drawn. The body's origin stays the GameObject's, and its
    // center of mass is the shape's. The origin when it moves no GameObject,
    // or that GameObject has no Prefab. Needs the DataModel lock; a read lock
    // is enough.
    //
    // A body is centered when its shape is made, and again when its
    // GameObject's Prefab changes or something other than physics moves the
    // GameObject. A Prefab's Models or Meshes changing during play do not
    // move a body that exists. The origin for a PlayerController, which is
    // never recentered.
    static Vec3 shape_center(const DataModel& game, const PhysicsBase& object);

    // What object's Size is multiplied by: the Scale of the GameObject it
    // moves, or 1 when it moves none. Its Mass stays as given. A body is
    // made again when that Scale changes. Needs the DataModel lock; a read
    // lock is enough. 1 for a PlayerController, which is never scaled.
    static float shape_scale(const DataModel& game, const PhysicsBase& object);

    // The edges of what a body made now for object would collide as, for the
    // Scene View to draw: line segments into lines, two points each, in the
    // body's space (body_pose), around center (shape_center), at Size times
    // scale (shape_scale). points and
    // triangles are its Mesh's, as Mesh::vertex_positions gives them; no
    // points is no Mesh. Built as the body's shape is, so a Hull, and a Custom
    // that is not Anchored, is the hull Box3D makes of them, an anchored Custom
    // is each edge of its triangles once, an unanchored Custom with pieces (as
    // known_pieces gives them) is each piece's hull, and one that cannot be
    // made is the Box it falls back to. Warns of nothing.
    static void collision_outline(const PhysicsObject& object, Vec3 center, const std::vector<Vec3>& points,
                                  const std::vector<std::uint32_t>& triangles, std::vector<Vec3>& lines,
                                  float scale = 1.f, const std::vector<anarchy::amesh::ConvexPiece>* pieces = nullptr);
    // Where a body made at transform is: its position and rotation, with any
    // scale taken out.
    static Matrix4 body_pose(const Matrix4& transform);
    // A PlayerController's edges, as collision_outline gives a PhysicsObject's:
    // its cylinder, from hover_gap() above its feet up to Height, and one
    // line from its feet up to the cylinder's bottom, in the body's space.
    static void collision_outline(const PlayerController& controller, std::vector<Vec3>& lines);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine_core
