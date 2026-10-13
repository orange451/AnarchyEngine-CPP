#include "PrefabLods.hpp"

#include "AssetInstances.hpp"
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

}  // namespace

bool LodView::operator==(const LodView& other) const {
    return id == other.id && name == other.name && distance == other.distance && kind == other.kind &&
           until == other.until && covered == other.covered && texture == other.texture && width == other.width &&
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
        view.texture = read_asset_part(world, layer->reference(engine_core::LodLayer::kTextureReference), "Textures");
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

std::vector<AssetChoice> texture_choices(const engine_core::DataModel& world) {
    return asset_choices(world, "Texture");
}

engine_core::InstanceId dragged_texture(const engine_core::DataModel& world,
                                        const std::vector<engine_core::InstanceId>& ids) {
    for (engine_core::InstanceId id : ids) {
        if (world.alive(id) && dynamic_cast<const engine_core::Texture*>(world.instance(id)) != nullptr) {
            return id;
        }
    }
    return 0;
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

std::optional<std::string> run_set_lod_texture(engine_core::DataModel& world, engine_core::InstanceId layer,
                                               engine_core::InstanceId texture) {
    ScopedRecording step(world, texture != 0 ? "Set LOD Texture" : "Clear LOD Texture");
    engine_core::LodLayer* live = live_layer(world, layer);
    if (live == nullptr) {
        return std::string(kGone);
    }
    if (texture != 0 &&
        (!world.alive(texture) || dynamic_cast<const engine_core::Texture*>(world.instance(texture)) == nullptr)) {
        return std::string("Texture must be a Texture");
    }
    engine_core::LuaSlot slot;
    if (texture != 0) {
        slot.kind = engine_core::LuaSlot::Kind::Instance;
        slot.id = texture;
    }
    return live->set_reference(engine_core::LodLayer::kTextureReference, slot);
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
