#include "PhysicsWorld.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "GameObject.hpp"
#include "PhysicsObject.hpp"

#pragma warning(push, 0)
#include "box3d/box3d.h"
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine_core {
namespace {

constexpr float kGravity = -9.81f;
// The most vertices a Hull asks Box3D to keep, then a second try with fewer:
// a hull past 128 faces or edges fails, and fewer vertices make fewer.
constexpr int kHullVertices = 64;
constexpr int kHullVerticesRetry = 32;

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

}  // namespace

struct PhysicsWorld::Impl {
    struct Body {
        b3BodyId body = b3_nullBodyId;
        b3ShapeId shape = b3_nullShapeId;
        // The shape's volume, which turns Mass into a density.
        float volume = 1.f;
        // An anchored Custom's triangles, which its shape points into.
        b3MeshData* mesh = nullptr;
        // The GameObject it moves, or 0, and the Transform it last gave it.
        InstanceId driven = 0;
        Matrix4 driven_pose{};
        std::uint64_t seen = 0;
    };

    b3WorldId world = b3_nullWorldId;
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
        def.gravity = b3Vec3{0.f, kGravity, 0.f};
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
        reconcile(game);
        b3World_Step(world, static_cast<float>(dt), 1);
        pull(game);
    }

    // ---- Which PhysicsObjects have bodies ------------------------------

    void reconcile(DataModel& game) {
        ++pass;
        game.physics_bodies(eligible);
        wanted.clear();
        claims.clear();
        bool shared = false;
        for (InstanceId id : eligible) {
            const auto* object = dynamic_cast<PhysicsObject*>(game.instance(id));
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
            auto* object = dynamic_cast<PhysicsObject*>(game.instance(id));
            if (target != 0 && claims[target] != id) {
                if (!object->warned_shared) {
                    object->warned_shared = true;
                    say("PhysicsObject " + game.name(id) + " has no body: an earlier PhysicsObject already moves " +
                        game.name(target));
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
            if (const auto* object = dynamic_cast<const PhysicsObject*>(game.instance(id))) {
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

    void keep(DataModel& game, PhysicsObject& object, InstanceId target) {
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
        follow_driven(game, object, body);
    }

    // ---- Bodies -------------------------------------------------------

    std::unordered_map<InstanceId, Body>::iterator create(DataModel& game, PhysicsObject& object, InstanceId target) {
        object.take_dirty();
        Body record;
        record.driven = target;
        Matrix4 start = object.transform();
        if (const GameObject* driven = target != 0 ? game.game_object(target) : nullptr) {
            start = driven->transform();
            record.driven_pose = start;
        }
        b3BodyDef def = b3DefaultBodyDef();
        def.type = object.anchored() ? b3_staticBody : b3_dynamicBody;
        pose_of(start, def.position, def.rotation);
        def.linearVelocity = to_b3(object.velocity());
        def.angularVelocity = to_b3(object.angular_velocity());
        def.linearDamping = static_cast<float>(object.linear_damping());
        def.angularDamping = static_cast<float>(object.angular_damping());
        def.userData = user_data(object.id());
        record.body = b3CreateBody(world, &def);
        make_shape(game, object, record);
        // Its Transform says where the body is from the start.
        if (target != 0) {
            object.store_simulated(matrix_of(def.position, def.rotation, object.transform()), object.velocity(),
                                   object.angular_velocity());
        }
        return bodies.emplace(object.id(), record).first;
    }

    // Puts the object's Shape on the body, replacing any shape it had, with
    // Friction, Bounciness, and a density that gives it its Mass.
    void make_shape(DataModel& game, PhysicsObject& object, Body& record) {
        drop_shape(record);
        b3ShapeDef def = b3DefaultShapeDef();
        def.baseMaterial.friction = static_cast<float>(object.friction());
        def.baseMaterial.restitution = static_cast<float>(object.bounciness());
        def.userData = user_data(object.id());
        def.updateBodyMass = false;
        const Vec3 size = object.size();
        switch (object.shape()) {
        case PhysicsObject::Shape::Sphere: {
            const b3Sphere sphere{b3Vec3{0.f, 0.f, 0.f}, size.x * 0.5f};
            record.volume = b3ComputeSphereMass(&sphere, 1.f).mass;
            def.density = density(object, record.volume);
            record.shape = b3CreateSphereShape(record.body, &def, &sphere);
            break;
        }
        case PhysicsObject::Shape::Capsule: {
            const float radius = size.x * 0.5f;
            const float half = std::max(size.y - size.x, 0.f) * 0.5f;
            const b3Capsule capsule{b3Vec3{0.f, -half, 0.f}, b3Vec3{0.f, half, 0.f}, radius};
            record.volume = b3ComputeCapsuleMass(&capsule, 1.f).mass;
            def.density = density(object, record.volume);
            record.shape = b3CreateCapsuleShape(record.body, &def, &capsule);
            break;
        }
        case PhysicsObject::Shape::Custom:
            // Box3D gives a mesh contacts only on a static body, so an
            // unanchored Custom is a hull of its mesh until it is anchored.
            if (object.anchored()) {
                if (b3MeshData* mesh = make_mesh(game, object)) {
                    record.mesh = mesh;
                    record.volume = size.x * size.y * size.z;
                    def.density = density(object, record.volume);
                    record.shape = b3CreateMeshShape(record.body, &def, mesh, b3Vec3{1.f, 1.f, 1.f});
                }
                break;
            }
            if (!object.warned_custom) {
                object.warned_custom = true;
                say("PhysicsObject " + game.name(object.id()) +
                    ": a Custom collides as its whole mesh only while Anchored; until then it is a Hull of it");
            }
            [[fallthrough]];
        case PhysicsObject::Shape::Hull:
            if (b3HullData* hull = make_hull(game, object)) {
                record.volume = b3ComputeHullMass(hull, 1.f).mass;
                def.density = density(object, record.volume);
                // Box3D copies the hull into the shape.
                record.shape = b3CreateHullShape(record.body, &def, hull);
                b3DestroyHull(hull);
            }
            break;
        case PhysicsObject::Shape::Box:
            break;
        }
        if (!b3Shape_IsValid(record.shape)) {
            const b3BoxHull box = b3MakeBoxHull(size.x * 0.5f, size.y * 0.5f, size.z * 0.5f);
            record.volume = size.x * size.y * size.z;
            def.density = density(object, record.volume);
            record.shape = b3CreateHullShape(record.body, &def, &box.base);
        }
        b3Body_ApplyMassFromShapes(record.body);
    }

    static float density(const PhysicsObject& object, float volume) {
        return static_cast<float>(object.mass()) / std::max(volume, 1e-9f);
    }

    // The Mesh's points into points, fitted to Size around the body's
    // origin, and with triangles, its triangles into triangles. Returns why
    // there are none, or an empty string.
    std::string fitted_mesh(DataModel& game, const PhysicsObject& object, bool with_triangles) {
        const InstanceId mesh_id = object.mesh_id();
        const auto* mesh = mesh_id != 0 ? dynamic_cast<const Mesh*>(game.instance(mesh_id)) : nullptr;
        if (mesh == nullptr) {
            return "it has no Mesh";
        }
        if (std::optional<std::string> error =
                mesh->vertex_positions(mesh_points, with_triangles ? &triangles : nullptr)) {
            return *error;
        }
        Vec3 low = mesh_points.front();
        Vec3 high = low;
        for (const Vec3& p : mesh_points) {
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
        const Vec3 size = object.size();
        const Vec3 center{(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
        auto fit = [](float target, float extent) { return extent > 1e-6f ? target / extent : 1.f; };
        const Vec3 scale{fit(size.x, high.x - low.x), fit(size.y, high.y - low.y), fit(size.z, high.z - low.z)};
        points.clear();
        for (const Vec3& p : mesh_points) {
            points.push_back(b3Vec3{(p.x - center.x) * scale.x, (p.y - center.y) * scale.y, (p.z - center.z) * scale.z});
        }
        return {};
    }

    // A hull of the Mesh's points. Null, with one warning, when there is no
    // hull to build.
    b3HullData* make_hull(DataModel& game, PhysicsObject& object) {
        std::string why = fitted_mesh(game, object, false);
        b3HullData* hull = nullptr;
        if (why.empty()) {
            const int count = static_cast<int>(points.size());
            hull = b3CreateHull(points.data(), count, kHullVertices);
            if (hull == nullptr) {
                hull = b3CreateHull(points.data(), count, kHullVerticesRetry);
            }
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
    b3MeshData* make_mesh(DataModel& game, PhysicsObject& object) {
        std::string why = fitted_mesh(game, object, true);
        b3MeshData* mesh = nullptr;
        if (why.empty()) {
            indices.assign(triangles.begin(), triangles.end());
            b3MeshDef def{};
            def.vertices = points.data();
            def.indices = indices.data();
            def.vertexCount = static_cast<int>(points.size());
            def.triangleCount = static_cast<int>(indices.size() / 3);
            // A mesh's faces each have their own corners; welding joins them
            // so edges between triangles are known.
            def.weldVertices = true;
            def.weldTolerance = 1e-4f;
            def.identifyEdges = true;
            mesh = b3CreateMesh(&def, nullptr, 0);
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

    static std::string shape_name(const PhysicsObject& object) {
        return object.shape() == PhysicsObject::Shape::Custom ? "Custom" : "Hull";
    }

    // Destroys the record's shape, and the mesh it held, if any.
    static void drop_shape(Body& record) {
        if (b3Shape_IsValid(record.shape)) {
            b3DestroyShape(record.shape, false);
        }
        record.shape = b3_nullShapeId;
        if (record.mesh != nullptr) {
            b3DestroyMesh(record.mesh);
            record.mesh = nullptr;
        }
    }

    // What writes changed since the last step, into the body.
    void push(DataModel& game, PhysicsObject& object, Body& record) {
        const std::uint32_t dirty = object.take_dirty();
        if (dirty == 0) {
            return;
        }
        // Anchoring a Custom turns its hull into its whole mesh, and back. The
        // old shape goes first, so a mesh is never on a dynamic body.
        const bool custom_type = (dirty & PhysicsObject::kDirtyType) != 0 &&
                                 object.shape() == PhysicsObject::Shape::Custom;
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
            if ((dirty & PhysicsObject::kDirtyMaterial) != 0 && b3Shape_IsValid(record.shape)) {
                b3Shape_SetFriction(record.shape, static_cast<float>(object.friction()));
                b3Shape_SetRestitution(record.shape, static_cast<float>(object.bounciness()));
            }
            if ((dirty & PhysicsObject::kDirtyMass) != 0 && b3Shape_IsValid(record.shape)) {
                b3Shape_SetDensity(record.shape, density(object, record.volume), true);
            }
        }
        if ((dirty & PhysicsObject::kDirtyDamping) != 0) {
            b3Body_SetLinearDamping(record.body, static_cast<float>(object.linear_damping()));
            b3Body_SetAngularDamping(record.body, static_cast<float>(object.angular_damping()));
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
            b3Body_SetLinearVelocity(record.body, to_b3(object.velocity()));
            b3Body_SetAngularVelocity(record.body, to_b3(object.angular_velocity()));
        }
        if ((dirty & ~PhysicsObject::kDirtyMaterial) != 0 && !object.anchored()) {
            b3Body_SetAwake(record.body, true);
        }
    }

    // A driven GameObject that is not where physics last put it was moved by
    // a script or Properties: the body jumps there.
    void follow_driven(DataModel& game, PhysicsObject& object, Body& record) {
        const GameObject* driven = record.driven != 0 ? game.game_object(record.driven) : nullptr;
        if (driven == nullptr) {
            return;
        }
        const Matrix4 now = driven->transform();
        if (same_matrix4(now, record.driven_pose)) {
            return;
        }
        b3Vec3 position{};
        b3Quat rotation{};
        pose_of(now, position, rotation);
        b3Body_SetTransform(record.body, position, rotation);
        if (!object.anchored()) {
            b3Body_SetAwake(record.body, true);
        }
        record.driven_pose = now;
        object.store_simulated(matrix_of(position, rotation, object.transform()), object.velocity(),
                               object.angular_velocity());
    }

    // What Box3D moved, back onto the instances.
    void pull(DataModel& game) {
        const b3BodyEvents events = b3World_GetBodyEvents(world);
        for (int index = 0; index < events.moveCount; ++index) {
            const b3BodyMoveEvent& event = events.moveEvents[index];
            const InstanceId id = id_of(event.userData);
            const auto found = bodies.find(id);
            auto* object = dynamic_cast<PhysicsObject*>(game.instance(id));
            if (found == bodies.end() || object == nullptr) {
                continue;
            }
            Body& record = found->second;
            const b3Vec3 position = event.transform.p;
            const b3Quat rotation = event.transform.q;
            object->store_simulated(matrix_of(position, rotation, object->transform()),
                                    from_b3(b3Body_GetLinearVelocity(record.body)),
                                    from_b3(b3Body_GetAngularVelocity(record.body)));
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

std::size_t PhysicsWorld::body_count() const { return impl_->bodies.size(); }

bool PhysicsWorld::has_body(InstanceId id) const { return impl_->bodies.count(id) != 0; }

void PhysicsWorld::set_warning_sink(std::function<void(const std::string&)> sink) { impl_->warn = std::move(sink); }

}  // namespace engine_core
