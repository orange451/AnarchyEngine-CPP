#pragma once

#include "DataModel.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ide {

// What the Configure Terrain tab shows and does, without widgets: a
// Terrain's TerrainMaterials, which Ids they cover, and the writes that
// change them. Patterned on PrefabModels, for Terrain instead of Prefab.

// One TerrainMaterial, as the Configure Terrain tab shows it.
struct TerrainMaterialView {
    engine_core::InstanceId id = 0;
    int material_id = 0;          // the TerrainMaterial's Id, 1-255
    std::string name;
    engine_core::InstanceId material = 0;   // 0: none or gone
    std::string material_name;
    bool material_missing = false;          // it names a Material that no longer exists
    bool in_use = false;                    // some voxel uses its Id
    bool operator==(const TerrainMaterialView& other) const {
        return id == other.id && material_id == other.material_id && name == other.name &&
               material == other.material && material_name == other.material_name &&
               material_missing == other.material_missing && in_use == other.in_use;
    }
};

// What a Terrain's tab shows. The caller holds a read lock.
struct TerrainMaterialsView {
    bool alive = false;                        // the Terrain exists
    std::string terrain_name;
    std::vector<TerrainMaterialView> materials;   // by Id
    std::vector<int> unassigned_in_use;           // Ids voxels use that no TerrainMaterial holds, ascending
    std::uint64_t voxel_revision = 0;
};
TerrainMaterialsView read_terrain_materials(const engine_core::DataModel& world, engine_core::InstanceId terrain);

// SimulationThread. Each returns the new or changed instance, or 0 with error set.
engine_core::InstanceId add_terrain_material(engine_core::DataModel& world, engine_core::InstanceId terrain,
                                             engine_core::InstanceId material, std::string& error);
std::optional<std::string> set_terrain_material(engine_core::DataModel& world, engine_core::InstanceId entry,
                                                engine_core::InstanceId material);
// What happens to a removed TerrainMaterial's voxels.
struct RemoveChoice {
    enum class Kind { KeepCells, Replace } kind = Kind::KeepCells;
    int replace_with = 0;   // Replace: another TerrainMaterial's Id, or 0 for the default
};
std::optional<std::string> remove_terrain_material(engine_core::DataModel& world, engine_core::InstanceId entry,
                                                   RemoveChoice choice);
// Unassigned Id from becomes Id to (0: default) across the Terrain.
std::optional<std::string> replace_unassigned(engine_core::DataModel& world, engine_core::InstanceId terrain,
                                              int from, int to);

// SimulationThread. The writes above as the Studio's Configure Terrain tab
// makes them, each one undo step: "Add Terrain Material", "Set Terrain
// Material", "Remove Terrain Material", and "Replace Terrain Material" (its
// voxels too: Terrain::edit_volume records them in the open step).
engine_core::InstanceId run_add(engine_core::DataModel& world, engine_core::InstanceId terrain,
                                engine_core::InstanceId material, std::string& error);
std::optional<std::string> run_set(engine_core::DataModel& world, engine_core::InstanceId entry,
                                   engine_core::InstanceId material);
std::optional<std::string> run_remove(engine_core::DataModel& world, engine_core::InstanceId entry,
                                      RemoveChoice choice);
std::optional<std::string> run_replace_unassigned(engine_core::DataModel& world, engine_core::InstanceId terrain,
                                                  int from, int to);

}  // namespace ide
