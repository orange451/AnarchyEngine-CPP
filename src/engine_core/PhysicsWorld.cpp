#include "PhysicsWorld.hpp"

#include "AssetInstances.hpp"
#include "ConvexDecomposition.hpp"
#include "DataModel.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "SceneService.hpp"

#pragma warning(push, 0)
#include "box3d/box3d.h"
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine_core {
namespace {

// The most vertices a Hull asks Box3D to keep, then a second try with fewer:
// a hull past 128 faces or edges fails, and fewer vertices make fewer.
constexpr int kHullVertices = 64;
constexpr int kHullVerticesRetry = 32;
// How near two corners of a Custom's triangles are to be joined as one.
constexpr float kWeldTolerance = 1e-4f;
// The sides around a Cylinder or a Cone, as Box3D builds no round hull. A
// Cylinder of n sides has 6n half-edges, and a hull keeps at most 128.
constexpr int kRoundSides = 16;
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
Matrix4 matrix_of(const b3Vec3& position, const b3Quat& rotation, const Matrix4& keep) {
    const b3Matrix3 basis = b3MakeMatrixFromQuat(rotation);
    const b3Vec3 columns[3] = {basis.cx, basis.cy, basis.cz};
    Matrix4 out = matrix4_identity();
    for (int column = 0; column < 3; ++column) {
        float scale = column_length(keep, column);
        if (!(scale > 1e-6f)) {
            scale = 1.f;
        }
        out.m[column * 4 + 0] = columns[column].x * scale;
        out.m[column * 4 + 1] = columns[column].y * scale;
        out.m[column * 4 + 2] = columns[column].z * scale;
    }
    out.m[12] = position.x;
    out.m[13] = position.y;
    out.m[14] = position.z;
    return out;
}

void* user_data(InstanceId id) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)); }

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

bool same_vec3(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

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
        std::uint64_t seen = 0;
    };

    b3WorldId world = b3_nullWorldId;
    // Up is positive: minus Workspace.Gravity, as the world was last given it.
    float gravity = -static_cast<float>(Workspace::kDefaultGravity);
    std::uint32_t generation = 0;
    std::uint64_t pass = 0;
    std::unordered_map<InstanceId, Body> bodies;
    std::function<void(const std::string&)> warn;

    // Scratch, kept between steps so a step does not allocate once warm.
    std::vector<InstanceId> eligible;
    std::vector<std::pair<InstanceId, InstanceId>> wanted;
    std::unordered_map<InstanceId, InstanceId> claims;
    std::vector<InstanceId> gone;
    std::vector<b3Vec3> points;
    std::vector<Vec3> mesh_points;
    std::vector<std::uint32_t> triangles;
    std::vector<std::int32_t> indices;
    std::vector<b3ContactData> contacts;

    Impl() {
        bodies.reserve(1024);
        eligible.reserve(1024);
        wanted.reserve(1024);
        claims.reserve(1024);
        gone.reserve(1024);
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
    }

    void begin(std::uint32_t next_generation) {
        reset();
        b3WorldDef def = b3DefaultWorldDef();
        def.gravity = b3Vec3{0.f, gravity, 0.f};
        world = b3CreateWorld(&def);
        generation = next_generation;
    }

    void say(const std::string& text) const {
        if (warn) {
            warn(text);
        }
    }

    void step(DataModel& game, double dt) {
        if (!game.simulation_running()) {
            return;
        }
        if (!b3World_IsValid(world) || generation != game.world_generation()) {
            begin(game.world_generation());
        }
        pull_gravity(game);
        reconcile(game);
        control(game, dt);
        b3World_Step(world, static_cast<float>(dt), 1);
        unclimb(game);
        pull(game);
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
        claims.clear();
        bool shared = false;
        for (InstanceId id : eligible) {
            const auto* object = dynamic_cast<PhysicsBase*>(game.instance(id));
            if (object == nullptr) {
                continue;
            }
            const InstanceId target = object->driven_game_object();
            wanted.emplace_back(id, target);
            if (target != 0 && !claims.emplace(target, id).second) {
                shared = true;
            }
        }
        if (shared) {
            first_in_tree_order(game);
        }
        for (const auto& [id, target] : wanted) {
            auto* object = dynamic_cast<PhysicsBase*>(game.instance(id));
            if (target != 0 && claims[target] != id) {
                if (!object->warned_shared) {
                    object->warned_shared = true;
                    say(std::string(object->class_name()) + " " + game.name(id) +
                        " has no body: an earlier body already moves " + game.name(target));
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
            if (auto* controller = dynamic_cast<PlayerController*>(game.instance(id))) {
                controller->store_ground(false, false);
            }
            destroy(id);
        }
    }

    // Each claimed GameObject goes to the first of its claimants in a
    // depth-first walk of Workspace. Only runs when two share one.
    void first_in_tree_order(DataModel& game) {
        claims.clear();
        std::vector<InstanceId> stack = game.get_children(game.scene_service("Workspace"));
        std::reverse(stack.begin(), stack.end());
        while (!stack.empty()) {
            const InstanceId id = stack.back();
            stack.pop_back();
            if (const auto* object = dynamic_cast<const PhysicsBase*>(game.instance(id))) {
                if (const InstanceId target = object->driven_game_object(); target != 0) {
                    claims.emplace(target, id);
                }
            }
            std::vector<InstanceId> children = game.get_children(id);
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
    }

    void destroy(InstanceId id) {
        const auto found = bodies.find(id);
        if (found == bodies.end()) {
            return;
        }
        drop_shape(found->second);
        if (b3Body_IsValid(found->second.body)) {
            b3DestroyBody(found->second.body);
        }
        bodies.erase(found);
    }

    void keep(DataModel& game, PhysicsBase& object, InstanceId target) {
        auto found = bodies.find(object.id());
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
    }

    // A GameObject with another Prefab, or one moved by someone else (moved),
    // may center the shape elsewhere: it is made again there. One scaled
    // another way, by its Transform or its Scale, makes it again at that size.
    void recenter(DataModel& game, PhysicsBase& object, Body& record, bool moved) {
        if (record.controller) {
            return;
        }
        const GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr;
        if (driven == nullptr) {
            return;
        }
        const bool rescaled = !same_vec3(scale_for(game, record.driven), record.scale);
        if (!moved && !rescaled && driven->prefab_guid() == record.prefab) {
            return;
        }
        record.prefab = driven->prefab_guid();
        if (rescaled || !same_vec3(center_for(game, record.driven), record.center)) {
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
            // Box3D stops a cast B3_LINEAR_SLOP short of what it hits.
            ground.gap = hits.closest * reach - kProbeSkin + B3_LINEAR_SLOP;
            ground.normal = face_normal(hits.shape, hits.point, hits.normal, origin.y);
            ground.point = hits.point;
            ground.body = b3Shape_GetBody(hits.shape);
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
                b3World_CastRay(world, origin, b3Vec3{0.f, -deeper, 0.f}, b3DefaultQueryFilter(), probe_hit, &middle);
                if (middle.hit && !middle.started_inside && middle.normal.y >= steepest(controller)) {
                    ground.walled = true;
                    ground.wall = ground.normal;
                    ground.wall_gap = ground.gap;
                    ground.gap = middle.closest * deeper - kProbeSkin;
                    ground.normal = middle.normal;
                    ground.point = middle.point;
                    ground.body = b3Shape_GetBody(middle.shape);
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
                const float load = static_cast<float>(controller->mass()) * weight;
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
            if (!record.controller || b3Body_GetType(record.body) != b3_dynamicBody) {
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
        if (dynamic_cast<PlayerController*>(&object) != nullptr) {
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
        // Its Transform says where the body is from the start.
        if (target != 0) {
            object.store_simulated(matrix_of(def.position, def.rotation, object.transform()), object.velocity());
        }
        return bodies.emplace(object.id(), record).first;
    }

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
        record.scale = kUnscaled;
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
            add_shape(record, b3CreateHullShape(record.body, &def, hull));
            b3DestroyHull(hull);
        }
        if (record.shapes.empty()) {
            const b3BoxHull box =
                b3MakeOffsetBoxHull(radius, tall * 0.5f, radius, b3Vec3{0.f, gap + tall * 0.5f, 0.f});
            record.volume = 4.f * radius * radius * tall;
            def.density = density(controller, record.volume);
            add_shape(record, b3CreateHullShape(record.body, &def, &box.base));
        }
        b3Body_ApplyMassFromShapes(record.body);
    }

    // Puts the object's Shape on the body, replacing any shape it had, with
    // Friction, Bounciness, and a density that gives it its Mass.
    void make_object_shape(DataModel& game, PhysicsObject& object, Body& record) {
        drop_shape(record);
        b3ShapeDef def = b3DefaultShapeDef();
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
            const b3Sphere sphere{center, size.x * 0.5f};
            record.volume = b3ComputeSphereMass(&sphere, 1.f).mass;
            def.density = density(object, record.volume);
            add_shape(record, b3CreateSphereShape(record.body, &def, &sphere));
            break;
        }
        case PhysicsObject::Shape::Capsule: {
            const float radius = size.x * 0.5f;
            const float half = std::max(size.y - size.x, 0.f) * 0.5f;
            const b3Capsule capsule{b3Vec3{center.x, center.y - half, center.z},
                                    b3Vec3{center.x, center.y + half, center.z}, radius};
            record.volume = b3ComputeCapsuleMass(&capsule, 1.f).mass;
            def.density = density(object, record.volume);
            add_shape(record, b3CreateCapsuleShape(record.body, &def, &capsule));
            break;
        }
        case PhysicsObject::Shape::Custom:
            // Box3D gives a mesh contacts only on a static body, so an
            // unanchored Custom is convex pieces of its mesh instead.
            if (object.anchored()) {
                if (b3MeshData* mesh = make_mesh(game, object, record.scale, record.center)) {
                    record.mesh = mesh;
                    const Vec3 bounds = scaled(bounds_size(mesh_points), record.scale);
                    record.volume = bounds.x * bounds.y * bounds.z;
                    def.density = density(object, record.volume);
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
                def.density = density(object, record.volume);
                // Box3D copies the hull into the shape.
                add_shape(record, b3CreateHullShape(record.body, &def, hull));
                b3DestroyHull(hull);
            }
            break;
        case PhysicsObject::Shape::Cylinder:
        case PhysicsObject::Shape::Cone:
        case PhysicsObject::Shape::Wedge:
            primitive_points(object.shape(), size, record.center, points);
            if (b3HullData* hull = build_hull(points)) {
                record.volume = b3ComputeHullMass(hull, 1.f).mass;
                def.density = density(object, record.volume);
                add_shape(record, b3CreateHullShape(record.body, &def, hull));
                b3DestroyHull(hull);
            }
            break;
        case PhysicsObject::Shape::Box:
            break;
        }
        if (record.shapes.empty()) {
            const b3BoxHull box = b3MakeOffsetBoxHull(size.x * 0.5f, size.y * 0.5f, size.z * 0.5f, center);
            record.volume = size.x * size.y * size.z;
            def.density = density(object, record.volume);
            add_shape(record, b3CreateHullShape(record.body, &def, &box.base));
        }
        b3Body_ApplyMassFromShapes(record.body);
    }

    static float density(const PhysicsBase& object, float volume) {
        return static_cast<float>(object.mass()) / std::max(volume, 1e-9f);
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
    b3MeshData* make_mesh(DataModel& game, PhysicsObject& object, Vec3 scale, Vec3 center) {
        std::string why = fitted_mesh(game, object, scale, center, true);
        b3MeshData* mesh = nullptr;
        if (why.empty()) {
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
        const std::vector<anarchy::amesh::ConvexPiece> pieces = pieces_for(*mesh, mesh_points, triangles);
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
        def.density = density(object, record.volume);
        for (b3HullData* hull : hulls) {
            // Box3D copies the hull into the shape.
            add_shape(record, b3CreateHullShape(record.body, &def, hull));
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
    static void drop_shape(Body& record) {
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
            if (rigid != nullptr && (dirty & PhysicsObject::kDirtyMaterial) != 0) {
                for (const b3ShapeId shape : record.shapes) {
                    b3Shape_SetFriction(shape, static_cast<float>(rigid->friction()));
                    b3Shape_SetRestitution(shape, static_cast<float>(rigid->bounciness()));
                }
            }
            if ((dirty & PhysicsObject::kDirtyMass) != 0) {
                for (const b3ShapeId shape : record.shapes) {
                    b3Shape_SetDensity(shape, density(object, record.volume), false);
                }
                b3Body_ApplyMassFromShapes(record.body);
            }
        }
        if ((dirty & PhysicsObject::kDirtyDamping) != 0) {
            b3Body_SetLinearDamping(record.body, static_cast<float>(object.linear_damping()));
            if (rigid != nullptr) {
                b3Body_SetAngularDamping(record.body, static_cast<float>(rigid->angular_damping()));
            }
        }
        if ((dirty & PhysicsObject::kDirtyPose) != 0) {
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

void PhysicsWorld::step(DataModel& game, double dt) { impl_->step(game, dt); }

void PhysicsWorld::follow_game_objects(DataModel& game) {
    std::vector<InstanceId> ids;
    game.physics_bodies(ids);
    for (InstanceId id : ids) {
        auto* object = dynamic_cast<PhysicsBase*>(game.instance(id));
        const InstanceId target = object != nullptr ? object->driven_game_object() : 0;
        const GameObject* driven = target != 0 ? game.game_object(target) : nullptr;
        if (driven == nullptr) {
            continue;
        }
        const bool controller = dynamic_cast<PlayerController*>(object) != nullptr;
        b3Vec3 position{};
        b3Quat rotation{};
        pose_of(controller ? upright_transform(driven->transform()) : driven->transform(), position, rotation);
        const Matrix4 followed = matrix_of(position, rotation, object->transform());
        if (!same_matrix4(followed, object->transform())) {
            object->store_simulated(followed, object->velocity());
        }
    }
}

std::size_t PhysicsWorld::body_count() const { return impl_->bodies.size(); }

bool PhysicsWorld::has_body(InstanceId id) const { return impl_->bodies.count(id) != 0; }

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
                                     Vec3 scale, const std::vector<anarchy::amesh::ConvexPiece>* pieces) {
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
        if (object.shape() == PhysicsObject::Shape::Custom && object.anchored()) {
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
