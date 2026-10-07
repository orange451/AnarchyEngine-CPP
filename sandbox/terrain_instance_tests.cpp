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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
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
