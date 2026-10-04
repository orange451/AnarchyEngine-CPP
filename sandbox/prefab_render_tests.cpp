// What the Scene View draws: a Workspace GameObject's Prefab, as its Models'
// Mesh Paths in the render snapshot, and GameObject::position().

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

TEST_CASE("a Model draws its Material's DiffuseTexture Path and Color", "[render]") {
    Scene scene;
    engine_core::Mesh& body = scene.mesh("Body", "meshes/body.amesh");
    engine_core::Prefab& crate = scene.prefab("Crate");
    engine_core::Model& part = scene.model(crate, body.id());
    engine_core::GameObject& box = scene.object(crate.id());
    auto surface = [&scene, &box]() -> const engine_core::VisualMesh& {
        const engine_core::VisualSnapshot& front = scene.pump.front();
        const std::uint32_t entry = scene.row(box.id())->prefab;
        REQUIRE(entry < front.prefabs.size());
        REQUIRE(front.prefabs[entry].meshes.size() == 1);
        return front.prefabs[entry].meshes[0];
    };

    // No Material: no texture, and white.
    scene.frame();
    REQUIRE(surface().diffuse_texture.empty());
    REQUIRE(surface().color.r == 1.f);
    REQUIRE(surface().color.b == 1.f);

    engine_core::Texture& wood = scene.game.create<engine_core::Texture>();
    scene.game.set_parent(wood.id(), scene.game.service("Textures"));
    REQUIRE_FALSE(wood.set_path("textures/wood.png"));
    engine_core::Material& varnish = scene.game.create<engine_core::Material>();
    scene.game.set_parent(varnish.id(), scene.game.service("Materials"));
    REQUIRE_FALSE(varnish.set_color(engine_core::ColorRgb{1.f, 0.5f, 0.25f, 1.f}));
    REQUIRE_FALSE(part.set_reference(engine_core::Model::kMaterialReference, instance_slot(varnish.id())));
    scene.frame();
    // A Material with no DiffuseTexture draws its Color alone.
    REQUIRE(surface().diffuse_texture.empty());
    REQUIRE(surface().color.g == 0.5f);
    REQUIRE(surface().color.b == 0.25f);

    REQUIRE_FALSE(varnish.set_reference(engine_core::Material::kDiffuseTextureReference, instance_slot(wood.id())));
    scene.frame();
    REQUIRE(surface().diffuse_texture == "textures/wood.png");

    // Edits to the Texture's Path show on the next frame, and clearing the Material undoes both.
    REQUIRE_FALSE(wood.set_path("textures/oak.png"));
    scene.frame();
    REQUIRE(surface().diffuse_texture == "textures/oak.png");
    REQUIRE_FALSE(part.set_reference(engine_core::Model::kMaterialReference, engine_core::LuaSlot()));
    scene.frame();
    REQUIRE(surface().diffuse_texture.empty());
    REQUIRE(surface().color.g == 1.f);
}

TEST_CASE("a Model draws its Material's other textures and numbers, clamped", "[render]") {
    Scene scene;
    engine_core::Mesh& body = scene.mesh("Body", "meshes/body.amesh");
    engine_core::Prefab& crate = scene.prefab("Crate");
    engine_core::Model& part = scene.model(crate, body.id());
    engine_core::GameObject& box = scene.object(crate.id());
    auto surface = [&scene, &box]() -> const engine_core::VisualMesh& {
        const engine_core::VisualSnapshot& front = scene.pump.front();
        const std::uint32_t entry = scene.row(box.id())->prefab;
        REQUIRE(entry < front.prefabs.size());
        REQUIRE(front.prefabs[entry].meshes.size() == 1);
        return front.prefabs[entry].meshes[0];
    };

    // No Material has a Material's defaults.
    scene.frame();
    REQUIRE(surface().metalness == 0.f);
    REQUIRE(surface().roughness == 0.4f);
    REQUIRE(surface().reflectivity == 0.5f);
    REQUIRE(surface().transparency == 0.f);
    REQUIRE(surface().emissive.r == 0.f);
    REQUIRE(surface().normal_texture.empty());

    engine_core::Texture& bumps = scene.game.create<engine_core::Texture>();
    scene.game.set_parent(bumps.id(), scene.game.service("Textures"));
    REQUIRE_FALSE(bumps.set_path("textures/bumps.png"));
    engine_core::Material& steel = scene.game.create<engine_core::Material>();
    scene.game.set_parent(steel.id(), scene.game.service("Materials"));
    REQUIRE_FALSE(steel.set_reference(engine_core::Material::kNormalTextureReference, instance_slot(bumps.id())));
    REQUIRE_FALSE(steel.set_reference(engine_core::Material::kRoughnessTextureReference, instance_slot(bumps.id())));
    REQUIRE_FALSE(steel.set_metalness(1.0));
    REQUIRE_FALSE(steel.set_roughness(3.0));
    REQUIRE_FALSE(steel.set_transparency(-1.0));
    REQUIRE_FALSE(steel.set_emissive(engine_core::ColorRgb{0.f, 0.25f, 0.f, 1.f}));
    REQUIRE_FALSE(part.set_reference(engine_core::Model::kMaterialReference, instance_slot(steel.id())));
    scene.frame();
    REQUIRE(surface().normal_texture == "textures/bumps.png");
    REQUIRE(surface().roughness_texture == "textures/bumps.png");
    REQUIRE(surface().metalness_texture.empty());
    REQUIRE(surface().metalness == 1.f);
    // Stored as given, drawn clamped.
    REQUIRE(steel.roughness() == 3.0);
    REQUIRE(surface().roughness == 1.f);
    REQUIRE(surface().transparency == 0.f);
    REQUIRE(surface().emissive.g == 0.25f);
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
    scene.frame();
    REQUIRE(scene.meshes(box.id()) == Paths{"meshes/body.amesh"});

    begin_step(scene.game);
    scene.game.destroy(crate_id);
    end_step(scene.game);
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
    scene.frame();

    begin_step(scene.game);
    scene.game.destroy(id);
    end_step(scene.game);
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

TEST_CASE("position() is the Transform's translation, and set_position() keeps the rotation", "[render]") {
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

    begin_step(scene.game);
    part.set_position(engine_core::Vec3{-1.f, 0.5f, 2.f});
    end_step(scene.game);
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

TEST_CASE("a GameObject's Color and Transparency reach its row, undo, save, and come back at Stop", "[render]") {
    Scene scene;
    engine_core::GameObject& box = scene.object(0);
    const InstanceId id = box.id();
    scene.frame();
    REQUIRE(scene.row(id)->color.g == 1.f);
    REQUIRE(scene.row(id)->transparency == 0.f);

    // Defaults save nothing.
    engine_core::PropertyBag saved;
    box.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Color") == nullptr);
    REQUIRE(engine_core::bag_find(saved, "Transparency") == nullptr);

    begin_step(scene.game, "Tint");
    REQUIRE_FALSE(box.set_color(rgb(1.f, 0.5f, 0.25f)));
    REQUIRE_FALSE(box.set_transparency(0.5));
    end_step(scene.game);
    scene.frame();
    REQUIRE(scene.row(id)->color.g == 0.5f);
    REQUIRE(scene.row(id)->color.b == 0.25f);
    REQUIRE(scene.row(id)->transparency == 0.5f);

    // Stored as given, drawn clamped.
    begin_step(scene.game, "Hide");
    REQUIRE_FALSE(box.set_transparency(3.0));
    end_step(scene.game);
    scene.frame();
    REQUIRE(box.transparency() == 3.0);
    REQUIRE(scene.row(id)->transparency == 1.f);
    REQUIRE(*box.set_transparency(std::nan("")) == "Transparency must be a finite number");
    REQUIRE(*box.set_color(rgb(std::nanf(""), 0.f, 0.f)) == "Color must be finite");

    saved.clear();
    box.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Color") != nullptr);
    REQUIRE(engine_core::bag_find(saved, "Transparency")->as_number() == 3.0);

    scene.game.history().undo();
    REQUIRE(box.transparency() == 0.5);
    scene.game.history().undo();
    REQUIRE(box.color().g == 1.f);
    REQUIRE(box.transparency() == 0.0);
    scene.frame();
    REQUIRE(scene.row(id)->color.g == 1.f);

    REQUIRE_FALSE(box.set_transparency(0.25));
    scene.game.capture_place();
    scene.game.start_simulation();
    REQUIRE_FALSE(scene.game.game_object(id)->set_transparency(0.75));
    REQUIRE_FALSE(scene.game.game_object(id)->set_color(rgb(0.f, 0.f, 0.f)));
    scene.game.stop_simulation();
    scene.frame();
    REQUIRE(scene.game.game_object(id)->transparency() == 0.25);
    REQUIRE(scene.game.game_object(id)->color().r == 1.f);
    REQUIRE(scene.row(id)->transparency == 0.25f);
}

TEST_CASE("a GameObject's Scale reaches its row, undoes, saves, and comes back at Stop", "[render]") {
    Scene scene;
    engine_core::GameObject& box = scene.object(0);
    const InstanceId id = box.id();
    scene.frame();
    REQUIRE(box.scale() == 1.0);
    REQUIRE(scene.row(id)->scale == 1.f);

    // The default saves nothing.
    engine_core::PropertyBag saved;
    box.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Scale") == nullptr);

    begin_step(scene.game, "Grow");
    REQUIRE_FALSE(box.set_scale(2.5));
    end_step(scene.game);
    scene.frame();
    REQUIRE(scene.row(id)->scale == 2.5f);
    // The Transform is the GameObject's own; Scale does not change it.
    REQUIRE(box.transform().m[0] == 1.f);

    REQUIRE(*box.set_scale(0.0) == "Scale must be a finite number above 0");
    REQUIRE(*box.set_scale(-1.0) == "Scale must be a finite number above 0");
    REQUIRE(*box.set_scale(std::nan("")) == "Scale must be a finite number above 0");
    REQUIRE(box.scale() == 2.5);

    box.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Scale")->as_number() == 2.5);

    scene.game.history().undo();
    REQUIRE(box.scale() == 1.0);
    scene.frame();
    REQUIRE(scene.row(id)->scale == 1.f);

    REQUIRE_FALSE(box.set_scale(0.5));
    scene.game.capture_place();
    scene.game.start_simulation();
    REQUIRE_FALSE(scene.game.game_object(id)->set_scale(4.0));
    scene.game.stop_simulation();
    scene.frame();
    REQUIRE(scene.game.game_object(id)->scale() == 0.5);
    REQUIRE(scene.row(id)->scale == 0.5f);
}

TEST_CASE("scripts read and write a GameObject's Scale", "[render]") {
    const engine_core::LuaField* field = engine_core::lua_class_find("GameObject", "Scale");
    REQUIRE(field != nullptr);
    REQUIRE(std::string(field->type_name) == "number");
    REQUIRE(field->writable);
}

TEST_CASE("a Light's Color is its own, not the GameObject tint", "[render]") {
    const engine_core::LuaField* field = engine_core::lua_class_find("PointLight", "Color");
    REQUIRE(field != nullptr);
    std::vector<engine_core::LuaField> saved = engine_core::lua_saved_fields("PointLight");
    int colors = 0;
    for (const engine_core::LuaField& each : saved) {
        colors += std::string(each.name) == "Color" ? 1 : 0;
    }
    REQUIRE(colors == 1);
    REQUIRE(field->read == engine_core::lua_class_find("SpotLight", "Color")->read);
    REQUIRE(field->read != engine_core::lua_class_find("GameObject", "Color")->read);
}
