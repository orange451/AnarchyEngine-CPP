#include "PhysicsWorld.hpp"

#include "Assemblies.hpp"
#include "AssetInstances.hpp"
#include "Brush.hpp"
#include "CollisionGroups.hpp"
#include "ConvexDecomposition.hpp"
#include "DataModel.hpp"
#include "GameObject.hpp"
#include "JointConstraint.hpp"
#include "JointFrame.hpp"
#include "LuaApi.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"
#include "TerrainWorld.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelSampler.hpp"
#include "profiler/Profiler.hpp"

#pragma warning(push, 0)
#include "box3d/box3d.h"
#pragma warning(pop)

#include <algorithm>
#include <iterator>
#include <map>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace engine_core {
namespace {

// The most vertices a Hull asks Box3D to keep, then a second try with fewer:
// a hull past 128 faces or edges fails, and fewer vertices make fewer.
constexpr int kHullVertices = 64;
constexpr int kHullVerticesRetry = 32;
// How near two corners of a Custom's triangles are to be joined as one.
constexpr float kWeldTolerance = 1e-4f;
// The sides around a Cone, and around an elliptical Cylinder (scaled unevenly
// across X and Z), as Box3D builds no round hull. A prism of n sides has 3n
// edges, and a hull keeps at most 128.
constexpr int kRoundSides = 16;
// An elliptical Cylinder PhysicsObject's shape: this many such hulls, each
// turned a further share of a side about its axis, so together they roll as
// one of kCylinderLayers * kRoundSides sides. A round one is a true cylinder
// (b3_cylinderShape, cmake/box3d/cylinder.c).
constexpr int kCylinderLayers = 4;
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
// How far past an edge the probe looks for the face it stands on.
constexpr float kFaceNudge = 0.02f;
// The hover: a spring at this many hertz, critically damped.
constexpr float kHoverFrequency = 6.f;
// Faster than this up, relative to the ground, and above the hover gap, a
// controller is leaving the ground: a jump.
constexpr float kRisingSpeed = 0.1f;
// A contact pushing on a controller with more than this much downward is an
// overhang it stops at, not one it may be pressed down by.
constexpr float kOverhangNormal = 0.05f;
// Ground whose normal points up less than this is a wall: a controller does
// not hover on it, but falls along it.
constexpr float kWallNormal = 0.05f;
constexpr float kPi = 3.14159265359f;
// Task 8 (spec "Physics"): a Terrain's chunk colliders exist only within
// this many chunks of a dynamic PhysicsObject or PlayerController.
constexpr int kColliderChunks = 3;

b3Vec3 to_b3(Vec3 v) { return b3Vec3{v.x, v.y, v.z}; }

Vec3 from_b3(b3Vec3 v) { return Vec3{v.x, v.y, v.z}; }

float column_length(const Matrix4& m, int column) {
    const float* axis = m.m + column * 4;
    return std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
}

// Where a Matrix4 puts the body: its translation, and its rotation with any
// scale taken out.
void pose_of(const Matrix4& m, b3Vec3& position, b3Quat& rotation) {
    position = b3Vec3{m.m[12], m.m[13], m.m[14]};
    rotation = b3Quat_identity;
    b3Vec3 columns[3];
    for (int column = 0; column < 3; ++column) {
        const float length = column_length(m, column);
        if (!(length > 1e-6f)) {
            return;
        }
        const float* axis = m.m + column * 4;
        columns[column] = b3Vec3{axis[0] / length, axis[1] / length, axis[2] / length};
    }
    const b3Matrix3 basis{columns[0], columns[1], columns[2]};
    rotation = b3NormalizeQuat(b3MakeQuatFromMatrix(&basis));
}

// The body's pose as a Matrix4 that keeps the scale of each axis of keep.
// When rotation is keep's own (within kSameTurn a component), keep's axes
// are kept bit for bit and only the position is new: a matrix to a
// quaternion and back is not exact, and scaling the result by keep's axis
// lengths again and again grew a body's scale each time a place was opened.
Matrix4 matrix_of(const b3Vec3& position, const b3Quat& rotation, const Matrix4& keep) {
    constexpr float kSameTurn = 1e-6f;
    b3Vec3 kept_position{};
    b3Quat kept{};
    pose_of(keep, kept_position, kept);
    // q and -q are one turn.
    const float dot = kept.v.x * rotation.v.x + kept.v.y * rotation.v.y + kept.v.z * rotation.v.z + kept.s * rotation.s;
    const float sign = dot < 0.f ? -1.f : 1.f;
    const auto same = [&](float a, float b) { return std::fabs(a * sign - b) <= kSameTurn; };
    if (same(kept.v.x, rotation.v.x) && same(kept.v.y, rotation.v.y) && same(kept.v.z, rotation.v.z) &&
        same(kept.s, rotation.s)) {
        Matrix4 out = keep;
        out.m[12] = position.x;
        out.m[13] = position.y;
        out.m[14] = position.z;
        return out;
    }
    const b3Matrix3 basis = b3MakeMatrixFromQuat(rotation);
    const b3Vec3 columns[3] = {basis.cx, basis.cy, basis.cz};
    Matrix4 out = matrix4_identity();
    for (int column = 0; column < 3; ++column) {
        float scale = column_length(keep, column);
        if (!(scale > 1e-6f)) {
            scale = 1.f;
        }
        // Unit first, so the scale is keep's and nothing more.
        const b3Vec3 axis = b3Normalize(columns[column]);
        out.m[column * 4 + 0] = axis.x * scale;
        out.m[column * 4 + 1] = axis.y * scale;
        out.m[column * 4 + 2] = axis.z * scale;
    }
    out.m[12] = position.x;
    out.m[13] = position.y;
    out.m[14] = position.z;
    return out;
}

void* user_data(InstanceId id) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)); }

// Filter bits (docs/superpowers/specs/2026-10-11-collision-groups-design.md
// §4). A shape's category is its body's group's bit; its mask, every group it
// collides with, plus the ground and ray query bits. CanCollide false: a
// sensor (docs/superpowers/specs/2026-10-11-touched-design.md §3), which
// never collides; its mask says what it senses, and that rays see it.
b3Filter shape_filter(const CollisionGroups& groups, std::string_view group, bool can_collide) {
    b3Filter filter = b3DefaultFilter();
    filter.categoryBits = groups.category(group);
    // A CanCollide-off shape is a sensor (it never collides): its mask is what
    // it senses, by group, and rays, but not the ground probe.
    filter.maskBits = can_collide
                          ? groups.collides_with(group) | CollisionGroups::kGroundBit | CollisionGroups::kRayBit
                          : groups.collides_with(group) | CollisionGroups::kRayBit;
    return filter;
}

// A ray in group (empty: every group) hits what that group collides with,
// CanCollide or not.
b3QueryFilter ray_filter(const CollisionGroups& groups, const std::string& group) {
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.categoryBits = CollisionGroups::kRayBit;
    filter.maskBits = group.empty() ? B3_DEFAULT_MASK_BITS : groups.collides_with(group);
    return filter;
}

// A PlayerController's ground: what can collide and its group collides with.
b3QueryFilter ground_filter(const CollisionGroups& groups, const std::string& group) {
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.categoryBits = CollisionGroups::kGroundBit;
    filter.maskBits = groups.collides_with(group);
    return filter;
}

InstanceId id_of(void* data) { return static_cast<InstanceId>(reinterpret_cast<std::uintptr_t>(data)); }

// How a Mesh's points are placed in the body's space: centered on their
// bounds, each axis at the body's scale (shape_scale), around center. A Hull
// or a Custom is its Mesh at the Mesh's own size; Size plays no part.
struct Fit {
    Vec3 middle;
    Vec3 scale;
    Vec3 center;
};

// The bounds of mesh_points, which are not empty.
void bounds_of(const std::vector<Vec3>& mesh_points, Vec3& low, Vec3& high) {
    low = mesh_points.front();
    high = low;
    for (const Vec3& p : mesh_points) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
}

// The extents of the bounds of mesh_points, which are not empty.
Vec3 bounds_size(const std::vector<Vec3>& mesh_points) {
    Vec3 low;
    Vec3 high;
    bounds_of(mesh_points, low, high);
    return Vec3{high.x - low.x, high.y - low.y, high.z - low.z};
}

// The fit of mesh_points, which are not empty, at scale around center.
Fit fit_of(const std::vector<Vec3>& mesh_points, Vec3 scale, Vec3 center) {
    Vec3 low;
    Vec3 high;
    bounds_of(mesh_points, low, high);
    return Fit{Vec3{(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f}, scale, center};
}

b3Vec3 apply_fit(const Fit& fit, Vec3 p) {
    return b3Vec3{(p.x - fit.middle.x) * fit.scale.x + fit.center.x, (p.y - fit.middle.y) * fit.scale.y + fit.center.y,
                  (p.z - fit.middle.z) * fit.scale.z + fit.center.z};
}

// A Mesh's points, which are not empty, at scale around center in the body's space.
void fit_points(const std::vector<Vec3>& mesh_points, Vec3 scale, Vec3 center, std::vector<b3Vec3>& points) {
    const Fit fit = fit_of(mesh_points, scale, center);
    points.clear();
    for (const Vec3& p : mesh_points) {
        points.push_back(apply_fit(fit, p));
    }
}

const Vec3 kUnscaled{1.f, 1.f, 1.f};

// shape_scale for a body that moves driven, or none at 0: per axis, the
// length of that axis of the GameObject's Transform times its Scale, as its
// Prefab is drawn.
Vec3 scale_for(const DataModel& game, InstanceId driven) {
    const GameObject* object = driven != 0 ? game.game_object(driven) : nullptr;
    if (object == nullptr) {
        return kUnscaled;
    }
    const Matrix4 transform = object->transform();
    const float scale = static_cast<float>(object->scale());
    return Vec3{column_length(transform, 0) * scale, column_length(transform, 1) * scale,
                column_length(transform, 2) * scale};
}

Vec3 scaled(Vec3 size, Vec3 scale) { return Vec3{size.x * scale.x, size.y * scale.y, size.z * scale.z}; }

// shape_center for a body that moves driven, or none at 0.
Vec3 center_for(const DataModel& game, InstanceId driven) {
    const GameObject* object = driven != 0 ? game.game_object(driven) : nullptr;
    if (object == nullptr) {
        return {};
    }
    const LuaSlot slot = object->prefab();
    const auto* prefab =
        slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Prefab*>(game.instance(slot.id)) : nullptr;
    if (prefab == nullptr) {
        return {};
    }
    // The body's space has the GameObject's rotation but not its Transform's
    // scale or its Scale, which the Prefab is drawn with.
    return scaled(prefab->origin_offset(), scale_for(game, driven));
}

// Equal but for rounding: each axis within one part in ten thousand of the
// larger of the two. A scale read from a Transform's axis lengths wobbles in
// its last bits every time physics turns it, and a shape made again for that
// drops its contacts, which lets a resting stack creep and fall.
bool near_vec3(Vec3 a, Vec3 b) {
    auto near = [](float x, float y) { return std::fabs(x - y) <= 1e-4f * std::max({std::fabs(x), std::fabs(y), 1e-3f}); };
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

// A hull of points, as a Hull's shape takes it. Null when Box3D builds none.
b3HullData* build_hull(const std::vector<b3Vec3>& points) {
    const int count = static_cast<int>(points.size());
    b3HullData* hull = b3CreateHull(points.data(), count, kHullVertices);
    if (hull == nullptr) {
        hull = b3CreateHull(points.data(), count, kHullVerticesRetry);
    }
    return hull;
}

// Each piece, fitted as its whole Mesh is (fit), as a hull. Pieces Box3D
// builds no hull from are left out. The caller destroys the hulls.
std::vector<b3HullData*> piece_hulls(const std::vector<anarchy::amesh::ConvexPiece>& pieces, const Fit& fit) {
    std::vector<b3HullData*> hulls;
    std::vector<b3Vec3> points;
    for (const anarchy::amesh::ConvexPiece& piece : pieces) {
        points.clear();
        for (const auto& p : piece.points) {
            points.push_back(apply_fit(fit, Vec3{p[0], p[1], p[2]}));
        }
        if (b3HullData* hull = build_hull(points)) {
            hulls.push_back(hull);
        }
    }
    return hulls;
}

// The corners of a Cylinder, Cone, or Wedge of size around center, into
// points. Each stands along Y; a Cylinder and a Cone are round across X, as
// a Mesh's AddCylinder and AddCone build them.
void primitive_points(PhysicsObject::Shape shape, Vec3 size, Vec3 center, std::vector<b3Vec3>& points) {
    points.clear();
    if (shape == PhysicsObject::Shape::Wedge) {
        const float half_x = size.x * 0.5f;
        const float half_y = size.y * 0.5f;
        const float half_z = size.z * 0.5f;
        // The bottom face, then the top edge along its back.
        for (const float x : {-half_x, half_x}) {
            points.push_back(b3Vec3{center.x + x, center.y - half_y, center.z - half_z});
            points.push_back(b3Vec3{center.x + x, center.y - half_y, center.z + half_z});
            points.push_back(b3Vec3{center.x + x, center.y + half_y, center.z + half_z});
        }
        return;
    }
    // Its sides alone: the hull closes the ends, and joins the corners the
    // mesh repeats at its seam and tip.
    anarchy::amesh::Data mesh;
    if (shape == PhysicsObject::Shape::Cylinder) {
        add_cylinder(mesh, size.x * 0.5f, size.y, kRoundSides, false, center);
    } else {
        add_cone(mesh, size.x * 0.5f, size.y, kRoundSides, false, center);
    }
    for (const anarchy::amesh::Vertex& vertex : mesh.vertices) {
        points.push_back(b3Vec3{vertex.p[0], vertex.p[1], vertex.p[2]});
    }
}

// points and triangles as one mesh, as an anchored Custom's shape takes it,
// with indices as scratch. Null when Box3D builds none.
b3MeshData* build_mesh(std::vector<b3Vec3>& points, const std::vector<std::uint32_t>& triangles,
                       std::vector<std::int32_t>& indices) {
    indices.assign(triangles.begin(), triangles.end());
    b3MeshDef def{};
    def.vertices = points.data();
    def.indices = indices.data();
    def.vertexCount = static_cast<int>(points.size());
    def.triangleCount = static_cast<int>(indices.size() / 3);
    // A mesh's faces each have their own corners; welding joins them
    // so edges between triangles are known.
    def.weldVertices = true;
    def.weldTolerance = kWeldTolerance;
    def.identifyEdges = true;
    return b3CreateMesh(&def, nullptr, 0);
}

// For PhysicsWorld::ray_cast_mesh_for_test and
// ray_cast_terrain_collider_for_test: casts each ray in origins/directions
// through data's BVH with b3RayCastMesh, one result per ray. A null data is
// every ray a miss (mesh.triangle count too small, or Box3D built nothing).
std::vector<PhysicsWorld::MeshRayCastHit> ray_cast_mesh_data(const b3MeshData* data,
                                                              const std::vector<Vec3>& origins,
                                                              const std::vector<Vec3>& directions) {
    std::vector<PhysicsWorld::MeshRayCastHit> results(origins.size());
    if (data == nullptr) {
        return results;
    }
    b3Mesh shape{};
    shape.data = data;
    shape.scale = b3Vec3{1.f, 1.f, 1.f};
    for (std::size_t i = 0; i < origins.size(); ++i) {
        b3RayCastInput input{};
        input.origin = to_b3(origins[i]);
        input.translation = to_b3(directions[i]);
        input.maxFraction = 1.f;
        const b3CastOutput out = b3RayCastMesh(&shape, &input);
        results[i].hit = out.hit;
        if (out.hit) {
            results[i].fraction = out.fraction;
            results[i].triangle = out.triangleIndex;
            results[i].material = out.materialIndex;
        }
    }
    return results;
}

// What build_terrain_collider hands TerrainWorld: one chunk's triangles as a
// Box3D mesh. Built on a TerrainMesher worker; read on SimulationThread by
// the shape made from it, which the body record keeps it alive for. Freed by
// whichever thread drops the last reference, as b3CreateMesh and
// b3DestroyMesh touch no Box3D world (only its allocator).
struct TerrainCollider {
    b3MeshData* mesh = nullptr;
    TerrainCollider() = default;
    TerrainCollider(const TerrainCollider&) = delete;
    TerrainCollider& operator=(const TerrainCollider&) = delete;
    ~TerrainCollider() {
        if (mesh != nullptr) {
            b3DestroyMesh(mesh);
        }
    }
};

// One surface material per Terrain material Id, the index as its
// userMaterialId, at a PhysicsObject's default Friction and Bounciness: a
// terrain shape's triangles index it by the Id they carry.
constexpr int kTerrainMaterialCount = 256;

std::array<b3SurfaceMaterial, kTerrainMaterialCount> terrain_materials() {
    std::array<b3SurfaceMaterial, kTerrainMaterialCount> materials{};
    for (int index = 0; index < kTerrainMaterialCount; ++index) {
        b3SurfaceMaterial& material = materials[static_cast<std::size_t>(index)];
        material = b3DefaultSurfaceMaterial();
        material.friction = static_cast<float>(PhysicsObject::kDefaultFriction);
        material.restitution = 0.f;
        material.userMaterialId = static_cast<std::uint64_t>(index);
    }
    return materials;
}

// ---- Outlines ---------------------------------------------------------

void add_line(std::vector<Vec3>& lines, Vec3 a, Vec3 b) {
    lines.push_back(a);
    lines.push_back(b);
}

// The arc of a circle of radius about center, in the plane of the unit
// axes u and v, from angle from to angle to, where angle 0 is along u.
void add_arc(std::vector<Vec3>& lines, Vec3 center, Vec3 u, Vec3 v, float radius, float from, float to) {
    constexpr float kSegmentsPerTurn = 32.f;
    constexpr float kTurn = 6.28318530718f;
    const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(to - from) / kTurn * kSegmentsPerTurn)));
    const auto at = [&](int step) {
        const float angle = from + (to - from) * static_cast<float>(step) / static_cast<float>(segments);
        const float c = std::cos(angle) * radius;
        const float s = std::sin(angle) * radius;
        return Vec3{center.x + u.x * c + v.x * s, center.y + u.y * c + v.y * s, center.z + u.z * c + v.z * s};
    };
    for (int step = 0; step < segments; ++step) {
        add_line(lines, at(step), at(step + 1));
    }
}

void outline_box(Vec3 half, Vec3 center, std::vector<Vec3>& lines) {
    const auto corner = [&](int index) {
        return Vec3{center.x + (index & 1 ? half.x : -half.x), center.y + (index & 2 ? half.y : -half.y),
                    center.z + (index & 4 ? half.z : -half.z)};
    };
    // Each pair of corners one bit apart.
    for (int index = 0; index < 8; ++index) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            if ((index & bit) == 0) {
                add_line(lines, corner(index), corner(index | bit));
            }
        }
    }
}

// Its points, which the hull's half-edges index: each edge once.
void outline_hull(const b3HullData& hull, std::vector<Vec3>& lines) {
    const b3Vec3* points = b3GetHullPoints(&hull);
    const b3HullHalfEdge* edges = b3GetHullEdges(&hull);
    for (int index = 0; index < hull.edgeCount; ++index) {
        const b3HullHalfEdge& edge = edges[index];
        if (index < edge.twin) {
            add_line(lines, from_b3(points[edge.origin]), from_b3(points[edges[edge.twin].origin]));
        }
    }
}

// Each edge of the triangles once, joining corners within the weld
// tolerance as the mesh shape does.
void outline_triangles(const std::vector<b3Vec3>& points, const std::vector<std::uint32_t>& triangles,
                       std::vector<Vec3>& lines) {
    struct Cell {
        std::int64_t x, y, z;
        bool operator==(const Cell& other) const { return x == other.x && y == other.y && z == other.z; }
    };
    struct CellHash {
        std::size_t operator()(const Cell& cell) const {
            const std::uint64_t h = static_cast<std::uint64_t>(cell.x) * 0x9E3779B97F4A7C15ull ^
                                    static_cast<std::uint64_t>(cell.y) * 0xC2B2AE3D27D4EB4Full ^
                                    static_cast<std::uint64_t>(cell.z) * 0x165667B19E3779F9ull;
            return static_cast<std::size_t>(h ^ (h >> 29));
        }
    };
    const auto cell_of = [](const b3Vec3& p) {
        return Cell{std::llround(p.x / kWeldTolerance), std::llround(p.y / kWeldTolerance),
                    std::llround(p.z / kWeldTolerance)};
    };
    std::unordered_map<Cell, std::uint32_t, CellHash> welded;
    welded.reserve(points.size());
    std::vector<std::uint32_t> corner(points.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
        corner[index] = welded.emplace(cell_of(points[index]), static_cast<std::uint32_t>(index)).first->second;
    }
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(triangles.size());
    for (std::size_t first = 0; first + 2 < triangles.size(); first += 3) {
        for (int side = 0; side < 3; ++side) {
            const std::uint32_t from = triangles[first + static_cast<std::size_t>(side)];
            const std::uint32_t to = triangles[first + static_cast<std::size_t>((side + 1) % 3)];
            if (from >= points.size() || to >= points.size()) {
                continue;
            }
            const std::uint32_t a = std::min(corner[from], corner[to]);
            const std::uint32_t b = std::max(corner[from], corner[to]);
            if (a != b && seen.insert(static_cast<std::uint64_t>(a) << 32 | b).second) {
                add_line(lines, from_b3(points[a]), from_b3(points[b]));
            }
        }
    }
}

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

// Whether id is one of instances or under one of them. parent gives
// kNoParent for a dead or unparented id, and again for kNoParent itself, so
// the walk stops there as well as at the root.
bool under_any(const DataModel& game, InstanceId id, const std::vector<InstanceId>& instances) {
    for (InstanceId at = id; at != 0 && at != DataModel::kNoParent; at = game.parent(at)) {
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
    // The shape's owner, not the body's: every shape of an assembly's one body
    // belongs to the member that made it.
    const InstanceId id = id_of(b3Shape_GetUserData(shape));
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

}  // namespace

struct PhysicsWorld::Impl {
    struct Body {
        b3BodyId body = b3_nullBodyId;
        // Its shapes: one, or one per convex piece of an unanchored Custom.
        std::vector<b3ShapeId> shapes;
        // The shapes' volume, summed, which turns Mass into a density.
        float volume = 1.f;
        // A PlayerController's: an upright cylinder, never recentered or scaled.
        bool controller = false;
        // A Touched or TouchEnded of this body has a connection: its shapes
        // ask Box3D for contact events.
        bool listened = false;
        // A controller: the shape it stands on this step (null for none), and
        // where, which way the ground faces, and the ground's velocity there
        // relative to it. Touched treats standing as touching.
        b3ShapeId stands_on = b3_nullShapeId;
        b3Vec3 stand_point{};
        b3Vec3 stand_normal{};
        b3Vec3 stand_velocity{};
        // A controller a write sped upward, as a jump does: it is leaving the
        // ground until it stops rising.
        bool launched = false;
        // A controller that was on ground last step, standing or sliding: it
        // keeps to ground that drops away under it, as stairs down do.
        bool grounded = false;
        // How fast it rose last step, relative to the ground, by moving
        // across a slope: its own climb, not a jump.
        float climb = 0.f;
        // The speed up and down it was to have after this step: what it set,
        // less gravity. A steep slope it hits may not add to it.
        float planned = 0.f;
        // An anchored Custom's triangles, which its shape points into.
        b3MeshData* mesh = nullptr;
        // The GameObject it moves, or 0, and the Transform it last gave it.
        InstanceId driven = 0;
        Matrix4 driven_pose{};
        // Where the shape was centered when it was made (shape_center), the
        // scale it was made at (shape_scale), and the GUID of the driven
        // GameObject's Prefab as last seen.
        Vec3 center{};
        Vec3 scale = kUnscaled;
        std::string prefab;
        // How many times its shape has been made (shapes_made).
        int made = 0;
        std::uint64_t seen = 0;
        // An unanchored Custom made while stopped, when its Mesh's pieces were
        // not known: it is a Hull, made again when the Mesh's file changes
        // (its stamp then) or when play starts.
        bool made_without_pieces = false;
        std::string pieces_stamp;
        // A member of an assembly's one body (a compound): the stamp of the
        // play assembly it was built for, or 0 for a body of its own; where its
        // shapes sit in that body; whether the assembly is anchored; and the
        // assembly's root, whose id the body carries.
        std::uint32_t compound = 0;
        b3Transform place = b3Transform_identity;
        bool compound_anchored = false;
        InstanceId compound_root = 0;
        // The PlayMember revision its shapes were placed at.
        std::uint32_t member_revision = 0;
    };

    // An assembly's one body, for the play assembly whose stamp keys it.
    struct Compound {
        b3BodyId body = b3_nullBodyId;
        InstanceId root = 0;
        bool anchored = false;
        std::uint64_t seen = 0;
    };

    b3WorldId world = b3_nullWorldId;
    // Up is positive: minus Workspace.Gravity, as the world was last given it.
    float gravity = -static_cast<float>(Workspace::kDefaultGravity);
    std::uint32_t generation = 0;
    std::uint64_t pass = 0;
    // A step has synced this world while the place played, and whether this
    // sync takes a driven body's pose from its GameObject rather than its own
    // Transform: while stopped, and until the first step of play. So a write
    // made just before Play and never seen stopped, or a script's write before
    // the first step, is dropped, as it was when bodies were made at that step
    // (see push). A Raycast's sync does not end that window: whether a script
    // cast a ray first must not change what the first step does.
    bool played = false;
    bool poses_from_game_objects = true;
    std::unordered_map<InstanceId, Body> bodies;
    // Active NoCollisionConstraints' pairs as last applied
    // (Assemblies::no_collide_pairs), as keys, and every body in one: only
    // those bodies' shapes ask the custom filter (no_collide_filter).
    std::vector<std::pair<InstanceId, InstanceId>> no_collide;
    std::unordered_set<std::uint64_t> no_collide_keys;
    std::unordered_set<InstanceId> no_collide_ids;
    std::vector<std::pair<InstanceId, InstanceId>> no_collide_diff;
    std::vector<InstanceId> no_collide_changed;
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
    // Touching (docs/superpowers/specs/2026-10-11-touched-design.md): each
    // touching shape pair (b3StoreShapeId, lower first) with the two
    // instances; per instance pair (pair_key) how many shape pairs touch and
    // the last facts known, oriented from the lower id; the seconds since
    // Play; how many bodies are listened to; and, while any is, each body's
    // motion from before the step (b3StoreBodyId).
    struct ShapeTouch {
        InstanceId a = 0;
        InstanceId b = 0;
        // The Box3D contact that touches (contact_key), 0 for a sensor or a
        // stand. A filter or type change replaces the contact while the shapes
        // keep touching: the new one begins before the old one's end is seen.
        std::uint64_t contact = 0;
    };
    struct PairTouch {
        int count = 0;
        // Its count reached 0 because PhysicsWorld removed shapes: it ends at
        // the end of the step unless it touches again first.
        bool pending_end = false;
        bool sensor = false;
        Vec3 point{};
        Vec3 normal_from_low{};
        Vec3 velocity_high_from_low{};
        // A trigger overlap's Point, per side: the other body's position.
        Vec3 position_low{};
        Vec3 position_high{};
    };
    struct StepMotion {
        b3Vec3 linear{};
        b3Vec3 angular{};
        b3Vec3 center{};
    };
    std::map<std::pair<std::uint64_t, std::uint64_t>, ShapeTouch> shape_touches;
    std::unordered_map<std::uint64_t, PairTouch> pair_touches;
    // Sorted by body key, kept (not freed) between steps, so a step allocates nothing.
    std::vector<std::pair<std::uint64_t, StepMotion>> before_step;
    std::vector<std::uint64_t> touch_scratch;
    std::vector<b3ContactData> contact_scratch;
    // Each controller's ground last step, as a shape key: a controller hovers,
    // so standing is its touch (stand).
    std::unordered_map<InstanceId, std::uint64_t> standing;
    // Each controller's sensors under its feet last step, sorted shape keys:
    // its cylinder hovers, so a thin trigger pad lying on the ground never
    // meets it (feet). The query's scratch beside it.
    std::unordered_map<InstanceId, std::vector<std::uint64_t>> feet_sensors;
    std::vector<std::uint64_t> feet_now;
    double play_seconds = 0.0;
    std::size_t listened = 0;
    // refilter_all's scratch: each group's name and filter bits this pass.
    struct GroupBits {
        const std::string* name;
        std::uint64_t category;
        std::uint64_t mask;
    };
    std::vector<GroupBits> refilter_bits;
    // The DataModel's collision groups, set at each sync, and the revision
    // whose filters the shapes have.
    const CollisionGroups* groups = nullptr;
    std::uint64_t groups_seen = ~std::uint64_t{0};
    std::unordered_map<std::uint32_t, Compound> compounds;
    // Where each body a hand holds must go this step (drive_held).
    std::vector<Assemblies::HeldTarget> held;
    std::function<void(const std::string&)> warn;

    // A Terrain's static body: one mesh shape per chunk collider.
    struct TerrainChunkShape {
        std::uint64_t revision = 0;
        // Keeps the mesh the shape points into alive as long as the shape.
        std::shared_ptr<void> collider;
        b3ShapeId shape = b3_nullShapeId;
        std::uint64_t seen = 0;
    };
    struct TerrainBody {
        b3BodyId body = b3_nullBodyId;
        Matrix4 transform{};
        // What the shapes were last made from: the TerrainView's
        // chunks_revision (TerrainWorld republishes the colliders with each)
        // and CanCollide. Both the same, the shapes are too.
        std::uint64_t chunks_revision = 0;
        bool can_collide = false;
        std::unordered_map<terrain::ChunkCoord, TerrainChunkShape, terrain::ChunkCoordHash> chunks;
        std::uint64_t seen = 0;
    };
    TerrainWorld* terrains = nullptr;
    std::unordered_map<InstanceId, TerrainBody> terrain_bodies;
    // Per Terrain, the collider interest last told to TerrainWorld: the
    // body-centre chunks it came from, and per chunk how many of those
    // centres ask for it (update_terrain_collider_interest).
    struct Interest {
        std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> centers;
        std::unordered_map<terrain::ChunkCoord, int, terrain::ChunkCoordHash> counts;
    };
    std::unordered_map<InstanceId, Interest> interests;
    const std::array<b3SurfaceMaterial, kTerrainMaterialCount> terrain_surfaces = terrain_materials();

    // Scratch, kept between steps so a step does not allocate once warm.
    std::vector<InstanceId> eligible;
    std::vector<std::pair<InstanceId, InstanceId>> wanted;
    // Which body moves each claimed GameObject, by the GameObject's slot, and
    // the GameObjects claimed this sync: reserved, so a sync allocates nothing.
    std::vector<InstanceId> claim_by_slot;
    std::vector<InstanceId> claimed;
    std::vector<InstanceId> claimants;
    std::vector<InstanceId> gone;
    std::vector<std::uint32_t> compounds_gone;
    // A body's motion as it was before its members moved to another body.
    struct Motion {
        b3BodyId body = b3_nullBodyId;
        float mass = 0.f;
        b3Vec3 velocity{};
        b3Vec3 angular{};
        b3Vec3 center{};
    };
    std::vector<Motion> motions;
    std::vector<InstanceId> members_scratch;
    std::vector<b3Vec3> points;
    std::vector<b3Vec3> turned_points;
    std::vector<Vec3> mesh_points;
    std::vector<std::uint32_t> triangles;
    std::vector<std::int32_t> indices;
    std::vector<b3ContactData> contacts;

    Impl() {
        bodies.reserve(1024);
        compounds.reserve(1024);
        compounds_gone.reserve(1024);
        motions.reserve(1024);
        members_scratch.reserve(1024);
        eligible.reserve(1024);
        wanted.reserve(1024);
        claim_by_slot.assign(DataModel::kMaxInstances, 0);
        claimed.reserve(DataModel::kMaxInstances);
        claimants.reserve(1024);
        gone.reserve(1024);
        joints.reserve(1024);
    }

    ~Impl() { reset(); }

    void reset() {
        if (b3World_IsValid(world)) {
            b3DestroyWorld(world);
        }
        world = b3_nullWorldId;
        // The world took its shapes with it; the meshes they pointed into are ours.
        for (auto& [id, record] : bodies) {
            if (record.mesh != nullptr) {
                b3DestroyMesh(record.mesh);
            }
        }
        bodies.clear();
        compounds.clear();
        // Terrain colliders go only now that no shape points into them.
        terrain_bodies.clear();
        // A new world has no shapes: the pairs are applied again as they are made.
        shape_touches.clear();
        pair_touches.clear();
        before_step.clear();
        standing.clear();
        feet_sensors.clear();
        no_collide.clear();
        no_collide_keys.clear();
        no_collide_ids.clear();
        // The world took the joints with it.
        joints.clear();
    }

    static std::uint64_t pair_key(InstanceId a, InstanceId b) {
        const InstanceId low = std::min(a, b);
        const InstanceId high = std::max(a, b);
        return (static_cast<std::uint64_t>(low) << 32) | high;
    }

    // Box3D asks this of a new pair when either shape enables custom
    // filtering: false keeps a NoCollisionConstraint's two bodies apart.
    static bool no_collide_filter(b3ShapeId a, b3ShapeId b, void* context) {
        const auto* self = static_cast<const Impl*>(context);
        const auto id_of = [](b3ShapeId shape) {
            return static_cast<InstanceId>(reinterpret_cast<std::uintptr_t>(b3Shape_GetUserData(shape)));
        };
        return self->no_collide_keys.count(pair_key(id_of(a), id_of(b))) == 0;
    }

    // The pairs changed: every body in a pair added or dropped gets its
    // shapes made again, since Box3D asks the filter only of a new proxy
    // pair, so two bodies already touching let go at once, and two kept
    // apart meet again.
    // ---- Touched and TouchEnded ------------------------------------------

    static std::uint64_t shape_key(b3ShapeId shape) { return b3StoreShapeId(shape); }
    static std::uint64_t contact_key(b3ContactId contact) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(contact.index1)) << 32) | contact.generation;
    }
    static InstanceId instance_of(b3ShapeId shape) {
        return static_cast<InstanceId>(reinterpret_cast<std::uintptr_t>(b3Shape_GetUserData(shape)));
    }
    static Vec3 vec_of(b3Vec3 v) { return Vec3{v.x, v.y, v.z}; }
    static b3Vec3 cross(b3Vec3 a, b3Vec3 b) {
        return b3Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    // Who listens, and their shapes' contact events to match. Stopped:
    // nobody, and every touch is forgotten without an event.
    void update_listened(DataModel& game, bool running) {
        listened = 0;
        for (auto& [id, record] : bodies) {
            const bool now =
                running && (game.event_listened(id, "Touched") || game.event_listened(id, "TouchEnded"));
            if (now != record.listened) {
                record.listened = now;
                for (const b3ShapeId shape : record.shapes) {
                    if (b3Shape_IsValid(shape)) {
                        b3Shape_EnableContactEvents(shape, now);
                    }
                }
                if (now) {
                    hear_from_now(game, id, record);
                }
            }
            listened += now ? 1u : 0u;
        }
        if (!running) {
            shape_touches.clear();
            pair_touches.clear();
            before_step.clear();
            standing.clear();
            feet_sensors.clear();
            play_seconds = 0.0;
        }
    }

    // A body newly listened to: Box3D fixes whether a contact reports when
    // the contact is made, so contacts it already has would never say they
    // end. Those touching now are counted, silently (connecting mid-contact
    // fires nothing), and its shapes are made again, so every contact is made
    // anew with events: the counted pairs wait as pending ends and resume.
    void hear_from_now(DataModel& game, InstanceId id, Body& record) {
        auto* object = dynamic_cast<PhysicsBase*>(game.instance(id));
        if (object == nullptr || record.shapes.empty()) {
            return;
        }
        for (const b3ShapeId shape : record.shapes) {
            if (!b3Shape_IsValid(shape)) {
                continue;
            }
            contact_scratch.resize(static_cast<std::size_t>(std::max(b3Shape_GetContactCapacity(shape), 0)));
            const int count = contact_scratch.empty()
                                  ? 0
                                  : b3Shape_GetContactData(shape, contact_scratch.data(),
                                                           static_cast<int>(contact_scratch.size()));
            for (int i = 0; i < count; ++i) {
                const b3ContactData& data = contact_scratch[static_cast<std::size_t>(i)];
                bool touching = false;
                for (int m = 0; m < data.manifoldCount; ++m) {
                    touching = touching || data.manifolds[m].pointCount > 0;
                }
                if (!touching || !b3Shape_IsValid(data.shapeIdA) || !b3Shape_IsValid(data.shapeIdB)) {
                    continue;
                }
                bool began = false;
                PairTouch* pair = begin_pair(data.shapeIdA, data.shapeIdB, began, contact_key(data.contactId));
                if (pair != nullptr && began) {
                    contact_facts(data.shapeIdA, data.shapeIdB, &data, *pair, false);
                }
            }
        }
        make_shape(game, *object, record);
    }

    // While anyone listens: every moving body's velocities, before the step
    // resolves its collisions (RelativeVelocity).
    void save_motion() {
        before_step.clear();
        if (listened == 0) {
            return;
        }
        for (const auto& [id, record] : bodies) {
            if (!b3Body_IsValid(record.body) || b3Body_GetType(record.body) == b3_staticBody) {
                continue;
            }
            // A compound's members share its body: once is enough, and the sort drops the rest.
            before_step.emplace_back(b3StoreBodyId(record.body),
                                     StepMotion{b3Body_GetLinearVelocity(record.body),
                                                b3Body_GetAngularVelocity(record.body),
                                                b3Body_GetWorldCenter(record.body)});
        }
        std::sort(before_step.begin(), before_step.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    // A body's velocity at point: from before the step when saved, else now.
    b3Vec3 point_velocity(b3BodyId body, b3Vec3 point, bool before) const {
        if (!b3Body_IsValid(body) || b3Body_GetType(body) == b3_staticBody) {
            return b3Vec3{0.f, 0.f, 0.f};
        }
        StepMotion motion{b3Body_GetLinearVelocity(body), b3Body_GetAngularVelocity(body),
                          b3Body_GetWorldCenter(body)};
        if (before) {
            const std::uint64_t key = b3StoreBodyId(body);
            const auto found = std::lower_bound(before_step.begin(), before_step.end(), key,
                                                [](const auto& entry, std::uint64_t k) { return entry.first < k; });
            if (found != before_step.end() && found->first == key) {
                motion = found->second;
            }
        }
        const b3Vec3 arm{point.x - motion.center.x, point.y - motion.center.y, point.z - motion.center.z};
        const b3Vec3 spin = cross(motion.angular, arm);
        return b3Vec3{motion.linear.x + spin.x, motion.linear.y + spin.y, motion.linear.z + spin.z};
    }

    // The shapes go: their touching pairs are let go. A pair left touching
    // nothing waits for the end of the step: made again in place, it
    // touches again and nothing fires.
    void forget_shapes(const Body& record) {
        for (const b3ShapeId shape : record.shapes) {
            forget_shape(shape);
        }
    }

    // One shape going, a body's or a Terrain chunk's.
    void forget_shape(b3ShapeId shape) {
        if (shape_touches.empty() || !b3Shape_IsValid(shape)) {
            return;
        }
        const std::uint64_t key = shape_key(shape);
        for (auto it = shape_touches.begin(); it != shape_touches.end();) {
            if (it->first.first != key && it->first.second != key) {
                ++it;
                continue;
            }
            const auto pair = pair_touches.find(pair_key(it->second.a, it->second.b));
            if (pair != pair_touches.end() && --pair->second.count <= 0) {
                pair->second.count = 0;
                pair->second.pending_end = true;
            }
            it = shape_touches.erase(it);
        }
    }

    // A shape pair began touching: true when its instance pair did too.
    PairTouch* begin_pair(b3ShapeId a, b3ShapeId b, bool& began, std::uint64_t contact = 0) {
        return begin_keys(shape_key(a), shape_key(b), instance_of(a), instance_of(b), began, contact);
    }

    // The same, by key: a shape's, or a stand's (stand_key).
    PairTouch* begin_keys(std::uint64_t ka, std::uint64_t kb, InstanceId ia, InstanceId ib, bool& began,
                          std::uint64_t contact = 0) {
        began = false;
        if (ia == 0 || ib == 0 || ia == ib) {
            return nullptr;
        }
        const auto key = std::make_pair(std::min(ka, kb), std::max(ka, kb));
        if (const auto found = shape_touches.find(key); found != shape_touches.end()) {
            // A contact made again for shapes still touching: the new one stands for it.
            found->second.contact = contact;
            return nullptr;
        }
        shape_touches[key] = ShapeTouch{ia, ib, contact};
        PairTouch& pair = pair_touches[pair_key(ia, ib)];
        if (pair.count++ == 0) {
            // Back from a pending end: it never stopped touching.
            began = !pair.pending_end;
            pair.pending_end = false;
        }
        return &pair;
    }

    // A shape pair stopped touching: true when its instance pair did too.
    PairTouch* end_pair(b3ShapeId a, b3ShapeId b, bool& ended, std::uint64_t contact = 0) {
        return end_keys(shape_key(a), shape_key(b), ended, contact);
    }

    PairTouch* end_keys(std::uint64_t ka, std::uint64_t kb, bool& ended, std::uint64_t contact = 0) {
        ended = false;
        const auto found = shape_touches.find(std::make_pair(std::min(ka, kb), std::max(ka, kb)));
        if (found == shape_touches.end()) {
            return nullptr;
        }
        if (contact != 0 && found->second.contact != 0 && found->second.contact != contact) {
            return nullptr;  // the end of a contact already replaced by a newer one
        }
        const std::uint64_t key = pair_key(found->second.a, found->second.b);
        shape_touches.erase(found);
        const auto pair = pair_touches.find(key);
        if (pair == pair_touches.end()) {
            return nullptr;
        }
        if (--pair->second.count <= 0) {
            pair->second.count = 0;
            ended = true;
        }
        return &pair->second;
    }

    // Fires name on each listened side of the pair (low, high).
    void fire_pair(DataModel& game, const char* name, InstanceId low, InstanceId high, const PairTouch& pair) {
        for (const InstanceId side : {low, high}) {
            const InstanceId other = side == low ? high : low;
            if (game.instance(side) == nullptr || !game.in_workspace(side) || !game.event_listened(side, name)) {
                continue;
            }
            const float sign = side == low ? 1.f : -1.f;
            LuaSlot slot;
            slot.kind = LuaSlot::Kind::TouchObject;
            slot.touch.time = play_seconds;
            slot.touch.instance = other;
            slot.touch.point = pair.sensor ? (side == low ? pair.position_high : pair.position_low) : pair.point;
            slot.touch.normal = Vec3{pair.normal_from_low.x * sign, pair.normal_from_low.y * sign,
                                     pair.normal_from_low.z * sign};
            slot.touch.relative_velocity =
                Vec3{pair.velocity_high_from_low.x * sign, pair.velocity_high_from_low.y * sign,
                     pair.velocity_high_from_low.z * sign};
            game.fire_event(side, name, EventArgs{slot});
        }
    }

    // The facts of a contact between shapes a and b, oriented from the lower
    // instance id: Point (manifold points averaged), Normal, RelativeVelocity.
    void contact_facts(b3ShapeId a, b3ShapeId b, const b3ContactData* data, PairTouch& pair, bool before) const {
        const b3BodyId body_a = b3Shape_GetBody(a);
        const b3BodyId body_b = b3Shape_GetBody(b);
        b3Vec3 sum{0.f, 0.f, 0.f};
        int points = 0;
        b3Vec3 normal{0.f, 0.f, 0.f};
        if (data != nullptr) {
            const b3Vec3 center = b3Body_GetWorldCenter(body_a);
            for (int m = 0; m < data->manifoldCount; ++m) {
                const b3Manifold& manifold = data->manifolds[m];
                if (manifold.pointCount > 0 && points == 0) {
                    normal = manifold.normal;
                }
                for (int i = 0; i < manifold.pointCount; ++i) {
                    sum.x += center.x + manifold.points[i].anchorA.x;
                    sum.y += center.y + manifold.points[i].anchorA.y;
                    sum.z += center.z + manifold.points[i].anchorA.z;
                    ++points;
                }
            }
        }
        b3Vec3 point = points > 0 ? b3Vec3{sum.x / points, sum.y / points, sum.z / points}
                                  : b3Body_GetWorldCenter(body_b);
        const auto convex = [](b3ShapeId shape) {
            const b3ShapeType type = b3Shape_GetType(shape);
            return type == b3_sphereShape || type == b3_capsuleShape || type == b3_hullShape;
        };
        if (data == nullptr && !(convex(a) && convex(b))) {
            // Parting from a mesh (Terrain, an anchored Custom): Box3D's
            // closest point is for convex shapes only, so the last facts known stand.
            const bool a_low = instance_of(a) < instance_of(b);
            const Vec3 last = pair.normal_from_low;
            point = b3Vec3{pair.point.x, pair.point.y, pair.point.z};
            normal = a_low ? b3Vec3{last.x, last.y, last.z} : b3Vec3{-last.x, -last.y, -last.z};
        } else if (data == nullptr) {
            // Parting: the closest points between the two, and the gap's direction.
            const b3Vec3 on_a = b3Shape_GetClosestPoint(a, b3Body_GetWorldCenter(body_b));
            const b3Vec3 on_b = b3Shape_GetClosestPoint(b, on_a);
            point = b3Vec3{(on_a.x + on_b.x) * 0.5f, (on_a.y + on_b.y) * 0.5f, (on_a.z + on_b.z) * 0.5f};
            const b3Vec3 gap{on_b.x - on_a.x, on_b.y - on_a.y, on_b.z - on_a.z};
            const float length = std::sqrt(gap.x * gap.x + gap.y * gap.y + gap.z * gap.z);
            if (length > 1e-5f) {
                normal = b3Vec3{gap.x / length, gap.y / length, gap.z / length};
            } else {
                const bool a_low = instance_of(a) < instance_of(b);
                const Vec3 last = pair.normal_from_low;
                normal = a_low ? b3Vec3{last.x, last.y, last.z} : b3Vec3{-last.x, -last.y, -last.z};
            }
        }
        const b3Vec3 va = point_velocity(body_a, point, before);
        const b3Vec3 vb = point_velocity(body_b, point, before);
        const bool a_low = instance_of(a) < instance_of(b);
        const float sign = a_low ? 1.f : -1.f;
        pair.sensor = false;
        pair.point = vec_of(point);
        pair.normal_from_low = Vec3{normal.x * sign, normal.y * sign, normal.z * sign};
        pair.velocity_high_from_low =
            Vec3{(vb.x - va.x) * sign, (vb.y - va.y) * sign, (vb.z - va.z) * sign};
    }

    // A trigger overlap's facts: each side's Point is the other's position;
    // no Normal; RelativeVelocity at the visitor's position.
    void overlap_facts(b3ShapeId sensor, b3ShapeId visitor, PairTouch& pair, bool before) const {
        const b3BodyId zone = b3Shape_GetBody(sensor);
        const b3BodyId mover = b3Shape_GetBody(visitor);
        const b3Vec3 at = b3Body_GetPosition(mover);
        const b3Vec3 vz = point_velocity(zone, at, before);
        const b3Vec3 vm = point_velocity(mover, at, before);
        const bool zone_low = instance_of(sensor) < instance_of(visitor);
        const float sign = zone_low ? 1.f : -1.f;
        pair.sensor = true;
        pair.normal_from_low = Vec3{};
        pair.velocity_high_from_low = Vec3{(vm.x - vz.x) * sign, (vm.y - vz.y) * sign, (vm.z - vz.z) * sign};
        const Vec3 zone_at = vec_of(b3Body_GetPosition(zone));
        const Vec3 mover_at = vec_of(at);
        pair.position_low = zone_low ? zone_at : mover_at;
        pair.position_high = zone_low ? mover_at : zone_at;
    }

    // A stand's key, for a controller: its instance id, whose upper 32 bits
    // are zero, which no shape's key has (b3StoreShapeId puts index1, from 1,
    // there).
    static std::uint64_t stand_key(InstanceId controller) { return controller; }

    // A controller hovers over its ground, so what it stands on is its touch:
    // a new ground begins one, leaving it ends one, as contacts do, counted
    // with any real contact between the two.
    void stand(DataModel& game) {
        for (auto& [id, record] : bodies) {
            if (!record.controller) {
                continue;
            }
            const std::uint64_t now = b3Shape_IsValid(record.stands_on) ? shape_key(record.stands_on) : 0;
            const auto found = standing.find(id);
            const std::uint64_t was = found != standing.end() ? found->second : 0;
            if (now == was) {
                continue;
            }
            if (was != 0) {
                end_stand(game, id, was);
            }
            if (now != 0) {
                const InstanceId ground = instance_of(record.stands_on);
                bool began = false;
                PairTouch* pair = begin_keys(stand_key(id), now, id, ground, began);
                if (pair != nullptr && began) {
                    // Oriented from the lower id: the controller looks down at its ground.
                    const bool controller_low = id < ground;
                    const float sign = controller_low ? 1.f : -1.f;
                    const b3Vec3 down{-record.stand_normal.x, -record.stand_normal.y, -record.stand_normal.z};
                    pair->sensor = false;
                    pair->point = vec_of(record.stand_point);
                    pair->normal_from_low = Vec3{down.x * sign, down.y * sign, down.z * sign};
                    pair->velocity_high_from_low = Vec3{record.stand_velocity.x * sign, record.stand_velocity.y * sign,
                                                        record.stand_velocity.z * sign};
                    fire_pair(game, "Touched", std::min(id, ground), std::max(id, ground), *pair);
                }
                standing[id] = now;
            } else {
                standing.erase(id);
            }
        }
        // Controllers that are gone: their stands end.
        touch_scratch.clear();
        for (const auto& [id, key] : standing) {
            if (bodies.find(id) == bodies.end()) {
                touch_scratch.push_back(id);
            }
        }
        for (const std::uint64_t id : touch_scratch) {
            const auto found = standing.find(static_cast<InstanceId>(id));
            end_stand(game, found->first, found->second);
            standing.erase(found);
        }
    }

    static bool feet_hit(b3ShapeId shape, void* context) {
        auto* found = static_cast<std::pair<b3BodyId, std::vector<std::uint64_t>*>*>(context);
        if (b3Shape_IsSensor(shape) && !B3_ID_EQUALS(b3Shape_GetBody(shape), found->first)) {
            found->second->push_back(b3StoreShapeId(shape));
        }
        return true;
    }

    // While anyone listens: each controller's feet, from the ground up its
    // hover gap, are a trigger pad's visitor, as its cylinder is above that.
    // New sensors there begin a touch, gone ones end it, keyed as a stand.
    void feet(DataModel& game) {
        for (auto& [id, record] : bodies) {
            if (!record.controller || record.compound != 0 || record.shapes.empty()) {
                continue;
            }
            const auto* controller = dynamic_cast<const PlayerController*>(game.instance(id));
            feet_now.clear();
            if (controller != nullptr && listened > 0 && controller->can_collide()) {
                const float radius = static_cast<float>(controller->radius());
                const float gap = static_cast<float>(controller->hover_gap());
                b3Vec3 ring[2 * kProbeSides];
                for (int side = 0; side < kProbeSides; ++side) {
                    const float angle = 2.f * kPi * static_cast<float>(side) / static_cast<float>(kProbeSides);
                    const float x = radius * std::cos(angle);
                    const float z = radius * std::sin(angle);
                    ring[side] = b3Vec3{x, 0.f, z};
                    ring[kProbeSides + side] = b3Vec3{x, gap, z};
                }
                b3ShapeProxy proxy;
                proxy.points = ring;
                proxy.count = 2 * kProbeSides;
                proxy.radius = 0.f;
                b3QueryFilter filter = b3DefaultQueryFilter();
                filter.categoryBits = CollisionGroups::kRayBit;
                filter.maskBits = groups->collides_with(controller->collision_group());
                std::pair<b3BodyId, std::vector<std::uint64_t>*> found{record.body, &feet_now};
                b3World_OverlapShape(world, b3Body_GetPosition(record.body), &proxy, filter, feet_hit, &found);
                std::sort(feet_now.begin(), feet_now.end());
                feet_now.erase(std::unique(feet_now.begin(), feet_now.end()), feet_now.end());
            }
            std::vector<std::uint64_t>& was = feet_sensors[id];
            if (was == feet_now) {
                continue;
            }
            for (const std::uint64_t key : feet_now) {
                if (std::binary_search(was.begin(), was.end(), key)) {
                    continue;
                }
                const b3ShapeId sensor = b3LoadShapeId(key);
                const InstanceId zone = instance_of(sensor);
                bool began = false;
                PairTouch* pair = begin_keys(stand_key(id), key, id, zone, began);
                if (pair != nullptr && began) {
                    overlap_facts(sensor, record.shapes.front(), *pair, true);
                    fire_pair(game, "Touched", std::min(id, zone), std::max(id, zone), *pair);
                }
            }
            for (const std::uint64_t key : was) {
                if (!std::binary_search(feet_now.begin(), feet_now.end(), key)) {
                    end_stand(game, id, key);
                }
            }
            was = feet_now;
        }
        // Controllers that are gone: what was under their feet ends.
        touch_scratch.clear();
        for (const auto& [id, keys] : feet_sensors) {
            if (bodies.find(id) == bodies.end()) {
                touch_scratch.push_back(id);
            }
        }
        for (const std::uint64_t id : touch_scratch) {
            const auto found = feet_sensors.find(static_cast<InstanceId>(id));
            for (const std::uint64_t key : found->second) {
                end_stand(game, found->first, key);
            }
            feet_sensors.erase(found);
        }
    }

    // A controller left the ground with key ground: from the last facts known.
    void end_stand(DataModel& game, InstanceId controller, std::uint64_t ground) {
        const auto touch = shape_touches.find(
            std::make_pair(std::min(stand_key(controller), ground), std::max(stand_key(controller), ground)));
        if (touch == shape_touches.end()) {
            return;  // forget_shapes let it go already
        }
        const InstanceId a = touch->second.a;
        const InstanceId b = touch->second.b;
        bool ended = false;
        PairTouch* pair = end_keys(stand_key(controller), ground, ended);
        if (pair != nullptr && ended) {
            fire_pair(game, "TouchEnded", std::min(a, b), std::max(a, b), *pair);
            pair_touches.erase(pair_key(a, b));
        }
    }

    // After a step: Box3D's contact and sensor events into Touched and
    // TouchEnded, then the pending ends that did not touch again.
    void touches(DataModel& game) {
        const b3ContactEvents contacts = b3World_GetContactEvents(world);
        for (int i = 0; i < contacts.beginCount; ++i) {
            const b3ContactBeginTouchEvent& event = contacts.beginEvents[i];
            if (!b3Shape_IsValid(event.shapeIdA) || !b3Shape_IsValid(event.shapeIdB)) {
                continue;
            }
            bool began = false;
            PairTouch* pair = begin_pair(event.shapeIdA, event.shapeIdB, began, contact_key(event.contactId));
            if (pair == nullptr || !began) {
                continue;
            }
            const bool valid = b3Contact_IsValid(event.contactId);
            const b3ContactData data = valid ? b3Contact_GetData(event.contactId) : b3ContactData{};
            contact_facts(event.shapeIdA, event.shapeIdB, valid ? &data : nullptr, *pair, true);
            const InstanceId a = instance_of(event.shapeIdA);
            const InstanceId b = instance_of(event.shapeIdB);
            fire_pair(game, "Touched", std::min(a, b), std::max(a, b), *pair);
        }
        const b3SensorEvents sensors = b3World_GetSensorEvents(world);
        for (int i = 0; i < sensors.beginCount; ++i) {
            const b3SensorBeginTouchEvent& event = sensors.beginEvents[i];
            if (!b3Shape_IsValid(event.sensorShapeId) || !b3Shape_IsValid(event.visitorShapeId)) {
                continue;
            }
            bool began = false;
            PairTouch* pair = begin_pair(event.sensorShapeId, event.visitorShapeId, began);
            if (pair == nullptr || !began) {
                continue;
            }
            overlap_facts(event.sensorShapeId, event.visitorShapeId, *pair, true);
            const InstanceId a = instance_of(event.sensorShapeId);
            const InstanceId b = instance_of(event.visitorShapeId);
            fire_pair(game, "Touched", std::min(a, b), std::max(a, b), *pair);
        }
        for (int i = 0; i < contacts.endCount; ++i) {
            const b3ContactEndTouchEvent& event = contacts.endEvents[i];
            if (!b3Shape_IsValid(event.shapeIdA) || !b3Shape_IsValid(event.shapeIdB)) {
                continue;  // shapes PhysicsWorld removed: forget_shapes settled them
            }
            bool ended = false;
            PairTouch* pair = end_pair(event.shapeIdA, event.shapeIdB, ended, contact_key(event.contactId));
            if (pair == nullptr || !ended) {
                continue;
            }
            contact_facts(event.shapeIdA, event.shapeIdB, nullptr, *pair, false);
            const InstanceId a = instance_of(event.shapeIdA);
            const InstanceId b = instance_of(event.shapeIdB);
            fire_pair(game, "TouchEnded", std::min(a, b), std::max(a, b), *pair);
            pair_touches.erase(pair_key(a, b));
        }
        for (int i = 0; i < sensors.endCount; ++i) {
            const b3SensorEndTouchEvent& event = sensors.endEvents[i];
            if (!b3Shape_IsValid(event.sensorShapeId) || !b3Shape_IsValid(event.visitorShapeId)) {
                continue;
            }
            bool ended = false;
            PairTouch* pair = end_pair(event.sensorShapeId, event.visitorShapeId, ended);
            if (pair == nullptr || !ended) {
                continue;
            }
            overlap_facts(event.sensorShapeId, event.visitorShapeId, *pair, false);
            const InstanceId a = instance_of(event.sensorShapeId);
            const InstanceId b = instance_of(event.visitorShapeId);
            fire_pair(game, "TouchEnded", std::min(a, b), std::max(a, b), *pair);
            pair_touches.erase(pair_key(a, b));
        }
        stand(game);
        feet(game);
        // Pending ends that did not touch again: they end now, from the last facts known.
        touch_scratch.clear();
        for (const auto& [key, pair] : pair_touches) {
            if (pair.pending_end && pair.count == 0) {
                touch_scratch.push_back(key);
            }
        }
        for (const std::uint64_t key : touch_scratch) {
            const auto found = pair_touches.find(key);
            const InstanceId low = static_cast<InstanceId>(key >> 32);
            const InstanceId high = static_cast<InstanceId>(key & 0xffffffffu);
            fire_pair(game, "TouchEnded", low, high, found->second);
            pair_touches.erase(found);
        }
    }

    void update_no_collide(DataModel& game) {
        const std::vector<std::pair<InstanceId, InstanceId>>& pairs = game.assemblies().no_collide_pairs();
        if (pairs == no_collide) {
            return;
        }
        no_collide_diff.clear();
        std::set_symmetric_difference(pairs.begin(), pairs.end(), no_collide.begin(), no_collide.end(),
                                      std::back_inserter(no_collide_diff));
        no_collide_changed.clear();
        for (const auto& [a, b] : no_collide_diff) {
            no_collide_changed.push_back(a);
            no_collide_changed.push_back(b);
        }
        no_collide = pairs;
        no_collide_keys.clear();
        no_collide_ids.clear();
        for (const auto& [a, b] : no_collide) {
            no_collide_keys.insert(pair_key(a, b));
            no_collide_ids.insert(a);
            no_collide_ids.insert(b);
        }
        for (const InstanceId id : no_collide_changed) {
            const auto found = bodies.find(id);
            auto* object = dynamic_cast<PhysicsBase*>(game.instance(id));
            if (found != bodies.end() && object != nullptr) {
                make_shape(game, *object, found->second);
            }
        }
    }


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
    // both bodies.
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
                // Box3D's rigid mode ignores the limit and the motor: either of
                // them asks for its spring mode, with no stiffness unless
                // EnableSpring, so a rope hangs free inside its limits.
                b3DistanceJoint_EnableSpring(
                    id, c.flag(P::EnableSpring) || c.flag(P::EnableLimit) || c.flag(P::EnableMotor));
                b3DistanceJoint_SetSpringHertz(id, c.flag(P::EnableSpring) ? f(P::Hertz) : 0.f);
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
            if (auto* dropped = dynamic_cast<JointConstraint*>(game.instance(it->first))) {
                dropped->set_reading(0.0);
            }
            it = joints.erase(it);
        }
    }

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

    void begin(std::uint32_t next_generation) {
        reset();
        b3WorldDef def = b3DefaultWorldDef();
        def.gravity = b3Vec3{0.f, gravity, 0.f};
        world = b3CreateWorld(&def);
        b3World_SetCustomFilterCallback(world, &no_collide_filter, this);
        generation = next_generation;
        played = false;
        interests.clear();
    }

    void say(const std::string& text) const {
        if (warn) {
            warn(text);
        }
    }

    void sync(DataModel& game, bool stepping = false) {
        groups = &game.collision_groups();
        if (!b3World_IsValid(world) || generation != game.world_generation()) {
            begin(game.world_generation());
        }
        if (groups->revision() != groups_seen) {
            groups_seen = groups->revision();
            refilter_all(game);
        }
        pull_gravity(game);
        // Assemblies first: which bodies are one body this sync.
        game.assemblies().sync_play(game);
        update_no_collide(game);
        const bool running = game.simulation_running();
        poses_from_game_objects = !running || !played;
        {
            PROFILE_SCOPE("Physics reconcile", profiler::Group::Physics);
            reconcile(game);
            settle_held_types(game);
        }
        {
            PROFILE_SCOPE("Physics joints", profiler::Group::Physics);
            update_joints(game, running);
        }
        {
            PROFILE_SCOPE("Terrain collider interest", profiler::Group::Physics);
            update_terrain_collider_interest(game);
        }
        {
            PROFILE_SCOPE("Terrain collider shapes", profiler::Group::Physics);
            reconcile_terrain();
        }
        update_listened(game, running);
        played = running && (stepping || played);
        game.set_physics_stepped(played);
    }

    void simulate(DataModel& game, double dt) {
        if (!game.simulation_running()) {
            return;
        }
        {
            PROFILE_SCOPE("Physics control", profiler::Group::Physics);
            control(game, dt);
            drive_held(game, dt);
        }
        save_motion();
        {
            PROFILE_SCOPE("Box3D step", profiler::Group::Physics);
            b3World_Step(world, static_cast<float>(dt), 1);
        }
        play_seconds += dt;
        if (!joints.empty()) {
            read_joints(game);
        }
        {
            PROFILE_SCOPE("Physics touches", profiler::Group::Physics);
            touches(game);
        }
        unclimb(game);
        {
            PROFILE_SCOPE("Physics pull", profiler::Group::Physics);
            pull(game);
            // The hand moved with its GameObject this step: what it holds is
            // drawn on it, though its body comes along only next step.
            game.assemblies().place_held(game);
        }
    }

    // A body a hand holds is kinematic in play: the hand moves it, and
    // gravity and contacts do not. Let go, it is what it was again.
    static void settle_type(b3BodyId body, bool anchored, bool held) {
        const b3BodyType now = b3Body_GetType(body);
        if (held) {
            if (now != b3_kinematicBody) {
                b3Body_SetType(body, b3_kinematicBody);
            }
            return;
        }
        if (now == b3_kinematicBody) {
            b3Body_SetType(body, anchored ? b3_staticBody : b3_dynamicBody);
            b3Body_SetLinearVelocity(body, b3Vec3{});
            b3Body_SetAngularVelocity(body, b3Vec3{});
        }
    }

    // A walking character's body (an Enabled PlayerController's, or an
    // assembly's it roots) never turns but about Y and never sleeps; any other
    // turns freely. Changed only when it differs.
    static void settle_locks(b3BodyId body, bool upright) {
        const b3MotionLocks locks = b3Body_GetMotionLocks(body);
        if (locks.angularX == upright && locks.angularY == upright && locks.angularZ == upright) {
            return;
        }
        b3MotionLocks next = locks;
        next.angularX = upright;
        next.angularY = upright;
        next.angularZ = upright;
        b3Body_SetMotionLocks(body, next);
        b3Body_EnableSleep(body, !upright);
    }

    void settle_held_types(DataModel& game) {
        const bool running = game.simulation_running();
        const Assemblies& assemblies = game.assemblies();
        for (auto& [id, record] : bodies) {
            if (record.compound != 0 || !b3Body_IsValid(record.body)) {
                continue;
            }
            const auto* object = dynamic_cast<const PhysicsBase*>(game.instance(id));
            if (object != nullptr) {
                settle_type(record.body, object->anchored(), running && (assemblies.flags(id) & Assemblies::kHeld) != 0);
                settle_locks(record.body, record.controller);
            }
        }
        for (auto& [stamp, compound] : compounds) {
            if (b3Body_IsValid(compound.body)) {
                settle_type(compound.body, compound.anchored,
                            running && (assemblies.flags(compound.root) & Assemblies::kHeld) != 0);
                settle_locks(compound.body, enabled_controller(game.instance(compound.root)) != nullptr);
            }
        }
    }

    // Gives each held body the velocity that takes it to its hand over this
    // step, so it pushes what it swings into.
    void drive_held(DataModel& game, double dt) {
        const Assemblies& assemblies = game.assemblies();
        assemblies.held_targets(game, held);
        const std::vector<Assemblies::PlayAssembly>& plays = assemblies.play();
        for (const Assemblies::HeldTarget& target : held) {
            b3BodyId body = b3_nullBodyId;
            if (target.assembly >= 0) {
                const auto found = compounds.find(plays[static_cast<std::size_t>(target.assembly)].stamp);
                if (found != compounds.end()) {
                    body = found->second.body;
                }
            } else if (const auto found = bodies.find(target.node); found != bodies.end() && found->second.compound == 0) {
                body = found->second.body;
            }
            if (!b3Body_IsValid(body) || b3Body_GetType(body) != b3_kinematicBody) {
                continue;
            }
            // A move too small to set a velocity leaves the last one: none.
            b3Body_SetLinearVelocity(body, b3Vec3{});
            b3Body_SetAngularVelocity(body, b3Vec3{});
            b3WorldTransform to{};
            pose_of(target.frame, to.p, to.q);
            b3Body_SetTargetTransform(body, to, static_cast<float>(dt), true);
        }
    }

    void step(DataModel& game, double dt) {
        if (!game.simulation_running()) {
            return;
        }
        sync(game, true);
        simulate(game, dt);
    }

    // Task 9 (spec "Physics"): whether PhysicsWorld currently has a shape
    // for coord on terrain's body -- what Box3D's own cast in raycast()
    // above has already tested. A chunk present but with no shape (meshed
    // with nothing to collide with, or not built yet) reads as no collider,
    // same as one never asked for.
    bool terrain_chunk_has_collider(InstanceId terrain_id, const terrain::ChunkCoord& coord) const {
        const auto found = terrain_bodies.find(terrain_id);
        if (found == terrain_bodies.end()) {
            return false;
        }
        const auto chunk = found->second.chunks.find(coord);
        return chunk != found->second.chunks.end() && b3Shape_IsValid(chunk->second.shape);
    }

    // Task 9: where view's voxel distance field crosses zero along the
    // world ray (origin, direction, up to length), over the chunks
    // PhysicsWorld holds no collider shape for. Updates hits in place when
    // this is nearer than whatever it already holds.
    void march_terrain(DataModel& game, const TerrainView& view, Vec3 origin, Vec3 direction, float length,
                       RayHits& hits) const {
        auto* terrain_instance = dynamic_cast<Terrain*>(game.instance(view.terrain));
        if (terrain_instance == nullptr) {
            return;
        }
        const terrain::VoxelVolume& volume = terrain_instance->volume();
        const terrain::ChunkMap& chunks = volume.chunks();
        terrain::ChunkCoord lo, hi;
        if (!volume.chunk_extent(lo, hi)) {
            return;   // nothing stored: no surface anywhere, nothing to march
        }
        const float voxel_size = static_cast<float>(volume.voxel_size());
        const float span = terrain::kChunkSize * voxel_size;

        // Terrain-local bounds of every stored chunk (chunk_extent: kept by
        // VoxelVolume as chunks come and go, never scanned here), padded by the
        // distance band's width: a stored chunk holds only cells within
        // kBandCells of a surface, so nothing closer than that to the
        // padded box's own faces can be solid -- the march below may start
        // right at the box without first checking what lies further out
        // (always air). Also RM4: clips a long ray down to the island's own
        // size rather than marching empty space all the way to its end.
        const float margin = terrain::kBandCells * voxel_size;
        const Vec3 bounds_min{lo.x * span - margin, lo.y * span - margin, lo.z * span - margin};
        const Vec3 bounds_max{(hi.x + 1) * span + margin, (hi.y + 1) * span + margin, (hi.z + 1) * span + margin};

        // Terrain-local ray: view.transform has no scale or shear (Terrain
        // refuses one), so matrix4_vector keeps direction's length, and a
        // local distance equals the same world distance.
        const Matrix4 inverse = matrix4_inverse(view.transform);
        const Vec3 local_origin = matrix4_point(inverse, origin);
        const Vec3 local_direction = matrix4_vector(inverse, direction);
        const float dir_length = std::sqrt(local_direction.x * local_direction.x +
                                           local_direction.y * local_direction.y +
                                           local_direction.z * local_direction.z);
        if (!(dir_length > 0.f)) {
            return;
        }
        const Vec3 dir_unit{local_direction.x / dir_length, local_direction.y / dir_length,
                            local_direction.z / dir_length};
        const auto at = [&](float t) {
            return Vec3{local_origin.x + dir_unit.x * t, local_origin.y + dir_unit.y * t,
                        local_origin.z + dir_unit.z * t};
        };

        // Clip [0, length] to the padded bounds: a slab test per axis. The
        // far end starts at the nearest hit found so far (Box3D's, or an
        // earlier Terrain's march): nothing past it can win, and a march
        // crossing inside a collided chunk (below) is then always behind
        // Box3D's own hit on that chunk's front face.
        float t0 = 0.f;
        float t1 = length * hits.closest;
        const float origin_axis[3] = {local_origin.x, local_origin.y, local_origin.z};
        const float dir_axis[3] = {dir_unit.x, dir_unit.y, dir_unit.z};
        const float min_axis[3] = {bounds_min.x, bounds_min.y, bounds_min.z};
        const float max_axis[3] = {bounds_max.x, bounds_max.y, bounds_max.z};
        for (int axis = 0; axis < 3; ++axis) {
            if (std::fabs(dir_axis[axis]) < 1e-9f) {
                if (origin_axis[axis] < min_axis[axis] || origin_axis[axis] > max_axis[axis]) {
                    return;
                }
                continue;
            }
            float ta = (min_axis[axis] - origin_axis[axis]) / dir_axis[axis];
            float tb = (max_axis[axis] - origin_axis[axis]) / dir_axis[axis];
            if (ta > tb) {
                std::swap(ta, tb);
            }
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) {
                return;
            }
        }

        terrain::VoxelSampler sampler(chunks, voxel_size);
        const auto chunk_coord_at = [&](Vec3 p) {
            return terrain::ChunkCoord{static_cast<int>(std::floor(p.x / span)),
                                       static_cast<int>(std::floor(p.y / span)),
                                       static_cast<int>(std::floor(p.z / span))};
        };
        // Which chunk's collider holds the triangles at a point is fuzzy
        // within a cell of a chunk face: SurfaceNets gives a chunk the
        // quads around the lattice edges it owns (lower sample 0..31 on
        // each axis), whose vertices sit in cells -1..31 -- so its
        // triangles reach up to a cell below its box's min faces and stop
        // anywhere in the last cell below its max faces. A point is surely
        // inside a collided chunk's triangles only when the chunks at it
        // and at it plus one cell on any mix of axes all have colliders.
        const auto surely_collided = [&](Vec3 p) {
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 q{p.x + ((corner & 1) != 0 ? voxel_size : 0.f),
                             p.y + ((corner & 2) != 0 ? voxel_size : 0.f),
                             p.z + ((corner & 4) != 0 ? voxel_size : 0.f)};
                if (!terrain_chunk_has_collider(view.terrain, chunk_coord_at(q))) {
                    return false;
                }
            }
            return true;
        };
        // The ray's exact exit parameter (a slab test, Terrain-local) from
        // the part of coord's box its own collider surely covers: the box
        // less its last cell below each max face (see surely_collided).
        const auto sure_exit_of = [&](const terrain::ChunkCoord& coord) {
            const int index[3] = {coord.x, coord.y, coord.z};
            float exit = std::numeric_limits<float>::max();
            for (int axis = 0; axis < 3; ++axis) {
                if (dir_axis[axis] > 1e-9f) {
                    exit = std::min(exit,
                                    ((index[axis] + 1) * span - voxel_size - origin_axis[axis]) / dir_axis[axis]);
                } else if (dir_axis[axis] < -1e-9f) {
                    exit = std::min(exit, (index[axis] * span - origin_axis[axis]) / dir_axis[axis]);
                }
            }
            return exit;
        };
        const auto in_sure_part = [&](const terrain::ChunkCoord& coord, Vec3 p) {
            return p.x < (coord.x + 1) * span - voxel_size && p.y < (coord.y + 1) * span - voxel_size &&
                   p.z < (coord.z + 1) * span - voxel_size;   // coord is p's own chunk: its min faces hold
        };

        const float min_step = 0.25f * voxel_size;
        // Past a face by this much, so the next chunk lookup lands beyond it
        // rather than rounding back.
        const float past_face = 1e-3f * voxel_size;
        constexpr int kBisections = 8;
        float t = t0;
        float prev_t = t0;   // always a sample known to be outside (d > 0)
        if (!(sampler.distance(at(t0)) > 0.f)) {
            return;   // already inside (or exactly on) the surface where the march begins
        }
        while (t < t1) {
            const float d = sampler.distance(at(t));
            if (d <= 0.f) {
                // A crossing between prev_t (outside) and t (inside).
                float lo_t = prev_t;
                float hi_t = t;
                for (int i = 0; i < kBisections; ++i) {
                    const float mid = (lo_t + hi_t) * 0.5f;
                    if (sampler.distance(at(mid)) > 0.f) {
                        lo_t = mid;
                    } else {
                        hi_t = mid;
                    }
                }
                const Vec3 local_point = at(hi_t);
                // Within a collider's triangles the crossing is Box3D's: its
                // own hit there, nearer than t1, is in hits already.
                if (surely_collided(local_point)) {
                    return;
                }
                const float world_fraction = hi_t / length;
                if (world_fraction < hits.closest) {
                    const Vec3 world_point = matrix4_point(view.transform, local_point);
                    const Vec3 world_normal = matrix4_vector(view.transform, sampler.gradient(local_point));
                    hits.hit = true;
                    hits.closest = world_fraction;
                    hits.instance = view.terrain;
                    hits.point = to_b3(world_point);
                    hits.normal = to_b3(world_normal);
                    hits.material = sampler.id(local_point);
                }
                return;   // only the nearest crossing in this Terrain matters
            }
            const Vec3 p = at(t);
            const terrain::ChunkCoord coord = chunk_coord_at(p);
            if (in_sure_part(coord, p) && terrain_chunk_has_collider(view.terrain, coord)) {
                // Box3D's own cast already tested this chunk's shape: jump
                // to the ray's exact exit from what that shape surely
                // covers (R28) rather than testing it again. Solid there
                // means the crossing lies inside it, where Box3D's
                // front-face hit is nearer: stop. In the chunk's last cell
                // below a max face, march on as usual.
                const float exit = sure_exit_of(coord) + past_face;
                if (exit > t) {
                    if (exit >= t1) {
                        return;
                    }
                    if (!(sampler.distance(at(exit)) > 0.f)) {
                        return;
                    }
                    prev_t = exit;
                    t = exit;
                    continue;
                }
            }
            prev_t = t;
            t += std::max(d, min_step);
        }
    }

    std::optional<RayHit> raycast(DataModel& game, Vec3 origin, Vec3 direction, const RayFilter& filter) {
        const float length =
            std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        if (!(length > 0.f) || !std::isfinite(length)) {
            return std::nullopt;
        }
        // Box3D asserts the origin is finite (b3IsValidPosition); a script can
        // pass one that is not (0/0, math.huge), so refuse it before that, same
        // as the direction above.
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) {
            return std::nullopt;
        }
        sync(game);
        RayHits hits;
        hits.game = &game;
        hits.filter = &filter;
        const b3QueryFilter rays = ray_filter(*groups, filter.collision_group);
        b3World_CastRay(world, to_b3(origin), to_b3(direction), rays, ray_hit, &hits);
        // Task 9 (spec "Physics"): Box3D's cast above tested every chunk that
        // has a collider shape; march each Terrain's own voxel distance
        // field, in its own local space, over exactly the chunks that do
        // not, so a chunk is tested once either way and a Raycast hits
        // terrain at any distance, the same whether or not colliders are
        // loaded where it hits (RM1). Keeps hits' nearest-so-far. Terrain is
        // in Default: a ray whose group does not collide with it marches
        // through none, as its collider shapes are filtered out.
        const bool sees_terrain =
            filter.collision_group.empty() ||
            (groups->collides_with(filter.collision_group) & groups->category(CollisionGroups::kDefault)) != 0;
        if (terrains != nullptr && sees_terrain) {
            for (const TerrainView& view : terrains->views()) {
                if (!view.can_collide) {
                    continue;   // no shapes at all (RM3): the march sees none either
                }
                if (under_any(game, view.terrain, filter.instances) != filter.include) {
                    continue;   // the filter would have hidden its collider shape too
                }
                march_terrain(game, view, origin, direction, length, hits);
            }
        }
        if (!hits.hit) {
            return std::nullopt;
        }
        RayHit out;
        out.instance = hits.instance;
        out.position = from_b3(hits.point);
        out.normal = from_b3(hits.normal);
        out.distance = hits.closest * length;
        // A Terrain's triangles carry its material Ids (terrain_materials).
        if (terrain_bodies.count(hits.instance) != 0) {
            out.has_material = true;
            out.material = static_cast<std::uint8_t>(hits.material);
        }
        // A Brush reports the face the point lies on, in its own space.
        if (const auto* brush = dynamic_cast<const Brush*>(game.instance(hits.instance))) {
            const Matrix4 inverse = matrix4_inverse(brush->transform());
            const Vec3 scale = scale_for(game, brush->driven_game_object());
            Vec3 local = matrix4_point(inverse, out.position);
            Vec3 tip = matrix4_point(inverse, Vec3{out.position.x + out.normal.x, out.position.y + out.normal.y,
                                                   out.position.z + out.normal.z});
            const brush::DVec3 p{local.x / scale.x, local.y / scale.y, local.z / scale.z};
            const brush::DVec3 n{(tip.x - local.x) * scale.x, (tip.y - local.y) * scale.y, (tip.z - local.z) * scale.z};
            const double tolerance = 1e-3 * std::max(1.0, brush::length(brush->shape().max - brush->shape().min));
            if (const std::optional<std::size_t> face = brush::face_at(brush->shape(), p, brush::normalize(n), tolerance)) {
                out.face = static_cast<int>(*face);
            }
        }
        return out;
    }

    // Workspace.Gravity into the world, when it changed.
    void pull_gravity(const DataModel& game) {
        const auto* workspace = dynamic_cast<const Workspace*>(game.instance(game.scene_service("Workspace")));
        const float next = workspace != nullptr ? -static_cast<float>(workspace->gravity()) : gravity;
        if (next != gravity) {
            gravity = next;
            b3World_SetGravity(world, b3Vec3{0.f, gravity, 0.f});
        }
    }

    // ---- Which PhysicsObjects have bodies ------------------------------

    void reconcile(DataModel& game) {
        ++pass;
        game.physics_bodies(eligible);
        wanted.clear();
        for (const InstanceId target : claimed) {
            claim_by_slot[id_slot(target)] = 0;
        }
        claimed.clear();
        bool shared = false;
        for (InstanceId id : eligible) {
            const auto* object = dynamic_cast<PhysicsBase*>(game.instance(id));
            if (object == nullptr) {
                continue;
            }
            const InstanceId target = object->driven_game_object();
            wanted.emplace_back(id, target);
            if (target != 0 && !claim(target, id)) {
                shared = true;
            }
        }
        if (shared) {
            first_in_tree_order(game);
        }
        if (game.simulation_running()) {
            keep_compounds(game);
        }
        for (const auto& [id, target] : wanted) {
            auto* object = dynamic_cast<PhysicsBase*>(game.instance(id));
            if (target != 0 && claim_by_slot[id_slot(target)] != id) {
                if (!object->warned_shared) {
                    object->warned_shared = true;
                    say(std::string(object->class_name()) + " " + game.name(id) +
                        " has no body: an earlier body already moves " + game.name(target));
                }
                // Stopped, its Transform still follows that GameObject, as a
                // body's does, so a dragger finds it there too.
                if (!game.simulation_running()) {
                    follow_without_body(game, *object, target);
                }
                continue;
            }
            object->warned_shared = false;
            keep(game, *object, target);
        }
        gone.clear();
        for (const auto& [id, body] : bodies) {
            if (body.seen != pass) {
                gone.push_back(id);
            }
        }
        for (InstanceId id : gone) {
            if (game.simulation_running()) {
                if (auto* controller = dynamic_cast<PlayerController*>(game.instance(id))) {
                    controller->store_ground(false, false);
                }
            }
            destroy(id);
        }
    }

    // ---- Terrain collider interest (Task 8) ------------------------------

    // Each dynamic (not Anchored) PhysicsObject or PlayerController's chunks
    // within kColliderChunks, per Terrain, in that Terrain's local chunk
    // space -- set_collider_interest's ask -- and, every sync, the chunk
    // under each body and its immediate neighbors built right here
    // (build_colliders_now) unless TerrainWorld already knows their collider
    // (no falling through). SimulationThread, under the write lock, after
    // reconcile (so every body's Box3D position is current) and before
    // reconcile_terrain (so a shape exists this very sync for whatever
    // build_colliders_now just built).
    void update_terrain_collider_interest(DataModel& game) {
        if (terrains == nullptr) {
            return;
        }
        for (const TerrainView& view : terrains->views()) {
            // Distinct body-centre chunks: thousands of bodies (a debris
            // pile) mostly share a few chunks.
            std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> centers;
            if (view.can_collide) {
                const terrain::LodTree* tree = terrains->lod_tree(view.terrain);
                const float voxel_size = tree != nullptr ? tree->voxel_size() : 1.f;
                const float span = terrain::kChunkSize * voxel_size;
                const Matrix4 inverse = matrix4_inverse(view.transform);
                for (const auto& [id, body] : bodies) {
                    if (!b3Body_IsValid(body.body)) {
                        continue;
                    }
                    const auto* base = dynamic_cast<const PhysicsBase*>(game.instance(id));
                    if (base == nullptr || anchored_as(body, *base)) {
                        continue;   // anchored bodies need no colliders around them
                    }
                    // An assembly's member asks around its own origin, not its body's.
                    const b3Vec3 origin = body.compound != 0 ? b3Body_GetWorldPoint(body.body, body.place.p)
                                                             : b3Body_GetPosition(body.body);
                    const Vec3 local = matrix4_point(inverse, from_b3(origin));
                    centers.insert(terrain::ChunkCoord{static_cast<int>(std::floor(local.x / span)),
                                                       static_cast<int>(std::floor(local.y / span)),
                                                       static_cast<int>(std::floor(local.z / span))});
                }
            }
            // The interest is every chunk within kColliderChunks of a centre.
            // It changes only where a centre comes or goes, so only those
            // centres' boxes are walked: a count per chunk of the centres
            // that ask for it, and TerrainWorld told just the chunks whose
            // count reached or left zero. Rebuilding the whole interest
            // (343 chunks a centre) whenever any body crossed a chunk border
            // cost milliseconds every substep with a few dozen bodies about.
            Interest& interest = interests[view.terrain];
            if (!interest.counts.empty() && !terrains->has_collider_interest(view.terrain)) {
                interest = Interest{};   // TerrainWorld's record was made again: tell it everything
            }
            std::vector<terrain::ChunkCoord> added;
            std::vector<terrain::ChunkCoord> removed;
            auto box = [&](const terrain::ChunkCoord& center, int delta) {
                for (int dz = -kColliderChunks; dz <= kColliderChunks; ++dz) {
                    for (int dy = -kColliderChunks; dy <= kColliderChunks; ++dy) {
                        for (int dx = -kColliderChunks; dx <= kColliderChunks; ++dx) {
                            const terrain::ChunkCoord c{center.x + dx, center.y + dy, center.z + dz};
                            int& count = interest.counts[c];
                            count += delta;
                            if (delta > 0 && count == 1) {
                                added.push_back(c);
                            } else if (count == 0) {
                                interest.counts.erase(c);
                                removed.push_back(c);
                            }
                        }
                    }
                }
            };
            for (auto it = interest.centers.begin(); it != interest.centers.end();) {
                if (centers.count(*it) == 0) {
                    box(*it, -1);
                    it = interest.centers.erase(it);
                } else {
                    ++it;
                }
            }
            for (const terrain::ChunkCoord& center : centers) {
                if (interest.centers.insert(center).second) {
                    box(center, +1);
                }
            }
            if (!added.empty() || !removed.empty()) {
                terrains->change_collider_interest(view.terrain, added, removed);
            }
            // No fall-through: the chunk under a body and its immediate
            // neighbors (a surface at a chunk border can belong to either
            // side) must hold their colliders before Box3D steps. Each one
            // TerrainWorld already knows (built, or meshed empty) is skipped,
            // so this costs lookups only, unless the job queue has not caught
            // up with the body -- and then a collider elsewhere around it is
            // no cover: checked per chunk, every sync, never "some neighbor
            // has one".
            std::vector<terrain::ChunkCoord> under_body;
            for (const terrain::ChunkCoord& center : centers) {
                under_body.clear();
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            under_body.push_back(terrain::ChunkCoord{center.x + dx, center.y + dy, center.z + dz});
                        }
                    }
                }
                terrains->build_colliders_now(game, view.terrain, under_body);
            }
        }
    }

    // ---- Terrain bodies -------------------------------------------------

    // Each Terrain TerrainWorld shows gets a static body and its chunks'
    // shapes; one it no longer shows loses them. SimulationThread, under the
    // write lock, after reconcile (which counted this pass).
    void reconcile_terrain() {
        if (terrains != nullptr) {
            for (const TerrainView& view : terrains->views()) {
                keep_terrain(view);
            }
        }
        gone.clear();
        for (const auto& [id, record] : terrain_bodies) {
            if (record.seen != pass) {
                gone.push_back(id);
            }
        }
        for (InstanceId id : gone) {
            destroy_terrain(id);
        }
    }

    void keep_terrain(const TerrainView& view) {
        auto [found, created] = terrain_bodies.try_emplace(view.terrain);
        TerrainBody& record = found->second;
        record.seen = pass;
        b3Vec3 position{};
        b3Quat rotation{};
        if (created) {
            b3BodyDef def = b3DefaultBodyDef();
            def.type = b3_staticBody;
            pose_of(view.transform, def.position, def.rotation);
            def.userData = user_data(view.terrain);
            record.body = b3CreateBody(world, &def);
            record.transform = view.transform;
        } else if (!same_matrix4(view.transform, record.transform)) {
            pose_of(view.transform, position, rotation);
            b3Body_SetTransform(record.body, position, rotation);
            record.transform = view.transform;
        }
        if (!created && view.chunks_revision == record.chunks_revision && view.can_collide == record.can_collide) {
            return;
        }
        record.chunks_revision = view.chunks_revision;
        record.can_collide = view.can_collide;
        const auto* colliders = view.can_collide ? terrains->colliders(view.terrain) : nullptr;
        if (colliders != nullptr) {
            for (const TerrainWorld::ChunkCollider& collider : *colliders) {
                keep_chunk(record, view.terrain, collider);
            }
        }
        for (auto chunk = record.chunks.begin(); chunk != record.chunks.end();) {
            if (chunk->second.seen != pass) {
                drop_chunk(chunk->second);
                chunk = record.chunks.erase(chunk);
            } else {
                ++chunk;
            }
        }
    }

    // A shape for collider on the Terrain's body, unless the one there was
    // made from this revision already. The old shape goes first.
    void keep_chunk(TerrainBody& record, InstanceId terrain, const TerrainWorld::ChunkCollider& collider) {
        TerrainChunkShape& chunk = record.chunks[collider.coord];
        chunk.seen = pass;
        if (b3Shape_IsValid(chunk.shape) && chunk.revision == collider.revision) {
            return;
        }
        drop_chunk(chunk);
        const auto* built = static_cast<const TerrainCollider*>(collider.collider.get());
        if (built == nullptr || built->mesh == nullptr) {
            return;
        }
        b3ShapeDef def = b3DefaultShapeDef();
        // Box3D copies the materials into the shape.
        def.materials = const_cast<b3SurfaceMaterial*>(terrain_surfaces.data());
        def.materialCount = kTerrainMaterialCount;
        def.baseMaterial = terrain_surfaces[0];
        def.userData = user_data(terrain);
        def.filter = shape_filter(*groups, CollisionGroups::kDefault, true);
        // A static shape otherwise meets only bodies that move: one resting
        // where a chunk was just made again would sink into it first.
        def.invokeContactCreation = true;
        chunk.shape = b3CreateMeshShape(record.body, &def, built->mesh, b3Vec3{1.f, 1.f, 1.f});
        chunk.revision = collider.revision;
        chunk.collider = collider.collider;
    }

    // Destroys the chunk's shape, waking what touched it, and only then
    // lets go of the mesh it pointed into.
    void drop_chunk(TerrainChunkShape& chunk) {
        if (b3Shape_IsValid(chunk.shape)) {
            forget_shape(chunk.shape);
            b3DestroyShape(chunk.shape, false);
        }
        chunk.shape = b3_nullShapeId;
        chunk.collider.reset();
        chunk.revision = 0;
    }

    void destroy_terrain(InstanceId id) {
        const auto found = terrain_bodies.find(id);
        if (found == terrain_bodies.end()) {
            return;
        }
        for (auto& [coord, chunk] : found->second.chunks) {
            drop_chunk(chunk);
        }
        if (b3Body_IsValid(found->second.body)) {
            b3DestroyBody(found->second.body);
        }
        terrain_bodies.erase(found);
    }

    // The first body to claim target this sync holds it. False when one did.
    bool claim(InstanceId target, InstanceId id) {
        InstanceId& holder = claim_by_slot[id_slot(target)];
        if (holder != 0) {
            return false;
        }
        holder = id;
        claimed.push_back(target);
        return true;
    }

    // Each claimed GameObject goes to the first of its claimants in a
    // depth-first walk of the tree (claim_winner, which Assemblies shares).
    // Only runs when two share one.
    void first_in_tree_order(DataModel& game) {
        for (const InstanceId target : claimed) {
            InstanceId& winner = claim_by_slot[id_slot(target)];
            claimants.clear();
            for (const auto& [id, driven] : wanted) {
                if (driven == target) {
                    claimants.push_back(id);
                }
            }
            if (claimants.size() > 1) {
                winner = claim_winner(game, claimants.data(), claimants.size());
            }
        }
    }

    // object's Transform at target's position and rotation (upright for a
    // PlayerController), keeping its own scale, stored as a physics move is.
    static void follow_without_body(DataModel& game, PhysicsBase& object, InstanceId target) {
        const GameObject* driven = game.game_object(target);
        if (driven == nullptr) {
            return;
        }
        const bool controller = enabled_controller(&object) != nullptr;
        b3Vec3 position{};
        b3Quat rotation{};
        pose_of(controller ? upright_transform(driven->transform()) : driven->transform(), position, rotation);
        const Matrix4 followed = matrix_of(position, rotation, object.transform());
        if (!same_matrix4(followed, object.transform())) {
            object.store_simulated(followed, object.velocity());
        }
    }

    void destroy(InstanceId id) {
        const auto found = bodies.find(id);
        if (found == bodies.end()) {
            return;
        }
        drop_shape(found->second);
        // A compound's body is the compound's; only this member's shapes go.
        if (found->second.compound == 0 && b3Body_IsValid(found->second.body)) {
            b3DestroyBody(found->second.body);
        }
        bodies.erase(found);
    }

    // ---- Assemblies: one body per play assembly with physics --------------

    static b3Transform transform_of(const Matrix4& m) {
        b3Transform out = b3Transform_identity;
        pose_of(m, out.p, out.q);
        return out;
    }

    // Every play assembly with physics has its body; one whose assembly is
    // gone goes, with its members' records.
    void keep_compounds(DataModel& game) {
        const std::vector<Assemblies::PlayAssembly>& plays = game.assemblies().play();
        for (std::size_t index = 0; index < plays.size(); ++index) {
            if (!plays[index].has_physics) {
                continue;
            }
            auto found = compounds.find(plays[index].stamp);
            if (found == compounds.end() && plays[index].reuse != 0) {
                found = adopt_compound(game, index);
            }
            if (found == compounds.end()) {
                found = make_compound(game, index);
            }
            if (found != compounds.end()) {
                found->second.seen = pass;
                refresh_compound(game, index, found->second);
            }
        }
        apply_assembly_writes(game);
        compounds_gone.clear();
        for (const auto& [stamp, compound] : compounds) {
            if (compound.seen != pass) {
                compounds_gone.push_back(stamp);
            }
        }
        for (const std::uint32_t stamp : compounds_gone) {
            hand_off(game, stamp);
            drop_compound(stamp);
        }
    }

    // How a body moves now, kept before its members go to other bodies.
    void note_motion(b3BodyId body) {
        if (!b3Body_IsValid(body)) {
            return;
        }
        for (const Motion& motion : motions) {
            if (B3_ID_EQUALS(motion.body, body)) {
                return;
            }
        }
        Motion motion;
        motion.body = body;
        motion.mass = b3Body_GetType(body) == b3_dynamicBody ? b3Body_GetMass(body) : 0.f;
        motion.velocity = b3Body_GetLinearVelocity(body);
        motion.angular = b3Body_GetAngularVelocity(body);
        motion.center = b3Body_GetWorldCenter(body);
        motions.push_back(motion);
    }

    // The motion a body made of the noted ones starts with: one body's own,
    // as its velocity was at center; several bodies' momentum.
    void inherit_motion(b3BodyId body) {
        if (motions.empty() || b3Body_GetType(body) != b3_dynamicBody) {
            return;
        }
        b3Vec3 velocity{};
        b3Vec3 angular{};
        if (motions.size() == 1) {
            const Motion& only = motions.front();
            const b3Vec3 center = b3Body_GetWorldCenter(body);
            const b3Vec3 r{center.x - only.center.x, center.y - only.center.y, center.z - only.center.z};
            velocity = b3Add(only.velocity, b3Cross(only.angular, r));
            angular = only.angular;
        } else {
            float total = 0.f;
            for (const Motion& motion : motions) {
                total += motion.mass;
                velocity = b3MulAdd(velocity, motion.mass, motion.velocity);
                angular = b3MulAdd(angular, motion.mass, motion.angular);
            }
            if (total > 0.f) {
                velocity = b3MulSV(1.f / total, velocity);
                angular = b3MulSV(1.f / total, angular);
            }
        }
        b3Body_SetLinearVelocity(body, velocity);
        b3Body_SetAngularVelocity(body, angular);
    }

    // A member leaving a body for a body of its own keeps the motion its old
    // body gave it there: the velocity at its origin, and the same spin.
    static void keep_motion(PhysicsBase& object, const Body& record) {
        const b3Vec3 at = b3Body_GetWorldPoint(record.body, record.place.p);
        object.store_simulated(object.transform(), from_b3(b3Body_GetWorldPointVelocity(record.body, at)));
        const Vec3 spin = from_b3(b3Body_GetAngularVelocity(record.body));
        if (auto* rigid = dynamic_cast<PhysicsObject*>(&object)) {
            rigid->store_angular_velocity(spin);
        } else if (auto* brush = dynamic_cast<Brush*>(&object)) {
            brush->store_angular_velocity(spin);
        }
    }

    // The members of a compound about to go that are in no assembly now keep
    // its motion for the bodies of their own they get next.
    void hand_off(DataModel& game, std::uint32_t stamp) {
        const auto found = compounds.find(stamp);
        if (found == compounds.end() || !b3Body_IsValid(found->second.body)) {
            return;
        }
        for (const auto& [id, record] : bodies) {
            if (record.compound != stamp || game.assemblies().play_of(id) >= 0) {
                continue;
            }
            if (auto* object = dynamic_cast<PhysicsBase*>(game.instance(id))) {
                keep_motion(*object, record);
            }
        }
    }

    // A split or a merge in play: the body the old root was on carries on,
    // its departing members' shapes go, arriving members' shapes come at
    // their offsets, and its motion takes in theirs.
    std::unordered_map<std::uint32_t, Compound>::iterator adopt_compound(DataModel& game, std::size_t index) {
        Assemblies& assemblies = game.assemblies();
        const Assemblies::PlayAssembly assembly = assemblies.play()[index];
        const auto old = compounds.find(assembly.reuse);
        if (old == compounds.end() || !b3Body_IsValid(old->second.body)) {
            return compounds.end();
        }
        Compound compound = old->second;
        compounds.erase(old);
        compound.root = assembly.root;
        compound.seen = pass;
        b3Body_SetUserData(compound.body, user_data(assembly.root));
        motions.clear();
        note_motion(compound.body);
        // Departing: on this body, in another assembly or none now.
        members_scratch.clear();
        for (const auto& [id, record] : bodies) {
            if (record.compound == assembly.reuse && assemblies.play_of(id) != static_cast<int>(index)) {
                members_scratch.push_back(id);
            }
        }
        for (const InstanceId id : members_scratch) {
            if (auto* object = dynamic_cast<PhysicsBase*>(game.instance(id))) {
                keep_motion(*object, bodies[id]);
            }
            destroy(id);
        }
        bool arrived = false;
        const std::vector<Assemblies::PlayMember>& members = assemblies.play_members();
        for (std::uint32_t i = assembly.first; i < assembly.first + assembly.count; ++i) {
            const Assemblies::PlayMember& member = members[i];
            auto* object = dynamic_cast<PhysicsBase*>(game.instance(member.node));
            if (object == nullptr) {
                continue;
            }
            const auto found = bodies.find(member.node);
            if (found != bodies.end() && found->second.compound == assembly.reuse) {
                // Staying: its shapes are where they were. A new root changes
                // which members ride massless, so their densities are set again.
                Body& record = found->second;
                record.compound = assembly.stamp;
                if (record.compound_root != assembly.root) {
                    record.compound_root = assembly.root;
                    set_densities(*object, record);
                }
                continue;
            }
            if (found != bodies.end()) {
                note_motion(found->second.body);
            }
            arrived = true;
            destroy(member.node);
            Body record;
            record.driven = member.driven;
            record.body = compound.body;
            record.compound = assembly.stamp;
            record.place = transform_of(member.offset);
            record.compound_anchored = compound.anchored;
            record.compound_root = assembly.root;
            record.member_revision = member.revision;
            record.seen = pass;
            object->take_dirty();
            make_shape(game, *object, record);
            bodies.insert_or_assign(member.node, std::move(record));
        }
        b3Body_ApplyMassFromShapes(compound.body);
        if (arrived) {
            inherit_motion(compound.body);
        }
        motions.clear();
        return compounds.emplace(assembly.stamp, compound).first;
    }

    // What changed on a body that stays: its anchoring (a Custom member is a
    // mesh only on a static body, so it is made again around the switch, and
    // a body that starts moving starts at rest), and the members whose
    // offsets moved in place, whose shapes alone are placed again.
    void refresh_compound(DataModel& game, std::size_t index, Compound& compound) {
        const Assemblies& assemblies = game.assemblies();
        const Assemblies::PlayAssembly& assembly = assemblies.play()[index];
        const std::vector<Assemblies::PlayMember>& members = assemblies.play_members();
        if (compound.anchored != assembly.anchored) {
            members_scratch.clear();
            for (std::uint32_t i = assembly.first; i < assembly.first + assembly.count; ++i) {
                const auto found = bodies.find(members[i].node);
                if (found == bodies.end() || found->second.compound != assembly.stamp) {
                    continue;
                }
                found->second.compound_anchored = assembly.anchored;
                const auto* rigid = dynamic_cast<const PhysicsObject*>(game.instance(members[i].node));
                if (rigid != nullptr && rigid->shape() == PhysicsObject::Shape::Custom) {
                    drop_shape(found->second);
                    members_scratch.push_back(members[i].node);
                }
            }
            b3Body_SetType(compound.body, assembly.anchored ? b3_staticBody : b3_dynamicBody);
            for (const InstanceId id : members_scratch) {
                if (auto* object = dynamic_cast<PhysicsBase*>(game.instance(id))) {
                    make_shape(game, *object, bodies[id]);
                }
            }
            if (!assembly.anchored) {
                b3Body_SetLinearVelocity(compound.body, b3Vec3{});
                b3Body_SetAngularVelocity(compound.body, b3Vec3{});
                b3Body_SetAwake(compound.body, true);
            }
            compound.anchored = assembly.anchored;
        }
        if (compound.root != assembly.root) {
            // A new root (one made Massless, or weighing more now): which
            // members ride massless changes, so their densities are set again.
            compound.root = assembly.root;
            b3Body_SetUserData(compound.body, user_data(assembly.root));
            for (std::uint32_t i = assembly.first; i < assembly.first + assembly.count; ++i) {
                const auto found = bodies.find(members[i].node);
                const auto* object = dynamic_cast<const PhysicsBase*>(game.instance(members[i].node));
                if (found == bodies.end() || found->second.compound != assembly.stamp || object == nullptr) {
                    continue;
                }
                found->second.compound_root = assembly.root;
                set_densities(*object, found->second);
            }
            b3Body_ApplyMassFromShapes(compound.body);
            if (!compound.anchored) {
                b3Body_SetAwake(compound.body, true);
            }
        }
        for (std::uint32_t i = assembly.first; i < assembly.first + assembly.count; ++i) {
            const Assemblies::PlayMember& member = members[i];
            const auto found = bodies.find(member.node);
            if (found == bodies.end() || found->second.compound != assembly.stamp ||
                found->second.member_revision == member.revision) {
                continue;
            }
            auto* object = dynamic_cast<PhysicsBase*>(game.instance(member.node));
            if (object == nullptr) {
                continue;
            }
            found->second.place = transform_of(member.offset);
            found->second.member_revision = member.revision;
            make_shape(game, *object, found->second);
            if (!compound.anchored) {
                b3Body_SetAwake(compound.body, true);
            }
        }
    }

    // What scripts and Properties wrote to members since the last sync: a move
    // of the whole assembly, and its velocities, centre of mass's, applied
    // after the shapes put the centre of mass. An upward write to a
    // PlayerController root's assembly is a jump, whichever member took it.
    void apply_assembly_writes(DataModel& game) {
        std::vector<Assemblies::PlayAssembly>& plays = game.assemblies().play();
        for (Assemblies::PlayAssembly& assembly : plays) {
            const auto found = compounds.find(assembly.stamp);
            const bool body = found != compounds.end() && b3Body_IsValid(found->second.body);
            if (assembly.teleport) {
                assembly.teleport = false;
                if (body) {
                    b3Vec3 position{};
                    b3Quat rotation{};
                    pose_of(assembly.frame, position, rotation);
                    b3Body_SetTransform(found->second.body, position, rotation);
                    if (!found->second.anchored) {
                        b3Body_SetAwake(found->second.body, true);
                    }
                }
            }
            if (!assembly.velocity_pending && !assembly.angular_pending) {
                continue;
            }
            if (body && !found->second.anchored) {
                const b3BodyId id = found->second.body;
                if (assembly.velocity_pending) {
                    const auto root = bodies.find(assembly.root);
                    if (root != bodies.end() && root->second.controller &&
                        assembly.velocity.y - b3Body_GetLinearVelocity(id).y > kRisingSpeed) {
                        root->second.launched = true;
                    }
                    b3Body_SetLinearVelocity(id, to_b3(assembly.velocity));
                }
                if (assembly.angular_pending) {
                    b3Body_SetAngularVelocity(id, to_b3(assembly.angular));
                }
                b3Body_SetAwake(id, true);
            }
            assembly.velocity_pending = false;
            assembly.angular_pending = false;
        }
    }

    void drop_compound(std::uint32_t stamp) {
        const auto found = compounds.find(stamp);
        if (found == compounds.end()) {
            return;
        }
        for (auto& [id, record] : bodies) {
            if (record.compound == stamp) {
                forget_shapes(record);
            }
        }
        // The body takes every member's shapes with it; only then do the
        // meshes they point into go.
        if (b3Body_IsValid(found->second.body)) {
            b3DestroyBody(found->second.body);
        }
        for (auto it = bodies.begin(); it != bodies.end();) {
            if (it->second.compound == stamp) {
                if (it->second.mesh != nullptr) {
                    b3DestroyMesh(it->second.mesh);
                }
                it = bodies.erase(it);
            } else {
                ++it;
            }
        }
        compounds.erase(found);
    }

    // One body at the assembly's frame, static when any member is anchored,
    // carrying every physics member's shapes at its offset, each with its own
    // density; it moves at its root's authored velocity, as its center of mass.
    std::unordered_map<std::uint32_t, Compound>::iterator make_compound(DataModel& game, std::size_t index) {
        Assemblies& assemblies = game.assemblies();
        const Assemblies::PlayAssembly assembly = assemblies.play()[index];
        auto* root = dynamic_cast<PhysicsBase*>(game.instance(assembly.root));
        if (root == nullptr) {
            return compounds.end();
        }
        b3BodyDef def = b3DefaultBodyDef();
        if (enabled_controller(root) != nullptr) {
            def.motionLocks.angularX = true;
            def.motionLocks.angularY = true;
            def.motionLocks.angularZ = true;
            def.enableSleep = false;
        }
        def.type = assembly.anchored ? b3_staticBody : b3_dynamicBody;
        pose_of(assembly.frame, def.position, def.rotation);
        def.linearDamping = static_cast<float>(root->linear_damping());
        Vec3 angular{};
        if (const auto* rigid = dynamic_cast<const PhysicsObject*>(root)) {
            def.angularDamping = static_cast<float>(rigid->angular_damping());
            angular = rigid->angular_velocity();
        } else if (const auto* brush = dynamic_cast<const Brush*>(root)) {
            def.angularDamping = static_cast<float>(brush->angular_damping());
            angular = brush->angular_velocity();
        }
        def.userData = user_data(assembly.root);
        Compound compound;
        compound.body = b3CreateBody(world, &def);
        compound.root = assembly.root;
        compound.anchored = assembly.anchored;
        compound.seen = pass;
        motions.clear();
        const std::vector<Assemblies::PlayMember>& members = assemblies.play_members();
        for (std::uint32_t i = assembly.first; i < assembly.first + assembly.count; ++i) {
            const Assemblies::PlayMember& member = members[i];
            auto* object = dynamic_cast<PhysicsBase*>(game.instance(member.node));
            if (object == nullptr) {
                continue;
            }
            if (const auto found = bodies.find(member.node); found != bodies.end()) {
                note_motion(found->second.body);
            }
            destroy(member.node);
            Body record;
            record.member_revision = member.revision;
            record.driven = member.driven;
            record.body = compound.body;
            record.compound = assembly.stamp;
            record.place = transform_of(member.offset);
            record.compound_anchored = assembly.anchored;
            record.compound_root = assembly.root;
            record.seen = pass;
            object->take_dirty();
            make_shape(game, *object, record);
            bodies.insert_or_assign(member.node, std::move(record));
        }
        Vec3 velocity = root->velocity();
        if (!assembly.anchored) {
            // Centre-of-mass velocities, now that the shapes put the centre of
            // mass: at play start the root's authored ones, later the motion
            // of the bodies its members came from.
            if (assembly.captured) {
                b3Body_SetLinearVelocity(compound.body, to_b3(velocity));
                b3Body_SetAngularVelocity(compound.body, to_b3(angular));
            } else {
                inherit_motion(compound.body);
                velocity = from_b3(b3Body_GetLinearVelocity(compound.body));
                angular = from_b3(b3Body_GetAngularVelocity(compound.body));
            }
        }
        motions.clear();
        const auto made = compounds.emplace(assembly.stamp, compound).first;
        // A static body reports no move, so its members are put in place now.
        assemblies.pose_members(game, index, matrix4_orthonormalize(assembly.frame), velocity, angular);
        return made;
    }

    // A member's own edits: its shape, material, and mass. The rest belong to
    // the assembly (Tasks 7 and 8).
    void keep_member(DataModel& game, PhysicsBase& object, Body& record) {
        record.seen = pass;
        const std::uint32_t dirty = object.take_dirty();
        if ((dirty & PhysicsObject::kDirtyShape) != 0) {
            make_shape(game, object, record);
            return;
        }
        if ((dirty & PhysicsObject::kDirtyFilter) != 0) {
            set_filters(object, record);
        }
        if ((dirty & PhysicsObject::kDirtyMaterial) != 0) {
            double friction = -1.0;
            double bounciness = 0.0;
            if (const auto* rigid = dynamic_cast<const PhysicsObject*>(&object)) {
                friction = rigid->friction();
                bounciness = rigid->bounciness();
            } else if (const auto* brush = dynamic_cast<const Brush*>(&object)) {
                friction = brush->friction();
                bounciness = brush->bounciness();
            }
            if (friction >= 0.0) {
                for (const b3ShapeId shape : record.shapes) {
                    b3Shape_SetFriction(shape, static_cast<float>(friction));
                    b3Shape_SetRestitution(shape, static_cast<float>(bounciness));
                }
            }
        }
        if ((dirty & PhysicsObject::kDirtyMass) != 0) {
            set_densities(object, record);
            b3Body_ApplyMassFromShapes(record.body);
        }
    }

    b3Filter filter_of(const PhysicsBase& object) const {
        return shape_filter(*groups, object.collision_group(), object.can_collide());
    }

    // CanCollide or CollisionGroup changed: the shapes stay; only what they
    // touch changes. Contacts are made again, so a pair that stopped
    // colliding lets go at once.
    void set_filters(const PhysicsBase& object, const Body& record) {
        const b3Filter filter = filter_of(object);
        for (const b3ShapeId shape : record.shapes) {
            b3Shape_SetFilter(shape, filter, true);
        }
    }

    // The groups changed: every body's shapes and every terrain chunk.
    void refilter_all(DataModel& game) {
        // Each group's bits once per pass, not a scan of the group names per body.
        refilter_bits.clear();
        for (auto& [id, record] : bodies) {
            const auto* object = dynamic_cast<const PhysicsBase*>(game.instance(id));
            if (object == nullptr) {
                continue;
            }
            const std::string& group = object->collision_group();
            const auto cached = std::find_if(refilter_bits.begin(), refilter_bits.end(),
                                             [&](const auto& bits) { return *bits.name == group; });
            const GroupBits bits = cached != refilter_bits.end()
                                       ? *cached
                                       : GroupBits{&group, groups->category(group), groups->collides_with(group)};
            if (cached == refilter_bits.end()) {
                refilter_bits.push_back(bits);
            }
            b3Filter filter = b3DefaultFilter();
            filter.categoryBits = bits.category;
            filter.maskBits = object->can_collide()
                                  ? bits.mask | CollisionGroups::kGroundBit | CollisionGroups::kRayBit
                                  : bits.mask | CollisionGroups::kRayBit;
            for (const b3ShapeId shape : record.shapes) {
                b3Shape_SetFilter(shape, filter, true);
            }
        }
        const b3Filter terrain = shape_filter(*groups, CollisionGroups::kDefault, true);
        for (auto& [id, body] : terrain_bodies) {
            for (auto& [coord, chunk] : body.chunks) {
                if (b3Shape_IsValid(chunk.shape)) {
                    b3Shape_SetFilter(chunk.shape, terrain, true);
                }
            }
        }
    }

    // A hull placed where the record's shapes sit: as made for a body of its
    // own, or at its offset in an assembly's body.
    static b3ShapeId create_hull(const Body& record, const b3ShapeDef& def, const b3HullData* hull) {
        if (record.compound == 0) {
            return b3CreateHullShape(record.body, &def, hull);
        }
        return b3CreateTransformedHullShape(record.body, &def, hull, record.place, b3Vec3{1.f, 1.f, 1.f});
    }

    static b3Vec3 placed_point(const Body& record, b3Vec3 point) {
        return record.compound == 0 ? point : b3TransformPoint(record.place, point);
    }

    // Whether the body a record's shapes are on is static: the assembly's
    // anchoring for a member, else the instance's own.
    static bool anchored_as(const Body& record, const PhysicsBase& object) {
        return record.compound != 0 ? record.compound_anchored : object.anchored();
    }

    void keep(DataModel& game, PhysicsBase& object, InstanceId target) {
        auto found = bodies.find(object.id());
        if (found != bodies.end() && found->second.compound != 0) {
            keep_member(game, object, found->second);
            return;
        }
        if (found != bodies.end() && found->second.driven != target) {
            destroy(object.id());
            found = bodies.end();
        }
        if (found == bodies.end()) {
            found = create(game, object, target);
        } else {
            push(game, object, found->second);
        }
        Body& body = found->second;
        body.seen = pass;
        recenter(game, object, body, follow_driven(game, object, body));
        if (body.made_without_pieces && !anchored_as(body, object)) {
            const auto* rigid = dynamic_cast<PhysicsObject*>(&object);
            const auto* mesh = rigid != nullptr ? dynamic_cast<const Mesh*>(game.instance(rigid->mesh_id())) : nullptr;
            if (game.simulation_running() || (mesh != nullptr && mesh->file_stamp() != body.pieces_stamp)) {
                make_shape(game, object, body);
            }
        }
    }

    // A GameObject with another Prefab, or one moved by someone else (moved),
    // may center the shape elsewhere: it is made again there. One scaled
    // another way, by its Transform or its Scale, makes it again at that size.
    void recenter(DataModel& game, PhysicsBase& object, Body& record, bool moved) {
        if (dynamic_cast<const PlayerController*>(&object) != nullptr) {
            return;
        }
        const GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr;
        if (driven == nullptr) {
            return;
        }
        const bool rescaled = !near_vec3(scale_for(game, record.driven), record.scale);
        if (!moved && !rescaled && driven->prefab_guid() == record.prefab) {
            return;
        }
        record.prefab = driven->prefab_guid();
        if (rescaled || !near_vec3(center_for(game, record.driven), record.center)) {
            make_shape(game, object, record);
        }
    }

    // What a PlayerController's probe found under it.
    struct Ground {
        bool hit = false;
        // How far below the cylinder's bottom the ground is: hover_gap() at rest.
        float gap = 0.f;
        b3Vec3 normal{0.f, 1.f, 0.f};
        b3Vec3 point{};
        b3BodyId body = b3_nullBodyId;
        // The shape it stands on (Touched: a controller hovers, never touching it).
        b3ShapeId shape = b3_nullShapeId;
        // The ground's own velocity at point.
        b3Vec3 velocity{};
        // Ground too steep under the ring's edge, beside what it stands on:
        // a wall to it, that ground's normal, and how far below the cylinder
        // it is there.
        bool walled = false;
        b3Vec3 wall{};
        float wall_gap = 0.f;
    };

    // The slope of the face a probe's hit is on. Where the ring meets an
    // edge, as of a step or a ramp's side, the cast's normal leans between
    // the faces there, steep as a wall though both are level. A ray down just
    // past the edge, away from where the normal leans, finds the top face.
    static b3Vec3 face_normal(b3ShapeId shape, b3Vec3 point, b3Vec3 normal, float top) {
        const float level = std::sqrt(normal.x * normal.x + normal.z * normal.z);
        if (level < 1e-3f) {
            return normal;
        }
        const b3Vec3 origin{point.x - kFaceNudge * normal.x / level, top, point.z - kFaceNudge * normal.z / level};
        const b3WorldCastOutput face =
            b3Shape_RayCast(shape, origin, b3Vec3{0.f, point.y - top - kFaceNudge, 0.f});
        return face.hit && face.fraction > 0.f ? face.normal : normal;
    }

    Ground probe(const PlayerController& controller, const Body& record) {
        // One that cannot collide stands on nothing: it falls through.
        if (!controller.can_collide()) {
            return Ground{};
        }
        const float radius = static_cast<float>(controller.radius());
        const float gap = static_cast<float>(controller.hover_gap());
        const b3Vec3 feet = b3Body_GetPosition(record.body);
        const float start = gap + kProbeSkin;
        const float reach = start + std::max(gap, kSnapMin);
        const b3Vec3 origin{feet.x, feet.y + start, feet.z};
        const b3QueryFilter underfoot = ground_filter(*groups, controller.collision_group());
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
            b3World_CastShape(world, origin, &proxy, b3Vec3{0.f, -reach, 0.f}, underfoot, probe_hit, &hits);
            if (hits.started_inside) {
                continue;
            }
            Ground ground;
            if (!hits.hit) {
                return ground;
            }
            ground.hit = true;
            // Box3D stops a cast B3_LINEAR_SLOP short of what it hits.
            ground.gap = hits.closest * reach - kProbeSkin + B3_LINEAR_SLOP;
            ground.normal = face_normal(hits.shape, hits.point, hits.normal, origin.y);
            ground.point = hits.point;
            ground.body = b3Shape_GetBody(hits.shape);
            ground.shape = hits.shape;
            ground.velocity = b3Body_GetWorldPointVelocity(ground.body, hits.point);
            if (ground.normal.y < steepest(controller)) {
                // The ring's edge is on ground too steep, as at the foot of a
                // steep slope. If it stands on ground it can under its middle,
                // it stands there, and the slope is a wall to it. Its middle
                // looks a hover gap further, for it may have hovered on the
                // slope that much above that ground.
                const float deeper = reach + gap;
                ProbeHits middle;
                middle.self = record.body;
                b3World_CastRay(world, origin, b3Vec3{0.f, -deeper, 0.f}, underfoot, probe_hit, &middle);
                if (middle.hit && !middle.started_inside && middle.normal.y >= steepest(controller)) {
                    ground.walled = true;
                    ground.wall = ground.normal;
                    ground.wall_gap = ground.gap;
                    ground.gap = middle.closest * deeper - kProbeSkin;
                    ground.normal = middle.normal;
                    ground.point = middle.point;
                    ground.body = b3Shape_GetBody(middle.shape);
                    ground.shape = middle.shape;
                    ground.velocity = b3Body_GetWorldPointVelocity(ground.body, middle.point);
                }
            }
            return ground;
        }
        return Ground{};
    }

    // The least a normal points up on ground a controller can stand on.
    static float steepest(const PlayerController& controller) {
        return std::cos(static_cast<float>(controller.max_slope()) * kPi / 180.f);
    }

    // ---- PlayerControllers ---------------------------------------------

    // Before Box3D steps: each controller probes for ground, says whether it
    // is on it or sliding, hovers hover_gap() above it either way, and on
    // ground slows across it by Friction. What
    // it does to its own velocity, it does the opposite of to a dynamic ground.
    void control(DataModel& game, double dt) {
        const float step = static_cast<float>(dt);
        for (auto& [id, record] : bodies) {
            if (!record.controller) {
                continue;
            }
            // Only an assembly's root controller steers it; another stands on nothing.
            if (record.compound != 0 && record.compound_root != id) {
                if (auto* rider = dynamic_cast<PlayerController*>(game.instance(id))) {
                    rider->store_ground(false, false);
                }
                continue;
            }
            auto* controller = dynamic_cast<PlayerController*>(game.instance(id));
            if (controller == nullptr || controller->anchored()) {
                if (controller != nullptr) {
                    controller->store_ground(false, false);
                }
                continue;
            }
            Ground ground = probe(*controller, record);
            const b3Vec3 before = b3Body_GetLinearVelocity(record.body);
            b3Vec3 velocity = before;
            stop_at_walls(record, steepest(*controller), velocity);
            const float gap = static_cast<float>(controller->hover_gap());
            const float into_wall = b3Dot(b3Sub(velocity, ground.velocity), ground.wall);
            if (ground.walled && ground.wall_gap < gap + kProbeSkin &&
                (into_wall < 0.f || (controller->is_sliding() && into_wall <= kRisingSpeed))) {
                // Moving into the steep slope it stands at the foot of, it
                // goes onto it, as onto any steep ground; on it already, it
                // stays while it does not leave it.
                ground.walled = false;
                ground.normal = ground.wall;
                ground.gap = ground.wall_gap;
            }
            const float upward = velocity.y - ground.velocity.y;
            if (!ground.hit || upward <= 0.f) {
                record.launched = false;
            }
            // How fast it rose last step following its slope, and past that,
            // by its own: the hover's speed.
            const float climbed = record.climb;
            const float own = upward - climbed;
            // Rising: leaving the ground faster than it, by a jump or by itself
            // going up, not by the ground dropping away under it, nor by its
            // own climb up a slope running out at the top.
            const bool rising =
                ground.hit && upward > kRisingSpeed &&
                (record.launched ||
                 (ground.gap > gap + kProbeSkin && own > kRisingSpeed && velocity.y > kRisingSpeed));
            const b3Vec3 normal = ground.normal;
            const float slope = std::acos(std::min(std::max(normal.y, -1.f), 1.f)) * 180.f / kPi;
            const bool steep = slope > static_cast<float>(controller->max_slope());
            // A controller in the air lands only on the step it reaches its
            // hover height; one already on ground keeps to it as it drops away.
            // One that surfed lands as from the air: it may be above the
            // floor at the slope's foot, going up the slope.
            const float falls = std::max(-upward, 0.f) * step;
            const bool reaches =
                (record.grounded && !controller->is_sliding()) || ground.gap <= gap + falls + kProbeSkin;
            // On ground too steep it surfs, as in Quake: it is on it while
            // within its hover gap and not leaving it, rising or not, and
            // keeps all of its speed along it.
            const bool surfing = ground.hit && steep && normal.y > kWallNormal &&
                                 ground.gap <= gap + falls + kProbeSkin &&
                                 b3Dot(b3Sub(velocity, ground.velocity), normal) <= kRisingSpeed;
            // On ground it stands; on ground too steep it slides. Either way it
            // hovers its gap above it. Near a wall it only falls.
            const bool touching =
                surfing || (ground.hit && !steep && !rising && reaches && normal.y > kWallNormal);
            const bool on_ground = touching && !steep;
            const bool was_grounded = record.grounded;
            record.grounded = touching;
            if (touching && b3Shape_IsValid(ground.shape)) {
                const b3Vec3 moving = b3Body_GetLinearVelocity(record.body);
                record.stands_on = ground.shape;
                record.stand_point = ground.point;
                record.stand_normal = ground.normal;
                record.stand_velocity = b3Sub(ground.velocity, moving);
            } else {
                record.stands_on = b3_nullShapeId;
            }
            controller->store_ground(on_ground, touching && steep);
            if (!touching) {
                hold_off_steep(*controller, record, ground, gap, velocity);
                record.climb = 0.f;
                record.planned = velocity.y + gravity * step;
                if (!same_velocity(velocity, before)) {
                    b3Body_SetLinearVelocity(record.body, velocity);
                }
                continue;
            }
            const float weight = -gravity * step;
            if (steep) {
                surf(record, ground, gap, weight, velocity);
            } else {
                if (ground.walled) {
                    // Come down beside the steep slope, as at the foot of one
                    // it slid, it may be part way into it already.
                    hold_off_steep(*controller, record, ground, gap, velocity);
                }
                // Its speed across the ground, relative to the ground's own.
                // Friction, across the ground only: it decays by exp(-Friction dt).
                const float keep = std::exp(-static_cast<float>(controller->friction()) * step);
                const float across_x = (velocity.x - ground.velocity.x) * keep;
                const float across_z = (velocity.z - ground.velocity.z) * keep;
                velocity.x = ground.velocity.x + across_x;
                velocity.z = ground.velocity.z + across_z;
                // Down no faster this step than gravity would take it.
                const float fastest = upward - weight;
                // Moving across a slope, it rises and falls with it, so the
                // hover has only edges to make up, not the slope.
                const float climb = -(normal.x * across_x + normal.z * across_z) / normal.y;
                record.climb = climb;
                const float follow = ground.velocity.y + climb;
                // Landing, above its hover height, it goes there at once. Below
                // it by no more than its own way down took it last step, as
                // where a slope it slid meets a floor or a step down ends, it
                // goes back up at once too: that is its own overshoot, not an
                // edge.
                const float short_by = gap - ground.gap;
                const float came_down = (std::max(-climbed, 0.f) + std::max(-own, 0.f)) * step;
                float lift = 0.f;
                bool held = false;
                if (short_by < 0.f) {
                    if (!was_grounded) {
                        lift = short_by;
                        held = true;
                    }
                } else {
                    lift = std::min(short_by, came_down);
                    held = short_by <= came_down;
                }
                if (lift != 0.f) {
                    b3Vec3 position = b3Body_GetPosition(record.body);
                    position.y += lift;
                    b3Body_SetTransform(record.body, position, b3Body_GetRotation(record.body));
                }
                if (held) {
                    // At its hover height: it moves up and down with the ground.
                    velocity.y = follow + weight;
                } else {
                    // Off it, as when an edge passes under it or it steps down
                    // off one: the hover takes it there smoothly, implicit as
                    // Box3D's mover sample's pogo, on its own speed up and
                    // down, past following the ground; down, no faster than
                    // gravity. Box3D adds this step's gravity after, so it is
                    // taken out first.
                    const float omega = 2.f * kPi * kHoverFrequency;
                    const float settled = (own + omega * omega * step * (short_by - lift)) /
                                          (1.f + 2.f * omega * step + omega * omega * step * step);
                    velocity.y = follow + std::max(settled, std::min(fastest - climb, 0.f)) + weight;
                }
            }
            b3Body_SetLinearVelocity(record.body, velocity);
            record.planned = velocity.y - weight;
            // A dynamic ground carries the controller's weight, and only that:
            // all of it where it stands, and where it slides, the part the
            // slope holds up, pushed into it.
            // Handing it the spring's or friction's reaction too couples the two
            // into a loop that rings when the ground is much lighter, and drags
            // the ground along under a walking controller, since Box3D sees no
            // push for the script's Velocity write.
            if (b3Body_IsValid(ground.body) && b3Body_GetType(ground.body) == b3_dynamicBody) {
                // The whole assembly's weight: what it carries stands on that ground too.
                const float mass = record.compound != 0 ? b3Body_GetMass(record.body)
                                                        : static_cast<float>(controller->mass());
                const float load = mass * weight;
                const b3Vec3 push = steep ? b3Vec3{-load * normal.y * normal.x, -load * normal.y * normal.y,
                                                   -load * normal.y * normal.z}
                                          : b3Vec3{0.f, -load, 0.f};
                b3Body_ApplyLinearImpulse(ground.body, push, ground.point, true);
            }
        }
    }

    // On ground too steep to stand on, a controller surfs, as Quake's
    // PM_ClipVelocity has it: of its speed relative to the ground, only what
    // goes into the slope is taken out, so speed into it turns up or down
    // along it. The slope holds up the part of gravity that pushes into it,
    // so gravity takes it down along it, and nothing slows it. It hovers its
    // gap above the slope, moved out square to it.
    static void surf(Body& record, const Ground& ground, float gap, float weight, b3Vec3& velocity) {
        const b3Vec3 normal = ground.normal;
        b3Vec3 relative = b3Sub(velocity, ground.velocity);
        const float into = b3Dot(relative, normal);
        if (into < 0.f) {
            relative = b3MulAdd(relative, -into, normal);
        }
        record.climb = relative.y;
        // Box3D adds this step's gravity after.
        velocity = b3Add(ground.velocity, b3MulAdd(relative, weight * normal.y, normal));
        const float short_by = gap - ground.gap;
        if (short_by != 0.f) {
            b3Vec3 position = b3Body_GetPosition(record.body);
            if (short_by > 0.f) {
                // As far out square to the slope as it falls away under it
                // by the shortfall.
                position = b3MulAdd(position, short_by * normal.y, normal);
            } else {
                position.y += short_by;
            }
            b3Body_SetTransform(record.body, position, b3Body_GetRotation(record.body));
        }
    }

    static bool same_velocity(b3Vec3 a, b3Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

    // Calls each(push) for each way a contact pushes on the controller.
    template <typename Each>
    void for_each_push(const Body& record, Each each) {
        const int capacity = b3Body_GetContactCapacity(record.body);
        if (capacity <= 0) {
            return;
        }
        contacts.resize(static_cast<std::size_t>(capacity));
        const int count = b3Body_GetContactData(record.body, contacts.data(), capacity);
        for (int index = 0; index < count; ++index) {
            const b3ContactData& contact = contacts[static_cast<std::size_t>(index)];
            const bool first = B3_ID_EQUALS(b3Shape_GetBody(contact.shapeIdA), record.body);
            // In an assembly, only the controller's own cylinder counts: what
            // it carries touching a wall or a ceiling does not stop it walking.
            if (record.compound != 0 && !owns_shape(record, first ? contact.shapeIdA : contact.shapeIdB)) {
                continue;
            }
            for (int which = 0; which < contact.manifoldCount; ++which) {
                const b3Manifold& manifold = contact.manifolds[which];
                if (manifold.pointCount == 0) {
                    continue;
                }
                // The normal runs from shape A to shape B: turned to push on the controller.
                each(first ? b3Vec3{-manifold.normal.x, -manifold.normal.y, -manifold.normal.z} : manifold.normal);
            }
        }
    }

    static bool owns_shape(const Body& record, b3ShapeId shape) {
        for (const b3ShapeId own : record.shapes) {
            if (B3_ID_EQUALS(own, shape)) {
                return true;
            }
        }
        return false;
    }

    // A contact on ground too steep to stand on, pushing up on the controller.
    static bool steep_push(b3Vec3 push, float steepest) { return push.y > kOverhangNormal && push.y < steepest; }

    // A contact that pushes down on the controller, as a ceiling or the
    // underside of a ramp does, would turn walking into it into a push into
    // the ground, since the cylinder has no friction. So the controller stops
    // at one as at a wall: the part of its speed across the ground that goes
    // into it is taken out. One on a slope too steep to stand on it surfs:
    // only the part of its speed into the slope is taken out. A wall that
    // stands straight up Box3D stops it at already, and it may shove one that
    // moves.
    void stop_at_walls(const Body& record, float steepest, b3Vec3& velocity) {
        for_each_push(record, [&](b3Vec3 push) {
            if (push.y <= -kOverhangNormal) {
                stop_against(push, velocity.x, velocity.z);
            } else if (steep_push(push, steepest)) {
                const float into = b3Dot(velocity, push);
                if (into < 0.f) {
                    velocity = b3MulAdd(velocity, -into, push);
                }
            }
        });
    }

    // Takes out the part of a speed across the ground (x, z) that goes into
    // a face whose normal is away, as a wall stops it.
    static void stop_against(b3Vec3 away, float& x, float& z) {
        const float level = std::sqrt(away.x * away.x + away.z * away.z);
        if (level < 1e-4f) {
            return;
        }
        const float away_x = away.x / level;
        const float away_z = away.z / level;
        const float into = x * away_x + z * away_z;
        if (into < 0.f) {
            x -= into * away_x;
            z -= into * away_z;
        }
    }

    // A controller off the ground, as one rising from a jump, beside ground
    // too steep to stand on: only its cylinder meets the slope, so the
    // slope would come up into the hover gap under it, where its feet are.
    // The slope is a wall to that gap too: the controller moves out from it
    // level, not up it, so its jump is no higher, and stops going into it.
    void hold_off_steep(const PlayerController& controller, const Body& record, const Ground& ground, float gap,
                        b3Vec3& velocity) {
        if (!ground.hit) {
            return;
        }
        const b3Vec3 face = ground.walled ? ground.wall : ground.normal;
        const float face_gap = ground.walled ? ground.wall_gap : ground.gap;
        if (face.y <= kWallNormal || face.y >= steepest(controller) || face_gap >= gap) {
            return;
        }
        const float level = std::sqrt(face.x * face.x + face.z * face.z);
        // Out level by this much, the face drops away under it by the gap's
        // shortfall.
        const float out = (gap - face_gap) * face.y / level;
        b3Vec3 position = b3Body_GetPosition(record.body);
        position.x += out * face.x / level;
        position.z += out * face.z / level;
        b3Body_SetTransform(record.body, position, b3Body_GetRotation(record.body));
        stop_against(face, velocity.x, velocity.z);
    }

    // After Box3D steps: a controller that met a slope too steep to stand
    // on, moving into it, was turned up along it by the contact. It keeps
    // the speed up and down it was to have.
    void unclimb(DataModel& game) {
        for (auto& [id, record] : bodies) {
            if (!record.controller || b3Body_GetType(record.body) != b3_dynamicBody ||
                (record.compound != 0 && record.compound_root != id)) {
                continue;
            }
            const auto* controller = dynamic_cast<const PlayerController*>(game.instance(id));
            if (controller == nullptr) {
                continue;
            }
            b3Vec3 velocity = b3Body_GetLinearVelocity(record.body);
            if (velocity.y <= record.planned) {
                continue;
            }
            const float least = steepest(*controller);
            bool steep = false;
            for_each_push(record, [&](b3Vec3 push) { steep = steep || steep_push(push, least); });
            if (steep) {
                velocity.y = record.planned;
                b3Body_SetLinearVelocity(record.body, velocity);
            }
        }
    }

    // ---- Bodies -------------------------------------------------------

    std::unordered_map<InstanceId, Body>::iterator create(DataModel& game, PhysicsBase& object, InstanceId target) {
        object.take_dirty();
        Body record;
        record.driven = target;
        Matrix4 start = object.transform();
        if (const GameObject* driven = target != 0 ? game.game_object(target) : nullptr) {
            start = driven->transform();
            record.driven_pose = start;
        }
        b3BodyDef def = b3DefaultBodyDef();
        const auto* rigid = dynamic_cast<PhysicsObject*>(&object);
        if (enabled_controller(&object) != nullptr) {
            // Upright, it never turns, and it never sleeps under its hover.
            start = upright_transform(start);
            def.motionLocks.angularX = true;
            def.motionLocks.angularY = true;
            def.motionLocks.angularZ = true;
            def.enableSleep = false;
        }
        def.type = object.anchored() ? b3_staticBody : b3_dynamicBody;
        pose_of(start, def.position, def.rotation);
        def.linearVelocity = to_b3(object.velocity());
        def.linearDamping = static_cast<float>(object.linear_damping());
        if (rigid != nullptr) {
            def.angularVelocity = to_b3(rigid->angular_velocity());
            def.angularDamping = static_cast<float>(rigid->angular_damping());
        }
        def.userData = user_data(object.id());
        record.body = b3CreateBody(world, &def);
        make_shape(game, object, record);
        // Its Transform says where the body is from the start, stopped too.
        if (target != 0) {
            object.store_simulated(matrix_of(def.position, def.rotation, object.transform()), object.velocity());
        }
        return bodies.emplace(object.id(), record).first;
    }

    // Puts the body's shape on it, replacing any it had: a PlayerController's
    // cylinder, or a PhysicsObject's Shape.
    void make_shape(DataModel& game, PhysicsBase& object, Body& record) {
        ++record.made;
        if (auto* controller = dynamic_cast<PlayerController*>(&object)) {
            make_controller_shape(*controller, record);
        } else if (auto* body = dynamic_cast<PhysicsObject*>(&object)) {
            make_object_shape(game, *body, record);
        } else if (auto* brush = dynamic_cast<Brush*>(&object)) {
            make_brush_shape(game, *brush, record);
        }
    }

    // A Brush's solid, exactly: one hull, or past Box3D's hull limits, convex
    // pieces that fill it (brush::hull_pieces), never simplified. Each piece
    // shares the Brush's Friction, Bounciness, and one density, so Mass splits
    // by volume. CanCollide false: the same shapes, touching nothing (shape_filter).
    void make_brush_shape(DataModel& game, Brush& brush, Body& record) {
        drop_shape(record);
        record.scale = scale_for(game, record.driven);
        // The Brush's solid is placed by its own faces, not by a Prefab, but
        // recenter() compares these against the driven GameObject's Prefab:
        // record them as make_object_shape does, or every move remakes the hull.
        record.center = center_for(game, record.driven);
        const GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr;
        record.prefab = driven != nullptr ? driven->prefab_guid() : std::string();
        b3ShapeDef def = b3DefaultShapeDef();
        def.filter = filter_of(brush);
        def.enableCustomFiltering = no_collide_ids.count(brush.id()) != 0;
        // CanCollide off: a sensor (a trigger zone). Every shape can set one off.
        def.isSensor = !brush.can_collide();
        def.enableSensorEvents = true;
        def.enableContactEvents = record.listened;
        def.baseMaterial.friction = static_cast<float>(brush.friction());
        def.baseMaterial.restitution = static_cast<float>(brush.bounciness());
        def.userData = user_data(brush.id());
        def.updateBodyMass = false;
        const std::vector<brush::Piece> pieces = brush::hull_pieces(brush.shape(), 64, 64);  // edges = V + F - 2, so under Box3D's 128 edges too
        std::vector<b3HullData*> hulls;
        hulls.reserve(pieces.size());
        record.volume = 0.f;
        for (const brush::Piece& piece : pieces) {
            points.clear();
            for (const brush::DVec3& p : piece.points) {
                points.push_back(b3Vec3{static_cast<float>(p.x) * record.scale.x, static_cast<float>(p.y) * record.scale.y,
                                        static_cast<float>(p.z) * record.scale.z});
            }
            // Asked for every vertex, so nothing is simplified away.
            if (b3HullData* hull = b3CreateHull(points.data(), static_cast<int>(points.size()), B3_MAX_HULL_VERTICES)) {
                record.volume += b3ComputeHullMass(hull, 1.f).mass;
                hulls.push_back(hull);
            }
        }
        def.density = shape_density(brush, record);
        for (b3HullData* hull : hulls) {
            add_shape(record, create_hull(record, def, hull));
            b3DestroyHull(hull);
        }
        b3Body_ApplyMassFromShapes(record.body);
    }

    // An upright cylinder of Radius from hover_gap() above the feet (the
    // body's origin) up to Height, with no friction and no bounce.
    void make_controller_shape(PlayerController& controller, Body& record) {
        drop_shape(record);
        // Disabled, it is an ordinary body: nothing that walks it runs.
        record.controller = controller.enabled();
        record.center = Vec3{};
        record.scale = kUnscaled;
        record.prefab.clear();
        const float radius = static_cast<float>(controller.radius());
        const float gap = static_cast<float>(controller.hover_gap());
        const float tall = static_cast<float>(controller.height()) - gap;
        primitive_points(PhysicsObject::Shape::Cylinder, Vec3{2.f * radius, tall, 2.f * radius},
                         Vec3{0.f, gap + tall * 0.5f, 0.f}, points);
        b3ShapeDef def = b3DefaultShapeDef();
        def.filter = filter_of(controller);
        def.enableCustomFiltering = no_collide_ids.count(controller.id()) != 0;
        // CanCollide off: a sensor (a trigger zone). Every shape can set one off.
        def.isSensor = !controller.can_collide();
        def.enableSensorEvents = true;
        def.enableContactEvents = record.listened;
        def.baseMaterial.friction = 0.f;
        def.baseMaterial.restitution = 0.f;
        def.userData = user_data(controller.id());
        def.updateBodyMass = false;
        if (b3HullData* hull = build_hull(points)) {
            record.volume = b3ComputeHullMass(hull, 1.f).mass;
            def.density = shape_density(controller, record);
            add_shape(record, create_hull(record, def, hull));
            b3DestroyHull(hull);
        }
        if (record.shapes.empty()) {
            const b3BoxHull box =
                b3MakeOffsetBoxHull(radius, tall * 0.5f, radius, b3Vec3{0.f, gap + tall * 0.5f, 0.f});
            record.volume = 4.f * radius * radius * tall;
            def.density = shape_density(controller, record);
            add_shape(record, create_hull(record, def, &box.base));
        }
        b3Body_ApplyMassFromShapes(record.body);
    }

    // Puts the object's Shape on the body, replacing any shape it had, with
    // Friction, Bounciness, and a density that gives it its Mass.
    void make_object_shape(DataModel& game, PhysicsObject& object, Body& record) {
        drop_shape(record);
        // Only make_pieces sets this, for the one case it covers: a stopped,
        // unanchored Custom whose Mesh's pieces were not known. Cleared here
        // so a shape made any other way (a different Shape, an anchored
        // Custom, or an unanchored Custom whose pieces are known) does not
        // keep asking every played sync to be made again.
        record.made_without_pieces = false;
        record.pieces_stamp.clear();
        b3ShapeDef def = b3DefaultShapeDef();
        def.filter = filter_of(object);
        def.enableCustomFiltering = no_collide_ids.count(object.id()) != 0;
        // CanCollide off: a sensor (a trigger zone). Every shape can set one off.
        def.isSensor = !object.can_collide();
        def.enableSensorEvents = true;
        def.enableContactEvents = record.listened;
        def.baseMaterial.friction = static_cast<float>(object.friction());
        def.baseMaterial.restitution = static_cast<float>(object.bounciness());
        def.userData = user_data(object.id());
        def.updateBodyMass = false;
        record.scale = scale_for(game, record.driven);
        const Vec3 size = scaled(object.size(), record.scale);
        record.center = center_for(game, record.driven);
        const GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr;
        record.prefab = driven != nullptr ? driven->prefab_guid() : std::string();
        const b3Vec3 center = to_b3(record.center);
        switch (object.shape()) {
        case PhysicsObject::Shape::Sphere: {
            const b3Sphere sphere{placed_point(record, center), size.x * 0.5f};
            record.volume = b3ComputeSphereMass(&sphere, 1.f).mass;
            def.density = shape_density(object, record);
            add_shape(record, b3CreateSphereShape(record.body, &def, &sphere));
            break;
        }
        case PhysicsObject::Shape::Capsule: {
            const float radius = size.x * 0.5f;
            const float half = std::max(size.y - size.x, 0.f) * 0.5f;
            const b3Capsule capsule{placed_point(record, b3Vec3{center.x, center.y - half, center.z}),
                                    placed_point(record, b3Vec3{center.x, center.y + half, center.z}), radius};
            record.volume = b3ComputeCapsuleMass(&capsule, 1.f).mass;
            def.density = shape_density(object, record);
            add_shape(record, b3CreateCapsuleShape(record.body, &def, &capsule));
            break;
        }
        case PhysicsObject::Shape::Custom:
            // Box3D gives a mesh contacts only on a static body, so an
            // unanchored Custom is convex pieces of its mesh instead.
            if (anchored_as(record, object)) {
                if (b3MeshData* mesh = make_mesh(game, object, record.scale, record.center, record)) {
                    record.mesh = mesh;
                    const Vec3 bounds = scaled(bounds_size(mesh_points), record.scale);
                    record.volume = bounds.x * bounds.y * bounds.z;
                    def.density = shape_density(object, record);
                    add_shape(record, b3CreateMeshShape(record.body, &def, mesh, b3Vec3{1.f, 1.f, 1.f}));
                }
                break;
            }
            if (make_pieces(game, object, record.scale, record, def)) {
                break;
            }
            [[fallthrough]];
        case PhysicsObject::Shape::Hull:
            if (b3HullData* hull = make_hull(game, object, record.scale, record.center)) {
                record.volume = b3ComputeHullMass(hull, 1.f).mass;
                def.density = shape_density(object, record);
                // Box3D copies the hull into the shape.
                add_shape(record, create_hull(record, def, hull));
                b3DestroyHull(hull);
            }
            break;
        case PhysicsObject::Shape::Cylinder: {
            // A true cylinder (b3_cylinderShape), round across X and Z. Scaled
            // unevenly across those it would be elliptical, which one cannot
            // be, and below Box3D's linear slop Box3D refuses one: layered
            // hulls then, as before.
            const float radius = size.x * 0.5f;
            const bool round = std::fabs(record.scale.x - record.scale.z) <=
                               1e-4f * std::max({std::fabs(record.scale.x), std::fabs(record.scale.z), 1.f});
            if (round && radius >= B3_LINEAR_SLOP && size.y >= B3_LINEAR_SLOP) {
                const float half = size.y * 0.5f;
                const b3Cylinder cylinder{placed_point(record, b3Vec3{center.x, center.y - half, center.z}),
                                          placed_point(record, b3Vec3{center.x, center.y + half, center.z}), radius};
                record.volume = b3ComputeCylinderMass(&cylinder, 1.f).mass;
                def.density = shape_density(object, record);
                add_shape(record, b3CreateCylinderShape(record.body, &def, &cylinder));
                break;
            }
            // Layered hulls (kCylinderLayers), sharing Mass by their summed volume.
            primitive_points(object.shape(), size, record.center, points);
            b3HullData* layers[kCylinderLayers] = {};
            float volume = 0.f;
            for (int layer = 0; layer < kCylinderLayers; ++layer) {
                const float angle = 2.f * kPi / static_cast<float>(kRoundSides) * static_cast<float>(layer) /
                                    static_cast<float>(kCylinderLayers);
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                turned_points.clear();
                for (const b3Vec3& p : points) {
                    const float x = p.x - record.center.x;
                    const float z = p.z - record.center.z;
                    turned_points.push_back(b3Vec3{record.center.x + x * c - z * s, p.y, record.center.z + x * s + z * c});
                }
                layers[layer] = build_hull(turned_points);
                if (layers[layer] != nullptr) {
                    volume += b3ComputeHullMass(layers[layer], 1.f).mass;
                }
            }
            record.volume = volume;
            def.density = shape_density(object, record);
            for (b3HullData* hull : layers) {
                if (hull != nullptr) {
                    add_shape(record, create_hull(record, def, hull));
                    b3DestroyHull(hull);
                }
            }
            break;
        }
        case PhysicsObject::Shape::Cone:
        case PhysicsObject::Shape::Wedge:
            primitive_points(object.shape(), size, record.center, points);
            if (b3HullData* hull = build_hull(points)) {
                record.volume = b3ComputeHullMass(hull, 1.f).mass;
                def.density = shape_density(object, record);
                add_shape(record, create_hull(record, def, hull));
                b3DestroyHull(hull);
            }
            break;
        case PhysicsObject::Shape::Box:
            break;
        }
        if (record.shapes.empty()) {
            const b3BoxHull box = b3MakeOffsetBoxHull(size.x * 0.5f, size.y * 0.5f, size.z * 0.5f, center);
            record.volume = size.x * size.y * size.z;
            def.density = shape_density(object, record);
            add_shape(record, create_hull(record, def, &box.base));
        }
        b3Body_ApplyMassFromShapes(record.body);
    }

    static float density(const PhysicsBase& object, float volume) {
        return static_cast<float>(object.mass()) / std::max(volume, 1e-9f);
    }

    // A shape's density on its body: density(), but a body that rides
    // massless (Massless, or a disabled PlayerController) welded into an
    // assembly it does not root weighs nothing there, so trim or a driver does
    // not change the car. Alone, or the root, it keeps its Mass: a dynamic
    // body with none does not fall.
    static float shape_density(const PhysicsBase& object, const Body& record) {
        const bool rider = record.compound != 0 && record.compound_root != object.id() && object.rides_massless();
        return rider ? 0.f : density(object, record.volume);
    }

    // Each of the record's shapes at shape_density. The caller applies the body's mass.
    static void set_densities(const PhysicsBase& object, const Body& record) {
        for (const b3ShapeId shape : record.shapes) {
            b3Shape_SetDensity(shape, shape_density(object, record), false);
        }
    }

    // The Mesh's points into points, at their own size times scale
    // (shape_scale) around center, and with triangles, its triangles into
    // triangles. Returns why there are none, or an empty string.
    std::string fitted_mesh(DataModel& game, const PhysicsObject& object, Vec3 scale, Vec3 center,
                            bool with_triangles) {
        const InstanceId mesh_id = object.mesh_id();
        const auto* mesh = mesh_id != 0 ? dynamic_cast<const Mesh*>(game.instance(mesh_id)) : nullptr;
        if (mesh == nullptr) {
            return "it has no Mesh";
        }
        if (std::optional<std::string> error =
                mesh->vertex_positions(mesh_points, with_triangles ? &triangles : nullptr)) {
            return *error;
        }
        fit_points(mesh_points, scale, center, points);
        return {};
    }

    // A hull of the Mesh's points. Null, with one warning, when there is no
    // hull to build.
    b3HullData* make_hull(DataModel& game, PhysicsObject& object, Vec3 scale, Vec3 center) {
        std::string why = fitted_mesh(game, object, scale, center, false);
        b3HullData* hull = nullptr;
        if (why.empty()) {
            hull = build_hull(points);
            if (hull == nullptr) {
                why = "Box3D could not build a hull from its points";
            }
        }
        if (hull == nullptr && !object.warned_hull) {
            object.warned_hull = true;
            say("PhysicsObject " + game.name(object.id()) + ": " + shape_name(object) + " fell back to Box (" + why +
                ")");
        } else if (hull != nullptr) {
            object.warned_hull = false;
        }
        return hull;
    }

    // The whole Mesh as triangles, for an anchored Custom. Box3D keeps a
    // pointer to it, so the body record owns it. Null, with one warning, when
    // there is none.
    b3MeshData* make_mesh(DataModel& game, PhysicsObject& object, Vec3 scale, Vec3 center, const Body& record) {
        std::string why = fitted_mesh(game, object, scale, center, true);
        b3MeshData* mesh = nullptr;
        if (why.empty()) {
            // A mesh shape takes no transform: a member's points are placed first.
            for (b3Vec3& point : points) {
                point = placed_point(record, point);
            }
            mesh = build_mesh(points, triangles, indices);
            if (mesh == nullptr) {
                why = "Box3D could not build a mesh from its triangles";
            }
        }
        if (mesh == nullptr && !object.warned_hull) {
            object.warned_hull = true;
            say("PhysicsObject " + game.name(object.id()) + ": Custom fell back to Box (" + why + ")");
        } else if (mesh != nullptr) {
            object.warned_hull = false;
        }
        return mesh;
    }

    // An unanchored Custom as one hull per convex piece of its Mesh, fitted as
    // its whole Mesh is. False when it has none; with a Mesh that has points,
    // it says so once, and the caller makes it a Hull.
    bool make_pieces(DataModel& game, PhysicsObject& object, Vec3 scale, Body& record, b3ShapeDef& def) {
        if (!fitted_mesh(game, object, scale, record.center, true).empty()) {
            // No Mesh, or no points: the Hull it falls to says why.
            return false;
        }
        const auto* mesh = dynamic_cast<const Mesh*>(game.instance(object.mesh_id()));
        std::vector<anarchy::amesh::ConvexPiece> pieces;
        if (game.simulation_running()) {
            pieces = pieces_for(*mesh, mesh_points, triangles);
        } else if (!known_pieces(*mesh, mesh_points, triangles, pieces)) {
            // Stopped, never decompose here: the studio's decomposer will.
            record.made_without_pieces = true;
            record.pieces_stamp = mesh->file_stamp();
            return false;
        }
        std::vector<b3HullData*> hulls = piece_hulls(pieces, fit_of(mesh_points, scale, record.center));
        if (hulls.empty()) {
            if (!object.warned_custom) {
                object.warned_custom = true;
                say("PhysicsObject " + game.name(object.id()) + ": Custom fell back to Hull (" +
                    (pieces.empty() ? "its Mesh split into no convex pieces"
                                    : "Box3D could not build a hull from any piece") +
                    ")");
            }
            return false;
        }
        object.warned_custom = false;
        record.volume = 0.f;
        for (b3HullData* hull : hulls) {
            record.volume += b3ComputeHullMass(hull, 1.f).mass;
        }
        def.density = shape_density(object, record);
        for (b3HullData* hull : hulls) {
            // Box3D copies the hull into the shape.
            add_shape(record, create_hull(record, def, hull));
            b3DestroyHull(hull);
        }
        return !record.shapes.empty();
    }

    static std::string shape_name(const PhysicsObject& object) {
        return object.shape() == PhysicsObject::Shape::Custom ? "Custom" : "Hull";
    }

    // Keeps shape on the record, when Box3D made one.
    static void add_shape(Body& record, b3ShapeId shape) {
        if (b3Shape_IsValid(shape)) {
            record.shapes.push_back(shape);
        }
    }

    // Destroys the record's shapes, and the mesh one held, if any.
    void drop_shape(Body& record) {
        forget_shapes(record);
        for (const b3ShapeId shape : record.shapes) {
            if (b3Shape_IsValid(shape)) {
                b3DestroyShape(shape, false);
            }
        }
        record.shapes.clear();
        if (record.mesh != nullptr) {
            b3DestroyMesh(record.mesh);
            record.mesh = nullptr;
        }
    }

    // What writes changed since the last step, into the body.
    void push(DataModel& game, PhysicsBase& object, Body& record) {
        const std::uint32_t dirty = object.take_dirty();
        if (dirty == 0) {
            return;
        }
        auto* rigid = dynamic_cast<PhysicsObject*>(&object);
        // Anchoring a Custom turns its convex pieces into its whole mesh, and back. The
        // old shape goes first, so a mesh is never on a dynamic body.
        const bool custom_type = rigid != nullptr && (dirty & PhysicsObject::kDirtyType) != 0 &&
                                 rigid->shape() == PhysicsObject::Shape::Custom;
        if (custom_type) {
            drop_shape(record);
        }
        if ((dirty & PhysicsObject::kDirtyType) != 0) {
            b3Body_SetType(record.body, object.anchored() ? b3_staticBody : b3_dynamicBody);
        }
        if ((dirty & PhysicsObject::kDirtyShape) != 0 || custom_type) {
            // A new shape takes the current Friction, Bounciness, and Mass too.
            make_shape(game, object, record);
        } else {
            if ((dirty & PhysicsObject::kDirtyFilter) != 0) {
                set_filters(object, record);
            }
            if (rigid != nullptr && (dirty & PhysicsObject::kDirtyMaterial) != 0) {
                for (const b3ShapeId shape : record.shapes) {
                    b3Shape_SetFriction(shape, static_cast<float>(rigid->friction()));
                    b3Shape_SetRestitution(shape, static_cast<float>(rigid->bounciness()));
                }
            }
            if (auto* brush = dynamic_cast<Brush*>(&object); brush != nullptr && (dirty & PhysicsObject::kDirtyMaterial) != 0) {
                for (const b3ShapeId shape : record.shapes) {
                    b3Shape_SetFriction(shape, static_cast<float>(brush->friction()));
                    b3Shape_SetRestitution(shape, static_cast<float>(brush->bounciness()));
                }
            }
            if ((dirty & PhysicsObject::kDirtyMass) != 0) {
                set_densities(object, record);
                b3Body_ApplyMassFromShapes(record.body);
            }
        }
        if ((dirty & PhysicsObject::kDirtyDamping) != 0) {
            b3Body_SetLinearDamping(record.body, static_cast<float>(object.linear_damping()));
            if (rigid != nullptr) {
                b3Body_SetAngularDamping(record.body, static_cast<float>(rigid->angular_damping()));
            } else if (auto* brush = dynamic_cast<Brush*>(&object)) {
                b3Body_SetAngularDamping(record.body, static_cast<float>(brush->angular_damping()));
            }
        }
        // Stopped, a driven body's Transform follows its GameObject, so a write
        // to it, as the Move tool's to every selected PVInstance or a
        // Properties edit, does not move the body or the GameObject (which
        // would be a GameObject move with no Changed and no history): the
        // GameObject decides, and the Transform goes back to where the body
        // is. The same after Play until the first step (played), for a write
        // made after the last stopped sync or by a script before that step.
        const bool pose_from_game_object = record.driven != 0 && poses_from_game_objects;
        if ((dirty & PhysicsObject::kDirtyPose) != 0 && pose_from_game_object) {
            const b3Vec3 position = b3Body_GetPosition(record.body);
            const b3Quat rotation = b3Body_GetRotation(record.body);
            object.store_simulated(matrix_of(position, rotation, object.transform()), object.velocity());
        }
        if ((dirty & PhysicsObject::kDirtyPose) != 0 && !pose_from_game_object) {
            b3Vec3 position{};
            b3Quat rotation{};
            pose_of(object.transform(), position, rotation);
            b3Body_SetTransform(record.body, position, rotation);
            // The GameObject goes with it now: a body that does not move
            // after this, as an anchored one, has no move event to carry it.
            if (GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr) {
                record.driven_pose = matrix_of(position, rotation, driven->transform());
                game.write_simulated_transform(record.driven, record.driven_pose);
            }
        }
        if ((dirty & PhysicsObject::kDirtyVelocity) != 0) {
            // A write that adds speed upward is a jump: the hover lets go at once.
            if (record.controller &&
                object.velocity().y - b3Body_GetLinearVelocity(record.body).y > kRisingSpeed) {
                record.launched = true;
            }
            b3Body_SetLinearVelocity(record.body, to_b3(object.velocity()));
            if (rigid != nullptr) {
                b3Body_SetAngularVelocity(record.body, to_b3(rigid->angular_velocity()));
            }
        }
        if ((dirty & ~PhysicsObject::kDirtyMaterial) != 0 && !object.anchored()) {
            b3Body_SetAwake(record.body, true);
        }
    }

    // A driven GameObject that is not where physics last put it was moved by
    // a script or Properties: the body jumps there. True when it did.
    bool follow_driven(DataModel& game, PhysicsBase& object, Body& record) {
        const GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr;
        if (driven == nullptr) {
            return false;
        }
        // Held, the hand puts the GameObject where it is drawn, and the body
        // goes there by its own velocity (drive_held), not by a jump.
        if (game.simulation_running() && (game.assemblies().flags(object.id()) & Assemblies::kHeld) != 0) {
            return false;
        }
        const Matrix4 now = driven->transform();
        if (same_matrix4(now, record.driven_pose)) {
            return false;
        }
        b3Vec3 position{};
        b3Quat rotation{};
        pose_of(record.controller ? upright_transform(now) : now, position, rotation);
        b3Body_SetTransform(record.body, position, rotation);
        if (!object.anchored()) {
            b3Body_SetAwake(record.body, true);
        }
        record.driven_pose = now;
        object.store_simulated(matrix_of(position, rotation, object.transform()), object.velocity());
        return true;
    }

    // What Box3D moved, back onto the instances.
    void pull(DataModel& game) {
        const b3BodyEvents events = b3World_GetBodyEvents(world);
        for (int index = 0; index < events.moveCount; ++index) {
            const b3BodyMoveEvent& event = events.moveEvents[index];
            const InstanceId id = id_of(event.userData);
            const auto found = bodies.find(id);
            auto* object = dynamic_cast<PhysicsBase*>(game.instance(id));
            if (found == bodies.end() || object == nullptr) {
                continue;
            }
            Body& record = found->second;
            const b3Vec3 position = event.transform.p;
            const b3Quat rotation = event.transform.q;
            if (record.compound != 0) {
                // An assembly's body: every member goes where it is.
                const int assembly = game.assemblies().play_of(record.compound_root);
                if (assembly >= 0) {
                    game.assemblies().pose_members(game, static_cast<std::size_t>(assembly),
                                                   matrix_of(position, rotation, matrix4_identity()),
                                                   from_b3(b3Body_GetLinearVelocity(record.body)),
                                                   from_b3(b3Body_GetAngularVelocity(record.body)));
                }
                continue;
            }
            object->store_simulated(matrix_of(position, rotation, object->transform()),
                                    from_b3(b3Body_GetLinearVelocity(record.body)));
            if (auto* rigid = dynamic_cast<PhysicsObject*>(object)) {
                rigid->store_angular_velocity(from_b3(b3Body_GetAngularVelocity(record.body)));
            }
            if (GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr) {
                record.driven_pose = matrix_of(position, rotation, driven->transform());
                game.write_simulated_transform(record.driven, record.driven_pose);
            }
        }
    }
};

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::sync(DataModel& game) { impl_->sync(game); }

void PhysicsWorld::simulate(DataModel& game, double dt) { impl_->simulate(game, dt); }

void PhysicsWorld::step(DataModel& game, double dt) { impl_->step(game, dt); }

std::optional<RayHit> PhysicsWorld::raycast(DataModel& game, Vec3 origin, Vec3 direction, const RayFilter& filter) {
    return impl_->raycast(game, origin, direction, filter);
}

std::size_t PhysicsWorld::body_count() const {
    // An assembly's members share one Box3D body.
    std::size_t own = 0;
    for (const auto& [id, record] : impl_->bodies) {
        if (record.compound == 0) {
            ++own;
        }
    }
    return own + impl_->compounds.size() + impl_->terrain_bodies.size();
}

namespace {

std::atomic<std::size_t> counted_box3d_allocations{0};

void* counting_box3d_alloc(std::size_t size, std::int32_t alignment) {
    counted_box3d_allocations.fetch_add(1);
#if defined(_WIN32)
    return _aligned_malloc(size, static_cast<std::size_t>(alignment));
#else
    void* memory = nullptr;
    return posix_memalign(&memory, static_cast<std::size_t>(alignment), size) == 0 ? memory : nullptr;
#endif
}

void counting_box3d_free(void* memory, std::size_t) {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

}  // namespace

void PhysicsWorld::count_box3d_allocations(bool on) {
    if (on) {
        b3SetAllocator(counting_box3d_alloc, counting_box3d_free);
    } else {
        b3SetAllocator(nullptr, nullptr);
    }
}

std::size_t PhysicsWorld::box3d_allocations() { return counted_box3d_allocations.load(); }

std::optional<Vec3> PhysicsWorld::body_center(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    if (found == impl_->bodies.end() || !b3Body_IsValid(found->second.body)) {
        return std::nullopt;
    }
    return from_b3(b3Body_GetWorldCenter(found->second.body));
}

float PhysicsWorld::body_damping(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    return found != impl_->bodies.end() ? b3Body_GetLinearDamping(found->second.body) : 0.f;
}

bool PhysicsWorld::has_body(InstanceId id) const {
    return impl_->bodies.count(id) != 0 || impl_->terrain_bodies.count(id) != 0;
}

int PhysicsWorld::shapes_made(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    return found != impl_->bodies.end() ? found->second.made : 0;
}

float PhysicsWorld::body_mass(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    return found != impl_->bodies.end() ? b3Body_GetMass(found->second.body) : 0.f;
}

std::vector<float> PhysicsWorld::shape_frictions(InstanceId id) const {
    std::vector<float> frictions;
    const auto found = impl_->bodies.find(id);
    if (found != impl_->bodies.end()) {
        for (const b3ShapeId shape : found->second.shapes) {
            frictions.push_back(b3Shape_GetFriction(shape));
        }
    }
    return frictions;
}

std::uint64_t PhysicsWorld::body_key(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    if (found == impl_->bodies.end()) {
        return 0;
    }
    // A new world numbers its bodies from the start again, so the handle alone
    // would match across a Stop; the generation in the top bits tells them apart.
    return b3StoreBodyId(found->second.body) ^ (static_cast<std::uint64_t>(impl_->generation) << 48);
}

std::uint64_t PhysicsWorld::shape_key(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    if (found == impl_->bodies.end() || found->second.shapes.empty()) {
        return 0;
    }
    return b3StoreShapeId(found->second.shapes.front());
}

std::optional<Vec3> PhysicsWorld::body_position(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    if (found == impl_->bodies.end()) {
        return std::nullopt;
    }
    return from_b3(b3Body_GetPosition(found->second.body));
}

void PhysicsWorld::set_terrain_world(TerrainWorld* terrains) {
    if (terrains == impl_->terrains) {
        return;
    }
    // Another TerrainWorld numbers its revisions afresh: its Terrains start over.
    impl_->gone.clear();
    for (const auto& [id, record] : impl_->terrain_bodies) {
        impl_->gone.push_back(id);
    }
    for (InstanceId id : impl_->gone) {
        impl_->destroy_terrain(id);
    }
    impl_->terrains = terrains;
}

std::shared_ptr<void> PhysicsWorld::build_terrain_collider(const terrain::ChunkMesh& mesh) {
    if (mesh.triangles.size() < 3 || mesh.positions.size() < 3) {
        return nullptr;
    }
    std::vector<b3Vec3> points;
    points.reserve(mesh.positions.size());
    for (const Vec3& p : mesh.positions) {
        points.push_back(to_b3(p));
    }
    std::vector<std::int32_t> indices(mesh.triangles.begin(), mesh.triangles.end());
    std::vector<std::uint8_t> ids(mesh.triangle_ids.begin(), mesh.triangle_ids.end());
    ids.resize(indices.size() / 3, 0);
    b3MeshDef def{};
    def.vertices = points.data();
    def.indices = indices.data();
    def.materialIndices = ids.data();
    def.vertexCount = static_cast<int>(points.size());
    def.triangleCount = static_cast<int>(indices.size() / 3);
    // Neighboring cells share corners only by position, as an anchored
    // Custom's faces do (build_mesh): welding joins them so edges between
    // triangles are known, and a body slides across them without catching.
    def.weldVertices = true;
    def.weldTolerance = kWeldTolerance;
    def.identifyEdges = true;
    // Splitting the tree at the middle instead of by surface area builds a
    // dense chunk's mesh in less than half the time (this runs on a mesher
    // worker for every edited chunk), and a chunk's even grid of triangles
    // gains little from the slower split.
    def.useMedianSplit = true;
    auto collider = std::make_shared<TerrainCollider>();
    collider->mesh = b3CreateMesh(&def, nullptr, 0);
    if (collider->mesh == nullptr) {
        return nullptr;
    }
    return collider;
}

std::vector<PhysicsWorld::MeshRayCastHit> PhysicsWorld::ray_cast_mesh_for_test(
    const std::vector<Vec3>& positions, const std::vector<std::uint32_t>& triangles, bool use_median_split,
    const std::vector<Vec3>& ray_origins, const std::vector<Vec3>& ray_directions) {
    std::vector<b3Vec3> points;
    points.reserve(positions.size());
    for (const Vec3& p : positions) {
        points.push_back(to_b3(p));
    }
    std::vector<std::int32_t> indices(triangles.begin(), triangles.end());
    b3MeshDef def{};
    def.vertices = points.data();
    def.indices = indices.data();
    def.vertexCount = static_cast<int>(points.size());
    def.triangleCount = static_cast<int>(indices.size() / 3);
    // Welded, with edges identified, as build_mesh builds an anchored
    // Custom's shape.
    def.weldVertices = true;
    def.weldTolerance = kWeldTolerance;
    def.identifyEdges = true;
    def.useMedianSplit = use_median_split;
    b3MeshData* mesh = b3CreateMesh(&def, nullptr, 0);
    std::vector<MeshRayCastHit> results = ray_cast_mesh_data(mesh, ray_origins, ray_directions);
    if (mesh != nullptr) {
        b3DestroyMesh(mesh);
    }
    return results;
}

std::vector<PhysicsWorld::MeshRayCastHit> PhysicsWorld::ray_cast_terrain_collider_for_test(
    const terrain::ChunkMesh& mesh, const std::vector<Vec3>& ray_origins, const std::vector<Vec3>& ray_directions) {
    const std::shared_ptr<void> collider = build_terrain_collider(mesh);
    const auto* terrain_collider = static_cast<const TerrainCollider*>(collider.get());
    return ray_cast_mesh_data(terrain_collider != nullptr ? terrain_collider->mesh : nullptr, ray_origins,
                               ray_directions);
}

bool PhysicsWorld::shapes_are_sensors(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    if (found == impl_->bodies.end() || found->second.shapes.empty()) {
        return false;
    }
    for (const b3ShapeId shape : found->second.shapes) {
        if (!b3Shape_IsSensor(shape)) {
            return false;
        }
    }
    return true;
}

bool PhysicsWorld::contact_events_enabled(InstanceId id) const {
    const auto found = impl_->bodies.find(id);
    if (found == impl_->bodies.end() || found->second.shapes.empty()) {
        return false;
    }
    for (const b3ShapeId shape : found->second.shapes) {
        if (!b3Shape_AreContactEventsEnabled(shape)) {
            return false;
        }
    }
    return true;
}

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

std::size_t PhysicsWorld::shape_count(InstanceId id) const {
    if (const auto found = impl_->bodies.find(id); found != impl_->bodies.end()) {
        return found->second.shapes.size();
    }
    std::size_t count = 0;
    if (const auto found = impl_->terrain_bodies.find(id); found != impl_->terrain_bodies.end()) {
        for (const auto& [coord, chunk] : found->second.chunks) {
            count += b3Shape_IsValid(chunk.shape) ? 1u : 0u;
        }
    }
    return count;
}

void PhysicsWorld::set_warning_sink(std::function<void(const std::string&)> sink) { impl_->warn = std::move(sink); }

Vec3 PhysicsWorld::shape_center(const DataModel& game, const PhysicsBase& object) {
    if (dynamic_cast<const PlayerController*>(&object) != nullptr) {
        return Vec3{};
    }
    return center_for(game, object.driven_game_object());
}

Vec3 PhysicsWorld::shape_scale(const DataModel& game, const PhysicsBase& object) {
    if (dynamic_cast<const PlayerController*>(&object) != nullptr) {
        return kUnscaled;
    }
    return scale_for(game, object.driven_game_object());
}

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

void PhysicsWorld::collision_outline(const PhysicsObject& object, Vec3 center, const std::vector<Vec3>& mesh_points,
                                     const std::vector<std::uint32_t>& triangles, std::vector<Vec3>& lines,
                                     Vec3 scale, const std::vector<anarchy::amesh::ConvexPiece>* pieces,
                                     int anchored_as) {
    const bool anchored = anchored_as >= 0 ? anchored_as != 0 : object.anchored();
    const Vec3 x{1.f, 0.f, 0.f};
    const Vec3 y{0.f, 1.f, 0.f};
    const Vec3 z{0.f, 0.f, 1.f};
    lines.clear();
    const Vec3 size = scaled(object.size(), scale);
    switch (object.shape()) {
    case PhysicsObject::Shape::Sphere: {
        const float radius = size.x * 0.5f;
        add_arc(lines, center, x, y, radius, 0.f, 2.f * kPi);
        add_arc(lines, center, y, z, radius, 0.f, 2.f * kPi);
        add_arc(lines, center, z, x, radius, 0.f, 2.f * kPi);
        return;
    }
    case PhysicsObject::Shape::Capsule: {
        // As make_shape builds it: a segment half tall each way along Y.
        const float radius = size.x * 0.5f;
        const float half = std::max(size.y - size.x, 0.f) * 0.5f;
        const Vec3 top{center.x, center.y + half, center.z};
        const Vec3 bottom{center.x, center.y - half, center.z};
        add_arc(lines, top, x, z, radius, 0.f, 2.f * kPi);
        add_arc(lines, bottom, x, z, radius, 0.f, 2.f * kPi);
        if (half > 0.f) {
            for (const Vec3& side : {Vec3{radius, 0.f, 0.f}, Vec3{-radius, 0.f, 0.f}, Vec3{0.f, 0.f, radius},
                                     Vec3{0.f, 0.f, -radius}}) {
                add_line(lines, Vec3{bottom.x + side.x, bottom.y, bottom.z + side.z},
                         Vec3{top.x + side.x, top.y, top.z + side.z});
            }
        }
        // The caps, across X and across Z.
        add_arc(lines, top, x, y, radius, 0.f, kPi);
        add_arc(lines, top, z, y, radius, 0.f, kPi);
        add_arc(lines, bottom, x, y, radius, kPi, 2.f * kPi);
        add_arc(lines, bottom, z, y, radius, kPi, 2.f * kPi);
        return;
    }
    case PhysicsObject::Shape::Custom:
    case PhysicsObject::Shape::Hull: {
        if (mesh_points.empty()) {
            break;
        }
        std::vector<b3Vec3> points;
        fit_points(mesh_points, scale, center, points);
        if (object.shape() == PhysicsObject::Shape::Custom && anchored) {
            std::vector<std::int32_t> indices;
            b3MeshData* mesh = build_mesh(points, triangles, indices);
            if (mesh == nullptr) {
                break;
            }
            b3DestroyMesh(mesh);
            outline_triangles(points, triangles, lines);
            return;
        }
        if (object.shape() == PhysicsObject::Shape::Custom && pieces != nullptr && !pieces->empty()) {
            std::vector<b3HullData*> hulls = piece_hulls(*pieces, fit_of(mesh_points, scale, center));
            for (b3HullData* hull : hulls) {
                outline_hull(*hull, lines);
                b3DestroyHull(hull);
            }
            if (!hulls.empty()) {
                return;
            }
        }
        b3HullData* hull = build_hull(points);
        if (hull == nullptr) {
            break;
        }
        outline_hull(*hull, lines);
        b3DestroyHull(hull);
        return;
    }
    case PhysicsObject::Shape::Cylinder:
        if (std::fabs(scale.x - scale.z) <= 1e-4f * std::max({std::fabs(scale.x), std::fabs(scale.z), 1.f}) &&
            size.x * 0.5f >= B3_LINEAR_SLOP && size.y >= B3_LINEAR_SLOP) {
            // As make_shape builds it: a true cylinder, two rims and four side lines along Y.
            const float radius = size.x * 0.5f;
            const float half = size.y * 0.5f;
            const Vec3 top{center.x, center.y + half, center.z};
            const Vec3 bottom{center.x, center.y - half, center.z};
            add_arc(lines, top, x, z, radius, 0.f, 2.f * kPi);
            add_arc(lines, bottom, x, z, radius, 0.f, 2.f * kPi);
            for (const Vec3& side : {Vec3{radius, 0.f, 0.f}, Vec3{-radius, 0.f, 0.f}, Vec3{0.f, 0.f, radius},
                                     Vec3{0.f, 0.f, -radius}}) {
                add_line(lines, Vec3{bottom.x + side.x, bottom.y, bottom.z + side.z},
                         Vec3{top.x + side.x, top.y, top.z + side.z});
            }
            return;
        }
        [[fallthrough]];
    case PhysicsObject::Shape::Cone:
    case PhysicsObject::Shape::Wedge: {
        std::vector<b3Vec3> points;
        primitive_points(object.shape(), size, center, points);
        b3HullData* hull = build_hull(points);
        if (hull == nullptr) {
            break;
        }
        outline_hull(*hull, lines);
        b3DestroyHull(hull);
        return;
    }
    case PhysicsObject::Shape::Box:
        break;
    }
    outline_box(Vec3{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f}, center, lines);
}

Matrix4 PhysicsWorld::body_pose(const Matrix4& transform) {
    b3Vec3 position;
    b3Quat rotation;
    pose_of(transform, position, rotation);
    return matrix_of(position, rotation, matrix4_identity());
}

}  // namespace engine_core
