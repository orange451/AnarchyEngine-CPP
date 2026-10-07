#include "ide/TerrainMaterials.hpp"

#include "AssetInstances.hpp"
#include "Game.hpp"
#include "ScriptRuntime.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "terrain/VoxelVolume.hpp"

#include <cstdio>
#include <string>
#include <vector>

// The data behind the Configure Terrain tab: what read_terrain_materials
// shows, and what add/set/remove/replace_unassigned do to a Terrain's
// TerrainMaterials and voxels.
namespace {

using engine_core::InstanceId;
namespace terrain = engine_core::terrain;

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

// Marks this thread as the simulation thread while it lives, as Terrain's
// setters and TerrainMaterial's setters need.
struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

terrain::Shape Ball(float x, float y, float z, float radius) {
    terrain::Shape shape;
    shape.kind = terrain::Shape::Kind::Ball;
    shape.center = engine_core::Vec3{x, y, z};
    shape.radius = radius;
    return shape;
}

// A place whose Materials hold Rock and Grass, and whose Workspace holds one
// empty Terrain. The host writes straight to the game, standing in for the
// simulation thread.
struct Rig {
    SimRole role;
    engine_core::ScriptRuntime runtime;
    engine_core::Game game;
    InstanceId rock = 0;
    InstanceId grass = 0;
    InstanceId terrain_id = 0;

    Rig() {
        rock = make<engine_core::Material>("Rock", game.service("Materials"));
        grass = make<engine_core::Material>("Grass", game.service("Materials"));
        engine_core::Terrain& placed = game.create<engine_core::Terrain>();
        terrain_id = placed.id();
        game.set_parent(terrain_id, game.scene_service("Workspace"));
    }

    template <typename T>
    InstanceId make(const char* name, InstanceId parent) {
        T& object = game.create<T>();
        game.set_name(object.id(), name);
        game.set_parent(object.id(), parent);
        return object.id();
    }

    engine_core::Terrain& terrain() {
        return *dynamic_cast<engine_core::Terrain*>(game.instance(terrain_id));
    }

    // Through Terrain::edit_volume, as a real sculpt does.
    void fill_ball(float x, float y, float z, float r, int id) {
        terrain().edit_volume([&](terrain::VoxelVolume& volume) {
            return volume.fill(Ball(x, y, z, r), static_cast<std::uint8_t>(id));
        });
    }

    int cell_id(int x, int y, int z) {
        return terrain().volume().cell(terrain::CellCoord{x, y, z}).material;
    }
};

void data_helpers() {
    Rig rig;
    std::string error;
    const InstanceId rock = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    const InstanceId grass = ide::add_terrain_material(rig.game, rig.terrain_id, rig.grass, error);
    auto view = ide::read_terrain_materials(rig.game, rig.terrain_id);
    Expect(view.alive && view.materials.size() == 2, "both are listed");
    Expect(view.materials[0].material_id == 1 && view.materials[0].name == "Rock",
           "Rock is Id 1, named after its Material");
    Expect(!view.materials[0].in_use, "nothing uses it yet");

    rig.fill_ball(0, 0, 0, 5, 1);     // Terrain::edit_volume fill with Id 1
    rig.fill_ball(100, 0, 0, 5, 9);   // an Id no TerrainMaterial holds
    view = ide::read_terrain_materials(rig.game, rig.terrain_id);
    Expect(view.materials[0].in_use && !view.materials[1].in_use, "In use follows the voxels");
    Expect(view.unassigned_in_use == std::vector<int>{9}, "Id 9 is used but unassigned");

    Expect(!ide::set_terrain_material(rig.game, grass, rig.rock), "Set points it at another Material");
    Expect(ide::read_terrain_materials(rig.game, rig.terrain_id).materials[1].material == rig.rock, "and it shows");

    Expect(!ide::remove_terrain_material(rig.game, rock, {ide::RemoveChoice::Kind::Replace, 2}),
           "Replace then remove");
    Expect(rig.cell_id(0, 0, 0) == 2, "Rock's voxels became Grass's Id");
    Expect(!ide::replace_unassigned(rig.game, rig.terrain_id, 9, 0), "unassigned Id 9 to the default");
    Expect(rig.cell_id(100, 0, 0) == 0, "and it is the default now");

    const InstanceId again = ide::add_terrain_material(rig.game, rig.terrain_id, 0, error);
    rig.fill_ball(200, 0, 0, 5, 1);
    Expect(!ide::remove_terrain_material(rig.game, again, {}), "Keep Cells removes only the TerrainMaterial");
    Expect(rig.cell_id(200, 0, 0) == 1, "its voxels keep Id 1");
}

}  // namespace

int main() {
    data_helpers();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return 1;
    }
    std::printf("terrain editor tests passed\n");
    return 0;
}
