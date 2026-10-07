#include "TerrainMaterial.hpp"

#include "Containment.hpp"
#include "Contract.hpp"
#include "LuaApi.hpp"
#include "Terrain.hpp"

#include <cmath>
#include <iterator>

namespace engine_core {
namespace {

constexpr const char* kDefaultName = "TerrainMaterial";

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("TerrainMaterial setters run on SimulationThread");
    }
}

std::string id_refused() { return std::string("Id must be a whole number from 1 to 255"); }

}  // namespace

std::optional<std::string> TerrainMaterial::set_material_id(int value) {
    require_thread(*this);
    if (value < 1 || value > kMaxId) {
        return id_refused();
    }
    if (value == material_id_) {
        return std::nullopt;
    }
    const int previous = material_id_;
    material_id_ = value;
    note_property_change("Id", number_slot(previous), number_slot(value));
    return std::nullopt;
}

LuaSlot TerrainMaterial::material() const { return instance_reference_slot(material_, "Material"); }

InstanceId TerrainMaterial::material_instance() const { return material().id; }

std::optional<std::string> TerrainMaterial::set_material(const LuaSlot& value) {
    require_thread(*this);
    const InstanceId old = material_instance();
    const std::string old_name = old != 0 ? name(old) : std::string();
    if (std::optional<std::string> error = set_instance_reference("Material", "Material", material_, value)) {
        return error;
    }
    // Only a live Material that a script or the user passed renames it. A
    // load, Stop, or undo names the GUID and restores the Name on its own.
    const InstanceId now = material_instance();
    if (value.kind != LuaSlot::Kind::Instance || value.id == 0 || now == 0 || now == old) {
        return std::nullopt;
    }
    const std::string current = name(id());
    if (current == kDefaultName || (!old_name.empty() && current == old_name)) {
        set_name(id(), name(now));
    }
    return std::nullopt;
}

void TerrainMaterial::on_reuse() {
    material_id_ = 0;
    material_.set_guid(std::string());
}

void TerrainMaterial::on_parent_changed(InstanceId previous, InstanceId next) {
    (void)previous;
    // SimulationThread, at the end of set_parent: load, paste, duplicate, and
    // undo of a delete all set the Id first and the parent second.
    const auto* terrain = next == kNoParent ? nullptr : dynamic_cast<const Terrain*>(instance(next));
    if (terrain == nullptr) {
        return;
    }
    bool clash = material_id_ == 0;
    for (InstanceId child = first_child(next); child != 0 && !clash; child = next_sibling(child)) {
        const auto* sibling = child == id() ? nullptr : dynamic_cast<const TerrainMaterial*>(instance(child));
        clash = sibling != nullptr && sibling->material_id() == material_id_;
    }
    if (!clash) {
        return;
    }
    // The one already there keeps the Id, so no voxel changes what it looks
    // like. With all 255 taken, this one is left on 0 and draws nothing.
    const int free = terrain->free_id();
    if (free == material_id_) {
        return;
    }
    material_id_ = free;
    emit_property("Id");
    note_unrecorded_edit(id());
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

TerrainMaterial* entry_of(DataModel& object) { return dynamic_cast<TerrainMaterial*>(&object); }

bool read_id(DataModel&, DataModel& object, LuaSlot& out) {
    const TerrainMaterial* entry = entry_of(object);
    if (entry == nullptr) {
        return false;
    }
    out = number_slot(entry->material_id());
    return true;
}

bool write_id(DataModel&, DataModel& object, LuaSlot& in) {
    TerrainMaterial* entry = entry_of(object);
    if (entry == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Number || !std::isfinite(in.number) || std::floor(in.number) != in.number ||
        in.number < 1.0 || in.number > TerrainMaterial::kMaxId) {
        in.error = id_refused();
        return false;
    }
    return refuse(in, entry->set_material_id(static_cast<int>(in.number)));
}

bool read_material(DataModel&, DataModel& object, LuaSlot& out) {
    const TerrainMaterial* entry = entry_of(object);
    if (entry == nullptr) {
        return false;
    }
    out = entry->material();
    return true;
}

bool write_material(DataModel&, DataModel& object, LuaSlot& in) {
    TerrainMaterial* entry = entry_of(object);
    return entry != nullptr && refuse(in, entry->set_material(in));
}

ANARCHY_LUA_REGISTER(register_terrain_material_lua) {
    LuaField id = lua_saved_property("Id", "number", read_id, write_id, "0");
    id.writable = false;   // scripts read it; load, Stop, undo, and paste still write it
    const LuaField fields[] = {
        id,
        lua_saved_property("Material", "Material?", read_material, write_material, "null"),
    };
    register_lua_class("TerrainMaterial", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("TerrainMaterial", {"Terrain"});
}

}  // namespace

}  // namespace engine_core
