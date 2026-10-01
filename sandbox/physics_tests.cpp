// PhysicsObject, the rigid body Box3D simulates while it is in Workspace and
// the place plays, and PhysicsWorld, which steps it and moves its GameObject.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "LuaApi.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using engine_core::GameObject;
using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::Vec3;

constexpr double kStep = 1.0 / 240.0;

Matrix4 at(float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); }

float y_of(const Matrix4& m) { return m.m[13]; }

float x_of(const Matrix4& m) { return m.m[12]; }

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

float column_length(const Matrix4& m, int column) {
    const float* axis = m.m + column * 4;
    return std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
}

// A Game, a physics world stepped by hand, and the warnings it gave.
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

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
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
    copy.load_property("Shape", engine_core::JsonValue::string("Cylinder"), error);
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
    REQUIRE(mesh->shown_when_value == static_cast<int>(PhysicsObject::Shape::Hull));
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
        _G.refused = not pcall(function() body.Shape = "Cylinder" end)
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
