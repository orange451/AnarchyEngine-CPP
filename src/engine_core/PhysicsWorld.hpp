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
class TerrainWorld;

namespace terrain {
struct ChunkMesh;
}

// Which instances a ray sees: with include, only those that are, or are under,
// one of instances; else every instance but those.
struct RayFilter {
    bool include = false;
    std::vector<InstanceId> instances;
};

// What a ray hit first.
struct RayHit {
    InstanceId instance = 0;   // The PhysicsBase or Terrain whose body was hit.
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
//   Stopped, the GameObject decides where its body is: each PhysicsBase that
//   moves a GameObject (PhysicsBase::driven_game_object) takes that
//   GameObject's position and rotation as its Transform, keeping its own
//   scale, a PlayerController upright, so the body sits where it starts at
//   play and a dragger over the GameObject's children finds it there. A
//   write to that PhysicsBase's own Transform moves neither the body nor the
//   GameObject; the Transform goes back to the body's. This holds after
//   Play until the first step too, for a write made just before Play or by
//   a script before that step, as when bodies were made at it. Those stores
//   are the only writes stopped sync makes to instances: no Changed, no
//   history, no dirty mark, and no ground for a PlayerController. Stopped,
//   sync never decomposes a Mesh: an unanchored Custom without known pieces
//   is a Hull until its pieces are stored, or until Play, when it is made
//   again.
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
// Terrain, once set_terrain_world gave it a TerrainWorld: early in sync,
// before reconcile_terrain, each dynamic (not Anchored) PhysicsObject or
// PlayerController's body asks that TerrainWorld (set_collider_interest) for
// colliders within kColliderChunks of it, per Terrain, in that Terrain's
// local chunk space, and the chunk under each body and its immediate
// neighbors, any whose collider TerrainWorld does not know yet, are built
// right there, synchronously (build_colliders_now) -- no falling through. Then, at the end of sync: each TerrainView gets a static body at
// its transform, which jumps there when the transform changes, and one mesh
// shape per chunk collider TerrainWorld now holds for it, made again when
// that chunk's collider revision changes (the old shape goes first), with
// none at all while CanCollide is false. A shape's triangles carry the
// Terrain's material Ids as Box3D surface materials, so a ray reports which
// Id it hit. A Terrain gone from the views loses its body.
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

    // How many bodies the world holds, and whether this PhysicsObject or
    // Terrain has one.
    std::size_t body_count() const;
    bool has_body(InstanceId id) const;
    // For tests: the body's mass, and each of its shapes' friction. 0 and
    // empty when the PhysicsObject has no body.
    float body_mass(InstanceId id) const;
    std::vector<float> shape_frictions(InstanceId id) const;
    // For tests: how many times this body's shape has been made, or 0.
    int shapes_made(InstanceId id) const;
    // For tests: a key naming the body's Box3D handle and the world_generation
    // it was made in, the same while the body lives and different after a
    // Stop (a new world hands out the same handles again); 0 when the
    // PhysicsBase has none. And where the body is.
    std::uint64_t body_key(InstanceId id) const;
    std::optional<Vec3> body_position(InstanceId id) const;
    // For tests: a key naming its first shape's Box3D handle, changing
    // whenever the shape is remade; 0 when it has none.
    std::uint64_t shape_key(InstanceId id) const;

    // Where Terrain bodies come from; null for none. SimulationThread, under
    // the write lock. The TerrainWorld must outlive this world or be unset
    // first. Non-const: sync() also drives it, asking for colliders around
    // each dynamic body or PlayerController (set_collider_interest) and, for
    // one with none around it, building them synchronously
    // (build_colliders_now), both before Box3D steps.
    void set_terrain_world(TerrainWorld* terrains);
    // A chunk mesh as a collider for its Terrain's body: its triangles,
    // welded, each carrying its material Id. Null for a mesh with no
    // triangles, or one Box3D builds nothing from. Any thread, no lock:
    // TerrainMesher's workers call it. What it holds only PhysicsWorld reads.
    static std::shared_ptr<void> build_terrain_collider(const terrain::ChunkMesh& mesh);
    // For tests: how many shapes the body of this PhysicsBase or Terrain has, or 0.
    std::size_t shape_count(InstanceId id) const;

    // For tests: one ray's result from ray_cast_mesh_for_test or
    // ray_cast_terrain_collider_for_test. fraction, triangle, and material
    // are unset (0.f, -1, -1) when hit is false. fraction is along the ray,
    // as cast (its full length is fraction 1). material is Box3D's
    // materialIndex for the hit triangle: the material id passed in for a
    // terrain mesh (ray_cast_terrain_collider_for_test), or 0 when none was
    // given (ray_cast_mesh_for_test).
    struct MeshRayCastHit {
        bool hit = false;
        float fraction = 0.f;
        int triangle = -1;
        int material = -1;
    };
    // For tests: welds positions and triangles into a Box3D mesh exactly as
    // an anchored Custom's shape is built (build_mesh: welded, with edges
    // identified), with the default SAH split, or, when use_median_split is
    // true, the split build_terrain_collider uses, then casts each ray
    // (ray_origins[i] to ray_origins[i] + ray_directions[i]) through it with
    // b3RayCastMesh. One result per ray, in the same order. Any thread, no
    // lock. This and ray_cast_terrain_collider_for_test are how a test
    // reaches Box3D's mesh ray cast without including Box3D itself.
    static std::vector<MeshRayCastHit> ray_cast_mesh_for_test(const std::vector<Vec3>& positions,
                                                               const std::vector<std::uint32_t>& triangles,
                                                               bool use_median_split,
                                                               const std::vector<Vec3>& ray_origins,
                                                               const std::vector<Vec3>& ray_directions);
    // For tests: build_terrain_collider's own mesh for mesh (its real
    // useMedianSplit, weld, and material-carrying code path), then the same
    // per-ray b3RayCastMesh cast as ray_cast_mesh_for_test. Every ray misses
    // when build_terrain_collider finds nothing to build.
    static std::vector<MeshRayCastHit> ray_cast_terrain_collider_for_test(const terrain::ChunkMesh& mesh,
                                                                           const std::vector<Vec3>& ray_origins,
                                                                           const std::vector<Vec3>& ray_directions);

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

    // What object's Size is multiplied by, per axis: the scale of the
    // GameObject it moves, which is the length of each axis of its Transform
    // times its Scale, as its Prefab is drawn; or (1, 1, 1) when it moves
    // none. Size itself is never changed, and Mass stays as given. A body is
    // made again when this changes. Needs the DataModel lock; a read lock is
    // enough. (1, 1, 1) for a PlayerController, which is never scaled.
    static Vec3 shape_scale(const DataModel& game, const PhysicsBase& object);

    // The edges of what a body made now for object would collide as, for the
    // Scene View to draw: line segments into lines, two points each, in the
    // body's space (body_pose), around center (shape_center), at Size times
    // scale (shape_scale), or for a Hull or a Custom its Mesh at its own size
    // times scale. points and triangles are its Mesh's, as
    // Mesh::vertex_positions gives them; no points is no Mesh. Built as the body's shape is, so a Hull, and a Custom
    // that is not Anchored, is the hull Box3D makes of them, an anchored Custom
    // is each edge of its triangles once, an unanchored Custom with pieces (as
    // known_pieces gives them) is each piece's hull, and one that cannot be
    // made is the Box it falls back to. Warns of nothing.
    static void collision_outline(const PhysicsObject& object, Vec3 center, const std::vector<Vec3>& points,
                                  const std::vector<std::uint32_t>& triangles, std::vector<Vec3>& lines,
                                  Vec3 scale = Vec3{1.f, 1.f, 1.f},
                                  const std::vector<anarchy::amesh::ConvexPiece>* pieces = nullptr);
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
