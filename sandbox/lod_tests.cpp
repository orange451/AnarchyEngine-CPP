// LodLayer: what a Prefab draws from a distance on, instead of its Models.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "runner/DrawBatches.hpp"
#include "runner/RenderMath.hpp"
#include "runner/ShadowPlanner.hpp"
#include "runner/Visibility.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using engine_core::InstanceId;
using engine_core::LodKind;
using engine_core::LodLayer;

engine_core::Prefab& add_prefab(engine_core::DataModel& game, const char* name) {
    engine_core::Prefab& made = game.create<engine_core::Prefab>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), game.service("Prefabs"));
    return made;
}

LodLayer& add_layer(engine_core::DataModel& game, InstanceId prefab, const char* name) {
    LodLayer& made = game.create<LodLayer>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), prefab);
    return made;
}

engine_core::Material& add_material(engine_core::DataModel& game, const char* name) {
    engine_core::Material& made = game.create<engine_core::Material>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), game.service("Materials"));
    return made;
}

engine_core::Texture& add_texture(engine_core::DataModel& game, const char* name, const char* path) {
    engine_core::Texture& made = game.create<engine_core::Texture>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), game.service("Textures"));
    REQUIRE_FALSE(made.set_path(path));
    return made;
}

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// The registered field named name.
const engine_core::LuaField& field(const char* name) {
    const engine_core::LuaField* found = engine_core::lua_class_find("LodLayer", name);
    REQUIRE(found != nullptr);
    return *found;
}

}  // namespace

TEST_CASE("LOD1 a LodLayer's Distance, Kind, Material and Size: defaults, limits, and the Lua fields",
          "[lod]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("LodLayer"));
    REQUIRE(engine_core::lua_creatable_known("LodLayer"));
    engine_core::Prefab& prefab = add_prefab(game, "Tree");
    LodLayer& layer = add_layer(game, prefab.id(), "Far");
    REQUIRE(layer.distance() == LodLayer::kDefaultDistance);
    REQUIRE(layer.kind() == LodKind::Model);
    REQUIRE(layer.size().x == 4.f);
    REQUIRE(layer.size().y == 8.f);
    // A layer at its defaults saves nothing.
    engine_core::PropertyBag saved;
    layer.save_properties(saved);
    REQUIRE(saved.empty());

    REQUIRE_FALSE(layer.set_distance(250.0));
    REQUIRE(layer.distance() == 250.0);
    REQUIRE(layer.set_distance(-1.0).has_value());
    REQUIRE(layer.distance() == 250.0);
    REQUIRE_FALSE(layer.set_kind(static_cast<int>(LodKind::Billboard)));
    REQUIRE(layer.kind() == LodKind::Billboard);
    REQUIRE(*layer.set_kind(7) == "Kind must be an Enum.LodKind");
    REQUIRE_FALSE(layer.set_size({3.f, 12.f}));
    REQUIRE(layer.set_size({0.f, 1.f}).has_value());
    REQUIRE(layer.size().x == 3.f);
    engine_core::Material& card = add_material(game, "TreeCard");
    REQUIRE_FALSE(layer.set_reference(LodLayer::kMaterialReference, instance_slot(card.id())));
    REQUIRE(layer.reference(LodLayer::kMaterialReference).id == card.id());
    // Only a Material: a Texture is refused.
    engine_core::Texture& texture = add_texture(game, "TreeCardTexture", "textures/tree_card.png");
    REQUIRE(layer.set_reference(LodLayer::kMaterialReference, instance_slot(texture.id())).has_value());
    REQUIRE(layer.reference(LodLayer::kMaterialReference).id == card.id());

    // The Lua fields read and write the same values.
    engine_core::LuaSlot slot;
    REQUIRE(field("Distance").read(game, layer, slot));
    REQUIRE(slot.number == 250.0);
    REQUIRE(field("Kind").read(game, layer, slot));
    REQUIRE(slot.enum_type == &engine_core::lod_kind_enum());
    REQUIRE(slot.number == static_cast<int>(LodKind::Billboard));
    REQUIRE(std::string(engine_core::enum_item_name(engine_core::lod_kind_enum(), 2)) == "None");
    slot.number = static_cast<int>(LodKind::None);
    REQUIRE(field("Kind").write(game, layer, slot));
    REQUIRE(layer.kind() == LodKind::None);
    REQUIRE(field("Size").read(game, layer, slot));
    REQUIRE(slot.vec.x == 3.f);
    REQUIRE(slot.vec.y == 12.f);
    REQUIRE(field("Material").read(game, layer, slot));
    REQUIRE(slot.id == card.id());
}

TEST_CASE("LOD2 a LodLayer lives in a Prefab, beside its Models, and holds only Models", "[lod]") {
    using engine_core::placement_error;
    REQUIRE_FALSE(placement_error("Prefab", "LodLayer", "Far").has_value());
    REQUIRE_FALSE(placement_error("Prefab", "Model", "Trunk").has_value());
    REQUIRE_FALSE(placement_error("LodLayer", "Model", "Trunk").has_value());
    REQUIRE(*placement_error("LodLayer", "Folder", "F") == "A LodLayer holds only Models");
    REQUIRE(*placement_error("LodLayer", "LodLayer", "L") == "A LodLayer holds only Models");
    REQUIRE(*placement_error("Prefab", "Folder", "F") == "A Prefab holds only Models and LodLayers");
    REQUIRE(placement_error("Workspace", "LodLayer", "Far").has_value());
}

TEST_CASE("LOD3 each LodLayer edit is one undo step", "[lod]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Prefab& prefab = add_prefab(game, "Tree");
    LodLayer& layer = add_layer(game, prefab.id(), "Far");
    begin_step(game, "Set Distance");
    REQUIRE_FALSE(layer.set_distance(300.0));
    end_step(game);
    begin_step(game, "Set Kind");
    REQUIRE_FALSE(layer.set_kind(static_cast<int>(LodKind::None)));
    end_step(game);
    begin_step(game, "Set Size");
    REQUIRE_FALSE(layer.set_size({2.f, 5.f}));
    end_step(game);
    game.history().undo();
    REQUIRE(layer.size().x == 4.f);
    REQUIRE(layer.kind() == LodKind::None);
    game.history().undo();
    REQUIRE(layer.kind() == LodKind::Model);
    REQUIRE(layer.distance() == 300.0);
    game.history().undo();
    REQUIRE(layer.distance() == LodLayer::kDefaultDistance);
    game.history().redo();
    game.history().redo();
    game.history().redo();
    REQUIRE(layer.distance() == 300.0);
    REQUIRE(layer.kind() == LodKind::None);
    REQUIRE(layer.size().y == 5.f);
}

TEST_CASE("LOD4 a Prefab's LodLayers, their Models and Material survive a save and a load", "[lod][project]") {
    SimRole role;
    TempDir dir;
    std::string billboard_guid;
    std::string swap_guid;
    std::string material_guid;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        engine_core::Prefab& prefab = add_prefab(game, "Tree");
        engine_core::Model& base = game.create<engine_core::Model>();
        game.set_name(base.id(), "Trunk");
        game.set_parent(base.id(), prefab.id());
        LodLayer& swap = add_layer(game, prefab.id(), "Low");
        REQUIRE_FALSE(swap.set_distance(80.0));
        engine_core::Model& low = game.create<engine_core::Model>();
        game.set_name(low.id(), "TrunkLow");
        game.set_parent(low.id(), swap.id());
        LodLayer& billboard = add_layer(game, prefab.id(), "Card");
        REQUIRE_FALSE(billboard.set_distance(200.0));
        REQUIRE_FALSE(billboard.set_kind(static_cast<int>(LodKind::Billboard)));
        REQUIRE_FALSE(billboard.set_size({5.f, 11.f}));
        engine_core::Material& card = add_material(game, "Card");
        REQUIRE_FALSE(billboard.set_reference(LodLayer::kMaterialReference, instance_slot(card.id())));
        billboard_guid = game.guid(billboard.id());
        swap_guid = game.guid(swap.id());
        material_guid = game.guid(card.id());
        project.save();
    }
    engine_core::Project loaded = engine_core::Project::load(dir.path);
    engine_core::DataModel& game = loaded.datamodel();
    const auto* swap = dynamic_cast<const LodLayer*>(game.instance(*loaded.instance_for(swap_guid)));
    const auto* billboard = dynamic_cast<const LodLayer*>(game.instance(*loaded.instance_for(billboard_guid)));
    REQUIRE(swap != nullptr);
    REQUIRE(billboard != nullptr);
    REQUIRE(swap->distance() == 80.0);
    REQUIRE(swap->kind() == LodKind::Model);
    REQUIRE(game.get_children(swap->id()).size() == 1);
    REQUIRE(game.name(game.get_children(swap->id())[0]) == "TrunkLow");
    REQUIRE(billboard->distance() == 200.0);
    REQUIRE(billboard->kind() == LodKind::Billboard);
    REQUIRE(billboard->size().x == 5.f);
    REQUIRE(billboard->size().y == 11.f);
    REQUIRE(billboard->reference(LodLayer::kMaterialReference).id == *loaded.instance_for(material_guid));
}

namespace {

const float kUnitMin[3] = {-0.5f, -0.5f, -0.5f};
const float kUnitMax[3] = {0.5f, 0.5f, 0.5f};

// From eye toward target, 60 degrees tall, square.
runner::CameraView LookFrom(engine_core::Vec3 eye, engine_core::Vec3 target) {
    runner::CameraView camera;
    camera.world = engine_core::matrix4_look_at(eye, target, {0.f, 1.f, 0.f});
    camera.viewProjection = engine_core::matrix4_multiply(runner::Perspective(60.f, 1.f, 0.1f, 1000.f),
                                                          runner::LookAtView(eye, target, {0.f, 1.f, 0.f}));
    camera.fovYDegrees = 60.f;
    camera.aspect = 1.f;
    camera.nearZ = 0.1f;
    camera.paneHeight = 100;
    return camera;
}

runner::DrawItem Banded(const engine_core::Matrix4& model, float lodNear, float lodFar) {
    runner::DrawItem item;
    item.model = &model;
    item.boundsMin = kUnitMin;
    item.boundsMax = kUnitMax;
    item.drawable = true;
    item.lodNear = lodNear;
    item.lodFar = lodFar;
    return item;
}

// Whether a is b, give or take float rounding.
bool Near(float a, float b) { return std::abs(a - b) < 1e-5f; }

std::vector<int> Seen(const runner::VisibilityResult& out) {
    std::vector<int> seen;
    for (const runner::VisibleDraw& draw : out.opaque) {
        seen.push_back(draw.index);
    }
    return seen;
}

}  // namespace

TEST_CASE("LOD5 neighbouring bands overlap across their fade so one can dither into the other", "[lod]") {
    constexpr float kForever = std::numeric_limits<float>::infinity();
    const engine_core::Matrix4 origin = engine_core::matrix4_translation(0.f, 0.f, 0.f);
    const runner::DrawItem items[2] = {Banded(origin, 0.f, 50.f), Banded(origin, 50.f, kForever)};
    runner::VisibilityResult out;
    // Just either side of 50, both are kept.
    runner::FindVisible(items, 2, LookFrom({0.f, 0.f, 49.f}, {0.f, 0.f, 0.f}), true, out);
    REQUIRE(Seen(out) == std::vector<int>{0, 1});
    runner::FindVisible(items, 2, LookFrom({0.f, 0.f, 51.f}, {0.f, 0.f, 0.f}), true, out);
    REQUIRE(Seen(out) == std::vector<int>{0, 1});
    // Past the fade, one alone.
    runner::FindVisible(items, 2, LookFrom({0.f, 0.f, 50.f + runner::LodFadeMargin(50.f) + 0.1f}, {0.f, 0.f, 0.f}),
                        true, out);
    REQUIRE(Seen(out) == std::vector<int>{1});
    REQUIRE(runner::LodFadeMargin(0.f) == 0.f);
    REQUIRE(runner::LodFadeMargin(kForever) == 0.f);
}

TEST_CASE("LOD5 a banded draw is seen and casts only while the camera is in its band", "[lod]") {
    constexpr float kForever = std::numeric_limits<float>::infinity();
    const engine_core::Matrix4 origin = engine_core::matrix4_translation(0.f, 0.f, 0.f);
    // The base layer up to 50, a layer from 50 on, and a draw with no band.
    const runner::DrawItem items[3] = {Banded(origin, 0.f, 50.f), Banded(origin, 50.f, kForever),
                                       Banded(origin, 0.f, kForever)};
    runner::VisibilityResult out;
    runner::FindVisible(items, 3, LookFrom({0.f, 0.f, 10.f}, {0.f, 0.f, 0.f}), true, out);
    REQUIRE(Seen(out) == std::vector<int>{0, 2});
    REQUIRE(out.outOfBand == std::vector<std::uint8_t>{0, 1, 0});
    // At 60 units the far layer takes over, culling on or off.
    const runner::CameraView far = LookFrom({0.f, 0.f, 60.f}, {0.f, 0.f, 0.f});
    runner::FindVisible(items, 3, far, false, out);
    REQUIRE(Seen(out) == std::vector<int>{1, 2});
    REQUIRE(out.outOfBand == std::vector<std::uint8_t>{1, 0, 0});
    REQUIRE(out.culled == 0);

    // What is outside its band casts nothing for the sun.
    const std::vector<std::uint64_t> owners = {1, 2, 3};
    const std::vector<std::uint64_t> fingerprints = {11, 12, 13};
    const runner::CasterView hidden{3, out.spheres.data(), owners.data(), fingerprints.data(), out.outOfBand.data()};
    const runner::CasterView all{3, out.spheres.data(), owners.data(), fingerprints.data()};
    runner::SunRequest sun;
    sun.shine = runner::Normalize({0.4f, -1.f, 0.3f});
    sun.shadowDistance = 120.f;
    const runner::ShadowSettings settings;
    runner::ShadowPlanner withHidden;
    runner::ShadowPlanner withAll;
    const std::vector<runner::CascadeDraw> drawn = withHidden.planCascadesView(&sun, hidden, far, settings, 500);
    const std::vector<runner::CascadeDraw> every = withAll.planCascadesView(&sun, all, far, settings, 500);
    // Every caster any cascade draws.
    const auto castersOf = [](const std::vector<runner::CascadeDraw>& cascades) {
        std::vector<int> found;
        for (const runner::CascadeDraw& cascade : cascades) {
            found.insert(found.end(), cascade.casters.begin(), cascade.casters.end());
        }
        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());
        return found;
    };
    REQUIRE(castersOf(every) == std::vector<int>{0, 1, 2});
    REQUIRE(castersOf(drawn) == std::vector<int>{1, 2});
}

TEST_CASE("LOD6 a billboard's instance carries its row's card as it is, for the vertex shader to turn",
          "[lod]") {
    // A 4 by 8 card at (10, 0, 0) (MakeRowDraw's axes), seen from above and to its side.
    engine_core::Matrix4 card = engine_core::matrix4_translation(10.f, 0.f, 0.f);
    card.m[0] = 4.f;
    card.m[5] = 8.f;
    const engine_core::Matrix4 view = runner::LookAtView({0.f, 30.f, 0.f}, {10.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    runner::DrawItem item;
    item.model = &card;
    item.boundsMin = kUnitMin;
    item.boundsMax = kUnitMax;
    item.drawable = true;
    item.slot = 1;
    item.billboard = true;
    runner::VisibilityResult visible;
    visible.opaque.push_back(runner::VisibleDraw{});
    runner::DrawBatches out;
    runner::BuildBatches(&item, visible, view, out);
    REQUIRE(out.instances.size() == 1);
    // Its model as the row made it: geometry.vert turns it (SceneRenderCheck draws that).
    const float* m = out.instances[0].model;
    for (int i = 0; i < 16; ++i) {
        REQUIRE(m[i] == card.m[i]);
    }
    // No normal matrix: the shader's normal faces the camera.
    for (const float value : out.instances[0].normal) {
        REQUIRE(value == 0.f);
    }
    REQUIRE_FALSE(out.runs[0].mirrored);
}
