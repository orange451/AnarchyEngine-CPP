// PhysicsWorld while the place is stopped: bodies exist and follow the tree,
// but nothing is simulated; a driven body's Transform follows its GameObject.

#include "physics_rig.hpp"
#include "support.hpp"

#include "AssetInstances.hpp"
#include "ConvexDecomposition.hpp"
#include "Engine.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>
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

// A temporary resources folder, so a stopped Mesh edit has somewhere to write
// its AMESH file. Removed when the test ends.
struct TempResourcesRoot {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("anarchy-edit-physics-test-" + process_id());

    explicit TempResourcesRoot(engine_core::Game& game) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        game.set_resources_root(path);
    }

    ~TempResourcesRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TempResourcesRoot(const TempResourcesRoot&) = delete;
    TempResourcesRoot& operator=(const TempResourcesRoot&) = delete;
};

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

TEST_CASE("E3 stopped, a body and its Transform follow its GameObject", "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 8.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->y == 8.f);
    REQUIRE(y_of(body.transform()) == 8.f);
    part.set_transform(at(5.f, 8.f, 0.f));
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->x == 5.f);
    REQUIRE(x_of(body.transform()) == 5.f);
    REQUIRE(y_of(body.transform()) == 8.f);
    // Nothing is simulated: the GameObject stays where it was put.
    REQUIRE(engine_core::same_matrix4(part.transform(), at(5.f, 8.f, 0.f)));
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
    REQUIRE(rig.physics.body_key(box.id()) != before);
    REQUIRE(rig.physics.body_position(box.id())->y == 5.f);
}

TEST_CASE("E6 stopped, an unanchored Custom without pieces is a Hull and decomposes nothing", "[physics][edit]") {
    PhysicsRig rig;
    TempResourcesRoot resources(rig.game);
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
    TempResourcesRoot resources(rig.game);
    engine_core::Mesh* mesh = nullptr;
    PhysicsObject& body = custom_cube(rig, mesh);
    // Two apart boxes, not one cube, so the pieces are more than the one Hull.
    REQUIRE_FALSE(mesh->edit_geometry([](anarchy::amesh::Data& data) {
        data = anarchy::amesh::Data{};
        engine_core::add_box(data, Vec3{0.4f, 1.f, 1.f}, Vec3{-0.3f, 0.f, 0.f});
        engine_core::add_box(data, Vec3{0.4f, 1.f, 1.f}, Vec3{0.3f, 0.f, 0.f});
    }));
    rig.sync_steps(1);
    REQUIRE(rig.physics.shape_frictions(body.id()).size() == 1);
    const std::uint64_t decomposed = engine_core::decompose_count();
    rig.play();
    rig.steps(1);
    REQUIRE(engine_core::decompose_count() == decomposed + 1);
    // Built again from the stored pieces, one shape each, not the one Hull.
    REQUIRE(rig.physics.shape_frictions(body.id()).size() > 1);
    engine_core::clear_piece_cache();
}

TEST_CASE("E8 stopped, a body made without pieces is made again once its Mesh stores them", "[physics][edit]") {
    PhysicsRig rig;
    TempResourcesRoot resources(rig.game);
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

TEST_CASE("E9 a body made without pieces, then given another Shape, does not keep remaking at Play",
          "[physics][edit]") {
    PhysicsRig rig;
    TempResourcesRoot resources(rig.game);
    engine_core::Mesh* mesh = nullptr;
    PhysicsObject& body = custom_cube(rig, mesh);
    rig.sync_steps(1);
    // Away from Custom, while still stopped: make_pieces, the only place that
    // used to clear made_without_pieces, is never reached again.
    REQUIRE_FALSE(body.set_shape(static_cast<int>(PhysicsObject::Shape::Box)));
    rig.sync_steps(1);
    rig.play();
    rig.steps(1);
    const std::uint64_t key = rig.physics.shape_key(body.id());
    rig.steps(1);
    REQUIRE(rig.physics.shape_key(body.id()) == key);
}

TEST_CASE("E10 a stopped Engine keeps bodies and registers its world", "[physics][edit][engine]") {
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

TEST_CASE("E11 stopped, a Move tool drag of a GameObject and its PhysicsObject moves the GameObject only by its own Transform",
          "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(10.f, 0.f, 0.f));
    // A child PhysicsObject drives its GameObject; stopped, its own Transform
    // follows the GameObject.
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->x == 10.f);
    REQUIRE(x_of(body.transform()) == 10.f);
    // What MoveTool.luau does to every selected PVInstance: its current
    // Transform, plus the drag.
    const Matrix4 part_now = part.transform();
    part.set_transform(at(x_of(part_now) + 1.f, y_of(part_now), z_of(part_now)));
    const Matrix4 body_now = body.transform();
    REQUIRE_FALSE(body.set_transform(at(x_of(body_now) + 1.f, y_of(body_now), z_of(body_now))));
    rig.sync_steps(1);
    REQUIRE(x_of(part.transform()) == 11.f);
    REQUIRE(y_of(part.transform()) == 0.f);
    REQUIRE(rig.physics.body_position(body.id())->x == 11.f);
    REQUIRE(rig.physics.body_position(body.id())->y == 0.f);
    REQUIRE(x_of(body.transform()) == 11.f);
}

TEST_CASE("E12 stopped, writing only a driven PhysicsObject's Transform moves neither it nor its GameObject",
          "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(10.f, 0.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
    rig.sync_steps(1);
    // As Properties would: a recorded write to the PhysicsObject alone. Moving
    // the GameObject for it would be a move with no Changed and no history.
    REQUIRE_FALSE(body.set_transform(at(20.f, 3.f, 0.f)));
    rig.sync_steps(1);
    REQUIRE(engine_core::same_matrix4(part.transform(), at(10.f, 0.f, 0.f)));
    REQUIRE(rig.physics.body_position(body.id())->x == 10.f);
    // Its Transform goes back to where the body and the GameObject are.
    REQUIRE(x_of(body.transform()) == 10.f);
    REQUIRE(y_of(body.transform()) == 0.f);
}

TEST_CASE("E13 a driven PhysicsObject's Transform written just before Play does not move its GameObject",
          "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(10.f, 0.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, true, part.id());
    rig.sync_steps(1);
    // Written after the last stopped sync, and Play pressed before the next.
    REQUIRE_FALSE(body.set_transform(at(20.f, 3.f, 0.f)));
    rig.play();
    rig.steps(1);
    REQUIRE(engine_core::same_matrix4(part.transform(), at(10.f, 0.f, 0.f)));
    REQUIRE(rig.physics.body_position(body.id())->x == 10.f);
    REQUIRE(x_of(body.transform()) == 10.f);
    // Once playing, a write to it moves the body and the GameObject, as before.
    REQUIRE_FALSE(body.set_transform(at(20.f, 3.f, 0.f)));
    rig.steps(1);
    REQUIRE(x_of(part.transform()) == 20.f);
    REQUIRE(y_of(part.transform()) == 3.f);
}

TEST_CASE("E14 stopped, a PhysicsObject whose GameObject an earlier body moves still follows it",
          "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(4.f, 2.f, 0.f));
    PhysicsObject& first = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
    PhysicsObject& second = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false, part.id());
    rig.sync_steps(1);
    REQUIRE(rig.physics.has_body(first.id()));
    REQUIRE_FALSE(rig.physics.has_body(second.id()));
    REQUIRE(x_of(second.transform()) == 4.f);
    REQUIRE(y_of(second.transform()) == 2.f);
}
