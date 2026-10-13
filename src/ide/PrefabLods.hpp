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
    // Billboard: its Texture, and its Size in units.
    PartView texture;
    float width = 0.f;
    float height = 0.f;
    // Model: its Models in sibling order.
    std::vector<ModelView> models;
    bool operator==(const LodView&) const;
};

// "Model", "Billboard" or "None".
const char* lod_kind_text(engine_core::LodKind kind);
// A distance as the editor writes it: up to three decimals, no trailing zeros.
std::string distance_text(double value);

// The callers of these hold the world's read lock.
// prefab's LodLayers as they draw: nearest first, two at one distance in
// sibling order. Empty when prefab is not a live Prefab.
std::vector<LodView> read_lods(const engine_core::DataModel& world, engine_core::InstanceId prefab);
// Every Texture under its Assets category, by name, ignoring case.
std::vector<AssetChoice> texture_choices(const engine_core::DataModel& world);
// Of the dragged ids, the first live Texture, or 0.
engine_core::InstanceId dragged_texture(const engine_core::DataModel& world,
                                        const std::vector<engine_core::InstanceId>& ids);

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
// texture 0 clears it.
std::optional<std::string> run_set_lod_texture(engine_core::DataModel& world, engine_core::InstanceId layer,
                                               engine_core::InstanceId texture);
std::optional<std::string> run_set_lod_size(engine_core::DataModel& world, engine_core::InstanceId layer, float width,
                                            float height);

}  // namespace ide
