// RigidConstraint in the editor: the sub's pivot follows its parent's pivot,
// live, in the same call and the same undo step as the edit that moved it.

#include "physics_rig.hpp"
#include "support.hpp"

#include "Assemblies.hpp"
#include "Attachment.hpp"
#include "Brush.hpp"
#include "Camera.hpp"
#include "Constraint.hpp"
#include "GameObject.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "Project.hpp"
#include "SceneService.hpp"
#include "brush/BrushGeometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

using engine_core::Assemblies;
using engine_core::Attachment;
using engine_core::DataModel;
using engine_core::GameObject;
using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::RigidConstraint;
using engine_core::Vec3;
using engine_core::WeldConstraint;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

Matrix4 at(float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); }

Matrix4 turned(Vec3 axis, double degrees, Matrix4 where) {
    Matrix4 out = engine_core::matrix4_axis_angle(axis, degrees * 3.14159265358979323846 / 180.0);
    out.m[12] = where.m[12];
    out.m[13] = where.m[13];
    out.m[14] = where.m[14];
    return out;
}

Matrix4 scaled(Matrix4 m, float sx, float sy, float sz) {
    const float s[3] = {sx, sy, sz};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            m.m[column * 4 + row] *= s[column];
        }
    }
    return m;
}

float column_length(const Matrix4& m, int column) {
    const float* axis = m.m + column * 4;
    return std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
}

// Same position and rotation, scale ignored.
bool same_pose(const Matrix4& a, const Matrix4& b, float tolerance = 1e-4f) {
    const Matrix4 pa = engine_core::matrix4_orthonormalize(a);
    const Matrix4 pb = engine_core::matrix4_orthonormalize(b);
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(pa.m[i] - pb.m[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

// A GameObject in Workspace with a PhysicsObject under it that moves it.
struct Part {
    GameObject* object = nullptr;
    PhysicsObject* body = nullptr;
};

Part add_part(DataModel& game, Matrix4 where) {
    Part part;
    part.object = &create_part(game);
    part.object->set_transform(where);
    part.body = &game.create<PhysicsObject>();
    game.set_parent(part.body->id(), part.object->id());
    return part;
}

RigidConstraint& rigid(DataModel& game, InstanceId parent, InstanceId sub) {
    RigidConstraint& constraint = game.create<RigidConstraint>();
    game.set_parent(constraint.id(), workspace_of(game));
    REQUIRE_FALSE(constraint.set_pv_instance1(instance_slot(parent)));
    REQUIRE_FALSE(constraint.set_pv_instance2(instance_slot(sub)));
    return constraint;
}

Attachment& add_attachment(DataModel& game, InstanceId parent, Matrix4 offset) {
    Attachment& attachment = game.create<Attachment>();
    REQUIRE_FALSE(attachment.set_offset(offset));
    game.set_parent(attachment.id(), parent);
    return attachment;
}

int undo_all(DataModel& game) {
    int steps = 0;
    while (game.history().can_undo().first) {
        game.history().undo();
        ++steps;
    }
    return steps;
}

}  // namespace

TEST_CASE("AE1 connecting a RigidConstraint snaps the sub, and one undo puts it back", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, turned({0, 1, 0}, 30.0, at(0.f, 5.f, 0.f)));
    Part b = add_part(game, at(10.f, 0.f, 0.f));
    RigidConstraint& r = game.create<RigidConstraint>();
    game.set_parent(r.id(), workspace_of(game));
    REQUIRE_FALSE(r.set_pv_instance1(instance_slot(a.object->id())));
    REQUIRE(same_pose(b.object->transform(), at(10.f, 0.f, 0.f)));
    begin_step(game, "Set PVInstance2");
    REQUIRE_FALSE(r.set_pv_instance2(instance_slot(b.object->id())));
    end_step(game);
    REQUIRE(same_pose(b.object->transform(), a.object->transform()));
    game.history().undo();
    REQUIRE(same_pose(b.object->transform(), at(10.f, 0.f, 0.f)));
    REQUIRE(r.pv_instance2_id() == 0);
}

TEST_CASE("AE2 moving the parent moves the sub in the same undo step, whichever half each end names",
          "[assembly][follow]") {
    for (int halves = 0; halves < 2; ++halves) {
        SimRole role;
        engine_core::Game game;
        Part a = add_part(game, at(0.f, 5.f, 0.f));
        Part b = add_part(game, at(3.f, 0.f, 0.f));
        if (halves == 0) {
            rigid(game, a.object->id(), b.object->id());
        } else {
            rigid(game, a.body->id(), b.body->id());
        }
        REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
        game.history().reset_waypoints();
        begin_step(game, "Move");
        a.object->set_transform(turned({1, 0, 0}, 45.0, at(7.f, 1.f, 2.f)));
        end_step(game);
        REQUIRE(same_pose(b.object->transform(), a.object->transform()));
        REQUIRE(undo_all(game) == 1);
        REQUIRE(same_pose(a.object->transform(), at(0.f, 5.f, 0.f)));
        REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
    }
}

TEST_CASE("AE3 a sub moved on its own snaps straight back", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    rigid(game, a.object->id(), b.object->id());
    b.object->set_transform(at(20.f, 20.f, 20.f));
    REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
}

TEST_CASE("AE4 an Attachment parent end puts the sub at its frame, and moving the Attachment moves the sub",
          "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, turned({0, 0, 1}, 90.0, at(0.f, 5.f, 0.f)));
    Attachment& point = add_attachment(game, a.object->id(), at(0.f, 2.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    rigid(game, point.id(), b.object->id());
    const Matrix4 expected = engine_core::matrix4_multiply(a.object->transform(), at(0.f, 2.f, 0.f));
    REQUIRE(same_pose(b.object->transform(), expected));
    REQUIRE_FALSE(point.set_offset(at(0.f, 3.f, 0.f)));
    REQUIRE(same_pose(b.object->transform(), engine_core::matrix4_multiply(a.object->transform(), at(0.f, 3.f, 0.f))));
}

TEST_CASE("AE5 a World-space Attachment sub is matched by position only", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, turned({0, 1, 0}, 90.0, at(0.f, 5.f, 0.f)));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    Attachment& grip = add_attachment(game, b.object->id(), at(1.f, 0.f, 0.f));
    REQUIRE_FALSE(grip.set_offset_space(static_cast<int>(engine_core::TransformSpace::World)));
    rigid(game, a.object->id(), grip.id());
    const Vec3 position = engine_core::matrix4_position(b.object->transform());
    REQUIRE(std::fabs(position.x - -1.f) < 1e-4f);
    REQUIRE(std::fabs(position.y - 5.f) < 1e-4f);
    REQUIRE(std::fabs(position.z) < 1e-4f);
    REQUIRE(same_pose(b.object->transform(), at(-1.f, 5.f, 0.f)));
}

TEST_CASE("AE6 a PlayerController sub under a tilted pivot settles upright in one write", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, turned({1, 0, 0}, 30.0, at(0.f, 5.f, 0.f)));
    engine_core::PlayerController& player = game.create<engine_core::PlayerController>();
    game.set_parent(player.id(), workspace_of(game));
    rigid(game, a.object->id(), player.id());
    const Matrix4 placed = player.transform();
    REQUIRE(same_pose(placed, at(0.f, 5.f, 0.f)));
    REQUIRE(game.assemblies().follow(game, 0) == 0);
}

TEST_CASE("AE7 a chain follows in order in one call", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 0.f, 0.f));
    Part b = add_part(game, at(5.f, 0.f, 0.f));
    Attachment& on_b = add_attachment(game, b.object->id(), at(0.f, 0.f, 4.f));
    Part c = add_part(game, at(9.f, 0.f, 0.f));
    rigid(game, a.object->id(), b.object->id());
    rigid(game, on_b.id(), c.object->id());
    a.object->set_transform(turned({0, 1, 0}, 90.0, at(1.f, 2.f, 3.f)));
    REQUIRE(same_pose(b.object->transform(), a.object->transform()));
    REQUIRE(same_pose(c.object->transform(), engine_core::matrix4_multiply(b.object->transform(), at(0.f, 0.f, 4.f))));
}

TEST_CASE("AE8 what is under a sub comes along, except what rides its parent already", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    GameObject& wheel_cap = game.create_game_object();
    wheel_cap.set_transform(at(3.f, 1.f, 0.f));
    game.set_parent(wheel_cap.id(), b.object->id());
    Attachment& mark = add_attachment(game, b.object->id(), at(0.f, 0.f, 1.f));
    rigid(game, a.object->id(), b.object->id());
    // b moved by (-3, 5, 0); the cap went with it.
    REQUIRE(same_pose(wheel_cap.transform(), at(0.f, 6.f, 0.f)));
    REQUIRE(same_pose(mark.offset(), at(0.f, 0.f, 1.f)));
}

TEST_CASE("AE9 a WeldConstraint moves nothing while editing", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    WeldConstraint& weld = game.create<WeldConstraint>();
    game.set_parent(weld.id(), workspace_of(game));
    REQUIRE_FALSE(weld.set_pv_instance1(instance_slot(a.object->id())));
    REQUIRE_FALSE(weld.set_pv_instance2(instance_slot(b.object->id())));
    a.object->set_transform(at(9.f, 9.f, 9.f));
    REQUIRE(same_pose(b.object->transform(), at(3.f, 0.f, 0.f)));
}

TEST_CASE("AE10 the view camera moves freely as a sub and moves nothing as a parent", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    engine_core::Camera& camera = game.create<engine_core::Camera>();
    game.set_parent(camera.id(), workspace_of(game));
    auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->set_current_camera(camera.id()));
    rigid(game, a.object->id(), camera.id());
    camera.set_transform(at(40.f, 40.f, 40.f));
    REQUIRE(same_pose(camera.transform(), at(40.f, 40.f, 40.f)));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    rigid(game, camera.id(), b.object->id());
    camera.set_transform(at(50.f, 0.f, 0.f));
    REQUIRE(same_pose(b.object->transform(), at(3.f, 0.f, 0.f)));
    REQUIRE(same_pose(a.object->transform(), at(0.f, 5.f, 0.f)));
}

TEST_CASE("AE11 writing a driven PhysicsObject's own Transform moves no sub", "[assembly][follow]") {
    physics_rig::PhysicsRig rig;
    Part a = add_part(rig.game, at(0.f, 5.f, 0.f));
    Part b = add_part(rig.game, at(3.f, 0.f, 0.f));
    rigid(rig.game, a.object->id(), b.object->id());
    rig.sync_steps(1);
    REQUIRE_FALSE(a.body->set_transform(at(30.f, 0.f, 0.f)));
    REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
    rig.sync_steps(1);
    REQUIRE(same_pose(a.object->transform(), at(0.f, 5.f, 0.f)));
    REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
}

TEST_CASE("AE12 a consistent place writes nothing on the stopped pass", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    rigid(game, a.object->id(), b.object->id());
    game.history().reset_waypoints();
    REQUIRE(game.assemblies().follow(game, 0) == 0);
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("AE13 a suspended scope snaps nothing, and the next pass does", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    rigid(game, a.object->id(), b.object->id());
    {
        engine_core::AssemblySuspend suspend(game);
        a.object->set_transform(at(8.f, 0.f, 0.f));
        REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
    }
    REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
    REQUIRE(game.assemblies().follow(game, 0) == 1);
    REQUIRE(same_pose(b.object->transform(), at(8.f, 0.f, 0.f)));
}

TEST_CASE("AE14 a saved snapped pair loads snapped", "[assembly][follow]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        DataModel& game = project.datamodel();
        Part a = add_part(game, turned({0, 1, 0}, 20.0, at(0.f, 5.f, 0.f)));
        game.set_name(a.object->id(), "A");
        Part b = add_part(game, at(3.f, 0.f, 0.f));
        game.set_name(b.object->id(), "B");
        rigid(game, a.object->id(), b.object->id());
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    REQUIRE(game.assemblies().follow(game, 0) == 0);
}

TEST_CASE("AE15 undo and redo of a parent move add no steps of their own", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    rigid(game, a.object->id(), b.object->id());
    game.history().reset_waypoints();
    begin_step(game, "Move");
    a.object->set_transform(at(6.f, 6.f, 6.f));
    end_step(game);
    game.history().undo();
    REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
    REQUIRE_FALSE(game.history().can_undo().first);
    game.history().redo();
    REQUIRE(same_pose(b.object->transform(), at(6.f, 6.f, 6.f)));
    REQUIRE_FALSE(game.history().can_redo().first);
    REQUIRE(undo_all(game) == 1);
}

TEST_CASE("AE16 undoing a deleted constraint or sub brings the snap back", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    RigidConstraint& r = rigid(game, a.object->id(), b.object->id());
    const InstanceId constraint = r.id();
    game.history().reset_waypoints();
    begin_step(game, "Delete");
    game.destroy(constraint);
    end_step(game);
    a.object->set_transform(at(9.f, 0.f, 0.f));  // no recording: not undoable
    REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
    game.history().undo();
    // The undo itself made no follow.
    REQUIRE(same_pose(b.object->transform(), at(0.f, 5.f, 0.f)));
    game.assemblies().refresh(game);
    REQUIRE(game.assemblies().active(constraint));
    REQUIRE(game.assemblies().follow(game, 0) == 1);
    REQUIRE(same_pose(b.object->transform(), at(9.f, 0.f, 0.f)));

    const InstanceId sub = b.object->id();
    begin_step(game, "Delete sub");
    game.destroy(sub);
    end_step(game);
    game.history().undo();
    REQUIRE(same_pose(b.object->transform(), at(9.f, 0.f, 0.f)));
    game.assemblies().refresh(game);
    REQUIRE(game.assemblies().active(constraint));
    a.object->set_transform(at(1.f, 1.f, 1.f));
    REQUIRE(same_pose(b.object->transform(), at(1.f, 1.f, 1.f)));
}

TEST_CASE("AE17 a 100-link chain follows its head in one call and undoes in one step", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    std::vector<Part> chain;
    for (int i = 0; i < 100; ++i) {
        chain.push_back(add_part(game, at(static_cast<float>(i), 0.f, 0.f)));
    }
    for (int i = 0; i + 1 < 100; ++i) {
        rigid(game, chain[i].object->id(), chain[i + 1].object->id());
    }
    REQUIRE(same_pose(chain[99].object->transform(), at(0.f, 0.f, 0.f)));
    game.history().reset_waypoints();
    begin_step(game, "Move");
    chain[0].object->set_transform(at(0.f, 50.f, 0.f));
    end_step(game);
    for (const Part& part : chain) {
        REQUIRE(same_pose(part.object->transform(), at(0.f, 50.f, 0.f)));
    }
    REQUIRE(undo_all(game) == 1);
    REQUIRE(same_pose(chain[99].object->transform(), at(0.f, 0.f, 0.f)));
}

TEST_CASE("AE18 pivots ignore scale, and the sub keeps its own", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, scaled(turned({0, 1, 0}, 30.0, at(0.f, 5.f, 0.f)), 2.f, 1.f, 3.f));
    Part b = add_part(game, scaled(at(3.f, 0.f, 0.f), 0.5f, 0.5f, 0.5f));
    rigid(game, a.object->id(), b.object->id());
    for (int column = 0; column < 3; ++column) {
        REQUIRE(std::fabs(column_length(b.object->transform(), column) - 0.5f) < 1e-4f);
    }
    REQUIRE(same_pose(b.object->transform(), a.object->transform()));
}

TEST_CASE("AE19 a Brush sub that recentres is snapped again in the same call", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    engine_core::Brush& brush = game.create<engine_core::Brush>();
    game.set_parent(brush.id(), workspace_of(game));
    rigid(game, a.object->id(), brush.id());
    REQUIRE(same_pose(brush.transform(), at(0.f, 5.f, 0.f)));
    REQUIRE_FALSE(brush.apply(engine_core::brush::move_face(brush.faces(), 0, 2.0)));
    REQUIRE(same_pose(brush.transform(), at(0.f, 5.f, 0.f)));
}

TEST_CASE("AE20 a carried child that is itself a sub snaps back onto its own parent", "[assembly][follow]") {
    SimRole role;
    engine_core::Game game;
    Part a = add_part(game, at(0.f, 5.f, 0.f));
    Part b = add_part(game, at(3.f, 0.f, 0.f));
    Part c = add_part(game, at(-4.f, 0.f, 0.f));
    GameObject& d = game.create_game_object();
    d.set_transform(at(3.f, 2.f, 0.f));
    game.set_parent(d.id(), b.object->id());
    rigid(game, c.object->id(), d.id());
    REQUIRE(same_pose(d.transform(), at(-4.f, 0.f, 0.f)));
    rigid(game, a.object->id(), b.object->id());
    a.object->set_transform(at(10.f, 10.f, 10.f));
    REQUIRE(same_pose(b.object->transform(), at(10.f, 10.f, 10.f)));
    REQUIRE(same_pose(d.transform(), at(-4.f, 0.f, 0.f)));
}
