// LodLayer: what a Prefab draws from a distance on, instead of its Models.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

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

TEST_CASE("LOD1 a LodLayer's Distance, Kind, Texture and Size: defaults, limits, and the Lua fields",
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
    engine_core::Texture& texture = add_texture(game, "TreeCard", "textures/tree_card.png");
    REQUIRE_FALSE(layer.set_reference(LodLayer::kTextureReference, instance_slot(texture.id())));
    REQUIRE(layer.reference(LodLayer::kTextureReference).id == texture.id());

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
    REQUIRE(field("Texture").read(game, layer, slot));
    REQUIRE(slot.id == texture.id());
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

TEST_CASE("LOD4 a Prefab's LodLayers, their Models and Texture survive a save and a load", "[lod][project]") {
    SimRole role;
    TempDir dir;
    std::string billboard_guid;
    std::string swap_guid;
    std::string texture_guid;
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
        engine_core::Texture& texture = add_texture(game, "Card", "textures/card.png");
        REQUIRE_FALSE(billboard.set_reference(LodLayer::kTextureReference, instance_slot(texture.id())));
        billboard_guid = game.guid(billboard.id());
        swap_guid = game.guid(swap.id());
        texture_guid = game.guid(texture.id());
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
    REQUIRE(billboard->reference(LodLayer::kTextureReference).id == *loaded.instance_for(texture_guid));
}
