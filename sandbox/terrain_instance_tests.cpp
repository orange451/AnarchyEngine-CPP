// Terrain and TerrainMaterial instances, and the engine rules they rely on.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "Containment.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "terrain/TerrainStash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;

// A Folder that keeps its parent, for the locked-parent rule.
class LockedFolder : public engine_core::Folder {
public:
    using Folder::Folder;
    bool parent_locked() const override { return true; }
};

std::string reason(const std::optional<std::string>& value) { return value ? *value : std::string(); }

}  // namespace

TEST_CASE("TE1 a locked parent refuses to change, but can be set from none", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    LockedFolder& locked = game.create<LockedFolder>();
    REQUIRE_FALSE(game.parent_error(locked.id(), workspace_of(game)));
    game.set_parent(locked.id(), workspace_of(game));
    engine_core::Folder& other = game.create<engine_core::Folder>();
    game.set_parent(other.id(), workspace_of(game));
    REQUIRE(reason(game.parent_error(locked.id(), other.id())) == "Folder cannot be reparented");
    REQUIRE(reason(game.parent_error(locked.id(), engine_core::DataModel::kNoParent)) == "Folder cannot be reparented");
    game.destroy(locked.id());
}

TEST_CASE("TE2 a paste-only creatable is made by lua_create_instance but not by scripts", "[terrain]") {
    engine_core::register_lua_creatable(
        "TestPasteOnly", [](engine_core::DataModel& world) -> engine_core::DataModel& {
            return world.create<engine_core::Folder>();
        }, false);
    REQUIRE(engine_core::lua_creatable_known("TestPasteOnly"));
    REQUIRE_FALSE(engine_core::lua_script_creatable("TestPasteOnly"));
    REQUIRE(engine_core::lua_script_creatable("Folder"));
    std::vector<std::string> names;
    engine_core::lua_creatable_names(names);
    REQUIRE(std::find(names.begin(), names.end(), std::string("TestPasteOnly")) == names.end());
}

namespace {

engine_core::Terrain& add_terrain(engine_core::DataModel& game) {
    engine_core::Terrain& terrain = game.create<engine_core::Terrain>();
    game.set_parent(terrain.id(), workspace_of(game));
    return terrain;
}

engine_core::Material& add_material_asset(engine_core::DataModel& game, const char* name) {
    engine_core::Material& material = game.create<engine_core::Material>();
    game.set_name(material.id(), name);
    game.set_parent(material.id(), game.service("Materials"));
    return material;
}

}  // namespace

TEST_CASE("TM1 a Terrain's properties are checked and saved", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("Terrain"));
    REQUIRE(engine_core::project_class_known("TerrainMaterial"));
    engine_core::Terrain& terrain = add_terrain(game);
    REQUIRE(terrain.voxel_size() == 1.0);
    REQUIRE(terrain.can_collide());
    REQUIRE_FALSE(terrain.set_transform(engine_core::matrix4_translation(1.f, 2.f, 3.f)));
    engine_core::Matrix4 scaled = engine_core::matrix4_identity();
    scaled.m[0] = 2.f;
    REQUIRE(reason(terrain.set_transform(scaled)) == "Terrain cannot be scaled");
    REQUIRE_FALSE(terrain.set_can_collide(false));
    engine_core::PropertyBag saved;
    terrain.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Transform") != nullptr);
    REQUIRE(engine_core::bag_find(saved, "CanCollide") != nullptr);
}

TEST_CASE("TM2 TerrainMaterials take the lowest free Id, up to 255", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::Material& rock = add_material_asset(game, "Rock");
    engine_core::TerrainMaterial* a = nullptr;
    engine_core::TerrainMaterial* b = nullptr;
    REQUIRE_FALSE(terrain.add_material(rock.id(), a));
    REQUIRE_FALSE(terrain.add_material(rock.id(), b));   // the same Material twice is fine
    REQUIRE(a->material_id() == 1);
    REQUIRE(b->material_id() == 2);
    REQUIRE(game.name(a->id()) == "Rock");
    REQUIRE(terrain.materials_for(rock.id()).size() == 2u);
    game.destroy(a->id());
    engine_core::TerrainMaterial* c = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, c));
    REQUIRE(c->material_id() == 1);
    for (int i = 0; i < 253; ++i) {
        engine_core::TerrainMaterial* more = nullptr;
        REQUIRE_FALSE(terrain.add_material(0, more));
    }
    engine_core::TerrainMaterial* full = nullptr;
    REQUIRE(reason(terrain.add_material(0, full)) == "Terrain can hold at most 255 Materials");
}

TEST_CASE("TM3 a TerrainMaterial lives only in a Terrain, hidden, and keeps its parent", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, entry));
    REQUIRE(entry->hidden_in_explorer());
    REQUIRE(reason(engine_core::placement_error("Workspace", "TerrainMaterial", "x")) ==
            "A TerrainMaterial must be in a Terrain");
    engine_core::Terrain& other = add_terrain(game);
    REQUIRE(reason(game.parent_error(entry->id(), other.id())) == "TerrainMaterial cannot be reparented");
}

TEST_CASE("TM4 deleting a TerrainMaterial keeps its cells' Id for the next one", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, entry));
    engine_core::terrain::Shape ball;
    ball.radius = 4.f;
    REQUIRE_FALSE(terrain.volume().fill(ball, static_cast<std::uint8_t>(entry->material_id())));
    game.destroy(entry->id());
    REQUIRE(terrain.volume().cell(engine_core::terrain::CellCoord{0, 0, 0}).material == 1);
    REQUIRE(terrain.material_by_id(1) == nullptr);
    engine_core::TerrainMaterial* next = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, next));
    REQUIRE(terrain.material_by_id(1) == next);
}

TEST_CASE("TM5 Id is read-only to scripts and Material refuses a non-Material", "[terrain]") {
    ScriptRig rig;
    add_script(rig.game, "T", R"(
        local t = Instance.new("Terrain", workspace)
        _G.made = pcall(function() Instance.new("TerrainMaterial", t) end)
        _G.ok = (not _G.made)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    bool ok = false;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
}

TEST_CASE("TM6 undoing a delete after a script took the Id gives the revived one a new Id", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* a = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, a));
    const InstanceId a_id = a->id();
    begin_step(game, "Delete");
    game.destroy(a_id);
    end_step(game);
    // Not recorded: a script in the command bar, outside any step.
    engine_core::TerrainMaterial* b = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, b));
    REQUIRE(b->material_id() == 1);
    game.history().undo();
    auto* back = dynamic_cast<engine_core::TerrainMaterial*>(game.instance(a_id));
    REQUIRE(back != nullptr);
    REQUIRE(b->material_id() == 1);       // the holder keeps it
    REQUIRE(back->material_id() == 2);    // the revived one moves
    REQUIRE(terrain.materials().size() == 2u);
}

TEST_CASE("TM7 a TerrainMaterial pasted into a Terrain that uses its Id gets the lowest free one", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* a = nullptr;
    engine_core::TerrainMaterial* b = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, a));
    REQUIRE_FALSE(terrain.add_material(0, b));
    // What paste does: make one, load its saved properties (Id 1), then parent it.
    engine_core::TerrainMaterial& copy = game.create<engine_core::TerrainMaterial>();
    REQUIRE_FALSE(copy.set_material_id(1));
    game.set_parent(copy.id(), terrain.id());
    REQUIRE(a->material_id() == 1);
    REQUIRE(copy.material_id() == 3);
}

TEST_CASE("TM8 a TerrainMaterial loaded with Id 0 takes the lowest free Id when it arrives", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* a = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, a));
    // What load does with an explicit "Id": 0: make one, load its properties, then parent it.
    engine_core::TerrainMaterial& loaded = game.create<engine_core::TerrainMaterial>();
    std::string error;
    REQUIRE(loaded.load_property("Id", engine_core::JsonValue::number(0), error));
    REQUIRE(error.empty());
    REQUIRE(loaded.material_id() == 0);
    game.set_parent(loaded.id(), terrain.id());
    REQUIRE(a->material_id() == 1);
    REQUIRE(loaded.material_id() == 2);
    // set_material_id itself still refuses 0.
    REQUIRE(reason(loaded.set_material_id(0)) == "Id must be a whole number from 1 to 255");
}

TEST_CASE("TM9 a mirrored Transform is refused as a scale", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::Matrix4 mirrored = engine_core::matrix4_identity();
    mirrored.m[0] = -1.f;
    REQUIRE(reason(terrain.set_transform(mirrored)) == "Terrain cannot be scaled");
    // A turn half way round about Y keeps its handedness and is allowed.
    engine_core::Matrix4 turned = engine_core::matrix4_identity();
    turned.m[0] = -1.f;
    turned.m[10] = -1.f;
    REQUIRE_FALSE(terrain.set_transform(turned));
}

TEST_CASE("TM10 an Id written to a TerrainMaterial already in a Terrain never takes a held Id or 0", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* one = nullptr;
    engine_core::TerrainMaterial* two = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, one));
    REQUIRE_FALSE(terrain.add_material(0, two));
    // What accepting changes from disk does: load_property on a live, parented entry.
    std::string error;
    REQUIRE(two->load_property("Id", engine_core::JsonValue::number(0), error));
    REQUIRE(error.empty());
    REQUIRE(two->material_id() != 0);
    REQUIRE(two->material_id() == 2);   // its own Id is the lowest one no other entry holds
    REQUIRE(terrain.materials().size() == 2u);
    REQUIRE(two->load_property("Id", engine_core::JsonValue::number(1), error));
    REQUIRE(error.empty());
    REQUIRE(one->material_id() == 1);   // the holder keeps it
    REQUIRE(two->material_id() == 2);
    // A free Id is taken as written.
    REQUIRE(two->load_property("Id", engine_core::JsonValue::number(7), error));
    REQUIRE(two->material_id() == 7);
    REQUIRE(terrain.free_id() == 2);
    REQUIRE(two->load_property("Id", engine_core::JsonValue::number(0), error));
    REQUIRE(two->material_id() == 2);
}

TEST_CASE("TM11 undo and redo of adding a TerrainMaterial", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* first = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, first));
    begin_step(game, "Add Terrain Material");
    engine_core::TerrainMaterial* added = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, added));
    end_step(game);
    const InstanceId added_id = added->id();
    REQUIRE(added->material_id() == 2);
    game.history().undo();
    REQUIRE_FALSE(game.alive(added_id));
    REQUIRE(terrain.materials().size() == 1u);
    REQUIRE(first->material_id() == 1);
    game.history().redo();
    auto* back = dynamic_cast<engine_core::TerrainMaterial*>(game.instance(added_id));
    REQUIRE(back != nullptr);
    REQUIRE(game.parent(added_id) == terrain.id());
    REQUIRE(back->material_id() == 2);
    REQUIRE(first->material_id() == 1);
    game.history().undo();
    REQUIRE_FALSE(game.alive(added_id));
    REQUIRE(terrain.materials().size() == 1u);
}

namespace {

engine_core::terrain::Shape ball_at(float x, float y, float z, float r) {
    engine_core::terrain::Shape s;
    s.center = engine_core::Vec3{x, y, z};
    s.radius = r;
    return s;
}

std::uint8_t id_at(const engine_core::Terrain& terrain, int x, int y, int z) {
    return terrain.volume().cell(engine_core::terrain::CellCoord{x, y, z}).material;
}

// Writes DataPath through its registry write with a string, as load does.
void write_data_path(engine_core::DataModel& game, engine_core::Terrain& terrain, const std::string& path) {
    for (const engine_core::LuaField& field : engine_core::lua_saved_fields("Terrain")) {
        if (std::string(field.name) == "DataPath") {
            engine_core::LuaSlot slot;
            slot.kind = engine_core::LuaSlot::Kind::String;
            slot.text = path;
            REQUIRE(field.write(game, terrain, slot));
            return;
        }
    }
    FAIL("Terrain has no DataPath field");
}

}  // namespace

TEST_CASE("TP1 Stop puts back the voxels edited during play", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 1));
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(terrain.volume().fill(ball_at(50.f, 0.f, 0.f, 4.f), 2));
    REQUIRE_FALSE(terrain.volume().subtract(ball_at(0.f, 0.f, 0.f, 10.f)));
    game.stop_simulation();
    REQUIRE(id_at(terrain, 0, 0, 0) == 1);
    REQUIRE(terrain.volume().cell(engine_core::terrain::CellCoord{50, 0, 0}).distance ==
            engine_core::terrain::kAirDistance);
}

TEST_CASE("TP2 TerrainMaterials added during play are gone after Stop", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    engine_core::TerrainMaterial* kept = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, kept));
    game.capture_place();
    game.start_simulation();
    engine_core::TerrainMaterial* temp = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, temp));
    game.stop_simulation();
    REQUIRE(terrain.materials().size() == 1u);
}

TEST_CASE("TP3 undoing a Terrain's delete brings its voxels back", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 3));
    const InstanceId id = terrain.id();
    begin_step(game, "Delete");
    game.destroy(id);
    end_step(game);
    game.history().undo();
    auto* back = dynamic_cast<engine_core::Terrain*>(game.instance(id));
    REQUIRE(back != nullptr);
    REQUIRE(id_at(*back, 0, 0, 0) == 3);
}

TEST_CASE("TP4 a pasted Terrain starts with its source's voxels and its own file", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    write_data_path(game, terrain, "terrain/Source.avox");
    REQUIRE(terrain.data_path() == "terrain/Source.avox");
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 2));
    // Paste builds from saved properties (src/ide/CutSet.cpp build_copy).
    engine_core::PropertyBag saved;
    terrain.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "DataPath") != nullptr);
    engine_core::Terrain& copy = game.create<engine_core::Terrain>();
    for (const engine_core::JsonValue::Member& member : saved) {
        std::string error;
        copy.load_property(member.first, member.second, error);
        REQUIRE(error.empty());
    }
    game.set_parent(copy.id(), workspace_of(game));
    REQUIRE(id_at(copy, 0, 0, 0) == 2);
    REQUIRE_FALSE(copy.data_path().empty());
    REQUIRE(copy.data_path() != terrain.data_path());   // its own file, assigned at paste
    REQUIRE(copy.data_path().rfind("terrain/", 0) == 0);
    REQUIRE(copy.data_path().find(game.guid(copy.id())) != std::string::npos);
    REQUIRE(terrain.data_path() == "terrain/Source.avox");   // the source keeps its own
}

namespace {

// What paste does (src/ide/CutSet.cpp build_copy): make one, load the saved
// properties one by one, then parent it.
engine_core::Terrain& paste_copy(engine_core::DataModel& game, const engine_core::PropertyBag& saved) {
    engine_core::Terrain& copy = game.create<engine_core::Terrain>();
    for (const engine_core::JsonValue::Member& member : saved) {
        std::string error;
        copy.load_property(member.first, member.second, error);
        REQUIRE(error.empty());
    }
    game.set_parent(copy.id(), workspace_of(game));
    return copy;
}

}  // namespace

TEST_CASE("TP5 undo then redo of a paste brings the copy back with its voxels and its own file", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    write_data_path(game, terrain, "terrain/Source.avox");
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 2));
    engine_core::PropertyBag saved;
    terrain.save_properties(saved);
    begin_step(game, "Paste");
    engine_core::Terrain& copy = paste_copy(game, saved);
    end_step(game);
    const InstanceId copy_id = copy.id();
    const std::string copy_path = copy.data_path();
    REQUIRE(id_at(copy, 0, 0, 0) == 2);
    game.history().undo();
    REQUIRE_FALSE(game.alive(copy_id));
    game.history().redo();
    auto* back = dynamic_cast<engine_core::Terrain*>(game.instance(copy_id));
    REQUIRE(back != nullptr);
    REQUIRE(id_at(*back, 0, 0, 0) == 2);
    REQUIRE_FALSE(back->data_path().empty());
    REQUIRE(back->data_path() == copy_path);
    REQUIRE(back->data_path() != terrain.data_path());
}

TEST_CASE("TP6 a Terrain cut and then pasted keeps its voxels and gets its own file", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    write_data_path(game, terrain, "terrain/Cut.avox");
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 4));
    engine_core::PropertyBag saved;
    terrain.save_properties(saved);
    // A cut: the copy is taken, then the source is deleted in a step.
    begin_step(game, "Cut");
    game.destroy(terrain.id());
    end_step(game);
    begin_step(game, "Paste");
    engine_core::Terrain& copy = paste_copy(game, saved);
    end_step(game);
    REQUIRE(id_at(copy, 0, 0, 0) == 4);
    REQUIRE_FALSE(copy.data_path().empty());
    REQUIRE(copy.data_path() != "terrain/Cut.avox");
}

TEST_CASE("TP7 loading a DataPath with history off never takes stashed voxels", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Terrain& terrain = add_terrain(game);
    write_data_path(game, terrain, "terrain/Reload.avox");
    REQUIRE_FALSE(terrain.volume().fill(ball_at(0.f, 0.f, 0.f, 4.f), 5));
    begin_step(game, "Delete");
    game.destroy(terrain.id());
    end_step(game);
    // A project load builds with history off; the path is the file's, kept as written.
    game.history().set_enabled(false);
    engine_core::Terrain& loaded = add_terrain(game);
    write_data_path(game, loaded, "terrain/Reload.avox");
    game.history().set_enabled(true);
    REQUIRE(loaded.data_path() == "terrain/Reload.avox");
    REQUIRE(loaded.volume().chunks().empty());
}

namespace {

// Fills a ball through Terrain::edit_volume, which marks the change.
void edit_ball(engine_core::Terrain& terrain, float x, float y, float z, float r, std::uint8_t id) {
    const std::optional<std::string> error = terrain.edit_volume(
        [&](engine_core::terrain::VoxelVolume& volume) { return volume.fill(ball_at(x, y, z, r), id); });
    REQUIRE_FALSE(error);
}

engine_core::Terrain& terrain_named(engine_core::DataModel& game, const char* name) {
    const InstanceId id = game.find_first_child(workspace_of(game), name);
    auto* terrain = dynamic_cast<engine_core::Terrain*>(game.instance(id));
    REQUIRE(terrain != nullptr);
    return *terrain;
}

std::filesystem::path avox_file(const TempDir& dir, const std::string& data_path) {
    return dir.path / "resources" / std::filesystem::u8path(data_path);
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A bool, so a failure never prints the binary bytes.
bool same_bytes(const std::filesystem::path& path, const std::string& bytes) { return file_bytes(path) == bytes; }

// A project with one Terrain named Island holding a ball of Id 2, saved.
// Returns the Terrain's DataPath.
std::string save_island(const TempDir& dir) {
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    engine_core::Terrain& terrain = add_terrain(game);
    game.set_name(terrain.id(), "Island");
    edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 2);
    project.save();
    return terrain.data_path();
}

}  // namespace

TEST_CASE("TP8 undo then redo of a Terrain made and edited in one step brings back its voxels and file", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    begin_step(game, "Insert Terrain");
    engine_core::Terrain& terrain = add_terrain(game);
    edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 6);
    end_step(game);
    const InstanceId id = terrain.id();
    const std::string path = terrain.data_path();
    REQUIRE_FALSE(path.empty());
    game.history().undo();
    REQUIRE_FALSE(game.alive(id));
    game.history().redo();
    auto* back = dynamic_cast<engine_core::Terrain*>(game.instance(id));
    REQUIRE(back != nullptr);
    REQUIRE(id_at(*back, 0, 0, 0) == 6);
    REQUIRE(back->data_path() == path);
}

TEST_CASE("TP9 many edits in the step that made a Terrain do not grow the stash per edit", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    begin_step(game, "Generate");
    engine_core::Terrain& terrain = add_terrain(game);
    edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 1);
    edit_ball(terrain, 10.f, 0.f, 0.f, 4.f, 1);
    const std::size_t settled = engine_core::terrain::TerrainStash::size();
    for (int i = 2; i <= 50; ++i) {
        edit_ball(terrain, static_cast<float>(i * 10), 0.f, 0.f, 4.f, 1);
    }
    REQUIRE(engine_core::terrain::TerrainStash::size() == settled);
    end_step(game);
    const InstanceId id = terrain.id();
    game.history().undo();
    game.history().redo();
    auto* back = dynamic_cast<engine_core::Terrain*>(game.instance(id));
    REQUIRE(back != nullptr);
    REQUIRE(id_at(*back, 500, 0, 0) == 1);   // the last edit is in the record
}

TEST_CASE("TS1 a Terrain's voxels are saved with the project and come back", "[terrain]") {
    SimRole role;
    TempDir dir;
    std::string path;
    {
        engine_core::Game game;
        engine_core::Project project = engine_core::Project::create(dir.path, game);
        engine_core::Terrain& terrain = add_terrain(game);
        game.set_name(terrain.id(), "Island");
        edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 2);
        // The first edit while stopped gives it its file's path, before any save.
        path = terrain.data_path();
        REQUIRE(path.rfind("terrain/Island." + game.guid(terrain.id()), 0) == 0);
        REQUIRE(path.size() > 5);
        REQUIRE(path.substr(path.size() - 5) == ".avox");
        project.save();
        REQUIRE(terrain.data_path() == path);
        REQUIRE(std::filesystem::is_regular_file(avox_file(dir, path)));
        REQUIRE_FALSE(std::filesystem::exists(avox_file(dir, path).string() + ".partial"));
    }
    engine_core::Project loaded = engine_core::Project::load(dir.path);
    engine_core::Terrain& terrain = terrain_named(loaded.datamodel(), "Island");
    REQUIRE(terrain.data_path() == path);
    REQUIRE(id_at(terrain, 0, 0, 0) == 2);
}

TEST_CASE("TS2 a missing or damaged .avox loads an empty Terrain and says so in Output", "[terrain]") {
    SimRole role;
    TempDir dir;
    const std::string path = save_island(dir);
    const std::filesystem::path file = avox_file(dir, path);
    REQUIRE(std::filesystem::is_regular_file(file));
    const std::string good = file_bytes(file);

    std::filesystem::remove(file);
    {
        engine_core::Game game;
        std::vector<std::string> lines;
        game.set_warning_sink([&lines](const std::string& text) { lines.push_back(text); });
        engine_core::Project project = engine_core::Project::load(dir.path, game);
        engine_core::Terrain& terrain = terrain_named(game, "Island");
        REQUIRE(terrain.volume().chunks().empty());
        REQUIRE(lines.size() == 1u);
        REQUIRE(lines[0] == "Terrain Island: its voxel file " + path + " is missing, so it is empty");
        REQUIRE(terrain.data_path() == path);
        // Nothing was edited: a save writes no empty file over the missing one.
        project.save();
        REQUIRE_FALSE(std::filesystem::exists(file));
    }

    // The file back, with its middle byte flipped.
    std::string bad = good;
    bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 0x5a);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bad.data(), static_cast<std::streamsize>(bad.size()));
    }
    engine_core::Game game;
    std::vector<std::string> lines;
    game.set_warning_sink([&lines](const std::string& text) { lines.push_back(text); });
    engine_core::Project project = engine_core::Project::load(dir.path, game);
    engine_core::Terrain& terrain = terrain_named(game, "Island");
    REQUIRE(terrain.volume().chunks().empty());
    REQUIRE(lines.size() == 1u);
    const std::string prefix = "Terrain Island: its voxel file " + path + " is damaged (";
    REQUIRE(lines[0].rfind(prefix, 0) == 0);
    REQUIRE(lines[0].size() > prefix.size() + 1);
    const std::string suffix = "), so it is empty";
    REQUIRE(lines[0].substr(lines[0].size() - suffix.size()) == suffix);
    // A damaged file is never written over: the Terrain took a new file at load.
    REQUIRE_FALSE(terrain.data_path().empty());
    REQUIRE(terrain.data_path() != path);
    const std::string fresh = terrain.data_path();
    // The new DataPath is the studio's change, not the disk's, and a save
    // writes it even with no edit.
    REQUIRE_FALSE(project.scan_disk().has_disk_changes);
    project.save();
    REQUIRE(same_bytes(file, bad));
    {
        engine_core::Game again;
        std::vector<std::string> said;
        again.set_warning_sink([&said](const std::string& text) { said.push_back(text); });
        engine_core::Project reopened = engine_core::Project::load(dir.path, again);
        REQUIRE(said.empty());
        REQUIRE(terrain_named(again, "Island").data_path() == fresh);
    }
    edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 3);
    project.save();
    REQUIRE(same_bytes(file, bad));
    REQUIRE(terrain.data_path() == fresh);
    REQUIRE(std::filesystem::is_regular_file(avox_file(dir, fresh)));
}

TEST_CASE("TS4 saving during play writes the voxels from Play's snapshot, not the runtime edits", "[terrain]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Game game;
        engine_core::Project project = engine_core::Project::create(dir.path, game);
        engine_core::Terrain& terrain = add_terrain(game);
        game.set_name(terrain.id(), "Island");
        edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 1);
        REQUIRE_FALSE(terrain.data_path().empty());
        game.capture_place();
        game.start_simulation();
        edit_ball(terrain, 50.f, 0.f, 0.f, 4.f, 2);
        project.save();
        game.stop_simulation();
    }
    engine_core::Project loaded = engine_core::Project::load(dir.path);
    engine_core::Terrain& terrain = terrain_named(loaded.datamodel(), "Island");
    REQUIRE(id_at(terrain, 0, 0, 0) == 1);
    REQUIRE(terrain.volume().cell(engine_core::terrain::CellCoord{50, 0, 0}).distance ==
            engine_core::terrain::kAirDistance);
}

TEST_CASE("TS3 voxel edits while stopped mark the place unsaved; edits during play do not", "[terrain]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    engine_core::Terrain& terrain = add_terrain(game);
    project.save();
    REQUIRE_FALSE(game.history().dirty());
    edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 1);
    REQUIRE(game.history().dirty());
    project.save();
    REQUIRE_FALSE(game.history().dirty());
    game.capture_place();
    game.start_simulation();
    edit_ball(terrain, 20.f, 0.f, 0.f, 4.f, 2);
    REQUIRE_FALSE(game.history().dirty());
    game.stop_simulation();
}

namespace {

// The Terrain's own .json file under src/, found by its GUID.
std::filesystem::path terrain_json(const TempDir& dir, const std::string& guid) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir.path / "src")) {
        const std::string leaf = entry.path().filename().u8string();
        if (entry.is_regular_file() && leaf.find(guid) != std::string::npos && entry.path().extension() == ".json") {
            return entry.path();
        }
    }
    FAIL("no file for " + guid);
    return {};
}

}  // namespace

TEST_CASE("TS5 a save stopped by an outside change writes no voxel file", "[terrain]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    engine_core::Terrain& terrain = add_terrain(game);
    game.set_name(terrain.id(), "Island");
    edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 2);
    project.save();
    const std::filesystem::path file = avox_file(dir, terrain.data_path());
    const std::string saved = file_bytes(file);

    // CanCollide changes on disk while the studio moves the Terrain and edits its voxels.
    const std::filesystem::path json = terrain_json(dir, game.guid(terrain.id()));
    engine_core::JsonValue doc;
    std::string message;
    REQUIRE(engine_core::parse_json(file_bytes(json), doc, message));
    doc.set("CanCollide", engine_core::JsonValue::boolean(false));
    {
        std::ofstream out(json, std::ios::binary | std::ios::trunc);
        out << engine_core::write_json(doc);
    }
    REQUIRE_FALSE(terrain.set_transform(engine_core::matrix4_translation(3.f, 0.f, 0.f)));
    edit_ball(terrain, 20.f, 0.f, 0.f, 4.f, 5);
    REQUIRE_THROWS_AS(project.save(), engine_core::ProjectConflict);
    REQUIRE(same_bytes(file, saved));
}

TEST_CASE("TS6 a save during play writes the voxels of a Terrain play destroyed", "[terrain]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Game game;
        engine_core::Project project = engine_core::Project::create(dir.path, game);
        engine_core::Terrain& terrain = add_terrain(game);
        game.set_name(terrain.id(), "Island");
        edit_ball(terrain, 0.f, 0.f, 0.f, 4.f, 1);
        game.capture_place();
        game.start_simulation();
        game.destroy(terrain.id());
        project.save();
        game.stop_simulation();
    }
    engine_core::Project loaded = engine_core::Project::load(dir.path);
    engine_core::Terrain& terrain = terrain_named(loaded.datamodel(), "Island");
    REQUIRE(id_at(terrain, 0, 0, 0) == 1);
}

TEST_CASE("TS7 a resource file that cannot be put in place leaves no .partial file", "[terrain]") {
    TempDir dir;
    // A non-empty folder where the file goes: the rename over it fails.
    std::filesystem::create_directories(dir.path / "terrain" / "Island.avox" / "inside");
    const std::vector<std::byte> bytes(64, std::byte{7});
    REQUIRE(reason(engine_core::write_resource_file(dir.path, "terrain/Island.avox", bytes)) ==
            "Could not write terrain/Island.avox");
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "terrain" / "Island.avox.partial"));
    REQUIRE(std::filesystem::is_directory(dir.path / "terrain" / "Island.avox" / "inside"));
}
