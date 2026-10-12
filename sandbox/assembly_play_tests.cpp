// Assemblies in play: welded physics bodies are one Box3D body, and what has
// no physics rides along with them.

#include "alloc_counter.hpp"
#include "physics_rig.hpp"
#include "support.hpp"

#include "Assemblies.hpp"
#include "AssetInstances.hpp"
#include "Attachment.hpp"
#include "Brush.hpp"
#include "Camera.hpp"
#include "Constraint.hpp"
#include "GameObject.hpp"
#include "Light.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "PlayerController.hpp"
#include "PropertyBag.hpp"
#include "Terrain.hpp"
#include "TerrainWorld.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

using engine_core::DataModel;
using engine_core::GameObject;
using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::RigidConstraint;
using engine_core::Vec3;
using engine_core::WeldConstraint;

using namespace physics_rig;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

WeldConstraint& weld(DataModel& game, InstanceId a, InstanceId b) {
    WeldConstraint& constraint = game.create<WeldConstraint>();
    REQUIRE_FALSE(constraint.set_pv_instance1(instance_slot(a)));
    REQUIRE_FALSE(constraint.set_pv_instance2(instance_slot(b)));
    game.set_parent(constraint.id(), workspace_of(game));
    return constraint;
}

Vec3 position_of(const Matrix4& m) { return Vec3{m.m[12], m.m[13], m.m[14]}; }

float distance(const Matrix4& a, const Matrix4& b) {
    const Vec3 pa = position_of(a);
    const Vec3 pb = position_of(b);
    return std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y) + (pa.z - pb.z) * (pa.z - pb.z));
}

// b's frame in a's, scale removed.
Matrix4 relative(const Matrix4& a, const Matrix4& b) {
    return engine_core::matrix4_multiply(engine_core::matrix4_inverse(engine_core::matrix4_orthonormalize(a)),
                                         engine_core::matrix4_orthonormalize(b));
}

bool same(const Matrix4& a, const Matrix4& b, float tolerance) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

Matrix4 turned_y(double degrees, Matrix4 where) {
    Matrix4 out = engine_core::matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, degrees * 3.14159265358979323846 / 180.0);
    out.m[12] = where.m[12];
    out.m[13] = where.m[13];
    out.m[14] = where.m[14];
    return out;
}

}  // namespace

TEST_CASE("AP1 two welded bodies are one body that falls as one and keeps its offset", "[assembly][play]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& low = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& high = rig.body(at(0.f, 8.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, low.id(), high.id());
    rig.play();
    rig.steps(1);
    REQUIRE(rig.physics.body_count() == 2);
    REQUIRE(near(rig.physics.body_mass(low.id()), 100.f, 1e-3f));
    REQUIRE(near(rig.physics.body_mass(high.id()), 100.f, 1e-3f));
    REQUIRE(rig.physics.shape_count(low.id()) == 1);
    REQUIRE(rig.physics.shape_count(high.id()) == 1);
    REQUIRE(rig.physics.body_key(low.id()) == rig.physics.body_key(high.id()));
    rig.seconds(3.0);
    INFO(y_of(low.transform()));
    REQUIRE(near(y_of(low.transform()), 0.5f, 0.05f));
    REQUIRE(near(distance(low.transform(), high.transform()), 3.f, 1e-3f));
    REQUIRE(rig.warnings.empty());
}

TEST_CASE("AP2 a GameObject, a Camera, and a Light welded to a body ride along, firing no Changed",
          "[assembly][play]") {
    ScriptRig rig;
    engine_core::PhysicsWorld physics;
    PhysicsObject& box = rig.game.create<PhysicsObject>();
    rig.game.set_name(box.id(), "Box");
    REQUIRE_FALSE(box.set_transform(at(0.f, 10.f, 0.f)));
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    engine_core::GameObject& object = rig.game.create_game_object();
    engine_core::Camera& camera = rig.game.create<engine_core::Camera>();
    engine_core::PointLight& light = rig.game.create<engine_core::PointLight>();
    engine_core::SpatialObject* riders[] = {&object, &camera, &light};
    for (int i = 1; i <= 3; ++i) {
        engine_core::SpatialObject& rider = *riders[i - 1];
        rig.game.set_name(rider.id(), ("Rider" + std::to_string(i)).c_str());
        rider.set_transform(at(static_cast<float>(i), 12.f, 0.f));
        rig.game.set_parent(rider.id(), workspace_of(rig.game));
        weld(rig.game, box.id(), rider.id());
    }
    add_script(rig.game, "Watch", R"(
        _G.changes = 0
        for i = 1, 3 do
            workspace["Rider" .. i].Changed:Connect(function(name)
                if name == "Transform" then _G.changes += 1 end
            end)
        end
    )");
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(1);
    for (int frame = 0; frame < 60; ++frame) {
        for (int i = 0; i < 4; ++i) {
            physics.step(rig.game, kStep);
        }
        rig.frames(1);
    }
    REQUIRE(y_of(box.transform()) < 9.f);
    for (int i = 1; i <= 3; ++i) {
        const InstanceId rider = rig.game.find_first_child(workspace_of(rig.game), ("Rider" + std::to_string(i)).c_str());
        const auto* placed = dynamic_cast<engine_core::PVInstance*>(rig.game.instance(rider));
        REQUIRE(placed != nullptr);
        INFO(i);
        REQUIRE(same(relative(box.transform(), placed->transform()), at(static_cast<float>(i), 2.f, 0.f), 1e-3f));
    }
    double changes = -1;
    REQUIRE(rig.runtime.global_number("changes", changes));
    REQUIRE(changes == 0);
}

TEST_CASE("AP3 a body welded to an anchored one is anchored too", "[assembly][play]") {
    PhysicsRig rig;
    PhysicsObject& anchor = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& hung = rig.body(at(0.f, 7.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, anchor.id(), hung.id());
    rig.play();
    rig.seconds(1.0);
    REQUIRE(y_of(hung.transform()) == 7.f);
    REQUIRE(rig.physics.body_mass(hung.id()) == 0.f);
}

TEST_CASE("AP4 a member's shape sits at its offset and turn, and its own scale is kept", "[assembly][play]") {
    PhysicsRig rig;
    PhysicsObject& anchor = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    GameObject& ball_object = create_part(rig.game);
    Matrix4 ball_at = turned_y(45.0, at(4.f, 3.f, 0.f));
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            ball_at.m[column * 4 + row] *= 2.f;
        }
    }
    ball_object.set_transform(ball_at);
    PhysicsObject& ball = rig.game.create<PhysicsObject>();
    REQUIRE_FALSE(ball.set_shape(static_cast<int>(PhysicsObject::Shape::Sphere)));
    rig.game.set_parent(ball.id(), ball_object.id());
    weld(rig.game, anchor.id(), ball_object.id());
    rig.play();
    rig.steps(1);
    // A sphere of Size 1 scaled by 2: radius 1 around (4, 3, 0).
    const auto hit = rig.physics.raycast(rig.game, Vec3{4.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, {});
    REQUIRE(hit);
    REQUIRE(hit->instance == ball.id());
    REQUIRE(near(hit->position.y, 4.f, 1e-3f));
    REQUIRE(near(column_length(ball_object.transform(), 0), 2.f, 1e-4f));
}

TEST_CASE("AP5 a ray reports the member it hit, not the root", "[assembly][play]") {
    PhysicsRig rig;
    PhysicsObject& heavy = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    REQUIRE_FALSE(heavy.set_mass(500.0));
    PhysicsObject& light = rig.body(at(5.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, heavy.id(), light.id());
    rig.play();
    rig.steps(1);
    const auto hit = rig.physics.raycast(rig.game, Vec3{5.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, {});
    REQUIRE(hit);
    REQUIRE(hit->instance == light.id());
}

TEST_CASE("AP6 a Custom welded to an anchored body is a mesh; unanchored, it is pieces", "[assembly][play]") {
    for (int anchored = 0; anchored < 2; ++anchored) {
        PhysicsRig rig;
        engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
        PhysicsObject& base = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, anchored == 1);
        PhysicsObject& custom = rig.body(at(3.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
        REQUIRE_FALSE(custom.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
        REQUIRE_FALSE(custom.set_mesh(instance_slot(mesh.id())));
        weld(rig.game, base.id(), custom.id());
        rig.play();
        REQUIRE_FALSE(edit_geometry(rig.game, mesh, [](anarchy::amesh::Data& data) {
            engine_core::add_box(data, Vec3{2.f, 2.f, 2.f}, Vec3{0.f, 0.f, 0.f});
        }));
        rig.steps(2);
        INFO(anchored);
        // A mesh shape adds no mass; pieces do.
        if (anchored == 1) {
            REQUIRE(rig.physics.body_mass(custom.id()) == 0.f);
            // The Scene View outlines it as its assembly is: a mesh, not its pieces.
            std::vector<Vec3> points;
            std::vector<std::uint32_t> triangles;
            REQUIRE_FALSE(mesh.vertex_positions(points, &triangles));
            std::vector<Vec3> as_mesh;
            std::vector<Vec3> as_own;
            engine_core::PhysicsWorld::collision_outline(custom, Vec3{}, points, triangles, as_mesh, Vec3{1.f, 1.f, 1.f},
                                                         nullptr, 1);
            engine_core::PhysicsWorld::collision_outline(custom, Vec3{}, points, triangles, as_own);
            REQUIRE(as_mesh.size() != as_own.size());
        } else {
            REQUIRE(rig.physics.body_mass(custom.id()) > 50.f);
        }
        REQUIRE(rig.physics.shape_count(custom.id()) >= 1);
    }
}

TEST_CASE("AP7 Stop puts every member back, and the next Test simulates again", "[assembly][play]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& low = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    GameObject& rider = create_part(rig.game);
    rider.set_transform(at(2.f, 6.f, 0.f));
    weld(rig.game, low.id(), rider.id());
    rig.play();
    rig.seconds(1.0);
    REQUIRE(y_of(rider.transform()) < 6.f);
    rig.game.stop_simulation();
    REQUIRE(y_of(rider.transform()) == 6.f);
    REQUIRE(y_of(low.transform()) == 5.f);
    rig.game.start_simulation();
    rig.seconds(0.5);
    REQUIRE(y_of(rider.transform()) < 6.f);
    REQUIRE(near(distance(low.transform(), rider.transform()), std::sqrt(5.f), 1e-3f));
}

TEST_CASE("AP9 a welded Brush falls with a box, and its AngularVelocity is written back", "[assembly][play]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::Brush& brush = rig.game.create<engine_core::Brush>();
    brush.set_anchored(false);
    REQUIRE_FALSE(brush.set_transform(at(0.f, 6.f, 0.f)));
    rig.game.set_parent(brush.id(), workspace_of(rig.game));
    PhysicsObject& box = rig.body(at(4.f, 6.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(box.set_angular_velocity(Vec3{0.f, 3.f, 0.f}));
    REQUIRE_FALSE(box.set_mass(1000.0));
    weld(rig.game, box.id(), brush.id());
    rig.play();
    rig.seconds(0.25);
    REQUIRE(y_of(brush.transform()) < 6.f);
    REQUIRE(std::fabs(brush.angular_velocity().y) > 0.5f);
    REQUIRE(near(distance(box.transform(), brush.transform()), 4.f, 1e-3f));
}

TEST_CASE("AP9b a compound body takes its root's damping", "[assembly][play]") {
    PhysicsRig rig;
    PhysicsObject& root = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(root.set_mass(500.0));
    REQUIRE_FALSE(root.set_linear_damping(0.7));
    PhysicsObject& other = rig.body(at(2.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(other.set_linear_damping(0.1));
    weld(rig.game, root.id(), other.id());
    rig.play();
    rig.steps(1);
    REQUIRE(near(rig.physics.body_damping(other.id()), 0.7f, 1e-6f));
    REQUIRE_FALSE(other.set_linear_damping(0.3));
    rig.steps(1);
    REQUIRE(near(rig.physics.body_damping(other.id()), 0.7f, 1e-6f));
}

TEST_CASE("AP9c a Simulated rider with a Velocity does not drift off its body", "[assembly][play]") {
    PhysicsRig rig;
    PhysicsObject& anchor = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    GameObject& rider = create_part(rig.game);
    rider.set_transform(at(2.f, 3.f, 0.f));
    rig.game.set_simulated(rider.id(), true);
    rider.set_linear_velocity(5.f, 0.f, 0.f);
    GameObject& loose = create_part(rig.game);
    loose.set_transform(at(0.f, 9.f, 0.f));
    rig.game.set_simulated(loose.id(), true);
    loose.set_linear_velocity(5.f, 0.f, 0.f);
    weld(rig.game, anchor.id(), rider.id());
    rig.play();
    rig.engine_steps(240);
    REQUIRE(near(x_of(rider.transform()), 2.f, 1e-4f));
    REQUIRE(x_of(loose.transform()) > 4.f);
}

TEST_CASE("AP9d both ends of a long welded pair over terrain get colliders and land", "[assembly][play]") {
    using namespace engine_core::terrain;
    PhysicsRig rig;
    auto& terrain = rig.game.create<engine_core::Terrain>();
    rig.game.set_parent(terrain.id(), workspace_of(rig.game));
    Shape slab;
    slab.kind = Shape::Kind::Block;
    slab.frame = engine_core::matrix4_translation(1024.f, -4.f, 1024.f);
    slab.size = Vec3{256.f, 8.f, 48.f};
    REQUIRE_FALSE(terrain.edit_volume([&](VoxelVolume& v) { return v.fill(slab, 1); }));
    engine_core::TerrainWorld world(engine_core::PhysicsWorld::build_terrain_collider);
    rig.physics.set_terrain_world(&world);
    for (int i = 0; i < 4; ++i) {
        world.update(rig.game);
        world.wait_idle();
        if (!world.first_build_done()) {
            i = 0;
        }
    }
    world.update(rig.game);
    // 110 units apart: past the 3-chunk collider box around the near one's origin.
    PhysicsObject& near_end = rig.body(at(965.f, 1.f, 1024.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(near_end.set_mass(500.0));
    PhysicsObject& far_end = rig.body(at(1075.f, 1.f, 1024.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, near_end.id(), far_end.id());
    rig.play();
    for (int frame = 0; frame < 120; ++frame) {
        world.update(rig.game);
        for (int step = 0; step < 4; ++step) {
            rig.physics.step(rig.game, kStep);
        }
    }
    INFO(y_of(far_end.transform()));
    REQUIRE(y_of(far_end.transform()) > 0.f);
    REQUIRE(y_of(near_end.transform()) > 0.f);
}

namespace {

// A GameObject with a PhysicsObject under it that moves it.
struct Part {
    GameObject* object = nullptr;
    PhysicsObject* body = nullptr;
};

Part add_part(DataModel& game, Matrix4 where, double mass = PhysicsObject::kDefaultMass) {
    Part part;
    part.object = &create_part(game);
    part.object->set_transform(where);
    part.body = &game.create<PhysicsObject>();
    REQUIRE_FALSE(part.body->set_mass(mass));
    game.set_parent(part.body->id(), part.object->id());
    return part;
}

}  // namespace

TEST_CASE("AP10 writing a member's Transform in play moves the whole assembly at once", "[assembly][play-move]") {
    ScriptRig rig;
    engine_core::PhysicsWorld physics;
    Part a = add_part(rig.game, at(0.f, 10.f, 0.f), 500.0);
    rig.game.set_name(a.object->id(), "A");
    Part b = add_part(rig.game, at(3.f, 10.f, 0.f));
    rig.game.set_name(b.object->id(), "B");
    weld(rig.game, a.object->id(), b.object->id());
    add_script(rig.game, "Watch", R"(
        _G.a = 0
        _G.b = 0
        workspace.A.Changed:Connect(function(name) if name == "Transform" then _G.a += 1 end end)
        workspace.B.Changed:Connect(function(name) if name == "Transform" then _G.b += 1 end end)
    )");
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(1);
    for (int i = 0; i < 40; ++i) {
        physics.step(rig.game, kStep);
    }
    b.object->set_transform(at(20.f, 30.f, 0.f));
    // At once, before any step: the root is 3 units to its -X.
    REQUIRE(near(x_of(a.object->transform()), 17.f, 1e-3f));
    REQUIRE(near(y_of(a.object->transform()), 30.f, 1e-3f));
    REQUIRE(near(x_of(a.body->transform()), 17.f, 1e-3f));
    rig.frames(1);
    physics.step(rig.game, kStep);
    REQUIRE(near(x_of(a.object->transform()), 17.f, 1e-2f));
    REQUIRE(y_of(a.object->transform()) > 29.f);
    double changes_a = -1;
    double changes_b = -1;
    REQUIRE(rig.runtime.global_number("a", changes_a));
    REQUIRE(rig.runtime.global_number("b", changes_b));
    REQUIRE(changes_a == 0);
    REQUIRE(changes_b == 1);
}

TEST_CASE("AP10b writing a member's PhysicsObject Transform moves both members and both GameObjects",
          "[assembly][play-move]") {
    for (int before_first_step = 0; before_first_step < 2; ++before_first_step) {
        PhysicsRig rig;
        Part a = add_part(rig.game, at(0.f, 10.f, 0.f), 500.0);
        Part b = add_part(rig.game, at(3.f, 10.f, 0.f));
        weld(rig.game, a.object->id(), b.object->id());
        rig.play();
        if (before_first_step == 0) {
            rig.steps(20);
        }
        REQUIRE_FALSE(b.body->set_transform(at(-10.f, 40.f, 5.f)));
        INFO(before_first_step);
        REQUIRE(near(x_of(b.object->transform()), -10.f, 1e-3f));
        REQUIRE(near(x_of(a.object->transform()), -13.f, 1e-3f));
        REQUIRE(near(z_of(a.body->transform()), 5.f, 1e-3f));
        rig.steps(1);
        REQUIRE(near(x_of(a.object->transform()), -13.f, 1e-2f));
        REQUIRE(y_of(b.object->transform()) > 39.f);
    }
}

TEST_CASE("AP10c two plain GameObjects welded move together, with no body", "[assembly][play-move]") {
    PhysicsRig rig;
    GameObject& a = create_part(rig.game);
    a.set_transform(at(0.f, 1.f, 0.f));
    GameObject& b = create_part(rig.game);
    b.set_transform(at(0.f, 1.f, 2.f));
    weld(rig.game, a.id(), b.id());
    rig.play();
    rig.steps(1);
    const std::size_t bodies = rig.physics.body_count();
    a.set_transform(at(5.f, 5.f, 5.f));
    REQUIRE(near(z_of(b.transform()), 7.f, 1e-4f));
    REQUIRE(near(x_of(b.transform()), 5.f, 1e-4f));
    rig.steps(1);
    REQUIRE(rig.physics.body_count() == bodies);
}

TEST_CASE("AP11 a member write before the first step moves the assembly, and its body starts there",
          "[assembly][play-move]") {
    PhysicsRig rig;
    Part a = add_part(rig.game, at(0.f, 10.f, 0.f), 500.0);
    Part b = add_part(rig.game, at(3.f, 10.f, 0.f));
    weld(rig.game, a.object->id(), b.object->id());
    rig.play();
    b.object->set_transform(at(50.f, 10.f, 0.f));
    REQUIRE(near(x_of(a.object->transform()), 47.f, 1e-3f));
    rig.steps(1);
    const auto position = rig.physics.body_position(a.body->id());
    REQUIRE(position);
    REQUIRE(near(position->x, 47.f, 1e-2f));
}

TEST_CASE("AP12 Velocity written through any member is the assembly's, and the last write wins",
          "[assembly][play-move]") {
    PhysicsRig rig;
    Part a = add_part(rig.game, at(0.f, 10.f, 0.f), 500.0);
    Part b = add_part(rig.game, at(3.f, 10.f, 0.f));
    weld(rig.game, a.object->id(), b.object->id());
    rig.play();
    rig.steps(1);
    REQUIRE_FALSE(a.body->set_velocity(Vec3{4.f, 0.f, 0.f}));
    REQUIRE_FALSE(b.body->set_velocity(Vec3{0.f, 10.f, 0.f}));
    // Read through the other member at once.
    REQUIRE(near(a.body->velocity().y, 10.f, 1e-4f));
    REQUIRE(near(a.body->velocity().x, 0.f, 1e-4f));
    rig.steps(1);
    REQUIRE(a.body->velocity().y > 9.f);
    REQUIRE(near(a.body->velocity().x, 0.f, 1e-3f));
    const float y = y_of(a.object->transform());
    rig.seconds(0.2);
    REQUIRE(y_of(a.object->transform()) > y);

    // A welded Brush's AngularVelocity turns the whole assembly.
    PhysicsRig spin;
    engine_core::Brush& brush = spin.game.create<engine_core::Brush>();
    brush.set_anchored(false);
    REQUIRE_FALSE(brush.set_transform(at(0.f, 10.f, 0.f)));
    spin.game.set_parent(brush.id(), workspace_of(spin.game));
    PhysicsObject& box = spin.body(at(4.f, 10.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(spin.game, brush.id(), box.id());
    spin.play();
    spin.steps(1);
    REQUIRE_FALSE(brush.set_angular_velocity(Vec3{0.f, 4.f, 0.f}));
    spin.steps(2);
    REQUIRE(std::fabs(box.angular_velocity().y) > 3.f);
}

TEST_CASE("AP13 an assembly's Velocity is its centre of mass's", "[assembly][play-move]") {
    PhysicsRig rig;
    PhysicsObject& root = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(root.set_mass(60.0));
    PhysicsObject& other = rig.body(at(4.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(other.set_mass(60.0));
    REQUIRE_FALSE(root.set_angular_velocity(Vec3{0.f, 5.f, 0.f}));
    weld(rig.game, root.id(), other.id());
    rig.play();
    rig.steps(1);
    const auto before = rig.physics.body_center(root.id());
    REQUIRE(before);
    REQUIRE(near(before->x, 2.f, 1e-3f));
    rig.steps(24);
    const auto after = rig.physics.body_center(root.id());
    REQUIRE(after);
    REQUIRE(near(after->x, 2.f, 1e-2f));
    REQUIRE(near(after->z, 0.f, 1e-2f));
    REQUIRE(near(root.velocity().x, 0.f, 1e-2f));
    REQUIRE(near(root.velocity().z, 0.f, 1e-2f));
}

TEST_CASE("AP14 a PlayerController carrying a heavier crate stays root, hovers, and stays upright",
          "[assembly][play-move]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::PlayerController& player = rig.controller(at(0.f, 0.5f, 0.f));
    Part crate = add_part(rig.game, at(1.5f, 1.5f, 0.f), 500.0);
    weld(rig.game, player.id(), crate.object->id());
    rig.game.assemblies().refresh(rig.game);
    REQUIRE(rig.game.assemblies().root_of(rig.game, crate.object->id()) == player.id());
    rig.play();
    rig.seconds(1.0);
    // At rest its feet, its origin, are on the floor, and its cylinder floats its hover gap above.
    INFO(y_of(player.transform()));
    REQUIRE(near(y_of(player.transform()), 0.f, 0.05f));
    REQUIRE(player.on_ground());
    // Tilted, the crate's write leaves the player upright and puts the crate
    // back where it rides.
    const Matrix4 tilted = engine_core::matrix4_multiply(
        at(1.5f, 3.f, 0.f), engine_core::matrix4_axis_angle(Vec3{1.f, 0.f, 0.f}, 30.0 * 3.14159265 / 180.0));
    crate.object->set_transform(tilted);
    REQUIRE(near(player.transform().m[5], 1.f, 1e-4f));
    REQUIRE(near(crate.object->transform().m[5], 1.f, 1e-4f));
}

TEST_CASE("AP15 a PlayerController presses its whole assembly's weight into what it stands on",
          "[assembly][play-move]") {
    float tip[2] = {0.f, 0.f};
    for (int loaded = 0; loaded < 2; ++loaded) {
        PhysicsRig rig;
        rig.floor();
        // A plank balanced on a ridge along Z, as C13 has it.
        rig.body(at(0.f, 0.25f, 0.f), Vec3{0.2f, 0.5f, 4.f}, true);
        PhysicsObject& plank = rig.body(at(0.f, 0.6f, 0.f), Vec3{6.f, 0.2f, 2.f}, false);
        REQUIRE_FALSE(plank.set_mass(10.0));
        engine_core::PlayerController& c = rig.controller(at(2.5f, 1.2f, 0.f));
        REQUIRE_FALSE(c.set_mass(5.0));
        if (loaded == 1) {
            // A heavy crate held over the plank's end, touching nothing.
            PhysicsObject& crate = rig.body(at(2.5f, 4.f, 0.f), Vec3{0.5f, 0.5f, 0.5f}, false);
            REQUIRE_FALSE(crate.set_mass(200.0));
            weld(rig.game, c.id(), crate.id());
        }
        rig.play();
        // Just after it lands, before either end reaches the floor: how fast it tips.
        rig.seconds(0.4);
        tip[loaded] = plank.transform().m[1];
    }
    INFO(tip[0] << " " << tip[1]);
    REQUIRE(tip[1] < tip[0] - 0.05f);
}

TEST_CASE("AP16 a PlayerController that is not its assembly's root is never on ground", "[assembly][play-move]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::PlayerController& first = rig.controller(at(0.f, 0.5f, 0.f));
    engine_core::PlayerController& second = rig.controller(at(2.f, 0.5f, 0.f));
    REQUIRE_FALSE(first.set_mass(80.0));
    weld(rig.game, first.id(), second.id());
    rig.play();
    rig.seconds(1.0);
    REQUIRE(first.on_ground());
    REQUIRE_FALSE(second.on_ground());
    REQUIRE_FALSE(second.is_sliding());
}

TEST_CASE("AP16c a PlayerController jumps when its welded crate's Velocity is written upward",
          "[assembly][play-move]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::PlayerController& player = rig.controller(at(0.f, 0.5f, 0.f));
    PhysicsObject& crate = rig.body(at(1.5f, 1.f, 0.f), Vec3{0.5f, 0.5f, 0.5f}, false);
    weld(rig.game, player.id(), crate.id());
    rig.play();
    rig.seconds(1.0);
    const float rest = y_of(player.transform());
    REQUIRE_FALSE(crate.set_velocity(Vec3{0.f, 20.f, 0.f}));
    rig.seconds(0.2);
    REQUIRE(y_of(player.transform()) > rest + 1.f);
}



TEST_CASE("AP17 disabling a weld mid-flight splits the body, and each piece keeps its motion",
          "[assembly][play-change]") {
    for (int spinning = 0; spinning < 2; ++spinning) {
        PhysicsRig rig;
        PhysicsObject& a = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
        PhysicsObject& b = rig.body(at(4.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
        if (spinning == 1) {
            REQUIRE_FALSE(a.set_angular_velocity(Vec3{0.f, 0.f, 4.f}));
        }
        WeldConstraint& w = weld(rig.game, a.id(), b.id());
        rig.play();
        rig.steps(30);
        const auto center = rig.physics.body_center(a.id());
        REQUIRE(center);
        // The old body's velocity at each piece's centre, the box's own origin.
        const Vec3 omega = a.angular_velocity();
        const Vec3 v = a.velocity();
        const auto point_velocity = [&](const PhysicsObject& piece) {
            const Vec3 p = position_of(piece.transform());
            const Vec3 r{p.x - center->x, p.y - center->y, p.z - center->z};
            return Vec3{v.x + omega.y * r.z - omega.z * r.y, v.y + omega.z * r.x - omega.x * r.z,
                        v.z + omega.x * r.y - omega.y * r.x};
        };
        const Vec3 expect_a = point_velocity(a);
        const Vec3 expect_b = point_velocity(b);
        REQUIRE_FALSE(w.set_enabled(false));
        rig.steps(1);
        INFO(spinning);
        REQUIRE(rig.physics.body_key(a.id()) != rig.physics.body_key(b.id()));
        // One step of gravity, 1/240 s at 9.81, on top of what each piece had.
        const float g = 9.81f / 240.f;
        REQUIRE(near(a.velocity().x, expect_a.x, 2e-2f));
        REQUIRE(near(a.velocity().y, expect_a.y - g, 2e-2f));
        REQUIRE(near(b.velocity().x, expect_b.x, 2e-2f));
        REQUIRE(near(b.velocity().y, expect_b.y - g, 2e-2f));
    }
}

TEST_CASE("AP18 enabling a weld between two falling boxes makes one body and keeps their momentum",
          "[assembly][play-change]") {
    PhysicsRig rig;
    PhysicsObject& a = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(a.set_mass(30.0));
    REQUIRE_FALSE(a.set_velocity(Vec3{6.f, 0.f, 0.f}));
    PhysicsObject& b = rig.body(at(4.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(b.set_mass(10.0));
    WeldConstraint& w = weld(rig.game, a.id(), b.id());
    REQUIRE_FALSE(w.set_enabled(false));
    rig.play();
    rig.steps(10);
    const float vy = a.velocity().y;
    REQUIRE_FALSE(w.set_enabled(true));
    rig.steps(1);
    REQUIRE(rig.physics.body_key(a.id()) == rig.physics.body_key(b.id()));
    // (30 * 6 + 10 * 0) / 40.
    REQUIRE(near(a.velocity().x, 4.5f, 1e-2f));
    REQUIRE(near(b.velocity().x, 4.5f, 1e-2f));
    REQUIRE(near(a.velocity().y, vy - 9.81f / 240.f, 1e-2f));
}

TEST_CASE("AP19 un-anchoring the anchored member makes a welded Custom collide as pieces",
          "[assembly][play-change]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
    PhysicsObject& base = rig.body(at(0.f, 4.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& custom = rig.body(at(3.f, 4.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(custom.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(custom.set_mesh(instance_slot(mesh.id())));
    weld(rig.game, base.id(), custom.id());
    rig.play();
    REQUIRE_FALSE(edit_geometry(rig.game, mesh, [](anarchy::amesh::Data& data) {
        engine_core::add_box(data, Vec3{2.f, 2.f, 2.f}, Vec3{0.f, 0.f, 0.f});
    }));
    rig.steps(2);
    REQUIRE(rig.physics.body_mass(custom.id()) == 0.f);
    base.set_anchored(false);
    rig.seconds(3.0);
    REQUIRE(rig.physics.body_mass(custom.id()) > 50.f);
    // It landed on the floor, a 2 unit cube resting on it, not through it.
    INFO(y_of(custom.transform()));
    REQUIRE(y_of(custom.transform()) > 0.5f);
}

TEST_CASE("AP20 destroying a member mid-fall leaves the survivor its own body and shape", "[assembly][play-change]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& a = rig.body(at(0.f, 8.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& b = rig.body(at(3.f, 8.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, a.id(), b.id());
    rig.play();
    rig.steps(20);
    rig.game.destroy(b.id());
    rig.steps(1);
    REQUIRE(rig.physics.has_body(a.id()));
    REQUIRE(rig.physics.shape_count(a.id()) == 1);
    REQUIRE(near(rig.physics.body_mass(a.id()), 50.f, 1e-3f));
    rig.steps(20);
    rig.game.stop_simulation();
    rig.physics.sync(rig.game);
    REQUIRE(y_of(a.transform()) == 8.f);
}

TEST_CASE("AP21 a RigidConstraint connected in play snaps the sub's whole assembly, then holds it",
          "[assembly][play-change]") {
    PhysicsRig rig;
    PhysicsObject& parent = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& sub = rig.body(at(10.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& rider = rig.body(at(12.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, sub.id(), rider.id());
    RigidConstraint& r = rig.game.create<RigidConstraint>();
    rig.game.set_parent(r.id(), workspace_of(rig.game));
    REQUIRE_FALSE(r.set_pv_instance1(instance_slot(parent.id())));
    rig.play();
    rig.steps(5);
    REQUIRE_FALSE(r.set_pv_instance2(instance_slot(sub.id())));
    rig.steps(1);
    // The sub's pivot is on the parent's, and its welded rider kept its offset.
    REQUIRE(near(x_of(sub.transform()), 0.f, 1e-3f));
    REQUIRE(near(y_of(sub.transform()), 50.f, 1e-3f));
    REQUIRE(near(x_of(rider.transform()), 2.f, 1e-3f));
    rig.seconds(0.5);
    // Anchored now, all three.
    REQUIRE(near(y_of(rider.transform()), 50.f, 1e-3f));
    REQUIRE(rig.physics.body_key(sub.id()) == rig.physics.body_key(parent.id()));
}

TEST_CASE("AP22 sliding an end Attachment's Offset in play moves only the sub's side", "[assembly][play-change]") {
    PhysicsRig rig;
    rig.floor();
    // A wide anchored slab, its top at y = 1, and a box resting on it.
    PhysicsObject& slab = rig.body(at(0.f, 0.5f, 0.f), Vec3{10.f, 1.f, 10.f}, true);
    PhysicsObject& resting = rig.body(at(-3.f, 1.5f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    engine_core::Attachment& slot = rig.game.create<engine_core::Attachment>();
    REQUIRE_FALSE(slot.set_offset(at(0.f, 3.f, 0.f)));
    rig.game.set_parent(slot.id(), slab.id());
    PhysicsObject& door = rig.body(at(0.f, 3.5f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    RigidConstraint& r = rig.game.create<RigidConstraint>();
    rig.game.set_parent(r.id(), workspace_of(rig.game));
    REQUIRE_FALSE(r.set_pv_instance1(instance_slot(slot.id())));
    REQUIRE_FALSE(r.set_pv_instance2(instance_slot(door.id())));
    rig.play();
    rig.seconds(0.5);
    const std::uint64_t slab_shape = rig.physics.shape_key(slab.id());
    const std::uint64_t door_shape = rig.physics.shape_key(door.id());
    for (int i = 1; i <= 60; ++i) {
        REQUIRE_FALSE(slot.set_offset(at(0.05f * static_cast<float>(i), 3.f, 0.f)));
        rig.steps(1);
    }
    REQUIRE(rig.physics.shape_key(slab.id()) == slab_shape);
    REQUIRE(rig.physics.shape_key(door.id()) != door_shape);
    REQUIRE(near(x_of(door.transform()), 3.f, 1e-3f));
    REQUIRE(near(resting.velocity().y, 0.f, 0.05f));
    REQUIRE(near(y_of(resting.transform()), 1.5f, 0.02f));
}

TEST_CASE("AP23 a face edit on a welded Brush in play moves no other member", "[assembly][play-change]") {
    PhysicsRig rig;
    engine_core::Brush& brush = rig.game.create<engine_core::Brush>();
    REQUIRE_FALSE(brush.set_transform(at(0.f, 5.f, 0.f)));
    rig.game.set_parent(brush.id(), workspace_of(rig.game));
    PhysicsObject& box = rig.body(at(5.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, brush.id(), box.id());
    rig.play();
    rig.steps(2);
    REQUIRE_FALSE(brush.apply(engine_core::brush::move_face(brush.faces(), 0, 2.0)));
    rig.steps(2);
    REQUIRE(near(x_of(box.transform()), 5.f, 1e-3f));
    REQUIRE(near(y_of(box.transform()), 5.f, 1e-3f));
}

TEST_CASE("AP24 Stop puts every member back after a play that snapped and slid", "[assembly][play-change]") {
    PhysicsRig rig;
    PhysicsObject& parent = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    engine_core::Attachment& slot = rig.game.create<engine_core::Attachment>();
    rig.game.set_parent(slot.id(), parent.id());
    PhysicsObject& sub = rig.body(at(10.f, 40.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    RigidConstraint& r = rig.game.create<RigidConstraint>();
    rig.game.set_parent(r.id(), workspace_of(rig.game));
    REQUIRE_FALSE(r.set_pv_instance1(instance_slot(slot.id())));
    REQUIRE_FALSE(r.set_enabled(false));
    REQUIRE_FALSE(r.set_pv_instance2(instance_slot(sub.id())));
    const Matrix4 authored = sub.transform();
    rig.play();
    rig.steps(5);
    REQUIRE_FALSE(r.set_enabled(true));
    rig.steps(5);
    REQUIRE_FALSE(slot.set_offset(at(2.f, 0.f, 0.f)));
    rig.steps(5);
    REQUIRE(near(x_of(sub.transform()), 2.f, 1e-3f));
    rig.game.stop_simulation();
    REQUIRE(same(sub.transform(), authored, 0.f));
    REQUIRE(same(slot.offset(), engine_core::matrix4_identity(), 0.f));
    REQUIRE_FALSE(r.enabled());
}

TEST_CASE("AP25 a member that leaves Workspace in play leaves its assembly, and both keep moving",
          "[assembly][play-change]") {
    PhysicsRig rig;
    PhysicsObject& a = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    GameObject& rider = create_part(rig.game);
    rider.set_transform(at(3.f, 50.f, 0.f));
    PhysicsObject& b = rig.body(at(-3.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    weld(rig.game, a.id(), rider.id());
    weld(rig.game, a.id(), b.id());
    rig.play();
    rig.steps(20);
    rig.game.set_parent(b.id(), rig.game.scene_service("Storage"));
    rig.steps(1);
    REQUIRE_FALSE(rig.physics.has_body(b.id()));
    const float y = y_of(a.transform());
    rig.steps(20);
    REQUIRE(y_of(a.transform()) < y);
    REQUIRE(near(distance(a.transform(), rider.transform()), 3.f, 1e-3f));
}

TEST_CASE("AP16b un-anchoring a seat makes the welded PlayerController root, and it hovers", "[assembly][play-change]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::PlayerController& player = rig.controller(at(0.f, 0.f, 0.f));
    PhysicsObject& seat = rig.body(at(1.5f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    weld(rig.game, player.id(), seat.id());
    rig.play();
    rig.steps(5);
    rig.game.assemblies().refresh(rig.game);
    REQUIRE(rig.game.assemblies().root_of(rig.game, player.id()) == seat.id());
    seat.set_anchored(false);
    rig.seconds(1.0);
    rig.game.assemblies().refresh(rig.game);
    REQUIRE(rig.game.assemblies().root_of(rig.game, player.id()) == player.id());
    REQUIRE(player.on_ground());
    REQUIRE(near(y_of(player.transform()), 0.f, 0.05f));
}

TEST_CASE("AP28 a RigidConstraint whose sub a script destroyed is Active again after Stop",
          "[assembly][play-change]") {
    PhysicsRig rig;
    PhysicsObject& parent = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& sub = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    const InstanceId sub_id = sub.id();
    RigidConstraint& r = rig.game.create<RigidConstraint>();
    rig.game.set_parent(r.id(), workspace_of(rig.game));
    REQUIRE_FALSE(r.set_pv_instance1(instance_slot(parent.id())));
    REQUIRE_FALSE(r.set_pv_instance2(instance_slot(sub_id)));
    rig.play();
    rig.steps(5);
    rig.game.destroy(sub_id);
    rig.steps(5);
    rig.game.stop_simulation();
    rig.game.assemblies().refresh(rig.game);
    REQUIRE(rig.game.assemblies().active(r.id()));
    REQUIRE(r.pv_instance2_id() == sub_id);
}

TEST_CASE("AP26 with assemblies warm, a step allocates nothing", "[assembly][alloc]") {
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    SKIP("Debug containers allocate proxies; run in Release");
#endif
    PhysicsRig rig;
    rig.floor();
    std::vector<PhysicsObject*> pairs;
    for (int i = 0; i < 20; ++i) {
        const float x = static_cast<float>(i) * 4.f;
        PhysicsObject& low = rig.body(at(x, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
        PhysicsObject& high = rig.body(at(x, 8.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
        weld(rig.game, low.id(), high.id());
        GameObject& rider = create_part(rig.game);
        rider.set_transform(at(x + 1.f, 6.f, 0.f));
        weld(rig.game, low.id(), rider.id());
        pairs.push_back(&low);
    }
    // Bodies that move GameObjects: PhysicsWorld settles who moves each one every sync.
    for (int i = 0; i < 10; ++i) {
        GameObject& part = create_part(rig.game);
        part.set_transform(at(static_cast<float>(i) * 4.f, 3.f, 10.f));
        PhysicsObject& body = rig.game.create<PhysicsObject>();
        rig.game.set_parent(body.id(), part.id());
    }
    rig.play();
    // Warm: landed and settled, every contact Box3D will keep already made,
    // and one round of the writes below already done.
    rig.engine_steps(720);
    for (int i = 0; i < 5; ++i) {
        pairs[static_cast<std::size_t>(i)]->set_transform(at(static_cast<float>(i) * 4.f, 20.f, 0.f));
        pairs[static_cast<std::size_t>(i)]->set_velocity(Vec3{0.f, 1.f, 0.f});
        rig.engine_steps(1);
    }
    rig.engine_steps(2400);
    engine_core::PhysicsWorld::count_box3d_allocations(true);
    const std::size_t box3d_before = engine_core::PhysicsWorld::box3d_allocations();
    alloc_counter::arm();
    rig.engine_steps(240);
    // Box3D's own allocations are counted over plain steps only: moving a
    // body (SetTransform) grows Box3D's broadphase, welded or not.
    const std::size_t box3d = engine_core::PhysicsWorld::box3d_allocations() - box3d_before;
    engine_core::PhysicsWorld::count_box3d_allocations(false);
    for (int i = 0; i < 5; ++i) {
        pairs[static_cast<std::size_t>(i)]->set_transform(at(static_cast<float>(i) * 4.f, 20.f, 0.f));
        pairs[static_cast<std::size_t>(i)]->set_velocity(Vec3{0.f, 1.f, 0.f});
        rig.engine_steps(1);
    }
    alloc_counter::disarm();
    INFO(alloc_counter::count() << " heap, " << box3d << " Box3D");
    REQUIRE(alloc_counter::count() == 0);
    REQUIRE(box3d == 0);
}

TEST_CASE("AP27 a moved slot queues one redraw, so many riders cannot overflow the queue", "[assembly][alloc]") {
    PhysicsRig rig;
    PhysicsObject& box = rig.body(at(0.f, 500.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    for (int i = 0; i < 2100; ++i) {
        GameObject& rider = create_part(rig.game);
        rider.set_transform(at(static_cast<float>(i % 50), 501.f, static_cast<float>(i / 50)));
        weld(rig.game, box.id(), rider.id());
    }
    rig.play();
    rig.steps(1);
    rig.game.invalidations().clear();
    rig.steps(32);
    REQUIRE_FALSE(rig.game.invalidations().overflow());
}


TEST_CASE("AP29 a RigidConstraint connected in play with the root named second moves only the other side",
          "[assembly][play-change]") {
    PhysicsRig rig;
    PhysicsObject& root = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& other = rig.body(at(10.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    RigidConstraint& r = rig.game.create<RigidConstraint>();
    rig.game.set_parent(r.id(), workspace_of(rig.game));
    REQUIRE_FALSE(r.set_pv_instance1(instance_slot(other.id())));
    rig.play();
    rig.steps(5);
    REQUIRE_FALSE(r.set_pv_instance2(instance_slot(root.id())));
    rig.steps(1);
    REQUIRE(near(x_of(root.transform()), 0.f, 1e-3f));
    REQUIRE(near(y_of(root.transform()), 50.f, 1e-3f));
    REQUIRE(near(x_of(other.transform()), 0.f, 1e-3f));
    REQUIRE(rig.physics.body_key(other.id()) == rig.physics.body_key(root.id()));
}

TEST_CASE("AP30 in play, a body's Transform write moves its GameObject at once, so a weld made then holds it there",
          "[assembly][play]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    GameObject& person = create_part(rig.game);
    person.set_transform(at(5.f, 0.f, 0.f));
    PlayerController& feet = rig.controller(at(5.f, 0.f, 0.f), person.id());
    rig.play();
    rig.seconds(0.5);
    feet.set_enabled(false);
    REQUIRE_FALSE(feet.set_transform(at(0.f, 1.f, 0.f)));
    // The GameObject it moves says so before any step.
    REQUIRE(near(x_of(person.transform()), 0.f, 1e-4f));
    REQUIRE(near(y_of(person.transform()), 1.f, 1e-4f));
    weld(rig.game, box.id(), feet.id());
    rig.seconds(1.0);
    // Held where it was written: one unit above the box's centre.
    REQUIRE(near(x_of(feet.transform()) - x_of(box.transform()), 0.f, 0.02f));
    REQUIRE(near(y_of(feet.transform()) - y_of(box.transform()), 0.5f, 0.02f));
}

TEST_CASE("AP31 a Massless body welded to another adds no mass, nor wins its root by Mass", "[assembly][play][massless]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& chassis = rig.body(at(0.f, 1.f, 0.f), Vec3{2.f, 0.5f, 4.f}, false);
    REQUIRE_FALSE(chassis.set_mass(50.0));
    PhysicsObject& trim = rig.body(at(0.f, 1.5f, 0.f), Vec3{4.f, 0.5f, 4.f}, false);
    REQUIRE_FALSE(trim.set_mass(100.0));
    trim.set_massless(true);
    weld(rig.game, chassis.id(), trim.id());
    // Heavier and larger, it still does not root the assembly.
    REQUIRE(rig.game.assemblies().root_of(rig.game, trim.id()) == chassis.id());
    rig.play();
    rig.engine_steps(2);
    REQUIRE(near(rig.physics.body_mass(chassis.id()), 50.f, 0.5f));
    // Weighing again, it adds its Mass, and roots the assembly by it.
    trim.set_massless(false);
    rig.engine_steps(2);
    REQUIRE(near(rig.physics.body_mass(chassis.id()), 150.f, 0.5f));
    REQUIRE(rig.game.assemblies().root_of(rig.game, chassis.id()) == trim.id());
}

TEST_CASE("AP32 a Massless body alone keeps its Mass, and falls", "[assembly][play][massless]") {
    PhysicsRig rig;
    PhysicsObject& box = rig.body(at(0.f, 10.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(box.set_mass(20.0));
    box.set_massless(true);
    rig.play();
    rig.seconds(0.5);
    REQUIRE(near(rig.physics.body_mass(box.id()), 20.f, 0.5f));
    REQUIRE(y_of(box.transform()) < 9.f);
}

TEST_CASE("AP33 in an assembly of Massless bodies, the root weighs its Mass and the rest nothing",
          "[assembly][play][massless]") {
    PhysicsRig rig;
    PhysicsObject& big = rig.body(at(0.f, 10.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(big.set_mass(30.0));
    big.set_massless(true);
    engine_core::Brush& brush = rig.game.create<engine_core::Brush>();
    brush.set_anchored(false);
    REQUIRE_FALSE(brush.set_transform(at(2.f, 10.f, 0.f)));
    rig.game.set_parent(brush.id(), workspace_of(rig.game));
    REQUIRE_FALSE(brush.set_mass(70.0));
    brush.set_massless(true);
    weld(rig.game, big.id(), brush.id());
    // Both ranked massless: the larger roots it, and weighs its Mass alone.
    const InstanceId root = rig.game.assemblies().root_of(rig.game, brush.id());
    REQUIRE((root == big.id() || root == brush.id()));
    const float expected = root == big.id() ? 30.f : 70.f;
    rig.play();
    rig.seconds(0.5);
    REQUIRE(near(rig.physics.body_mass(big.id()), expected, 0.5f));
    REQUIRE(y_of(big.transform()) < 9.f);
}

TEST_CASE("AP34 made Massless in play, a root hands the assembly to another, and weighs nothing there",
          "[assembly][play][massless]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& heavy = rig.body(at(0.f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(heavy.set_mass(100.0));
    PhysicsObject& light = rig.body(at(1.f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(light.set_mass(10.0));
    weld(rig.game, heavy.id(), light.id());
    rig.play();
    rig.engine_steps(2);
    REQUIRE(near(rig.physics.body_mass(heavy.id()), 110.f, 0.5f));
    heavy.set_massless(true);
    rig.engine_steps(2);
    REQUIRE(rig.game.assemblies().root_of(rig.game, heavy.id()) == light.id());
    REQUIRE(near(rig.physics.body_mass(light.id()), 10.f, 0.5f));
    // And back: it roots again, with its Mass.
    heavy.set_massless(false);
    rig.engine_steps(2);
    REQUIRE(rig.game.assemblies().root_of(rig.game, light.id()) == heavy.id());
    REQUIRE(near(rig.physics.body_mass(light.id()), 110.f, 0.5f));
}

TEST_CASE("AP35 Massless starts false, saves only when on, and comes back at Stop", "[assembly][massless]") {
    PhysicsRig rig;
    REQUIRE(engine_core::lua_class_find("PhysicsBase", "Massless") != nullptr);
    PhysicsObject& box = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(box.massless());
    engine_core::PropertyBag saved;
    box.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Massless") == nullptr);
    box.set_massless(true);
    saved.clear();
    box.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Massless") != nullptr);
    box.set_massless(false);
    rig.play();
    box.set_massless(true);
    rig.game.stop_simulation();
    REQUIRE_FALSE(box.massless());
}
