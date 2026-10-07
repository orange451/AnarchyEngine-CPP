#include "TerrainMaterials.hpp"

#include "LuaApi.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"

#include <array>
#include <cstdint>

namespace ide {

namespace {

using engine_core::InstanceId;
using engine_core::LuaSlot;
using engine_core::Terrain;
using engine_core::TerrainMaterial;

const Terrain* live_terrain(const engine_core::DataModel& world, InstanceId id) {
    return world.alive(id) ? dynamic_cast<const Terrain*>(world.instance(id)) : nullptr;
}

Terrain* live_terrain(engine_core::DataModel& world, InstanceId id) {
    return world.alive(id) ? dynamic_cast<Terrain*>(world.instance(id)) : nullptr;
}

TerrainMaterial* live_entry(engine_core::DataModel& world, InstanceId id) {
    return world.alive(id) ? dynamic_cast<TerrainMaterial*>(world.instance(id)) : nullptr;
}

bool bit_set(const std::array<std::uint64_t, 4>& used, int id) {
    return ((used[static_cast<std::size_t>(id) >> 6] >> (id & 63)) & 1ull) != 0ull;
}

std::string id_range_error() { return "Id must be a whole number from 0 to 255"; }

}  // namespace

TerrainMaterialsView read_terrain_materials(const engine_core::DataModel& world, InstanceId terrain_id) {
    TerrainMaterialsView view;
    const Terrain* terrain = live_terrain(world, terrain_id);
    if (terrain == nullptr) {
        return view;
    }
    view.alive = true;
    view.terrain_name = world.name(terrain_id);
    const std::array<std::uint64_t, 4> used = terrain->volume().ids_used();
    std::array<bool, 256> covered{};
    for (TerrainMaterial* entry : terrain->materials()) {
        TerrainMaterialView entry_view;
        entry_view.id = entry->id();
        entry_view.material_id = entry->material_id();
        entry_view.name = world.name(entry->id());
        const LuaSlot slot = entry->material();
        if (slot.kind == LuaSlot::Kind::Instance && slot.id != 0) {
            entry_view.material = slot.id;
            entry_view.material_name = world.name(slot.id);
        } else {
            entry_view.material_missing = !slot.text.empty();
        }
        entry_view.in_use = bit_set(used, entry_view.material_id);
        covered[static_cast<std::size_t>(entry_view.material_id)] = true;
        view.materials.push_back(std::move(entry_view));
    }
    // Id 0 is the default material, not a TerrainMaterial any entry can hold,
    // so it is left out of both in_use accounting and this list.
    for (int id = 1; id <= 255; ++id) {
        if (bit_set(used, id) && !covered[static_cast<std::size_t>(id)]) {
            view.unassigned_in_use.push_back(id);
        }
    }
    view.voxel_revision = terrain->volume().revision();
    return view;
}

InstanceId add_terrain_material(engine_core::DataModel& world, InstanceId terrain_id, InstanceId material,
                                std::string& error) {
    Terrain* terrain = live_terrain(world, terrain_id);
    if (terrain == nullptr) {
        error = "That Terrain no longer exists";
        return 0;
    }
    TerrainMaterial* entry = nullptr;
    if (std::optional<std::string> add_error = terrain->add_material(material, entry)) {
        error = *add_error;
        return 0;
    }
    return entry->id();
}

std::optional<std::string> set_terrain_material(engine_core::DataModel& world, InstanceId entry_id,
                                                InstanceId material) {
    TerrainMaterial* entry = live_entry(world, entry_id);
    if (entry == nullptr) {
        return std::string("That TerrainMaterial no longer exists");
    }
    LuaSlot slot;
    if (material != 0) {
        slot.kind = LuaSlot::Kind::Instance;
        slot.id = material;
    }
    return entry->set_material(slot);
}

std::optional<std::string> remove_terrain_material(engine_core::DataModel& world, InstanceId entry_id,
                                                    RemoveChoice choice) {
    TerrainMaterial* entry = live_entry(world, entry_id);
    if (entry == nullptr) {
        return std::string("That TerrainMaterial no longer exists");
    }
    if (choice.kind == RemoveChoice::Kind::Replace) {
        if (choice.replace_with < 0 || choice.replace_with > 255) {
            return id_range_error();
        }
        if (auto* terrain = dynamic_cast<Terrain*>(world.instance(world.parent(entry_id)))) {
            terrain->replace_material_everywhere(entry->material_id(), choice.replace_with);
        }
    }
    // As delete_instances does: ask first, so a refusal leaves the entry alone.
    if (std::optional<std::string> error = world.destroy_error(entry_id)) {
        return error;
    }
    world.destroy_tree(entry_id);
    return std::nullopt;
}

std::optional<std::string> replace_unassigned(engine_core::DataModel& world, InstanceId terrain_id, int from,
                                              int to) {
    if (from < 0 || from > 255 || to < 0 || to > 255) {
        return id_range_error();
    }
    Terrain* terrain = live_terrain(world, terrain_id);
    if (terrain == nullptr) {
        return std::string("That Terrain no longer exists");
    }
    terrain->replace_material_everywhere(from, to);
    return std::nullopt;
}

}  // namespace ide
