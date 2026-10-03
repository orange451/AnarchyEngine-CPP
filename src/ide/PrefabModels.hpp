#pragma once

#include "AssetChoices.hpp"

#include "DataModel.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

// What the Prefab editor shows and does, without widgets: a Prefab's Models,
// the Meshes and Materials they can hold, and the writes that change them.

// A Model's two references, in its reference order.
enum class ModelPart { Mesh, Material };
// "Mesh" or "Material": the property, and the class it holds.
const char* model_part_name(ModelPart part);

// One side of a Model: what it refers to, and its folder from its category
// down, such as "Meshes/Props". id is 0 when it refers to nothing live;
// missing is set when it still names an asset that is gone.
struct PartView {
    engine_core::InstanceId id = 0;
    std::string name;
    std::string where;
    bool missing = false;
    bool operator==(const PartView&) const;
};

// How far a Model is from being drawn.
enum class ModelStatus { Ready, NeedsMesh, NeedsMaterial, NeedsBoth, Missing };

// One Model of a Prefab, as a card shows it.
struct ModelView {
    engine_core::InstanceId id = 0;
    std::string name;
    PartView mesh;
    PartView material;
    const PartView& part(ModelPart which) const { return which == ModelPart::Mesh ? mesh : material; }
    ModelStatus status() const;
    bool operator==(const ModelView&) const;
};

// The words a card shows under a Model's name.
const char* model_status_text(ModelStatus status);

// The callers of these hold the world's read lock.
// prefab's Models in sibling order. Empty when prefab is not a live Prefab.
std::vector<ModelView> read_models(const engine_core::DataModel& world, engine_core::InstanceId prefab);
// Every Mesh, or every Material, under its Assets category, by name, ignoring case.
std::vector<AssetChoice> part_choices(const engine_core::DataModel& world, ModelPart part);
// Of the dragged ids, the first live Mesh and the first live Material, 0 for none.
struct DraggedParts {
    engine_core::InstanceId mesh = 0;
    engine_core::InstanceId material = 0;
    bool any() const { return mesh != 0 || material != 0; }
};
DraggedParts dragged_parts(const engine_core::DataModel& world, const std::vector<engine_core::InstanceId>& ids);

// These run on the simulation thread, inside the caller's undo recording.
// A new Model last in prefab, holding mesh and material when they are not 0.
// It is named after its Mesh, or "Model", with a number when a sibling has
// that name. 0, with error set, when prefab is gone or the place is full.
engine_core::InstanceId add_model(engine_core::DataModel& world, engine_core::InstanceId prefab,
                                  engine_core::InstanceId mesh, engine_core::InstanceId material, std::string& error);
// Points model's part at target, or at nothing when target is 0. A Model
// still named as add_model named it takes a new Mesh's name. Why it was
// refused, changing nothing.
std::optional<std::string> set_model_part(engine_core::DataModel& world, engine_core::InstanceId model, ModelPart part,
                                          engine_core::InstanceId target);

}  // namespace ide
