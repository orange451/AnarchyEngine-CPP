// ViewportPane scenes in the render snapshot: what a pane draws, and that its
// revision moves exactly when that changes.

#include "skinning_support.hpp"

#include "AssetInstances.hpp"
#include "Bone.hpp"
#include "Brush.hpp"
#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "Light.hpp"
#include "Skybox.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

namespace {

using engine_core::InstanceId;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// The published scene of pane in pump's front snapshot, or null.
std::shared_ptr<const engine_core::VisualViewport> published(const engine_core::SnapshotPump& pump, InstanceId pane) {
    for (const auto& found : pump.front().viewports) {
        if (found != nullptr && found->pane == pane) {
            return found;
        }
    }
    return nullptr;
}

// A game with a pump, a ScreenGui under Gui holding a pane, and a Prefab of one Model on a Mesh.
struct PaneScene {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    engine_core::ViewportPane* pane = nullptr;
    engine_core::Mesh* mesh = nullptr;
    engine_core::Material* material = nullptr;
    engine_core::Prefab* prefab = nullptr;

    PaneScene() {
        pump.reserve(engine_core::DataModel::kMaxInstances);
        engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
        game.set_parent(screen.id(), game.scene_service("Gui"));
        pane = &game.create<engine_core::ViewportPane>();
        game.set_parent(pane->id(), screen.id());
        mesh = &game.create<engine_core::Mesh>();
        game.set_parent(mesh->id(), game.service("Meshes"));
        REQUIRE_FALSE(mesh->set_path("meshes/box.amesh"));
        material = &game.create<engine_core::Material>();
        game.set_parent(material->id(), game.service("Materials"));
        prefab = &game.create<engine_core::Prefab>();
        game.set_parent(prefab->id(), game.service("Prefabs"));
        engine_core::Model& model = game.create<engine_core::Model>();
        game.set_parent(model.id(), prefab->id());
        REQUIRE_FALSE(model.set_reference(engine_core::Model::kMeshReference, instance_slot(mesh->id())));
        REQUIRE_FALSE(model.set_reference(engine_core::Model::kMaterialReference, instance_slot(material->id())));
    }

    void frame() {
        pump.prepare_copy(game);
        pump.publish();
    }

    engine_core::GameObject& object(InstanceId parent) {
        engine_core::GameObject& made = game.create_game_object();
        game.set_parent(made.id(), parent);
        REQUIRE_FALSE(made.set_prefab(instance_slot(prefab->id())));
        return made;
    }

    std::shared_ptr<const engine_core::VisualViewport> scene(InstanceId id) { return published(pump, id); }

    std::uint64_t revision() {
        const auto found = scene(pane->id());
        REQUIRE(found != nullptr);
        return found->revision;
    }
};

}  // namespace

TEST_CASE("VS1 a pane's scene holds its GameObjects, lights, and Skybox, and the place's snapshot does not",
          "[viewport]") {
    PaneScene s;
    engine_core::GameObject& thing = s.object(s.pane->id());
    thing.set_transform(engine_core::matrix4_translation(1.f, 2.f, 3.f));
    engine_core::PointLight& light = s.game.create<engine_core::PointLight>();
    s.game.set_parent(light.id(), s.pane->id());
    engine_core::Skybox& sky = s.game.create<engine_core::Skybox>();
    s.game.set_parent(sky.id(), s.pane->id());
    s.frame();
    const auto scene = s.scene(s.pane->id());
    REQUIRE(scene != nullptr);
    REQUIRE(scene->rows.size() == 2);
    REQUIRE(scene->rows[0].id == thing.id());
    REQUIRE(scene->rows[0].world.m[13] == 2.f);
    REQUIRE(scene->rows[0].prefab != 0);
    REQUIRE(scene->prefabs[scene->rows[0].prefab]->meshes.size() == 1);
    REQUIRE(scene->prefabs[scene->rows[0].prefab]->meshes[0].path == "meshes/box.amesh");
    REQUIRE(scene->rows[1].light.kind == engine_core::VisualLight::Kind::Point);
    REQUIRE(scene->sky.present);
    REQUIRE(s.pump.find(thing.id()) == nullptr);
    REQUIRE_FALSE(s.pump.front().sky.present);
}

TEST_CASE("VS2 nothing changed, the same scene pointer and revision", "[viewport]") {
    PaneScene s;
    s.object(s.pane->id());
    s.frame();
    const auto first = s.scene(s.pane->id());
    s.frame();
    s.frame();
    REQUIRE(s.scene(s.pane->id()) == first);
}

TEST_CASE("VS3 a Transform, a Color, and a pane property each move the revision", "[viewport]") {
    PaneScene s;
    engine_core::GameObject& thing = s.object(s.pane->id());
    s.frame();
    std::uint64_t last = s.revision();
    thing.set_transform(engine_core::matrix4_translation(0.f, 1.f, 0.f));
    s.frame();
    REQUIRE(s.revision() != last);
    last = s.revision();
    REQUIRE_FALSE(thing.set_color(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}));
    s.frame();
    REQUIRE(s.revision() != last);
    last = s.revision();
    REQUIRE_FALSE(s.pane->set_ambient(engine_core::ColorRgb{0.1f, 0.1f, 0.1f, 1.f}));
    s.frame();
    REQUIRE(s.revision() != last);
    REQUIRE(s.scene(s.pane->id())->ambient.r == 0.1f);
}

TEST_CASE("VS4 a Material, a Mesh path, and a Prefab's Model each move the revision", "[viewport]") {
    PaneScene s;
    s.object(s.pane->id());
    s.frame();
    std::uint64_t last = s.revision();
    REQUIRE_FALSE(s.material->set_color(engine_core::ColorRgb{0.f, 1.f, 0.f, 1.f}));
    s.frame();
    REQUIRE(s.revision() != last);
    last = s.revision();
    REQUIRE_FALSE(s.mesh->set_path("meshes/other.amesh"));
    s.frame();
    REQUIRE(s.revision() != last);
    REQUIRE(s.scene(s.pane->id())->prefabs[1]->meshes[0].path == "meshes/other.amesh");
    last = s.revision();
    engine_core::Model& second = s.game.create<engine_core::Model>();
    s.game.set_parent(second.id(), s.prefab->id());
    REQUIRE_FALSE(second.set_reference(engine_core::Model::kMeshReference, instance_slot(s.mesh->id())));
    s.frame();
    REQUIRE(s.revision() != last);
}

TEST_CASE("VS5 the camera: CurrentCamera, else the first inside, else none; one outside the pane counts",
          "[viewport]") {
    PaneScene s;
    s.frame();
    REQUIRE_FALSE(s.scene(s.pane->id())->has_camera);
    engine_core::Camera& inside = s.game.create<engine_core::Camera>();
    s.game.set_parent(inside.id(), s.pane->id());
    s.frame();
    auto scene = s.scene(s.pane->id());
    REQUIRE(scene->has_camera);
    REQUIRE(scene->camera == inside.id());
    REQUIRE(scene->camera_inside);
    engine_core::Camera& outside = s.game.create<engine_core::Camera>();
    s.game.set_parent(outside.id(), s.game.scene_service("Workspace"));
    REQUIRE_FALSE(s.pane->set_current_camera(instance_slot(outside.id())));
    s.frame();
    scene = s.scene(s.pane->id());
    REQUIRE(scene->camera == outside.id());
    REQUIRE_FALSE(scene->camera_inside);
    const std::uint64_t last = scene->revision;
    outside.set_transform(engine_core::matrix4_translation(0.f, 0.f, 9.f));
    s.frame();
    REQUIRE(s.revision() != last);
    REQUIRE(s.scene(s.pane->id())->camera_world.m[14] == 9.f);
}

TEST_CASE("VS6 CurrentCamera deleted and undone: the first Camera inside, then CurrentCamera again", "[viewport]") {
    PaneScene s;
    engine_core::Camera& fallback = s.game.create<engine_core::Camera>();
    s.game.set_parent(fallback.id(), s.pane->id());
    engine_core::Camera& chosen = s.game.create<engine_core::Camera>();
    s.game.set_parent(chosen.id(), s.pane->id());
    REQUIRE_FALSE(s.pane->set_current_camera(instance_slot(chosen.id())));
    const std::string guid = s.game.guid(chosen.id());
    s.frame();
    REQUIRE(s.scene(s.pane->id())->camera == chosen.id());
    begin_step(s.game, "Delete");
    s.game.destroy_tree(chosen.id());
    end_step(s.game);
    s.frame();
    REQUIRE(s.scene(s.pane->id())->camera == fallback.id());
    s.game.history().undo();
    s.frame();
    REQUIRE(s.scene(s.pane->id())->camera == *s.game.find_guid(guid));
}

TEST_CASE("VS7 a skinned GameObject's new pose moves the revision", "[viewport]") {
    skinning_test::Rig rig;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::ScreenGui& screen = rig.game.create<engine_core::ScreenGui>();
    rig.game.set_parent(screen.id(), rig.game.scene_service("Gui"));
    engine_core::ViewportPane& pane = rig.game.create<engine_core::ViewportPane>();
    rig.game.set_parent(pane.id(), screen.id());
    engine_core::GameObject& arm = rig.arm_object();
    rig.game.set_parent(arm.id(), pane.id());
    engine_core::Bone& hand = rig.game.create<engine_core::Bone>();
    rig.game.set_name(hand.id(), "Hand");
    rig.game.set_parent(hand.id(), arm.id());
    const auto frame = [&] {
        pump.prepare_copy(rig.game);
        pump.publish();
        return published(pump, pane.id());
    };
    const auto first = frame();
    REQUIRE(first != nullptr);
    REQUIRE(first->rows.size() == 1);
    REQUIRE(first->rows[0].pose != nullptr);
    REQUIRE(frame() == first);
    REQUIRE_FALSE(hand.set_offset(skinning_test::spin_z()));
    const auto second = frame();
    REQUIRE(second->revision != first->revision);
    REQUIRE(second->rows[0].pose != first->rows[0].pose);
}

TEST_CASE("VS8 contents moved between two panes leave neither stale", "[viewport]") {
    PaneScene s;
    engine_core::ViewportPane& other = s.game.create<engine_core::ViewportPane>();
    s.game.set_parent(other.id(), s.game.parent(s.pane->id()));
    engine_core::GameObject& thing = s.object(s.pane->id());
    s.frame();
    REQUIRE(s.scene(s.pane->id())->rows.size() == 1);
    REQUIRE(s.scene(other.id())->rows.empty());
    s.game.set_parent(thing.id(), other.id());
    s.frame();
    REQUIRE(s.scene(s.pane->id())->rows.empty());
    REQUIRE(s.scene(other.id())->rows.size() == 1);
    // A pane moved to another ScreenGui keeps its scene.
    engine_core::ScreenGui& second = s.game.create<engine_core::ScreenGui>();
    s.game.set_parent(second.id(), s.game.scene_service("Gui"));
    s.game.set_parent(other.id(), second.id());
    s.frame();
    REQUIRE(s.scene(other.id())->rows.size() == 1);
}

TEST_CASE("VS9 Stop shows the restored scene on the next frame", "[viewport]") {
    PaneScene s;
    engine_core::GameObject& thing = s.object(s.pane->id());
    thing.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));
    s.frame();
    s.game.capture_place();
    s.game.start_simulation();
    thing.set_transform(engine_core::matrix4_translation(5.f, 0.f, 0.f));
    REQUIRE_FALSE(thing.set_color(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}));
    s.frame();
    REQUIRE(s.scene(s.pane->id())->rows[0].world.m[12] == 5.f);
    const std::uint64_t playing = s.revision();
    // Stop relinks the tree and asks for a resync; the pane is built again.
    s.game.stop_simulation();
    s.frame();
    const auto restored = s.scene(s.pane->id());
    REQUIRE(restored->revision != playing);
    REQUIRE(restored->rows.size() == 1);
    REQUIRE(restored->rows[0].world.m[12] == 1.f);
    REQUIRE(restored->rows[0].color.g == 1.f);
}

TEST_CASE("VS10 a pane destroyed leaves the list", "[viewport]") {
    PaneScene s;
    s.frame();
    REQUIRE(s.scene(s.pane->id()) != nullptr);
    const InstanceId gone = s.pane->id();
    s.game.destroy_tree(gone);
    s.frame();
    REQUIRE(s.scene(gone) == nullptr);
}

TEST_CASE("VS11 a Brush in a pane draws in the pane, on its own, and not in the place", "[viewport]") {
    PaneScene s;
    engine_core::Brush& brush = s.game.create<engine_core::Brush>();
    s.game.set_parent(brush.id(), s.pane->id());
    s.frame();
    const auto scene = s.scene(s.pane->id());
    REQUIRE(scene != nullptr);
    REQUIRE_FALSE(scene->brushes.empty());
    for (const engine_core::VisualBrushDraw& draw : scene->brushes) {
        REQUIRE(draw.owner == brush.id());
        REQUIRE(draw.look.session != nullptr);
    }
    REQUIRE(s.pump.front().brushes.empty());
    // Nothing changed: the same scene.
    s.frame();
    REQUIRE(s.scene(s.pane->id()) == scene);
}

TEST_CASE("VS12 a pane Brush's color, transparency, and Transform each move the revision", "[viewport]") {
    PaneScene s;
    engine_core::Brush& brush = s.game.create<engine_core::Brush>();
    s.game.set_parent(brush.id(), s.pane->id());
    s.frame();
    std::uint64_t last = s.revision();
    REQUIRE_FALSE(brush.set_color(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}));
    s.frame();
    REQUIRE(s.revision() != last);
    last = s.revision();
    REQUIRE_FALSE(brush.set_transparency(0.5));
    s.frame();
    REQUIRE(s.revision() != last);
    REQUIRE(s.scene(s.pane->id())->brushes[0].transparency == 0.5f);
    last = s.revision();
    REQUIRE_FALSE(brush.set_transform(engine_core::matrix4_translation(3.f, 0.f, 0.f)));
    s.frame();
    REQUIRE(s.revision() != last);
    REQUIRE(s.scene(s.pane->id())->brushes[0].world.m[12] == 3.f);
}
