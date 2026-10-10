// TerrainTextures: which layers each Terrain needs, caching them across
// Terrains that share a Material, building them on a worker thread off
// SimulationThread, and publishing immutable sets. Task 5 of the terrain
// textures sub-project; Task 6 uploads what this publishes.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "TerrainTextures.hpp"
#include "TerrainWorld.hpp"
#include "terrain/LayerBuilder.hpp"
#include "terrain/VoxelVolume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using engine_core::Game;
using engine_core::InstanceId;
using engine_core::LuaSlot;
using engine_core::Material;
using engine_core::Terrain;
using engine_core::TerrainMaterial;
using engine_core::TerrainTextures;
using engine_core::TerrainTextureSet;
using engine_core::TerrainWorld;
using engine_core::TextureSize;
using engine_core::Texture;

namespace {

// Writes a binary PPM (P6), as sandbox/terrain_layer_tests.cpp does: the
// simplest format stb_image can decode, so no PNG/TGA encoder is needed here.
std::filesystem::path write_ppm(const std::filesystem::path& dir, const char* name, int w, int h,
                                 const std::vector<std::uint8_t>& rgb) {
    REQUIRE(rgb.size() == size_t(w) * size_t(h) * 3);
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
    REQUIRE(bool(out));
    return path;
}

std::vector<std::uint8_t> flat_rgb(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::uint8_t> out(size_t(w) * size_t(h) * 3);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        out[i * 3 + 0] = r;
        out[i * 3 + 1] = g;
        out[i * 3 + 2] = b;
    }
    return out;
}

Terrain& add_terrain(Game& game) {
    Terrain& terrain = game.create<Terrain>();
    game.set_parent(terrain.id(), workspace_of(game));
    return terrain;
}

Material& add_material(Game& game, const char* name) {
    Material& material = game.create<Material>();
    game.set_name(material.id(), name);
    game.set_parent(material.id(), game.service("Materials"));
    return material;
}

// Creates a Texture under the Textures service whose Path is name, relative
// to game's resources root (so it must already have one set).
InstanceId add_texture(Game& game, const char* label, const char* name) {
    Texture& texture = game.create<Texture>();
    game.set_name(texture.id(), label);
    game.set_parent(texture.id(), game.service("Textures"));
    REQUIRE_FALSE(texture.set_path(name));
    return texture.id();
}

void set_diffuse(Material& material, InstanceId texture) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Instance;
    slot.id = texture;
    REQUIRE_FALSE(material.set_reference(Material::kDiffuseTextureReference, slot));
}

void set_normal(Material& material, InstanceId texture) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Instance;
    slot.id = texture;
    REQUIRE_FALSE(material.set_reference(Material::kNormalTextureReference, slot));
}

engine_core::terrain::Shape ball_at(float x, float y, float z, float r) {
    engine_core::terrain::Shape shape;
    shape.center = engine_core::Vec3{x, y, z};
    shape.radius = r;
    return shape;
}

// Runs update()+wait_idle() a few times, as TerrainWorld's own settle() in
// terrain_surface_tests.cpp does, so every build this update() queued has
// landed and been drained by a following update() before the test reads
// published()/layer_of().
void settle(TerrainTextures& textures, Game& game) {
    for (int i = 0; i < 6; ++i) {
        textures.update(game);
        textures.wait_idle();
    }
    textures.update(game);
}

}  // namespace

TEST_CASE("TT1 a Terrain with two textured Materials publishes a set with 3 layers, and the look points each Id at "
          "its layer",
          "[terrain][textures]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    write_ppm(dir.path, "diffuseA.ppm", 4, 4, flat_rgb(4, 4, 200, 10, 10));
    write_ppm(dir.path, "diffuseB.ppm", 4, 4, flat_rgb(4, 4, 10, 200, 10));

    Material& matA = add_material(game, "A");
    set_diffuse(matA, add_texture(game, "TexA", "diffuseA.ppm"));
    Material& matB = add_material(game, "B");
    set_diffuse(matB, add_texture(game, "TexB", "diffuseB.ppm"));

    Terrain& t = add_terrain(game);
    REQUIRE_FALSE(t.set_texture_size(static_cast<int>(TextureSize::Small)));
    TerrainMaterial* e1 = nullptr;
    REQUIRE_FALSE(t.add_material(matA.id(), e1));
    TerrainMaterial* e2 = nullptr;
    REQUIRE_FALSE(t.add_material(matB.id(), e2));
    REQUIRE(e1 != nullptr);
    REQUIRE(e2 != nullptr);

    TerrainTextures textures;
    settle(textures, game);

    const auto set = textures.published(t.id());
    REQUIRE(set != nullptr);
    REQUIRE(set->size == 256);
    REQUIRE(set->layers.size() == 3u);
    REQUIRE(textures.layer_of(t.id(), matA.id()) == 1);
    REQUIRE(textures.layer_of(t.id(), matB.id()) == 2);

    TerrainWorld world;
    world.set_terrain_textures(&textures);
    world.update(game);
    REQUIRE(world.views().size() == 1u);
    const engine_core::TerrainLook& look = *world.views()[0].look;
    const std::size_t row2 = 2 * 256 * 4;
    REQUIRE(look.texels[row2 + static_cast<std::size_t>(e1->material_id()) * 4] == 1.f);
    REQUIRE(look.texels[row2 + static_cast<std::size_t>(e2->material_id()) * 4] == 2.f);
}

TEST_CASE("TT2 two Terrains using the same Material at the same size share the built layer", "[terrain][textures]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    write_ppm(dir.path, "diffuse.ppm", 4, 4, flat_rgb(4, 4, 50, 60, 70));

    Material& mat = add_material(game, "Shared");
    set_diffuse(mat, add_texture(game, "Tex", "diffuse.ppm"));

    Terrain& t1 = add_terrain(game);
    Terrain& t2 = add_terrain(game);
    REQUIRE_FALSE(t1.set_texture_size(static_cast<int>(TextureSize::Small)));
    REQUIRE_FALSE(t2.set_texture_size(static_cast<int>(TextureSize::Small)));
    TerrainMaterial* e1 = nullptr;
    REQUIRE_FALSE(t1.add_material(mat.id(), e1));
    TerrainMaterial* e2 = nullptr;
    REQUIRE_FALSE(t2.add_material(mat.id(), e2));

    TerrainTextures textures;
    settle(textures, game);

    const auto set1 = textures.published(t1.id());
    const auto set2 = textures.published(t2.id());
    REQUIRE(set1 != nullptr);
    REQUIRE(set2 != nullptr);
    REQUIRE(set1->layers.size() == 2u);
    REQUIRE(set2->layers.size() == 2u);
    REQUIRE(set1->layers[0] == set2->layers[0]);   // layer 0: same empty-sources key
    REQUIRE(set1->layers[1] == set2->layers[1]);   // the shared Material's layer
}

TEST_CASE("TT3 changing a Material's TextureScale changes the look only, not the published texture set",
          "[terrain][textures]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    write_ppm(dir.path, "diffuse.ppm", 4, 4, flat_rgb(4, 4, 80, 80, 80));

    Material& mat = add_material(game, "M");
    set_diffuse(mat, add_texture(game, "Tex", "diffuse.ppm"));
    Terrain& t = add_terrain(game);
    REQUIRE_FALSE(t.set_texture_size(static_cast<int>(TextureSize::Small)));
    TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(t.add_material(mat.id(), entry));

    TerrainTextures textures;
    settle(textures, game);
    const auto before = textures.published(t.id());
    REQUIRE(before != nullptr);

    TerrainWorld world;
    world.set_terrain_textures(&textures);
    world.update(game);
    REQUIRE(world.views().size() == 1u);
    const std::uint64_t look_before = world.views()[0].look->revision;

    REQUIRE_FALSE(mat.set_texture_scale(16.0));
    textures.update(game);
    world.update(game);

    const auto after = textures.published(t.id());
    REQUIRE(after == before);   // same set: TextureScale is not a build input
    REQUIRE(world.views()[0].look->revision != look_before);   // but the look changed
    const std::size_t row2 = 2 * 256 * 4;
    REQUIRE(world.views()[0].look->texels[row2 + static_cast<std::size_t>(entry->material_id()) * 4 + 1] == 16.f);
}

TEST_CASE("TL-T1 a Material's Color, TextureScale, BlendSharpness, and HeightStrength each change the next look "
          "within one update, and re-mesh nothing",
          "[terrain][textures]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    write_ppm(dir.path, "diffuse.ppm", 4, 4, flat_rgb(4, 4, 80, 80, 80));

    Material& mat = add_material(game, "M");
    set_diffuse(mat, add_texture(game, "Tex", "diffuse.ppm"));
    Terrain& t = add_terrain(game);
    REQUIRE_FALSE(t.set_texture_size(static_cast<int>(TextureSize::Small)));
    TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(t.add_material(mat.id(), entry));
    // A real chunk to mesh, so "re-meshes nothing" has something to prove
    // against: an unrelated Terrain edit elsewhere must not re-mesh it either.
    REQUIRE_FALSE(t.edit_volume([&](engine_core::terrain::VoxelVolume& volume) {
        return volume.fill(ball_at(5.f, 5.f, 5.f, 4.f), static_cast<std::uint8_t>(entry->material_id()));
    }));

    TerrainTextures textures;
    settle(textures, game);

    TerrainWorld world;
    world.set_terrain_textures(&textures);
    world.update(game);
    world.wait_idle();
    world.update(game);
    REQUIRE(world.views().size() == 1u);
    std::uint64_t look_before = world.views()[0].look->revision;
    const std::uint64_t chunks_revision = world.views()[0].chunks_revision;
    const std::uint64_t meshed_count = world.meshed_count();
    REQUIRE(chunks_revision != 0u);    // the ball actually meshed something
    REQUIRE(meshed_count != 0u);

    const std::size_t id = static_cast<std::size_t>(entry->material_id());
    const std::size_t row0 = id * 4;
    const std::size_t row2 = 2 * 256 * 4 + id * 4;

    // Each property change lands in the very next update() (one tick), and
    // touches neither the chunk mesh nor the mesher: rebuild_look only ever
    // replaces TerrainRecord::look, never queues a chunk or a collider.
    const auto change_and_check = [&](const char* what, const std::function<void()>& apply, std::size_t texel,
                                       float expected) {
        INFO(what);
        apply();
        textures.update(game);
        world.update(game);
        REQUIRE(world.views().size() == 1u);
        const std::uint64_t look_after = world.views()[0].look->revision;
        REQUIRE(look_after != look_before);
        REQUIRE(world.views()[0].chunks_revision == chunks_revision);
        REQUIRE(world.meshed_count() == meshed_count);
        REQUIRE(world.views()[0].look->texels[texel] == expected);
        look_before = look_after;
    };

    change_and_check("Color", [&] { REQUIRE_FALSE(mat.set_color(engine_core::ColorRgb{0.25f, 0.5f, 0.75f, 1.f})); },
                      row0, 0.25f);
    change_and_check("TextureScale", [&] { REQUIRE_FALSE(mat.set_texture_scale(12.0)); }, row2 + 1, 12.f);
    change_and_check("BlendSharpness", [&] { REQUIRE_FALSE(mat.set_blend_sharpness(0.8)); }, row2 + 2, 0.8f);
    change_and_check("HeightStrength", [&] { REQUIRE_FALSE(mat.set_height_strength(2.5)); }, row2 + 3, 2.5f);
}

TEST_CASE("TT4 touching one texture file rebuilds only its layer; the old set stays published until the new one "
          "lands",
          "[terrain][textures]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    write_ppm(dir.path, "diffuseA.ppm", 4, 4, flat_rgb(4, 4, 90, 90, 90));
    write_ppm(dir.path, "diffuseB.ppm", 4, 4, flat_rgb(4, 4, 30, 30, 30));

    Material& matA = add_material(game, "A");
    set_diffuse(matA, add_texture(game, "TexA", "diffuseA.ppm"));
    Material& matB = add_material(game, "B");
    set_diffuse(matB, add_texture(game, "TexB", "diffuseB.ppm"));

    Terrain& t = add_terrain(game);
    REQUIRE_FALSE(t.set_texture_size(static_cast<int>(TextureSize::Small)));
    TerrainMaterial* e1 = nullptr;
    REQUIRE_FALSE(t.add_material(matA.id(), e1));
    TerrainMaterial* e2 = nullptr;
    REQUIRE_FALSE(t.add_material(matB.id(), e2));

    TerrainTextures textures;
    settle(textures, game);
    const auto old_set = textures.published(t.id());
    REQUIRE(old_set != nullptr);
    REQUIRE(old_set->layers.size() == 3u);

    // Past the once-a-second stamp check, so the next update() actually
    // re-stats the files instead of reusing what it already has.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write_ppm(dir.path, "diffuseA.ppm", 4, 4, flat_rgb(4, 4, 5, 5, 5));   // touched: new content and mtime

    textures.update(game);   // drains nothing new yet; only queues the rebuild
    REQUIRE(textures.published(t.id()) == old_set);   // still the old set: the rebuild has not landed

    textures.wait_idle();
    textures.update(game);   // drains the finished rebuild
    const auto new_set = textures.published(t.id());
    REQUIRE(new_set != nullptr);
    REQUIRE(new_set != old_set);
    REQUIRE(new_set->layers.size() == 3u);
    REQUIRE(new_set->layers[0] == old_set->layers[0]);   // untouched
    REQUIRE(new_set->layers[1] != old_set->layers[1]);   // A's layer rebuilt
    REQUIRE(new_set->layers[2] == old_set->layers[2]);   // B's layer untouched
}

TEST_CASE("TL-T2 assigning a NormalTexture rebuilds that layer only, the same update() it is assigned in",
          "[terrain][textures]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    write_ppm(dir.path, "diffuseA.ppm", 4, 4, flat_rgb(4, 4, 90, 90, 90));
    write_ppm(dir.path, "diffuseB.ppm", 4, 4, flat_rgb(4, 4, 30, 30, 30));
    write_ppm(dir.path, "normalA.ppm", 4, 4, flat_rgb(4, 4, 128, 128, 255));

    Material& matA = add_material(game, "A");
    set_diffuse(matA, add_texture(game, "TexA", "diffuseA.ppm"));
    Material& matB = add_material(game, "B");
    set_diffuse(matB, add_texture(game, "TexB", "diffuseB.ppm"));

    Terrain& t = add_terrain(game);
    REQUIRE_FALSE(t.set_texture_size(static_cast<int>(TextureSize::Small)));
    TerrainMaterial* e1 = nullptr;
    REQUIRE_FALSE(t.add_material(matA.id(), e1));
    TerrainMaterial* e2 = nullptr;
    REQUIRE_FALSE(t.add_material(matB.id(), e2));

    TerrainTextures textures;
    settle(textures, game);
    const auto old_set = textures.published(t.id());
    REQUIRE(old_set != nullptr);
    REQUIRE(old_set->layers.size() == 3u);

    // No sleep: assigning a reference is an in-memory change, not a file
    // touch, so it must not wait on the once-a-second disk-stamp throttle
    // (TT4's) to be noticed -- that throttle is for last_write_time() calls,
    // which this never needed in the first place.
    set_normal(matA, add_texture(game, "NormA", "normalA.ppm"));
    textures.update(game);   // queues A's rebuild this very tick
    REQUIRE(textures.published(t.id()) == old_set);   // still the old set: the rebuild has not landed

    textures.wait_idle();
    textures.update(game);   // drains the finished rebuild
    const auto new_set = textures.published(t.id());
    REQUIRE(new_set != nullptr);
    REQUIRE(new_set != old_set);
    REQUIRE(new_set->layers.size() == 3u);
    REQUIRE(new_set->layers[0] == old_set->layers[0]);   // untouched
    REQUIRE(new_set->layers[1] != old_set->layers[1]);   // A's layer rebuilt (new NormalTexture)
    REQUIRE(new_set->layers[2] == old_set->layers[2]);   // B's layer untouched

    // A second update() right away (still inside the same throttle window)
    // changes nothing further: the reference comparison it relies on sees
    // no further change, so it costs a cheap resolve, not a rebuild.
    textures.update(game);
    REQUIRE(textures.published(t.id()) == new_set);
}

TEST_CASE("TT5 toggling TextureSize Small and Max ten times quickly ends with one set at the last size",
          "[terrain][textures]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    Terrain& t = add_terrain(game);

    TerrainTextures textures;
    TextureSize last = TextureSize::Small;
    for (int i = 0; i < 10; ++i) {
        last = (i % 2 == 0) ? TextureSize::Small : TextureSize::Max;
        REQUIRE_FALSE(t.set_texture_size(static_cast<int>(last)));
        textures.update(game);   // each one supersedes an older queued request for this Terrain
    }
    settle(textures, game);

    const auto set = textures.published(t.id());
    REQUIRE(set != nullptr);
    REQUIRE(set->layers.size() == 1u);
    const int expected_size = last == TextureSize::Max ? 2048 : 256;
    REQUIRE(set->size == expected_size);
    REQUIRE(textures.memory_bytes(t.id()) == engine_core::terrain::layer_bytes(expected_size));
}

TEST_CASE("TT6 memory_bytes matches layers * layer_bytes(size)", "[terrain][textures]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    write_ppm(dir.path, "diffuse.ppm", 4, 4, flat_rgb(4, 4, 1, 2, 3));

    Material& mat = add_material(game, "M");
    set_diffuse(mat, add_texture(game, "Tex", "diffuse.ppm"));
    Terrain& t = add_terrain(game);
    REQUIRE_FALSE(t.set_texture_size(static_cast<int>(TextureSize::Small)));
    TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(t.add_material(mat.id(), entry));

    TerrainTextures textures;
    settle(textures, game);

    const auto set = textures.published(t.id());
    REQUIRE(set != nullptr);
    REQUIRE(set->layers.size() == 2u);
    const std::size_t expected = 2u * engine_core::terrain::layer_bytes(256);
    REQUIRE(textures.memory_bytes(t.id()) == expected);
}

// ---- Streaming: placeholders at once, previews before bakes, the cache ----

#include "texture/BlockCompress.hpp"
#include "texture/TexturePool.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <mutex>

namespace {

using engine_core::texture::JobPriority;
using engine_core::texture::TexturePool;

// Records every priority submitted to pool, in order.
struct PriorityLog {
    std::mutex mutex;
    std::vector<JobPriority> seen;
    explicit PriorityLog(TexturePool& pool) {
        pool.set_on_submit([this](JobPriority p) {
            std::lock_guard<std::mutex> lock(mutex);
            seen.push_back(p);
        });
    }
    std::vector<JobPriority> copy() {
        std::lock_guard<std::mutex> lock(mutex);
        return seen;
    }
};

// Level 0 of layer's color plane, decoded: its first texel's red.
int first_red(const engine_core::terrain::LayerBytes& layer) {
    const auto formats = engine_core::terrain::layer_formats();
    const auto rgba = engine_core::texture::decode_level(formats[0], layer.planes[0][0].data(), layer.size, layer.size);
    return rgba[0];
}

// A Terrain at Medium (512) with one textured Material per color, its
// resources folder under dir/resources so its cache lands in dir/.cache.
struct StreamingScene {
    Game game;
    Terrain* terrain = nullptr;
    std::vector<Material*> materials;
    // write false reuses the files an earlier scene wrote, keeping their times.
    StreamingScene(const TempDir& dir, const std::vector<std::uint8_t>& reds, bool write = true) {
        const std::filesystem::path resources = dir.path / "resources";
        game.set_resources_root(resources);
        terrain = &add_terrain(game);
        REQUIRE_FALSE(terrain->set_texture_size(static_cast<int>(TextureSize::Medium)));
        for (std::size_t i = 0; i < reds.size(); ++i) {
            const std::string name = "d" + std::to_string(i) + ".ppm";
            if (write) write_ppm(resources, name.c_str(), 128, 128, flat_rgb(128, 128, reds[i], 20, 20));
            Material& m = add_material(game, ("M" + std::to_string(i)).c_str());
            set_diffuse(m, add_texture(game, ("T" + std::to_string(i)).c_str(), name.c_str()));
            TerrainMaterial* entry = nullptr;
            REQUIRE_FALSE(terrain->add_material(m.id(), entry));
            materials.push_back(&m);
        }
    }
};

}  // namespace

TEST_CASE("TT7 every layer publishes at once as a grey placeholder, before anything is built", "[terrain][textures]") {
    SimRole role;
    TempDir dir;
    StreamingScene scene(dir, {200, 40});
    TexturePool pool(1);
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();
    pool.submit(JobPriority::TerrainPreview, [opened] { opened.wait(); });   // nothing builds yet
    TerrainTextures textures(pool);
    textures.update(scene.game);
    const auto set = textures.published(scene.terrain->id());
    REQUIRE(set != nullptr);
    REQUIRE(set->layers.size() == 3u);
    REQUIRE(set->layer_revisions.size() == 3u);
    for (std::size_t i = 1; i < 3; ++i) {
        REQUIRE(set->layers[i] != nullptr);
        CHECK(set->layers[i]->first_level == 0);
        CHECK(std::abs(first_red(*set->layers[i]) - 128) <= 2);   // grey, not white
    }
    CHECK(first_red(*set->layers[0]) >= 250);   // layer 0, the untextured default, stays white
    gate.set_value();
    pool.wait_idle();
}

TEST_CASE("TT8 on a cold cache every layer previews before any full bake starts", "[terrain][textures]") {
    SimRole role;
    TempDir dir;
    StreamingScene scene(dir, {200, 40, 90});
    TexturePool pool(1);
    PriorityLog log(pool);
    TerrainTextures textures(pool);
    settle(textures, scene.game);
    const std::vector<JobPriority> seen = log.copy();
    const auto first_bake = std::find(seen.begin(), seen.end(), JobPriority::TerrainBake);
    REQUIRE(first_bake != seen.end());
    CHECK(std::count(seen.begin(), first_bake, JobPriority::TerrainPreview) == 3);
    const auto set = textures.published(scene.terrain->id());
    REQUIRE(set != nullptr);
    CHECK(set->layers[1]->first_level == 0);
    CHECK(std::abs(first_red(*set->layers[1]) - 200) <= 4);
    CHECK(std::abs(first_red(*set->layers[2]) - 40) <= 4);
    CHECK(std::filesystem::exists(dir.path / ".cache" / "textures"));
}

TEST_CASE("TT9 a warm cache reads levels and bakes nothing", "[terrain][textures]") {
    SimRole role;
    TempDir dir;
    {
        StreamingScene scene(dir, {200, 40});
        TerrainTextures textures;
        settle(textures, scene.game);
    }
    StreamingScene again(dir, {200, 40}, false);   // the same files, at the same times
    TexturePool pool(1);
    PriorityLog log(pool);
    TerrainTextures textures(pool);
    settle(textures, again.game);
    const std::vector<JobPriority> seen = log.copy();
    CHECK(std::count(seen.begin(), seen.end(), JobPriority::TerrainBake) == 0);
    CHECK(std::count(seen.begin(), seen.end(), JobPriority::TerrainLargeLevels) > 0);
    const auto set = textures.published(again.terrain->id());
    REQUIRE(set != nullptr);
    CHECK(set->layers[1]->first_level == 0);
    CHECK(std::abs(first_red(*set->layers[1]) - 200) <= 4);
}

TEST_CASE("TT10 a reference changed while its layer builds drops the stale build", "[terrain][textures]") {
    SimRole role;
    TempDir dir;
    StreamingScene scene(dir, {200});
    write_ppm(dir.path / "resources", "other.ppm", 128, 128, flat_rgb(128, 128, 10, 20, 20));
    const engine_core::InstanceId other = add_texture(scene.game, "Other", "other.ppm");
    TexturePool pool(1);
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();
    pool.submit(JobPriority::TerrainPreview, [opened] { opened.wait(); });
    TerrainTextures textures(pool);
    textures.update(scene.game);              // queues a build of the red diffuse
    set_diffuse(*scene.materials[0], other);  // and now it is another texture
    textures.update(scene.game);
    gate.set_value();
    settle(textures, scene.game);
    const auto set = textures.published(scene.terrain->id());
    REQUIRE(set != nullptr);
    CHECK(std::abs(first_red(*set->layers[1]) - 10) <= 4);
}
