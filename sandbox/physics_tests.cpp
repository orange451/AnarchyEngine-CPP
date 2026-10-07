// PhysicsObject, the rigid body Box3D simulates while it is in Workspace and
// the place plays, and PhysicsWorld, which steps it and moves its GameObject.

#include "physics_rig.hpp"
#include "support.hpp"

#include "AssetInstances.hpp"
#include "ConvexDecomposition.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "LuaApi.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "Project.hpp"
#include "SceneService.hpp"
#include "PropertyBag.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

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

// An open-topped box, 4 by 3 by 4, its floor half a unit thick and its walls
// too, standing on y = 0 in the Mesh's space.
void add_cup(anarchy::amesh::Data& data) {
    engine_core::add_box(data, Vec3{4.f, 0.5f, 4.f}, Vec3{0.f, 0.25f, 0.f});
    engine_core::add_box(data, Vec3{0.5f, 3.f, 4.f}, Vec3{-1.75f, 1.5f, 0.f});
    engine_core::add_box(data, Vec3{0.5f, 3.f, 4.f}, Vec3{1.75f, 1.5f, 0.f});
    engine_core::add_box(data, Vec3{3.f, 3.f, 0.5f}, Vec3{0.f, 1.5f, -1.75f});
    engine_core::add_box(data, Vec3{3.f, 3.f, 0.5f}, Vec3{0.f, 1.5f, 1.75f});
}

// An unanchored Custom cup of size, its bottom on the floor, and a ball of
// diameter 1 to drop into it at x.
struct CupScene {
    PhysicsObject* cup = nullptr;
    PhysicsObject* ball = nullptr;
    engine_core::Mesh* mesh = nullptr;
};

CupScene cup_scene(PhysicsRig& rig, Vec3 size, float ball_x = 0.f) {
    rig.floor();
    CupScene scene;
    scene.mesh = &rig.game.create<engine_core::Mesh>();
    scene.cup = &rig.body(at(0.f, size.y * 0.5f, 0.f), size, false);
    REQUIRE_FALSE(scene.cup->set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(scene.cup->set_mesh(instance_slot(scene.mesh->id())));
    REQUIRE_FALSE(scene.cup->set_mass(20.0));
    scene.ball = &rig.body(at(ball_x, 8.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(scene.ball->set_shape(static_cast<int>(PhysicsObject::Shape::Sphere)));
    return scene;
}

}  // namespace

TEST_CASE("P1 a dynamic box falls and comes to rest on an anchored floor", "[physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    rig.seconds(0.25);
    REQUIRE(y_of(box.transform()) < 5.f);
    REQUIRE(box.velocity().y < 0.f);
    rig.seconds(3.0);
    INFO(y_of(box.transform()));
    REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));
    REQUIRE(rig.physics.body_count() == 2);
}

TEST_CASE("P2 an anchored body does not move", "[physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    const Matrix4 before = floor.transform();
    rig.play();
    rig.seconds(1.0);
    REQUIRE(engine_core::same_matrix4(floor.transform(), before));
}

TEST_CASE("P3 only a PhysicsObject in Workspace has a body", "[physics]") {
    PhysicsRig rig;
    PhysicsObject& box = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    rig.steps(10);
    REQUIRE(rig.physics.has_body(box.id()));

    rig.game.set_parent(box.id(), rig.game.scene_service("Storage"));
    rig.steps(1);
    REQUIRE_FALSE(rig.physics.has_body(box.id()));
    const float held = y_of(box.transform());
    rig.steps(30);
    REQUIRE(y_of(box.transform()) == held);

    rig.game.set_parent(box.id(), workspace_of(rig.game));
    rig.steps(30);
    REQUIRE(rig.physics.has_body(box.id()));
    REQUIRE(y_of(box.transform()) < held);
}

TEST_CASE("P4 moving a Folder out of Workspace takes its bodies out", "[physics]") {
    PhysicsRig rig;
    engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(folder.id(), workspace_of(rig.game));
    PhysicsObject& box = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false, folder.id());
    rig.play();
    rig.steps(1);
    REQUIRE(rig.physics.has_body(box.id()));
    rig.game.set_parent(folder.id(), rig.game.scene_service("Storage"));
    rig.steps(1);
    REQUIRE_FALSE(rig.physics.has_body(box.id()));
}

TEST_CASE("P5 a GameObject seeds the body and follows it, keeping its scale", "[physics]") {
    PhysicsRig rig;
    rig.floor();
    GameObject& part = create_part(rig.game);
    Matrix4 scaled = at(3.f, 10.f, 0.f);
    scaled.m[0] = scaled.m[5] = scaled.m[10] = 2.f;
    part.set_transform(scaled);
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.play();
    rig.steps(1);
    REQUIRE(near(x_of(body.transform()), 3.f, 1e-4f));
    rig.seconds(0.5);
    REQUIRE(y_of(part.transform()) < 10.f);
    REQUIRE(near(y_of(part.transform()), y_of(body.transform()), 1e-5f));
    REQUIRE(near(column_length(part.transform(), 0), 2.f, 1e-4f));
    REQUIRE(near(column_length(part.transform(), 1), 2.f, 1e-4f));
}

TEST_CASE("P6 the first PhysicsObject in tree order drives a shared GameObject", "[physics]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 20.f, 0.f));
    PhysicsObject& first = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& second = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(first.set_game_object(instance_slot(part.id())));
    REQUIRE_FALSE(second.set_game_object(instance_slot(part.id())));
    rig.play();
    rig.steps(5);
    REQUIRE(rig.physics.has_body(first.id()));
    REQUIRE_FALSE(rig.physics.has_body(second.id()));
    REQUIRE(rig.warnings.size() == 1);
    REQUIRE(rig.warnings.front().find("no body") != std::string::npos);
    rig.steps(5);
    REQUIRE(rig.warnings.size() == 1);

    rig.game.destroy(first.id());
    rig.steps(1);
    REQUIRE(rig.physics.has_body(second.id()));
}

TEST_CASE("P7 writing a Transform during play teleports the body", "[physics]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 20.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.play();
    rig.steps(10);

    REQUIRE_FALSE(body.set_transform(at(7.f, 30.f, 0.f)));
    rig.steps(1);
    REQUIRE(near(x_of(body.transform()), 7.f, 1e-3f));
    REQUIRE(near(x_of(part.transform()), 7.f, 1e-3f));
    REQUIRE(y_of(part.transform()) > 29.f);

    part.set_transform(at(-4.f, 40.f, 0.f));
    rig.steps(1);
    REQUIRE(near(x_of(body.transform()), -4.f, 1e-3f));
    REQUIRE(y_of(body.transform()) > 39.f);
}

TEST_CASE("P8 writing Velocity during play sets the body's velocity", "[physics]") {
    PhysicsRig rig;
    PhysicsObject& body = rig.body(at(0.f, 100.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    rig.steps(1);
    REQUIRE_FALSE(body.set_velocity(Vec3{12.f, 0.f, 0.f}));
    rig.seconds(0.5);
    REQUIRE(near(x_of(body.transform()), 6.f, 0.2f));
    REQUIRE(near(body.velocity().x, 12.f, 0.1f));
}

TEST_CASE("P9 Stop puts back what was authored, and the next Test simulates again", "[physics]") {
    PhysicsRig rig;
    rig.floor();
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 8.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.play();
    rig.seconds(1.0);
    REQUIRE(y_of(part.transform()) < 8.f);
    rig.game.stop_simulation();
    REQUIRE(y_of(part.transform()) == 8.f);
    REQUIRE(engine_core::same_matrix4(body.transform(), engine_core::matrix4_identity()));
    REQUIRE(body.velocity().y == 0.f);

    rig.game.start_simulation();
    rig.steps(1);
    REQUIRE(rig.physics.has_body(body.id()));
    rig.seconds(0.5);
    REQUIRE(y_of(part.transform()) < 8.f);
}

TEST_CASE("P10 physics moves fire no Changed", "[physics]") {
    ScriptRig rig;
    engine_core::PhysicsWorld physics;
    add_script(rig.game, "Watch", R"(
        local body = Instance.new("PhysicsObject")
        body.Name = "Body"
        body.Transform = Matrix4.new(0, 10, 0)
        body.Parent = workspace
        _G.changes = 0
        body.Changed:Connect(function(name) _G.changes = _G.changes + 1 end)
    )");
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(1);
    for (int frame = 0; frame < 30; ++frame) {
        for (int i = 0; i < 4; ++i) {
            physics.step(rig.game, kStep);
        }
        rig.frames(1);
    }
    const InstanceId body = rig.game.find_first_child(workspace_of(rig.game), "Body");
    REQUIRE(body != 0);
    const auto* object = dynamic_cast<PhysicsObject*>(rig.game.instance(body));
    REQUIRE(object != nullptr);
    REQUIRE(y_of(object->transform()) < 10.f);
    double changes = -1;
    REQUIRE(rig.runtime.global_number("changes", changes));
    REQUIRE(changes == 0);
}

TEST_CASE("P11 a Hull from a Mesh rests like a box; no Mesh falls back to a Box", "[physics]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
    PhysicsObject& hull = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(hull.set_shape(static_cast<int>(PhysicsObject::Shape::Hull)));
    REQUIRE_FALSE(hull.set_mesh(instance_slot(mesh.id())));
    PhysicsObject& bare = rig.body(at(5.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(bare.set_shape(static_cast<int>(PhysicsObject::Shape::Hull)));
    rig.play();
    // A unit box in the Mesh's session geometry: the hull fits it to Size, 2.
    REQUIRE_FALSE(mesh.edit_geometry([](anarchy::amesh::Data& data) {
        engine_core::add_box(data, Vec3{1.f, 1.f, 1.f}, Vec3{0.f, 0.f, 0.f});
    }));
    rig.seconds(3.0);
    INFO(y_of(hull.transform()));
    REQUIRE(near(y_of(hull.transform()), 1.f, 0.05f));
    REQUIRE(near(y_of(bare.transform()), 0.5f, 0.05f));
    REQUIRE(rig.warnings.size() == 1);
    REQUIRE(rig.warnings.front().find("Hull fell back to Box") != std::string::npos);
}

TEST_CASE("P12 Shape, Size, Mass, and Anchored change a body during play", "[physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& body = rig.body(at(0.f, 10.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    rig.seconds(0.5);
    body.set_anchored(true);
    rig.steps(1);
    const float held = y_of(body.transform());
    rig.seconds(0.5);
    REQUIRE(y_of(body.transform()) == held);

    REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Sphere)));
    REQUIRE_FALSE(body.set_size(Vec3{3.f, 3.f, 3.f}));
    REQUIRE_FALSE(body.set_mass(50.0));
    body.set_anchored(false);
    rig.seconds(4.0);
    INFO(y_of(body.transform()));
    REQUIRE(near(y_of(body.transform()), 1.5f, 0.05f));
}

TEST_CASE("P15 a PhysicsObject under a GameObject moves it until it is moved elsewhere", "[physics]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 20.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
    REQUIRE(body.driven_game_object() == part.id());
    rig.play();
    rig.seconds(0.5);
    REQUIRE(y_of(part.transform()) < 20.f);
    REQUIRE(near(y_of(part.transform()), y_of(body.transform()), 1e-5f));
    // The link is the parent, never written to the GameObject property.
    REQUIRE(body.game_object().kind == engine_core::LuaSlot::Kind::Nil);

    rig.game.set_parent(body.id(), workspace_of(rig.game));
    rig.steps(1);
    REQUIRE(body.driven_game_object() == 0);
    REQUIRE(rig.physics.has_body(body.id()));
    const float left = y_of(part.transform());
    rig.seconds(0.5);
    REQUIRE(y_of(part.transform()) == left);
    REQUIRE(y_of(body.transform()) < left);

    // An explicit link wins over the parent.
    GameObject& other = create_part(rig.game);
    other.set_transform(at(10.f, 30.f, 0.f));
    rig.game.set_parent(body.id(), part.id());
    REQUIRE_FALSE(body.set_game_object(instance_slot(other.id())));
    rig.steps(1);
    REQUIRE(body.driven_game_object() == other.id());
    rig.seconds(0.25);
    REQUIRE(y_of(other.transform()) < 30.f);
    REQUIRE(y_of(part.transform()) == left);
}

TEST_CASE("P16 an anchored Custom collides as its whole mesh, and still falls when unanchored", "[physics]") {
    PhysicsRig rig;
    engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
    // A floor with a step: only a whole-mesh collider has the gap between the two.
    PhysicsObject& ground = rig.body(at(0.f, -0.5f, 0.f), Vec3{20.f, 2.f, 20.f}, true);
    REQUIRE_FALSE(ground.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(ground.set_mesh(instance_slot(mesh.id())));
    PhysicsObject& on_low = rig.body(at(5.f, 4.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& on_high = rig.body(at(-5.f, 4.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    // Two slabs, x from -10 to 0 one unit higher than x from 0 to 10, so the
    // whole surface fitted to Size runs from y -1.5 to 0.5 with its top step at
    // x = 0: the high slab's top is 0.5, the low one's -0.5.
    REQUIRE_FALSE(mesh.edit_geometry([](anarchy::amesh::Data& data) {
        engine_core::add_box(data, Vec3{10.f, 2.f, 20.f}, Vec3{-5.f, 0.f, 0.f});
        engine_core::add_box(data, Vec3{10.f, 1.f, 20.f}, Vec3{5.f, -0.5f, 0.f});
    }));
    rig.seconds(3.0);
    INFO(y_of(on_low.transform()) << " " << y_of(on_high.transform()));
    REQUIRE(near(y_of(on_high.transform()), 1.f, 0.05f));
    REQUIRE(near(y_of(on_low.transform()), 0.f, 0.05f));
    REQUIRE(rig.warnings.empty());

    // Unanchored, it falls as convex pieces of the same mesh, with no warning.
    PhysicsObject& loose = rig.body(at(30.f, 10.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(loose.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(loose.set_mesh(instance_slot(mesh.id())));
    rig.seconds(0.5);
    REQUIRE(y_of(loose.transform()) < 10.f);
    REQUIRE(rig.warnings.empty());
    // Anchoring it makes it its whole mesh, and holds it there.
    loose.set_anchored(true);
    rig.steps(1);
    const float held = y_of(loose.transform());
    rig.seconds(0.25);
    REQUIRE(y_of(loose.transform()) == held);
    REQUIRE(rig.warnings.empty());
}

TEST_CASE("P13 PhysicsObject properties are checked, saved, and come back at Stop", "[physics]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("PhysicsObject"));
    PhysicsObject& body = game.create<PhysicsObject>();
    game.set_parent(body.id(), workspace_of(game));

    engine_core::PropertyBag saved;
    body.save_properties(saved);
    REQUIRE(saved.empty());

    REQUIRE_FALSE(body.set_mass(0.0));
    REQUIRE(body.mass() == PhysicsObject::kMinMass);
    REQUIRE(*body.set_mass(std::nan("")) == "Mass must be a finite number");
    REQUIRE_FALSE(body.set_friction(-1.0));
    REQUIRE(body.friction() == 0.0);
    REQUIRE_FALSE(body.set_size(Vec3{0.f, 2.f, 3.f}));
    REQUIRE(body.size().x == PhysicsObject::kMinSize);
    REQUIRE(body.set_shape(9).has_value());
    REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Capsule)));
    REQUIRE_FALSE(body.set_transform(at(1.f, 2.f, 3.f)));

    engine_core::PropertyBag changed;
    body.save_properties(changed);
    const engine_core::JsonValue* shape = engine_core::bag_find(changed, "Shape");
    REQUIRE(shape != nullptr);
    REQUIRE(shape->is_string());
    REQUIRE(shape->as_string() == "Capsule");
    const engine_core::JsonValue* transform = engine_core::bag_find(changed, "Transform");
    REQUIRE(transform != nullptr);
    REQUIRE(transform->is_array());
    REQUIRE(transform->items().size() == 16);

    // A load reads the item's name back, and refuses one the type does not have.
    PhysicsObject& copy = game.create<PhysicsObject>();
    std::string error;
    REQUIRE(copy.load_property("Shape", engine_core::JsonValue::string("Hull"), error));
    REQUIRE(error.empty());
    REQUIRE(copy.shape() == PhysicsObject::Shape::Hull);
    copy.load_property("Shape", engine_core::JsonValue::string("Torus"), error);
    REQUIRE(error.find("Enum.PhysicsShape") != std::string::npos);
    REQUIRE(copy.shape() == PhysicsObject::Shape::Hull);

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Sphere)));
    body.set_anchored(true);
    game.stop_simulation();
    REQUIRE(body.shape() == PhysicsObject::Shape::Capsule);
    REQUIRE_FALSE(body.anchored());

    // Mesh is shown only for a Hull.
    const engine_core::LuaField* mesh = engine_core::lua_class_find("PhysicsObject", "Mesh");
    REQUIRE(mesh != nullptr);
    REQUIRE(std::string(mesh->shown_when) == "Shape");
    REQUIRE(mesh->shown_for(static_cast<int>(PhysicsObject::Shape::Hull)));
    REQUIRE(mesh->shown_for(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(mesh->shown_for(static_cast<int>(PhysicsObject::Shape::Box)));
}

TEST_CASE("P14 scripts set Shape by item, name, or value, and nothing else", "[physics]") {
    ScriptRig rig;
    add_script(rig.game, "Shapes", R"(
        local body = Instance.new("PhysicsObject", workspace)
        _G.default = body.Shape == Enum.PhysicsShape.Box and body.Mass == 1 and body.Anchored == false
            and body.Size == Vector3.new(1, 1, 1) and body.GameObject == nil and body.Mesh == nil
        body.Shape = Enum.PhysicsShape.Sphere
        _G.item = body.Shape == Enum.PhysicsShape.Sphere
        body.Shape = "Capsule"
        _G.name = body.Shape == Enum.PhysicsShape.Capsule
        body.Shape = 3
        _G.value = body.Shape == Enum.PhysicsShape.Hull and body.Shape.Name == "Hull"
        _G.refused = not pcall(function() body.Shape = "Torus" end)
            and not pcall(function() body.Shape = Enum.KeyCode.A end)
            and not pcall(function() body.Shape = Vector3.new() end)
            and body.Shape == Enum.PhysicsShape.Hull
        local part = Instance.new("GameObject", workspace)
        body.GameObject = part
        _G.ref = body.GameObject == part
            and not pcall(function() body.GameObject = Instance.new("Folder") end)
        body.Mass = 0
        _G.mass = body.Mass == 0.001
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"default", "item", "name", "value", "refused", "ref", "mass"}) {
        INFO(name);
        bool value = false;
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

namespace {

// A unit cube's 12 triangles, each with corners of its own, as an AMESH's
// faces have: points three to a triangle, and triangles indexing them in turn.
void unwelded_cube(std::vector<Vec3>& points, std::vector<std::uint32_t>& triangles) {
    const Vec3 c[8] = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f}, {-.5f, .5f, -.5f},
                       {-.5f, -.5f, .5f},  {.5f, -.5f, .5f},  {.5f, .5f, .5f},  {-.5f, .5f, .5f}};
    const int faces[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                              {3, 6, 2}, {3, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    points.clear();
    triangles.clear();
    for (const auto& face : faces) {
        for (int corner : face) {
            triangles.push_back(static_cast<std::uint32_t>(points.size()));
            points.push_back(c[corner]);
        }
    }
}

bool on_box_corner(Vec3 p, Vec3 half) {
    return near(std::fabs(p.x), half.x, 1e-4f) && near(std::fabs(p.y), half.y, 1e-4f) &&
           near(std::fabs(p.z), half.z, 1e-4f);
}

}  // namespace

TEST_CASE("P17 a collision outline traces what the body collides as, in the body's space", "[physics]") {
    PhysicsRig rig;
    PhysicsObject& body = rig.body(at(4.f, 5.f, 6.f), Vec3{2.f, 4.f, 6.f}, false);
    const Vec3 half{1.f, 2.f, 3.f};
    const std::vector<Vec3> no_points;
    const std::vector<std::uint32_t> no_triangles;
    std::vector<Vec3> lines{Vec3{9.f, 9.f, 9.f}};

    SECTION("a Box is its twelve edges, whatever Mesh it has") {
        std::vector<Vec3> points;
        std::vector<std::uint32_t> triangles;
        unwelded_cube(points, triangles);
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, points, triangles, lines);
        REQUIRE(lines.size() == 24);
        for (const Vec3& p : lines) {
            REQUIRE(on_box_corner(p, half));
        }
    }

    SECTION("a Sphere is rings at its radius, half of Size's X") {
        REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Sphere)));
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, no_points, no_triangles, lines);
        REQUIRE(lines.size() >= 6);
        REQUIRE(lines.size() % 2 == 0);
        for (const Vec3& p : lines) {
            REQUIRE(near(std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z), 1.f, 1e-4f));
        }
    }

    SECTION("a Capsule is its radius around a segment along Y, Size's Y tall") {
        REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Capsule)));
        REQUIRE_FALSE(body.set_size(Vec3{1.f, 3.f, 1.f}));
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, no_points, no_triangles, lines);
        REQUIRE(lines.size() % 2 == 0);
        float low = 0.f;
        float high = 0.f;
        for (const Vec3& p : lines) {
            const float y = std::clamp(p.y, -1.f, 1.f);
            REQUIRE(near(std::sqrt(p.x * p.x + (p.y - y) * (p.y - y) + p.z * p.z), 0.5f, 1e-4f));
            low = std::min(low, p.y);
            high = std::max(high, p.y);
        }
        REQUIRE(near(low, -1.5f, 1e-4f));
        REQUIRE(near(high, 1.5f, 1e-4f));
    }

    SECTION("a Hull is the hull of its Mesh's points, fitted to Size") {
        REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Hull)));
        std::vector<Vec3> points;
        std::vector<std::uint32_t> triangles;
        unwelded_cube(points, triangles);
        // A point inside is not on the hull.
        points.push_back(Vec3{0.1f, 0.f, -0.2f});
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, points, no_triangles, lines);
        REQUIRE(lines.size() >= 24);
        REQUIRE(lines.size() % 2 == 0);
        for (const Vec3& p : lines) {
            REQUIRE(on_box_corner(p, half));
        }
    }

    SECTION("a Hull with no Mesh, or points with no volume, is the Box it falls back to") {
        REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Hull)));
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, no_points, no_triangles, lines);
        REQUIRE(lines.size() == 24);
        for (const Vec3& p : lines) {
            REQUIRE(on_box_corner(p, half));
        }
        const std::vector<Vec3> flat{{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 1.f, 0.f}};
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, flat, no_triangles, lines);
        REQUIRE(lines.size() == 24);
    }

    SECTION("an anchored Custom is each edge of its triangles once; unanchored it is a Hull") {
        REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
        std::vector<Vec3> points;
        std::vector<std::uint32_t> triangles;
        unwelded_cube(points, triangles);
        body.set_anchored(true);
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, points, triangles, lines);
        // Twelve box edges and a diagonal across each of the six faces.
        REQUIRE(lines.size() == 2 * 18);
        for (const Vec3& p : lines) {
            REQUIRE(on_box_corner(p, half));
        }
        body.set_anchored(false);
        std::vector<Vec3> hull;
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, points, triangles, hull);
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, points, no_triangles, lines);
        REQUIRE_FALSE(hull.empty());
        REQUIRE(hull.size() == lines.size());
    }

    SECTION("a Cylinder is sixteen sides around Y at half of Size's X, Size's Y tall") {
        REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, no_points, no_triangles, lines);
        // Sixteen edges around each cap and sixteen between them.
        REQUIRE(lines.size() == 2 * 48);
        for (const Vec3& p : lines) {
            REQUIRE(near(std::sqrt(p.x * p.x + p.z * p.z), 1.f, 1e-4f));
            REQUIRE(near(std::fabs(p.y), 2.f, 1e-4f));
        }
    }

    SECTION("a Cone is a base at half of Size's X, Size's Y below its tip") {
        REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Cone)));
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, no_points, no_triangles, lines);
        // Sixteen edges around the base and sixteen up to the tip.
        REQUIRE(lines.size() == 2 * 32);
        int tips = 0;
        for (const Vec3& p : lines) {
            if (near(p.y, 2.f, 1e-4f)) {
                REQUIRE(near(p.x, 0.f, 1e-4f));
                REQUIRE(near(p.z, 0.f, 1e-4f));
                ++tips;
            } else {
                REQUIRE(near(p.y, -2.f, 1e-4f));
                REQUIRE(near(std::sqrt(p.x * p.x + p.z * p.z), 1.f, 1e-4f));
            }
        }
        REQUIRE(tips == 16);
    }

    SECTION("a Wedge is Size's box sloped from its bottom front up to its top back") {
        REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Wedge)));
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, no_points, no_triangles, lines);
        // Six corners, five faces, nine edges.
        REQUIRE(lines.size() == 2 * 9);
        for (const Vec3& p : lines) {
            REQUIRE(on_box_corner(p, half));
            // No corner at the top front.
            REQUIRE_FALSE((p.y > 0.f && p.z < 0.f));
        }
    }
}

TEST_CASE("P18 a body's pose is its Transform's position and rotation, without the scale", "[physics]") {
    Matrix4 transform = engine_core::matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, 0.5);
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            transform.m[column * 4 + row] *= static_cast<float>(column + 2);
        }
    }
    transform.m[12] = 1.f;
    transform.m[13] = 2.f;
    transform.m[14] = 3.f;
    const Matrix4 pose = engine_core::PhysicsWorld::body_pose(transform);
    for (int column = 0; column < 3; ++column) {
        REQUIRE(near(column_length(pose, column), 1.f, 1e-5f));
        const float scale = column_length(transform, column);
        for (int row = 0; row < 3; ++row) {
            REQUIRE(near(pose.m[column * 4 + row], transform.m[column * 4 + row] / scale, 1e-5f));
        }
    }
    REQUIRE(x_of(pose) == 1.f);
    REQUIRE(y_of(pose) == 2.f);
    REQUIRE(pose.m[14] == 3.f);
}

namespace {

// A GameObject at where in Workspace, drawing a Prefab of one Model whose
// Mesh is returned, and the PhysicsObject under it that moves it.
struct PrefabBody {
    engine_core::Mesh* mesh = nullptr;
    engine_core::Prefab* prefab = nullptr;
    GameObject* object = nullptr;
    PhysicsObject* body = nullptr;
};

PrefabBody prefab_body(PhysicsRig& rig, const Matrix4& where, Vec3 size, bool linked = true) {
    PrefabBody out;
    engine_core::Game& game = rig.game;
    out.mesh = &game.create<engine_core::Mesh>();
    game.set_parent(out.mesh->id(), game.service("Meshes"));
    out.prefab = &game.create<engine_core::Prefab>();
    game.set_parent(out.prefab->id(), game.service("Prefabs"));
    auto& model = game.create<engine_core::Model>();
    game.set_parent(model.id(), out.prefab->id());
    REQUIRE_FALSE(model.set_reference(engine_core::Model::kMeshReference, instance_slot(out.mesh->id())));
    out.object = &game.create<GameObject>();
    out.object->set_transform(where);
    if (linked) {
        REQUIRE_FALSE(out.object->set_prefab(instance_slot(out.prefab->id())));
    }
    game.set_parent(out.object->id(), workspace_of(game));
    out.body = &rig.body(engine_core::matrix4_identity(), size, false, out.object->id());
    return out;
}

// A 2 by 2 by 2 box whose bottom is at the Mesh's origin: its box's middle is (0, 1, 0).
void box_above_origin(anarchy::amesh::Data& data) { engine_core::add_box(data, Vec3{2.f, 2.f, 2.f}, Vec3{0.f, 1.f, 0.f}); }

}  // namespace

TEST_CASE("P19 a body sits in the middle of its GameObject's Prefab", "[physics]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("its shape is centered on the Prefab's OriginOffset, so the mesh's bottom rests on the floor") {
        PrefabBody pot = prefab_body(rig, at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f});
        rig.play();
        REQUIRE_FALSE(pot.mesh->edit_geometry(box_above_origin));
        rig.seconds(3.0);
        INFO(y_of(pot.object->transform()));
        REQUIRE(near(y_of(pot.object->transform()), 0.f, 0.05f));
        // The body's origin stays the GameObject's.
        REQUIRE(near(y_of(pot.body->transform()), y_of(pot.object->transform()), 1e-4f));
    }

    SECTION("the offset grows with the GameObject's scale, as the drawn mesh does") {
        Matrix4 tall = at(0.f, 5.f, 0.f);
        tall.m[5] = 2.f;
        PrefabBody pot = prefab_body(rig, tall, Vec3{2.f, 4.f, 2.f});
        rig.play();
        REQUIRE_FALSE(pot.mesh->edit_geometry(box_above_origin));
        rig.seconds(3.0);
        INFO(y_of(pot.object->transform()));
        // The middle is 2 above the origin, and the 4 tall shape reaches down to it.
        REQUIRE(near(y_of(pot.object->transform()), 0.f, 0.05f));
        REQUIRE(near(column_length(pot.object->transform(), 1), 2.f, 1e-4f));
    }

    SECTION("a GameObject that gets its Prefab during play centers its body then") {
        PrefabBody pot = prefab_body(rig, at(0.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
        rig.play();
        REQUIRE_FALSE(pot.mesh->edit_geometry(box_above_origin));
        rig.seconds(2.0);
        REQUIRE(near(y_of(pot.object->transform()), 1.f, 0.05f));
        REQUIRE_FALSE(pot.object->set_prefab(instance_slot(pot.prefab->id())));
        rig.seconds(2.0);
        INFO(y_of(pot.object->transform()));
        REQUIRE(near(y_of(pot.object->transform()), 0.f, 0.05f));
    }

    SECTION("a PhysicsObject that moves no GameObject is centered on its own origin") {
        PhysicsObject& loose = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
        REQUIRE(engine_core::PhysicsWorld::shape_center(rig.game, loose).y == 0.f);
        rig.play();
        rig.seconds(2.0);
        REQUIRE(near(y_of(loose.transform()), 1.f, 0.05f));
    }
}

TEST_CASE("P20 a collision outline is centered where the body's shape is", "[physics]") {
    PhysicsRig rig;
    PrefabBody pot = prefab_body(rig, at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f});
    rig.play();
    REQUIRE_FALSE(pot.mesh->edit_geometry(box_above_origin));
    const Vec3 center = engine_core::PhysicsWorld::shape_center(rig.game, *pot.body);
    REQUIRE(near(center.x, 0.f, 1e-5f));
    REQUIRE(near(center.y, 1.f, 1e-5f));
    REQUIRE(near(center.z, 0.f, 1e-5f));
    std::vector<Vec3> lines;
    engine_core::PhysicsWorld::collision_outline(*pot.body, center, {}, {}, lines);
    REQUIRE(lines.size() == 24);
    for (const Vec3& p : lines) {
        REQUIRE(on_box_corner(Vec3{p.x, p.y - 1.f, p.z}, Vec3{1.f, 1.f, 1.f}));
    }
}

TEST_CASE("P21 a Cylinder, a Cone, and a Wedge rest on their bottoms, a Cylinder on its side rolls", "[physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& cylinder = rig.body(at(-4.f, 3.f, 0.f), Vec3{1.f, 2.f, 1.f}, false);
    REQUIRE_FALSE(cylinder.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    PhysicsObject& cone = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(cone.set_shape(static_cast<int>(PhysicsObject::Shape::Cone)));
    PhysicsObject& wedge = rig.body(at(4.f, 3.f, 0.f), Vec3{2.f, 1.f, 2.f}, false);
    REQUIRE_FALSE(wedge.set_shape(static_cast<int>(PhysicsObject::Shape::Wedge)));
    // On its side, along X, and pushed along Z.
    PhysicsObject& log = rig.body(engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 1.5707963), Vec3{1.f, 2.f, 1.f},
                                  false);
    Matrix4 above = log.transform();
    above.m[12] = 8.f;
    above.m[13] = 0.6f;
    REQUIRE_FALSE(log.set_transform(above));
    REQUIRE_FALSE(log.set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
    REQUIRE_FALSE(log.set_velocity(Vec3{0.f, 0.f, 2.f}));
    rig.play();
    rig.seconds(3.0);
    INFO(y_of(cylinder.transform()) << " " << y_of(cone.transform()) << " " << y_of(wedge.transform()));
    REQUIRE(near(y_of(cylinder.transform()), 1.f, 0.05f));
    REQUIRE(near(y_of(cone.transform()), 1.f, 0.05f));
    REQUIRE(near(y_of(wedge.transform()), 0.5f, 0.05f));
    REQUIRE(near(y_of(log.transform()), 0.5f, 0.05f));
    // A box pushed so slides to a stop within a third of a unit; a log rolls on.
    REQUIRE(log.transform().m[14] > 1.f);
    REQUIRE(rig.warnings.empty());
}

TEST_CASE("P22 a GameObject's Scale scales the body that moves it", "[physics]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("a 1 wide box under a GameObject of Scale 2 rests 1 above the floor") {
        GameObject& part = create_part(rig.game);
        part.set_transform(at(0.f, 5.f, 0.f));
        REQUIRE_FALSE(part.set_scale(2.0));
        PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
        // Size is the PhysicsObject's own; Scale multiplies it.
        REQUIRE(body.size().y == 1.f);
        rig.play();
        rig.seconds(3.0);
        INFO(y_of(part.transform()));
        REQUIRE(near(y_of(part.transform()), 1.f, 0.05f));
        // The Transform keeps no scale of its own.
        REQUIRE(near(column_length(part.transform(), 1), 1.f, 1e-4f));
    }

    SECTION("the Prefab's offset scales too, so the scaled mesh's bottom rests on the floor") {
        PrefabBody pot = prefab_body(rig, at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f});
        REQUIRE_FALSE(pot.object->set_scale(2.0));
        rig.play();
        REQUIRE_FALSE(pot.mesh->edit_geometry(box_above_origin));
        const Vec3 center = engine_core::PhysicsWorld::shape_center(rig.game, *pot.body);
        REQUIRE(near(center.y, 2.f, 1e-5f));
        rig.seconds(3.0);
        INFO(y_of(pot.object->transform()));
        REQUIRE(near(y_of(pot.object->transform()), 0.f, 0.05f));
    }

    SECTION("a Scale written during play makes the shape again") {
        GameObject& part = create_part(rig.game);
        part.set_transform(at(0.f, 2.f, 0.f));
        rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
        rig.play();
        rig.seconds(2.0);
        REQUIRE(near(y_of(part.transform()), 0.5f, 0.05f));
        REQUIRE_FALSE(part.set_scale(3.0));
        rig.seconds(3.0);
        INFO(y_of(part.transform()));
        REQUIRE(near(y_of(part.transform()), 1.5f, 0.05f));
    }

    SECTION("its collision outline is the scaled shape") {
        GameObject& part = create_part(rig.game);
        REQUIRE_FALSE(part.set_scale(3.0));
        PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 2.f, 1.f}, false, part.id());
        const float scale = engine_core::PhysicsWorld::shape_scale(rig.game, body);
        REQUIRE(scale == 3.f);
        std::vector<Vec3> lines;
        engine_core::PhysicsWorld::collision_outline(body, Vec3{}, {}, {}, lines, scale);
        REQUIRE(lines.size() == 24);
        for (const Vec3& p : lines) {
            REQUIRE(on_box_corner(p, Vec3{1.5f, 3.f, 1.5f}));
        }
    }
}

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

namespace {

engine_core::Workspace& workspace_service(engine_core::Game& game) {
    auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    return *workspace;
}

}  // namespace

TEST_CASE("P24 Workspace.Gravity is checked, saved, and comes back at Stop", "[physics]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Workspace& workspace = workspace_service(game);
    REQUIRE(workspace.gravity() == engine_core::Workspace::kDefaultGravity);
    engine_core::PropertyBag saved;
    workspace.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Gravity") == nullptr);

    REQUIRE(*workspace.set_gravity(std::nan("")) == "Gravity must be a finite number");
    REQUIRE(workspace.gravity() == engine_core::Workspace::kDefaultGravity);
    // Below 0 pulls up, and 0 is none: both are allowed.
    REQUIRE_FALSE(workspace.set_gravity(-3.0));
    REQUIRE(workspace.gravity() == -3.0);
    REQUIRE_FALSE(workspace.set_gravity(20.0));
    engine_core::PropertyBag changed;
    workspace.save_properties(changed);
    const engine_core::JsonValue* gravity = engine_core::bag_find(changed, "Gravity");
    REQUIRE(gravity != nullptr);
    REQUIRE(gravity->as_number() == 20.0);

    const engine_core::LuaField* field = engine_core::lua_class_find("Workspace", "Gravity");
    REQUIRE(field != nullptr);

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(workspace.set_gravity(0.0));
    game.stop_simulation();
    REQUIRE(workspace.gravity() == 20.0);
}

TEST_CASE("P25 bodies fall as fast as Workspace.Gravity says, even when it changes during play", "[physics]") {
    PhysicsRig rig;
    engine_core::Workspace& workspace = workspace_service(rig.game);
    PhysicsObject& box = rig.body(at(0.f, 50.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(workspace.set_gravity(20.0));
    rig.play();
    rig.seconds(0.5);
    // v = g t.
    INFO(box.velocity().y);
    REQUIRE(near(box.velocity().y, -10.f, 0.2f));

    // None: it keeps the speed it had.
    REQUIRE_FALSE(workspace.set_gravity(0.0));
    rig.seconds(0.5);
    REQUIRE(near(box.velocity().y, -10.f, 0.2f));

    // Below 0: it slows, and then rises.
    REQUIRE_FALSE(workspace.set_gravity(-20.0));
    rig.seconds(1.0);
    REQUIRE(near(box.velocity().y, 10.f, 0.3f));
}

TEST_CASE("P26 a PlayerController falls as fast as Workspace.Gravity says", "[physics]") {
    PhysicsRig rig;
    engine_core::Workspace& workspace = workspace_service(rig.game);
    PlayerController& controller = rig.controller(at(0.f, 50.f, 0.f));
    REQUIRE_FALSE(workspace.set_gravity(4.0));
    rig.play();
    rig.seconds(0.5);
    INFO(controller.velocity().y);
    REQUIRE(near(controller.velocity().y, -2.f, 0.1f));
}

TEST_CASE("P27 an unanchored Custom cup catches a ball", "[physics]") {
    engine_core::clear_piece_cache();
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.seconds(4.0);
    INFO(y_of(scene.ball->transform()));
    // On the cup's floor: 0.5 up, plus the ball's radius. A single hull would hold it at 3.5.
    REQUIRE(near(y_of(scene.ball->transform()), 1.f, 0.15f));
    REQUIRE(rig.physics.shape_frictions(scene.cup->id()).size() >= 2);
    REQUIRE(rig.warnings.empty());
}

TEST_CASE("P28 a body of pieces weighs its Mass", "[physics]") {
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.steps(1);
    REQUIRE(near(rig.physics.body_mass(scene.cup->id()), 20.f, 0.01f));
    // Every shape kind still weighs its Mass, with one shape.
    REQUIRE(near(rig.physics.body_mass(scene.ball->id()), 1.f, 0.001f));
    REQUIRE(rig.physics.shape_frictions(scene.ball->id()).size() == 1);
}

TEST_CASE("P29 a Custom whose Mesh has no pieces falls back to a Hull and says so once", "[physics]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
    PhysicsObject& body = rig.body(at(0.f, 3.f, 0.f), Vec3{2.f, 2.f, 2.f}, false);
    REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    REQUIRE_FALSE(body.set_mesh(instance_slot(mesh.id())));
    rig.play();
    anarchy::amesh::Data cube;
    engine_core::add_box(cube, Vec3{1.f, 1.f, 1.f}, Vec3{0.f, 0.f, 0.f});
    std::vector<Vec3> points;
    for (const auto& v : cube.vertices) {
        points.push_back(Vec3{v.p[0], v.p[1], v.p[2]});
    }
    // Known to split into nothing.
    engine_core::remember_pieces(points, cube.indices, {});
    REQUIRE_FALSE(mesh.edit_geometry([&cube](anarchy::amesh::Data& data) { data = cube; }));
    rig.seconds(3.0);
    REQUIRE(near(y_of(body.transform()), 1.f, 0.05f));
    REQUIRE(rig.warnings.size() == 1);
    REQUIRE(rig.warnings.front().find("Custom fell back to Hull") != std::string::npos);
    engine_core::clear_piece_cache();
}

TEST_CASE("P30 Friction set during play reaches every piece", "[physics]") {
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.steps(1);
    REQUIRE_FALSE(scene.cup->set_friction(0.125));
    rig.steps(1);
    const std::vector<float> frictions = rig.physics.shape_frictions(scene.cup->id());
    REQUIRE(frictions.size() >= 2);
    for (const float friction : frictions) {
        REQUIRE(near(friction, 0.125f, 1e-6f));
    }
}

TEST_CASE("P31 a cup stretched by Size still holds a ball", "[physics]") {
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{8.f, 3.f, 4.f}, 2.f);
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.seconds(4.0);
    INFO(x_of(scene.ball->transform()) << " " << y_of(scene.ball->transform()));
    REQUIRE(y_of(scene.ball->transform()) < 2.f);
    REQUIRE(std::fabs(x_of(scene.ball->transform())) < 4.f);
}

TEST_CASE("P32 pieces in the Mesh's file are used without decomposing, anchored or not", "[physics]") {
    PhysicsRig rig;
    const std::filesystem::path resources =
        std::filesystem::temp_directory_path() / ("anarchy-physics-pieces-test-" + process_id());
    std::filesystem::remove_all(resources);
    std::filesystem::create_directories(resources);
    rig.game.set_resources_root(resources);
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    // Stopped: the cup goes into the Mesh's file, then its pieces do.
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    std::vector<Vec3> points;
    std::vector<std::uint32_t> triangles;
    REQUIRE_FALSE(scene.mesh->vertex_positions(points, &triangles));
    REQUIRE_FALSE(scene.mesh->store_pieces(engine_core::kRecipe, engine_core::decompose(points, triangles)));
    engine_core::clear_piece_cache();
    const std::uint64_t before = engine_core::decompose_count();

    rig.play();
    rig.seconds(4.0);
    REQUIRE(near(y_of(scene.ball->transform()), 1.f, 0.15f));
    scene.cup->set_anchored(true);
    rig.steps(1);
    scene.cup->set_anchored(false);
    rig.seconds(1.0);
    REQUIRE(near(y_of(scene.ball->transform()), 1.f, 0.15f));
    REQUIRE(engine_core::decompose_count() == before);
    std::error_code ignored;
    std::filesystem::remove_all(resources, ignored);
}

TEST_CASE("P33 an unanchored Custom's outline is its pieces when they are known", "[physics]") {
    PhysicsRig rig;
    PhysicsObject& cup = rig.body(at(0.f, 0.f, 0.f), Vec3{4.f, 3.f, 4.f}, false);
    REQUIRE_FALSE(cup.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    anarchy::amesh::Data data;
    add_cup(data);
    std::vector<Vec3> points;
    for (const auto& v : data.vertices) {
        points.push_back(Vec3{v.p[0], v.p[1], v.p[2]});
    }
    const auto pieces = engine_core::decompose(points, data.indices);
    REQUIRE(pieces.size() >= 2);

    std::vector<Vec3> hull_lines;
    engine_core::PhysicsWorld::collision_outline(cup, Vec3{}, points, data.indices, hull_lines);
    std::vector<Vec3> piece_lines;
    engine_core::PhysicsWorld::collision_outline(cup, Vec3{}, points, data.indices, piece_lines, 1.f, &pieces);
    REQUIRE(piece_lines.size() > hull_lines.size());
    for (const Vec3& p : piece_lines) {
        REQUIRE(std::fabs(p.x) <= 2.05f);
        REQUIRE(std::fabs(p.y) <= 1.55f);
        REQUIRE(std::fabs(p.z) <= 2.05f);
    }
    // Anchored, it is its triangles whatever pieces it has.
    cup.set_anchored(true);
    std::vector<Vec3> anchored_lines;
    engine_core::PhysicsWorld::collision_outline(cup, Vec3{}, points, data.indices, anchored_lines, 1.f, &pieces);
    std::vector<Vec3> triangle_lines;
    engine_core::PhysicsWorld::collision_outline(cup, Vec3{}, points, data.indices, triangle_lines);
    REQUIRE(anchored_lines.size() == triangle_lines.size());
}

TEST_CASE("P34 Mass set during play reweighs a body of pieces", "[physics]") {
    PhysicsRig rig;
    CupScene scene = cup_scene(rig, Vec3{4.f, 3.f, 4.f});
    rig.play();
    REQUIRE_FALSE(scene.mesh->edit_geometry(add_cup));
    rig.steps(1);
    REQUIRE(rig.physics.shape_frictions(scene.cup->id()).size() >= 2);
    REQUIRE(near(rig.physics.body_mass(scene.cup->id()), 20.f, 0.01f));
    // Each piece's density changes in place, and the body's mass with them.
    REQUIRE_FALSE(scene.cup->set_mass(35.0));
    rig.steps(1);
    REQUIRE(near(rig.physics.body_mass(scene.cup->id()), 35.f, 0.01f));
}

TEST_CASE("P35 stopped, a body's Transform follows the GameObject it moves", "[physics]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    Matrix4 scaled = at(3.f, 10.f, 0.f);
    scaled.m[0] = scaled.m[5] = scaled.m[10] = 2.f;
    part.set_transform(scaled);
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    PhysicsObject& loose = rig.body(at(0.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);

    engine_core::PhysicsWorld::follow_game_objects(rig.game);
    REQUIRE(near(x_of(body.transform()), 3.f, 1e-5f));
    REQUIRE(near(y_of(body.transform()), 10.f, 1e-5f));
    // Only the pose: the body keeps its own scale, the GameObject its Transform,
    // and a body that moves nothing stays where it was authored.
    REQUIRE(near(column_length(body.transform(), 0), 1.f, 1e-5f));
    REQUIRE(engine_core::same_matrix4(part.transform(), scaled));
    REQUIRE(y_of(loose.transform()) == 2.f);

    // Moved again, by Properties or a dragger, the body comes along.
    part.set_transform(at(5.f, 10.f, 0.f));
    engine_core::PhysicsWorld::follow_game_objects(rig.game);
    REQUIRE(near(x_of(body.transform()), 5.f, 1e-5f));

    // A body under a GameObject follows its parent; a PlayerController stays
    // upright, as it starts at play.
    GameObject& other = create_part(rig.game);
    Matrix4 tilted = engine_core::matrix4_axis_angle(Vec3{1.f, 0.f, 0.f}, 0.5);
    tilted.m[12] = -4.f;
    tilted.m[13] = 1.f;
    tilted.m[14] = 6.f;
    other.set_transform(tilted);
    PlayerController& controller = rig.controller(engine_core::matrix4_identity(), other.id());
    REQUIRE(controller.driven_game_object() == other.id());
    engine_core::PhysicsWorld::follow_game_objects(rig.game);
    REQUIRE(near(x_of(controller.transform()), -4.f, 1e-5f));
    REQUIRE(near(z_of(controller.transform()), 6.f, 1e-5f));
    REQUIRE(near(controller.transform().m[5], 1.f, 1e-5f));
    REQUIRE(near(controller.transform().m[6], 0.f, 1e-5f));
}
