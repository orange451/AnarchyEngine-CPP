#include "PrefabModels.hpp"

#include "AssetInstances.hpp"
#include "LuaApi.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace ide {

namespace {

constexpr const char* kDefaultModelName = "Model";

std::size_t part_index(ModelPart part) { return part == ModelPart::Mesh ? 0 : 1; }

bool is_part(const engine_core::DataModel* object, ModelPart part) {
    return part == ModelPart::Mesh ? dynamic_cast<const engine_core::Mesh*>(object) != nullptr
                                   : dynamic_cast<const engine_core::Material*>(object) != nullptr;
}

PartView read_part(const engine_core::DataModel& world, const engine_core::Model& model, ModelPart part) {
    const engine_core::LuaSlot slot = model.reference(part_index(part));
    PartView view;
    if (slot.kind == engine_core::LuaSlot::Kind::Instance && slot.id != 0) {
        view.id = slot.id;
        view.name = world.name(slot.id);
        // Folder names up to and including the category, joined top down.
        const engine_core::InstanceId category = world.service(part == ModelPart::Mesh ? "Meshes" : "Materials");
        for (engine_core::InstanceId at = world.parent(slot.id);
             at != 0 && at != engine_core::DataModel::kNoParent && world.alive(at); at = world.parent(at)) {
            view.where = view.where.empty() ? world.name(at) : world.name(at) + "/" + view.where;
            if (at == category) {
                break;
            }
        }
    } else {
        view.missing = !slot.text.empty();
    }
    return view;
}

// name is base, or base and a space and a number: what unique_name makes from base.
bool named_from(const std::string& name, const std::string& base) {
    if (base.empty() || name.compare(0, base.size(), base) != 0) {
        return false;
    }
    if (name.size() == base.size()) {
        return true;
    }
    if (name[base.size()] != ' ' || name.size() == base.size() + 1) {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(base.size()) + 1, name.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

// base, or "base 2", "base 3", and on, whichever no child of parent but skip has.
std::string unique_name(const engine_core::DataModel& world, engine_core::InstanceId parent, const std::string& base,
                        engine_core::InstanceId skip = 0) {
    std::vector<std::string> taken;
    for (engine_core::InstanceId child : world.get_children(parent)) {
        if (child != skip) {
            taken.push_back(world.name(child));
        }
    }
    std::string name = base;
    for (int number = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++number) {
        name = base + " " + std::to_string(number);
    }
    return name;
}

}  // namespace

const char* model_part_name(ModelPart part) { return part == ModelPart::Mesh ? "Mesh" : "Material"; }

bool PartView::operator==(const PartView& other) const {
    return id == other.id && name == other.name && where == other.where && missing == other.missing;
}

bool ModelView::operator==(const ModelView& other) const {
    return id == other.id && name == other.name && mesh == other.mesh && material == other.material;
}

ModelStatus ModelView::status() const {
    if (mesh.missing || material.missing) {
        return ModelStatus::Missing;
    }
    if (mesh.id == 0 && material.id == 0) {
        return ModelStatus::NeedsBoth;
    }
    if (mesh.id == 0) {
        return ModelStatus::NeedsMesh;
    }
    if (material.id == 0) {
        return ModelStatus::NeedsMaterial;
    }
    return ModelStatus::Ready;
}

const char* model_status_text(ModelStatus status) {
    switch (status) {
    case ModelStatus::Ready:
        return "Ready";
    case ModelStatus::NeedsMesh:
        return "Needs a mesh";
    case ModelStatus::NeedsMaterial:
        return "Needs a material";
    case ModelStatus::NeedsBoth:
        return "Needs a mesh and a material";
    case ModelStatus::Missing:
        return "Refers to a deleted asset";
    }
    return "";
}

std::vector<ModelView> read_models(const engine_core::DataModel& world, engine_core::InstanceId prefab) {
    std::vector<ModelView> models;
    if (dynamic_cast<const engine_core::Prefab*>(world.instance(prefab)) == nullptr) {
        return models;
    }
    for (engine_core::InstanceId child : world.get_children(prefab)) {
        const auto* model = dynamic_cast<const engine_core::Model*>(world.instance(child));
        if (model == nullptr) {
            continue;
        }
        ModelView view;
        view.id = child;
        view.name = world.name(child);
        view.mesh = read_part(world, *model, ModelPart::Mesh);
        view.material = read_part(world, *model, ModelPart::Material);
        models.push_back(std::move(view));
    }
    return models;
}

std::vector<AssetChoice> part_choices(const engine_core::DataModel& world, ModelPart part) {
    return asset_choices(world, model_part_name(part));
}

DraggedParts dragged_parts(const engine_core::DataModel& world, const std::vector<engine_core::InstanceId>& ids) {
    DraggedParts parts;
    for (engine_core::InstanceId id : ids) {
        const engine_core::DataModel* object = world.alive(id) ? world.instance(id) : nullptr;
        if (parts.mesh == 0 && is_part(object, ModelPart::Mesh)) {
            parts.mesh = id;
        } else if (parts.material == 0 && is_part(object, ModelPart::Material)) {
            parts.material = id;
        }
    }
    return parts;
}

engine_core::InstanceId add_model(engine_core::DataModel& world, engine_core::InstanceId prefab,
                                  engine_core::InstanceId mesh, engine_core::InstanceId material, std::string& error) {
    if (!world.alive(prefab) || dynamic_cast<engine_core::Prefab*>(world.instance(prefab)) == nullptr) {
        error = "That Prefab no longer exists";
        return 0;
    }
    if (world.room_left() == 0) {
        error = engine_core::InstanceCapacityError().what();
        return 0;
    }
    engine_core::Model& model = world.create<engine_core::Model>();
    const std::string base = mesh != 0 && world.alive(mesh) ? world.name(mesh) : std::string(kDefaultModelName);
    world.set_name(model.id(), unique_name(world, prefab, base));
    world.set_parent(model.id(), prefab);
    for (const auto& [part, target] : {std::pair{ModelPart::Mesh, mesh}, std::pair{ModelPart::Material, material}}) {
        if (target == 0) {
            continue;
        }
        engine_core::LuaSlot slot;
        slot.kind = engine_core::LuaSlot::Kind::Instance;
        slot.id = target;
        // A target of the wrong class stays unset; the Model is still made.
        (void)model.set_reference(part_index(part), slot);
    }
    return model.id();
}

std::optional<std::string> set_model_part(engine_core::DataModel& world, engine_core::InstanceId model_id,
                                          ModelPart part, engine_core::InstanceId target) {
    auto* model = world.alive(model_id) ? dynamic_cast<engine_core::Model*>(world.instance(model_id)) : nullptr;
    if (model == nullptr) {
        return std::string("That Model no longer exists");
    }
    if (target != 0 && (!world.alive(target) || !is_part(world.instance(target), part))) {
        return std::string(model_part_name(part)) + " must be a " + model_part_name(part);
    }
    // Read before the write: the name the Model would have kept from its old Mesh.
    const engine_core::LuaSlot before = model->reference(part_index(part));
    const std::string old_base = part == ModelPart::Mesh && before.kind == engine_core::LuaSlot::Kind::Instance
                                     ? world.name(before.id)
                                     : std::string();
    engine_core::LuaSlot slot;
    if (target != 0) {
        slot.kind = engine_core::LuaSlot::Kind::Instance;
        slot.id = target;
    }
    if (std::optional<std::string> error = model->set_reference(part_index(part), slot)) {
        return error;
    }
    if (part == ModelPart::Mesh && target != 0) {
        const std::string name = world.name(model_id);
        if (named_from(name, kDefaultModelName) || named_from(name, old_base)) {
            const engine_core::InstanceId prefab = world.parent(model_id);
            world.set_name(model_id, unique_name(world, prefab, world.name(target), model_id));
        }
    }
    return std::nullopt;
}

}  // namespace ide
