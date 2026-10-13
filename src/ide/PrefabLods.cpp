#include "PrefabLods.hpp"

#include "AssetInstances.hpp"
#include "Folder.hpp"
#include "ScopedRecording.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace ide {

namespace {

constexpr const char* kDefaultLodName = "LOD";

engine_core::LodLayer* live_layer(engine_core::DataModel& world, engine_core::InstanceId layer) {
    return world.alive(layer) ? dynamic_cast<engine_core::LodLayer*>(world.instance(layer)) : nullptr;
}

constexpr const char* kGone = "That LOD no longer exists";

// A reference to id, or to nothing for 0.
engine_core::LuaSlot instance_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    if (id != 0) {
        slot.kind = engine_core::LuaSlot::Kind::Instance;
        slot.id = id;
    }
    return slot;
}

// category's Billboards Folder, where baked cards are kept, made the first time.
engine_core::InstanceId billboards_folder(engine_core::DataModel& world, engine_core::InstanceId category) {
    const engine_core::InstanceId found = world.find_first_child(category, "Billboards");
    if (found != 0 && dynamic_cast<engine_core::Folder*>(world.instance(found)) != nullptr) {
        return found;
    }
    engine_core::Folder& made = world.create<engine_core::Folder>();
    world.set_name(made.id(), unique_child_name(world, category, "Billboards"));
    world.set_parent(made.id(), category);
    return made.id();
}

// What reference names when it is a live instance in folder: a card a bake made.
engine_core::DataModel* own_card(engine_core::DataModel& world, const engine_core::LuaSlot& reference,
                                 engine_core::InstanceId folder) {
    if (reference.kind != engine_core::LuaSlot::Kind::Instance || !world.alive(reference.id) ||
        world.parent(reference.id) != folder) {
        return nullptr;
    }
    return world.instance(reference.id);
}

}  // namespace

bool LodView::operator==(const LodView& other) const {
    return id == other.id && name == other.name && distance == other.distance && kind == other.kind &&
           until == other.until && covered == other.covered && material == other.material && width == other.width &&
           height == other.height && models == other.models;
}

const char* lod_kind_text(engine_core::LodKind kind) {
    switch (kind) {
    case engine_core::LodKind::Model:
        return "Model";
    case engine_core::LodKind::Billboard:
        return "Billboard";
    case engine_core::LodKind::None:
        return "None";
    }
    return "";
}

std::string distance_text(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f", value);
    std::string out = text;
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    return out;
}

std::vector<LodView> read_lods(const engine_core::DataModel& world, engine_core::InstanceId prefab) {
    std::vector<LodView> lods;
    if (dynamic_cast<const engine_core::Prefab*>(world.instance(prefab)) == nullptr) {
        return lods;
    }
    for (engine_core::InstanceId child : world.get_children(prefab)) {
        const auto* layer = dynamic_cast<const engine_core::LodLayer*>(world.instance(child));
        if (layer == nullptr) {
            continue;
        }
        LodView view;
        view.id = child;
        view.name = world.name(child);
        view.distance = layer->distance();
        view.kind = layer->kind();
        view.material = read_asset_part(world, layer->reference(engine_core::LodLayer::kMaterialReference), "Materials");
        view.width = layer->size().x;
        view.height = layer->size().y;
        view.models = read_models(world, child);
        lods.push_back(std::move(view));
    }
    // As they draw: nearest first, and of two at one distance the later one draws.
    std::stable_sort(lods.begin(), lods.end(),
                     [](const LodView& a, const LodView& b) { return a.distance < b.distance; });
    for (std::size_t index = 0; index + 1 < lods.size(); ++index) {
        lods[index].until = lods[index + 1].distance;
        lods[index].covered = !(lods[index].distance < lods[index + 1].distance);
    }
    return lods;
}

engine_core::InstanceId run_add_lod(engine_core::DataModel& world, engine_core::InstanceId prefab,
                                    std::string& error) {
    ScopedRecording step(world, "Add LOD");
    if (!world.alive(prefab) || dynamic_cast<engine_core::Prefab*>(world.instance(prefab)) == nullptr) {
        error = "That Prefab no longer exists";
        return 0;
    }
    if (world.room_left() == 0) {
        error = engine_core::InstanceCapacityError(world.instance_capacity()).what();
        return 0;
    }
    double farthest = 0.0;
    for (engine_core::InstanceId child : world.get_children(prefab)) {
        if (const auto* layer = dynamic_cast<const engine_core::LodLayer*>(world.instance(child))) {
            farthest = std::max(farthest, layer->distance());
        }
    }
    engine_core::LodLayer& layer = world.create<engine_core::LodLayer>();
    world.set_name(layer.id(), unique_child_name(world, prefab, kDefaultLodName));
    (void)layer.set_distance(farthest > 0.0 ? farthest * 2.0 : engine_core::LodLayer::kDefaultDistance);
    world.set_parent(layer.id(), prefab);
    return layer.id();
}

std::optional<std::string> run_set_lod_distance(engine_core::DataModel& world, engine_core::InstanceId layer,
                                                double distance) {
    ScopedRecording step(world, "Set LOD Distance");
    engine_core::LodLayer* live = live_layer(world, layer);
    if (live == nullptr) {
        return std::string(kGone);
    }
    return live->set_distance(distance);
}

std::optional<std::string> run_set_lod_kind(engine_core::DataModel& world, engine_core::InstanceId layer,
                                            engine_core::LodKind kind) {
    ScopedRecording step(world, std::string("Make LOD ") + lod_kind_text(kind));
    engine_core::LodLayer* live = live_layer(world, layer);
    if (live == nullptr) {
        return std::string(kGone);
    }
    return live->set_kind(static_cast<int>(kind));
}

std::optional<std::string> run_set_lod_material(engine_core::DataModel& world, engine_core::InstanceId layer,
                                                engine_core::InstanceId material) {
    ScopedRecording step(world, material != 0 ? "Set LOD Material" : "Clear LOD Material");
    engine_core::LodLayer* live = live_layer(world, layer);
    if (live == nullptr) {
        return std::string(kGone);
    }
    if (material != 0 &&
        (!world.alive(material) || dynamic_cast<const engine_core::Material*>(world.instance(material)) == nullptr)) {
        return std::string("Material must be a Material");
    }
    return live->set_reference(engine_core::LodLayer::kMaterialReference, instance_slot(material));
}

std::optional<std::string> run_set_lod_card(engine_core::DataModel& world, engine_core::InstanceId layer,
                                            const CardFiles& files) {
    ScopedRecording step(world, "Bake Billboard");
    engine_core::LodLayer* live = live_layer(world, layer);
    if (live == nullptr) {
        return std::string(kGone);
    }
    const engine_core::InstanceId textures = world.service("Textures");
    const engine_core::InstanceId materials = world.service("Materials");
    if (textures == 0 || materials == 0) {
        return std::string("This place has no Textures and Materials to keep the card in");
    }
    // Two folders, a Material, and a Texture for each of its five maps, at most.
    if (world.room_left() < 8) {
        return std::string(engine_core::InstanceCapacityError(world.instance_capacity()).what());
    }
    const engine_core::InstanceId texture_folder = billboards_folder(world, textures);
    const engine_core::InstanceId material_folder = billboards_folder(world, materials);
    const std::string prefab = world.name(world.parent(layer));
    // The layer's own card Material when it draws one from there; else a new one.
    auto* material = dynamic_cast<engine_core::Material*>(
        own_card(world, live->reference(engine_core::LodLayer::kMaterialReference), material_folder));
    if (material == nullptr) {
        engine_core::Material& made = world.create<engine_core::Material>();
        world.set_name(made.id(), unique_child_name(world, material_folder, prefab + " Card"));
        world.set_parent(made.id(), material_folder);
        material = &made;
    }
    // Each map in its slot: the Material's own card Texture there, else a new one.
    const auto put = [&](std::size_t slot, const std::string& path, const char* map) -> std::optional<std::string> {
        if (path.empty()) {
            return material->set_reference(slot, engine_core::LuaSlot{});
        }
        auto* texture = dynamic_cast<engine_core::Texture*>(own_card(world, material->reference(slot), texture_folder));
        if (texture == nullptr) {
            engine_core::Texture& made = world.create<engine_core::Texture>();
            world.set_name(made.id(), unique_child_name(world, texture_folder, prefab + " Card " + map));
            world.set_parent(made.id(), texture_folder);
            texture = &made;
        }
        if (std::optional<std::string> error = texture->set_path(path)) {
            return error;
        }
        return material->set_reference(slot, instance_slot(texture->id()));
    };
    using engine_core::Material;
    for (std::optional<std::string> error :
         {put(Material::kDiffuseTextureReference, files.color, "Color"),
          put(Material::kNormalTextureReference, files.normal, "Normal"),
          put(Material::kRoughnessTextureReference, files.roughness_map, "Roughness"),
          put(Material::kMetalnessTextureReference, files.metalness_map, "Metalness"),
          put(Material::kEmissiveTextureReference, files.emissive, "Glow")}) {
        if (error) {
            return error;
        }
    }
    // The maps hold the card's look: the numbers only scale them, or stand in for one.
    for (std::optional<std::string> error :
         {material->set_color(Material::kDefaultColor), material->set_transparency(0.0),
          material->set_metalness(files.metalness), material->set_roughness(files.roughness),
          material->set_reflectivity(files.reflectivity),
          material->set_emissive(files.emissive.empty() ? Material::kDefaultEmissive : Material::kDefaultColor)}) {
        if (error) {
            return error;
        }
    }
    if (std::optional<std::string> error =
            live->set_reference(engine_core::LodLayer::kMaterialReference, instance_slot(material->id()))) {
        return error;
    }
    return live->set_size({files.width, files.height});
}

std::optional<std::string> run_set_lod_size(engine_core::DataModel& world, engine_core::InstanceId layer, float width,
                                            float height) {
    ScopedRecording step(world, "Set LOD Size");
    engine_core::LodLayer* live = live_layer(world, layer);
    if (live == nullptr) {
        return std::string(kGone);
    }
    return live->set_size({width, height});
}

}  // namespace ide
