// What the Scene View draws: a Workspace GameObject's Prefab, as its Models'
// Mesh Paths in the render snapshot, and GameObject.Position.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A game with a pump, and assets under Assets: Meshes named by path, and a Prefab of Models.
struct Scene {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;

    Scene() { pump.reserve(engine_core::DataModel::kMaxInstances); }

    void frame() {
        pump.prepare_copy(game);
        pump.publish();
    }

    engine_core::Mesh& mesh(const char* name, const char* path) {
        engine_core::Mesh& made = game.create<engine_core::Mesh>();
        game.set_name(made.id(), name);
        game.set_parent(made.id(), game.service("Meshes"));
        REQUIRE_FALSE(made.set_path(path));
        return made;
    }

    engine_core::Prefab& prefab(const char* name) {
        engine_core::Prefab& made = game.create<engine_core::Prefab>();
        game.set_name(made.id(), name);
        game.set_parent(made.id(), game.service("Prefabs"));
        return made;
    }

    // A Model in prefab, on mesh when it is not 0.
    engine_core::Model& model(engine_core::Prefab& in, InstanceId mesh) {
        engine_core::Model& made = game.create<engine_core::Model>();
        game.set_parent(made.id(), in.id());
        if (mesh != 0) {
            REQUIRE_FALSE(made.set_reference(engine_core::Model::kMeshReference, instance_slot(mesh)));
        }
        return made;
    }

    engine_core::GameObject& object(InstanceId prefab) {
        engine_core::GameObject& made = game.create_game_object();
        game.set_parent(made.id(), workspace_of(game));
        if (prefab != 0) {
            REQUIRE_FALSE(made.set_prefab(instance_slot(prefab)));
        }
        return made;
    }

    const engine_core::VisualInstance* row(InstanceId id) { return pump.find(id); }

    // The Mesh Paths the row draws, in order; empty for none.
    std::vector<std::string> meshes(InstanceId id) {
        const engine_core::VisualInstance* found = row(id);
        REQUIRE(found != nullptr);
        const engine_core::VisualSnapshot& front = pump.front();
        REQUIRE(front.prefabs.size() >= 1);
        REQUIRE(front.prefabs[0].meshes.empty());
        if (found->prefab == 0) {
            return {};
        }
        REQUIRE(found->prefab < front.prefabs.size());
        std::vector<std::string> paths;
        for (const engine_core::VisualMesh& mesh : front.prefabs[found->prefab].meshes) {
            // Outside play a Mesh draws its file.
            REQUIRE(mesh.session == nullptr);
            paths.push_back(mesh.path);
        }
        return paths;
    }
};

using Paths = std::vector<std::string>;

}  // namespace

TEST_CASE("a GameObject without a Prefab draws nothing", "[render]") {
    Scene scene;
    engine_core::GameObject& plain = scene.object(0);
    scene.frame();
    REQUIRE(scene.row(plain.id()) != nullptr);
    REQUIRE(scene.row(plain.id())->prefab == 0);
    REQUIRE(scene.meshes(plain.id()).empty());
}

TEST_CASE("a GameObject draws its Prefab's Models' Meshes, in order", "[render]") {
    Scene scene;
    engine_core::Mesh& body = scene.mesh("Body", "meshes/body.amesh");
    engine_core::Mesh& lid = scene.mesh("Lid", "meshes/lid.amesh");
    engine_core::Mesh& unset = scene.mesh("Unset", "");
    engine_core::Prefab& crate = scene.prefab("Crate");
    scene.model(crate, body.id());
    scene.model(crate, 0);           // no Mesh: adds nothing
    scene.model(crate, unset.id());  // a Mesh with no Path: adds nothing
    scene.model(crate, lid.id());
    engine_core::GameObject& box = scene.object(crate.id());
    box.set_position(engine_core::Vec3{1.f, 2.f, 3.f});
    scene.frame();
    REQUIRE(scene.meshes(box.id()) == Paths{"meshes/body.amesh", "meshes/lid.amesh"});
    REQUIRE(scene.row(box.id())->world.m[12] == 1.f);
    REQUIRE(scene.row(box.id())->world.m[14] == 3.f);
}

TEST_CASE("GameObjects on one Prefab share its entry, and edits show on the next frame", "[render]") {
    Scene scene;
    engine_core::Mesh& body = scene.mesh("Body", "meshes/body.amesh");
    engine_core::Prefab& crate = scene.prefab("Crate");
    engine_core::Model& part = scene.model(crate, body.id());
    engine_core::GameObject& a = scene.object(crate.id());
    engine_core::GameObject& b = scene.object(crate.id());
    scene.frame();
    REQUIRE(scene.row(a.id())->prefab != 0);
    REQUIRE(scene.row(a.id())->prefab == scene.row(b.id())->prefab);

    // A new Path, a second Model, and a Model's Mesh cleared: no GameObject changed.
    REQUIRE_FALSE(body.set_path("meshes/body2.amesh"));
    scene.frame();
    REQUIRE(scene.meshes(b.id()) == Paths{"meshes/body2.amesh"});
    engine_core::Mesh& wheel = scene.mesh("Wheel", "meshes/wheel.amesh");
    scene.model(crate, wheel.id());
    scene.frame();
    REQUIRE(scene.meshes(a.id()) == Paths{"meshes/body2.amesh", "meshes/wheel.amesh"});
    REQUIRE_FALSE(part.set_reference(engine_core::Model::kMeshReference, engine_core::LuaSlot()));
    scene.frame();
    REQUIRE(scene.meshes(a.id()) == Paths{"meshes/wheel.amesh"});
}

TEST_CASE("changing or clearing a GameObject's Prefab changes what it draws", "[render]") {
    Scene scene;
    engine_core::Mesh& body = scene.mesh("Body", "meshes/body.amesh");
    engine_core::Mesh& barrel_mesh = scene.mesh("Barrel", "meshes/barrel.amesh");
    engine_core::Prefab& crate = scene.prefab("Crate");
    scene.model(crate, body.id());
    engine_core::Prefab& barrel = scene.prefab("Barrel");
    scene.model(barrel, barrel_mesh.id());
    engine_core::GameObject& thing = scene.object(crate.id());
    scene.frame();
    const std::uint32_t crate_entry = scene.row(thing.id())->prefab;
    REQUIRE(scene.meshes(thing.id()) == Paths{"meshes/body.amesh"});

    REQUIRE_FALSE(thing.set_prefab(instance_slot(barrel.id())));
    scene.frame();
    REQUIRE(scene.meshes(thing.id()) == Paths{"meshes/barrel.amesh"});

    REQUIRE_FALSE(thing.set_prefab(engine_core::LuaSlot()));
    scene.frame();
    REQUIRE(scene.row(thing.id())->prefab == 0);

    // The freed entries are reused, so the table does not grow with each change.
    engine_core::GameObject& other = scene.object(crate.id());
    scene.frame();
    REQUIRE(scene.row(other.id())->prefab <= crate_entry + 1);
    REQUIRE(scene.pump.front().prefabs.size() <= 3);
}

TEST_CASE("a destroyed Prefab draws nothing until undo brings it back", "[render]") {
    Scene scene;
    engine_core::Mesh& body = scene.mesh("Body", "meshes/body.amesh");
    engine_core::Prefab& crate = scene.prefab("Crate");
    scene.model(crate, body.id());
    engine_core::GameObject& box = scene.object(crate.id());
    const InstanceId crate_id = crate.id();
    scene.game.history().end_gesture();
    scene.frame();
    REQUIRE(scene.meshes(box.id()) == Paths{"meshes/body.amesh"});

    scene.game.destroy(crate_id);
    scene.game.history().end_gesture();
    scene.frame();
    REQUIRE(scene.meshes(box.id()).empty());

    scene.game.history().undo();
    scene.frame();
    REQUIRE(scene.meshes(box.id()) == Paths{"meshes/body.amesh"});
}

TEST_CASE("undo of a GameObject's delete keeps its Prefab", "[render]") {
    Scene scene;
    engine_core::Mesh& body = scene.mesh("Body", "meshes/body.amesh");
    engine_core::Prefab& crate = scene.prefab("Crate");
    scene.model(crate, body.id());
    engine_core::GameObject& box = scene.object(crate.id());
    const InstanceId id = box.id();
    scene.game.history().end_gesture();
    scene.frame();

    scene.game.destroy(id);
    scene.game.history().end_gesture();
    scene.frame();
    REQUIRE(scene.row(id) == nullptr);

    scene.game.history().undo();
    scene.frame();
    REQUIRE(scene.game.alive(id));
    REQUIRE(scene.game.game_object(id)->prefab().id == crate.id());
    REQUIRE(scene.meshes(id) == Paths{"meshes/body.amesh"});
}

TEST_CASE("Stop puts back the Prefab a GameObject drew before play", "[render]") {
    Scene scene;
    engine_core::Mesh& body = scene.mesh("Body", "meshes/body.amesh");
    engine_core::Prefab& crate = scene.prefab("Crate");
    scene.model(crate, body.id());
    engine_core::GameObject& box = scene.object(crate.id());
    const InstanceId id = box.id();
    scene.frame();

    scene.game.capture_place();
    scene.game.start_simulation();
    REQUIRE_FALSE(scene.game.game_object(id)->set_prefab(engine_core::LuaSlot()));
    scene.frame();
    REQUIRE(scene.row(id)->prefab == 0);
    scene.game.stop_simulation();
    scene.frame();
    REQUIRE(scene.meshes(id) == Paths{"meshes/body.amesh"});
}

TEST_CASE("Position is the Transform's translation, and a write keeps the rotation", "[render]") {
    Scene scene;
    engine_core::GameObject& part = scene.object(0);
    // A quarter turn about Y, at (4, 5, 6).
    engine_core::Matrix4 turned = engine_core::matrix4_identity();
    turned.m[0] = 0.f;
    turned.m[2] = -1.f;
    turned.m[8] = 1.f;
    turned.m[10] = 0.f;
    turned.m[12] = 4.f;
    turned.m[13] = 5.f;
    turned.m[14] = 6.f;
    part.set_transform(turned);
    REQUIRE(part.position().x == 4.f);
    REQUIRE(part.position().y == 5.f);
    REQUIRE(part.position().z == 6.f);

    scene.game.history().end_gesture();
    part.set_position(engine_core::Vec3{-1.f, 0.5f, 2.f});
    scene.game.history().end_gesture();
    const engine_core::Matrix4 moved = part.transform();
    REQUIRE(moved.m[2] == -1.f);
    REQUIRE(moved.m[8] == 1.f);
    REQUIRE(moved.m[12] == -1.f);
    REQUIRE(moved.m[13] == 0.5f);
    REQUIRE(moved.m[14] == 2.f);
    scene.frame();
    REQUIRE(scene.row(part.id())->world.m[12] == -1.f);

    // One Transform edit: undo puts back the whole matrix.
    scene.game.history().undo();
    REQUIRE(engine_core::same_matrix4(part.transform(), turned));
}
