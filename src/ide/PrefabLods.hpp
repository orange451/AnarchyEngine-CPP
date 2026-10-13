#pragma once

#include "AssetChoices.hpp"
#include "PrefabModels.hpp"

#include "DataModel.hpp"
#include "Enum.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ide {

// What the Prefab editor's LOD section shows and does, without widgets: a
// Prefab's LodLayers, and the writes that change them.

// One LodLayer, as its card shows it.
struct LodView {
    engine_core::InstanceId id = 0;
    std::string name;
    double distance = 0.0;
    engine_core::LodKind kind = engine_core::LodKind::Model;
    // Where the next layer takes over, or none for the last.
    std::optional<double> until;
    // Another layer at its distance, later in sibling order, draws instead.
    bool covered = false;
    // Billboard: its Material, and its Size in units.
    PartView material;
    float width = 0.f;
    float height = 0.f;
    // Model: its Models in sibling order.
    std::vector<ModelView> models;
    bool operator==(const LodView&) const;
};

// What a bake saved for a card (save_card): each map's Path under the
// resources folder, empty where the card did not vary, and the numbers its
// Material takes, a map multiplying its number.
struct CardFiles {
    std::string color;
    std::string normal;
    std::string roughness_map;
    std::string metalness_map;
    std::string emissive;
    // 1 with its map, which holds it; else the card's one value.
    float roughness = 1.f;
    float metalness = 1.f;
    float reflectivity = 0.5f;
    // The card's Size, in units.
    float width = 0.f;
    float height = 0.f;
};

// "Model", "Billboard" or "None".
const char* lod_kind_text(engine_core::LodKind kind);
// A distance as the editor writes it: up to three decimals, no trailing zeros.
std::string distance_text(double value);

// The callers of these hold the world's read lock.
// prefab's LodLayers as they draw: nearest first, two at one distance in
// sibling order. Empty when prefab is not a live Prefab.
std::vector<LodView> read_lods(const engine_core::DataModel& world, engine_core::InstanceId prefab);

// These run on the simulation thread, each one undo step.
// A new LodLayer last in prefab: Kind Model, named "LOD" (with a number when
// a sibling has that name), from twice the farthest layer's distance, or 100
// for the first. 0, with error set, when prefab is gone or the place is full.
engine_core::InstanceId run_add_lod(engine_core::DataModel& world, engine_core::InstanceId prefab,
                                    std::string& error);
// Each sets one of layer's properties, or says why it was refused, changing nothing.
std::optional<std::string> run_set_lod_distance(engine_core::DataModel& world, engine_core::InstanceId layer,
                                                double distance);
std::optional<std::string> run_set_lod_kind(engine_core::DataModel& world, engine_core::InstanceId layer,
                                            engine_core::LodKind kind);
// material 0 clears it.
std::optional<std::string> run_set_lod_material(engine_core::DataModel& world, engine_core::InstanceId layer,
                                                engine_core::InstanceId material);
std::optional<std::string> run_set_lod_size(engine_core::DataModel& world, engine_core::InstanceId layer, float width,
                                            float height);
// A card baked for layer (BillboardBaker), saved as files says, as layer's
// Material and Size. The Material is layer's own when it already draws a card
// from Assets' Materials/Billboards folder, else a new one there named after
// the Prefab; each map is a Texture in Textures/Billboards, the Material's own
// when it has one from there in that slot, its Path the map's. A map not saved
// leaves its slot empty, and the Material's number holds it. One undo step.
std::optional<std::string> run_set_lod_card(engine_core::DataModel& world, engine_core::InstanceId layer,
                                            const CardFiles& files);

}  // namespace ide
