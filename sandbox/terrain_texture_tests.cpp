// Terrain texture settings: Enum.TextureSize, Material's HeightTexture,
// TextureScale, BlendSharpness, and HeightStrength, Terrain.TextureSize, and
// Lighting.TerrainQuality. Task 1 of the terrain textures sub-project: the
// user-facing settings later tasks (texture arrays, the shader) consume.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Enum.hpp"
#include "Lighting.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"
#include "Terrain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace {

using engine_core::EffectQuality;
using engine_core::Lighting;
using engine_core::Material;
using engine_core::Terrain;
using engine_core::TextureSize;

std::string reason(const std::optional<std::string>& value) { return value ? *value : std::string(); }

Lighting& lighting_in(engine_core::Game& game) {
    auto* lighting = dynamic_cast<Lighting*>(game.instance(game.scene_service("Lighting")));
    REQUIRE(lighting != nullptr);
    return *lighting;
}

Terrain& add_terrain(engine_core::Game& game) {
    Terrain& terrain = game.create<Terrain>();
    game.set_parent(terrain.id(), workspace_of(game));
    return terrain;
}

Material& add_material(engine_core::Game& game, const char* name) {
    Material& material = game.create<Material>();
    game.set_name(material.id(), name);
    game.set_parent(material.id(), game.service("Materials"));
    return material;
}

engine_core::InstanceId add_texture(engine_core::Game& game, const char* name) {
    engine_core::DataModel& texture = game.create<engine_core::Texture>();
    game.set_name(texture.id(), name);
    game.set_parent(texture.id(), game.service("Textures"));
    return texture.id();
}

}  // namespace

TEST_CASE("TX1 Enum.TextureSize has Small, Medium, Large, and Max", "[terrain][textures]") {
    const engine_core::EnumType& type = engine_core::texture_size_enum();
    REQUIRE(engine_core::enum_item_value(type, "Small") == 0);
    REQUIRE(engine_core::enum_item_value(type, "Medium") == 1);
    REQUIRE(engine_core::enum_item_value(type, "Large") == 2);
    REQUIRE(engine_core::enum_item_value(type, "Max") == 3);
    REQUIRE(engine_core::enum_item_name(type, 4) == nullptr);
}

TEST_CASE("TX2 Material's new properties default, save, undo, and refuse bad values", "[terrain][textures]") {
    SimRole role;
    engine_core::Game game;
    Material& material = add_material(game, "Rock");

    REQUIRE(material.texture_scale() == 8.0);
    REQUIRE(material.blend_sharpness() == 0.5);
    REQUIRE(material.height_strength() == 1.0);
    REQUIRE(material.reference(Material::kHeightTextureReference).kind == engine_core::LuaSlot::Kind::Nil);

    // Defaults save nothing.
    engine_core::PropertyBag saved;
    material.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "TextureScale") == nullptr);
    REQUIRE(engine_core::bag_find(saved, "BlendSharpness") == nullptr);
    REQUIRE(engine_core::bag_find(saved, "HeightStrength") == nullptr);
    REQUIRE(engine_core::bag_find(saved, "HeightTexture") == nullptr);

    begin_step(game, "Set texture settings");
    REQUIRE_FALSE(material.set_texture_scale(16.0));
    REQUIRE_FALSE(material.set_blend_sharpness(0.25));
    REQUIRE_FALSE(material.set_height_strength(2.0));
    end_step(game);
    REQUIRE(material.texture_scale() == 16.0);
    REQUIRE(material.blend_sharpness() == 0.25);
    REQUIRE(material.height_strength() == 2.0);

    game.history().undo();
    REQUIRE(material.texture_scale() == 8.0);
    REQUIRE(material.blend_sharpness() == 0.5);
    REQUIRE(material.height_strength() == 1.0);
    game.history().redo();
    REQUIRE(material.texture_scale() == 16.0);
    REQUIRE(material.blend_sharpness() == 0.25);
    REQUIRE(material.height_strength() == 2.0);

    // Bad values are refused with the exact message, changing nothing.
    REQUIRE(reason(material.set_texture_scale(0.0)) == "TextureScale must be greater than 0");
    REQUIRE(reason(material.set_texture_scale(-1.0)) == "TextureScale must be greater than 0");
    REQUIRE(reason(material.set_texture_scale(std::numeric_limits<double>::quiet_NaN())) ==
            "TextureScale must be greater than 0");
    REQUIRE(material.texture_scale() == 16.0);

    REQUIRE(reason(material.set_blend_sharpness(-0.01)) == "BlendSharpness must be from 0 to 1");
    REQUIRE(reason(material.set_blend_sharpness(1.01)) == "BlendSharpness must be from 0 to 1");
    REQUIRE(material.blend_sharpness() == 0.25);

    REQUIRE(reason(material.set_height_strength(-0.5)) == "HeightStrength must not be negative");
    REQUIRE(material.height_strength() == 2.0);

    // HeightTexture accepts a Texture and nil, like Material's other texture references.
    const engine_core::InstanceId bump = add_texture(game, "Bump");
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = bump;
    REQUIRE_FALSE(material.set_reference(Material::kHeightTextureReference, slot));
    REQUIRE(material.reference(Material::kHeightTextureReference).id == bump);

    engine_core::LuaSlot nil_slot;
    nil_slot.kind = engine_core::LuaSlot::Kind::Nil;
    REQUIRE_FALSE(material.set_reference(Material::kHeightTextureReference, nil_slot));
    REQUIRE(material.reference(Material::kHeightTextureReference).kind == engine_core::LuaSlot::Kind::Nil);

    const engine_core::InstanceId mesh = game.create<engine_core::Mesh>().id();
    game.set_parent(mesh, game.service("Meshes"));
    engine_core::LuaSlot wrong;
    wrong.kind = engine_core::LuaSlot::Kind::Instance;
    wrong.id = mesh;
    REQUIRE(reason(material.set_reference(Material::kHeightTextureReference, wrong)) ==
            "HeightTexture must be a Texture");

    // What save writes, a load reads back.
    saved.clear();
    material.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "TextureScale")->as_number() == 16.0);
    REQUIRE(engine_core::bag_find(saved, "BlendSharpness")->as_number() == 0.25);
    REQUIRE(engine_core::bag_find(saved, "HeightStrength")->as_number() == 2.0);
    REQUIRE(engine_core::bag_find(saved, "HeightTexture") == nullptr);  // cleared back to nil above
}

TEST_CASE("TX3 Terrain.TextureSize defaults to Large, saves, and undoes", "[terrain][textures]") {
    SimRole role;
    engine_core::Game game;
    Terrain& terrain = add_terrain(game);
    REQUIRE(terrain.texture_size() == TextureSize::Large);

    engine_core::PropertyBag saved;
    terrain.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "TextureSize") == nullptr);

    begin_step(game, "Set TextureSize");
    REQUIRE_FALSE(terrain.set_texture_size(static_cast<int>(TextureSize::Small)));
    end_step(game);
    REQUIRE(terrain.texture_size() == TextureSize::Small);
    game.history().undo();
    REQUIRE(terrain.texture_size() == TextureSize::Large);
    game.history().redo();
    REQUIRE(terrain.texture_size() == TextureSize::Small);

    REQUIRE(reason(terrain.set_texture_size(7)) == "TextureSize must be an Enum.TextureSize");
    REQUIRE(terrain.texture_size() == TextureSize::Small);

    saved.clear();
    terrain.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "TextureSize") != nullptr);
}

TEST_CASE("TX4 Lighting.TerrainQuality defaults to High and reaches the snapshot", "[terrain][textures]") {
    SimRole role;
    engine_core::Game game;
    Lighting& lighting = lighting_in(game);
    REQUIRE(lighting.terrain_quality() == EffectQuality::High);

    engine_core::PropertyBag saved;
    lighting.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "TerrainQuality") == nullptr);

    begin_step(game, "Set TerrainQuality");
    REQUIRE_FALSE(lighting.set_terrain_quality(static_cast<int>(EffectQuality::Low)));
    end_step(game);
    REQUIRE(lighting.terrain_quality() == EffectQuality::Low);
    game.history().undo();
    REQUIRE(lighting.terrain_quality() == EffectQuality::High);
    game.history().redo();
    REQUIRE(lighting.terrain_quality() == EffectQuality::Low);

    REQUIRE(reason(lighting.set_terrain_quality(9)) == "TerrainQuality must be an Enum.EffectQuality");

    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    REQUIRE(pump.front().lighting.terrain_quality == static_cast<int>(EffectQuality::Low));
    REQUIRE_FALSE(lighting.set_terrain_quality(static_cast<int>(EffectQuality::High)));
    frame();
    REQUIRE(pump.front().lighting.terrain_quality == static_cast<int>(EffectQuality::High));
}
