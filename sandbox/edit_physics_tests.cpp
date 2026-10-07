// PhysicsWorld while the place is stopped: bodies exist and follow the tree,
// but nothing is simulated and nothing is written back to instances.

#include "physics_rig.hpp"
#include "support.hpp"

#include "AssetInstances.hpp"
#include "ConvexDecomposition.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <system_error>
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

TEST_CASE("E3 stopped, a body follows its GameObject without rewriting its own Transform", "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 8.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->y == 8.f);
    part.set_transform(at(5.f, 8.f, 0.f));
    rig.sync_steps(1);
    REQUIRE(rig.physics.body_position(body.id())->x == 5.f);
    REQUIRE(engine_core::same_matrix4(body.transform(), engine_core::matrix4_identity()));
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
    REQUIRE(rig.physics.body_position(box.id())->y == 5.f);
}

TEST_CASE("E5 an anchored body made while stopped seeds its Transform on the first played tick", "[physics][edit]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(2.f, 3.f, 0.f));
    PhysicsObject& body = rig.body(engine_core::matrix4_identity(), Vec3{1.f, 1.f, 1.f}, true);
    REQUIRE_FALSE(body.set_game_object(instance_slot(part.id())));
    rig.sync_steps(5);
    REQUIRE(engine_core::same_matrix4(body.transform(), engine_core::matrix4_identity()));
    rig.play();
    rig.steps(1);
    REQUIRE(x_of(body.transform()) == 2.f);
    REQUIRE(y_of(body.transform()) == 3.f);
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
    (void)body;
    rig.sync_steps(1);
    const std::uint64_t decomposed = engine_core::decompose_count();
    rig.play();
    rig.steps(1);
    REQUIRE(engine_core::decompose_count() == decomposed + 1);
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
