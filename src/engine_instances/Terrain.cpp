#include "Terrain.hpp"

#include "Containment.hpp"
#include "Contract.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"
#include "TerrainMaterial.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <iterator>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot matrix_slot(const Matrix4& value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Matrix4;
    slot.transform = value;
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("Terrain setters run on SimulationThread");
    }
}

// True when the matrix's axes are unit length and at right angles.
bool rigid(const Matrix4& m) {
    const float* a = m.m;
    auto dot = [&](int i, int j) { return a[i] * a[j] + a[i + 1] * a[j + 1] + a[i + 2] * a[j + 2]; };
    constexpr float tolerance = 1e-3f;
    return std::fabs(dot(0, 0) - 1.f) < tolerance && std::fabs(dot(4, 4) - 1.f) < tolerance &&
           std::fabs(dot(8, 8) - 1.f) < tolerance && std::fabs(dot(0, 4)) < tolerance &&
           std::fabs(dot(0, 8)) < tolerance && std::fabs(dot(4, 8)) < tolerance;
}

}  // namespace

Terrain::Terrain(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PVInstance(tag, state, id) {}

std::optional<std::string> Terrain::set_transform(const Matrix4& transform) {
    require_thread(*this);
    for (float value : transform.m) {
        if (!std::isfinite(value)) {
            return std::string("Transform must be finite");
        }
    }
    if (!rigid(transform)) {
        return std::string("Terrain cannot be scaled");
    }
    if (same_matrix4(transform_, transform)) {
        return std::nullopt;
    }
    const Matrix4 previous = transform_;
    transform_ = transform;
    note_property_change("Transform", matrix_slot(previous), matrix_slot(transform));
    return std::nullopt;
}

std::optional<std::string> Terrain::set_can_collide(bool value) {
    require_thread(*this);
    if (can_collide_ == value) {
        return std::nullopt;
    }
    can_collide_ = value;
    note_property_change("CanCollide", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::vector<TerrainMaterial*> Terrain::materials() const {
    std::vector<TerrainMaterial*> out;
    for (InstanceId child = first_child(id()); child != 0; child = next_sibling(child)) {
        auto* entry = dynamic_cast<TerrainMaterial*>(const_cast<Terrain*>(this)->instance(child));
        if (entry != nullptr && entry->material_id() >= 1) {
            out.push_back(entry);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const TerrainMaterial* a, const TerrainMaterial* b) { return a->material_id() < b->material_id(); });
    return out;
}

TerrainMaterial* Terrain::material_by_id(int id) const {
    if (id < 1) {
        return nullptr;
    }
    for (TerrainMaterial* entry : materials()) {
        if (entry->material_id() == id) {
            return entry;
        }
    }
    return nullptr;
}

std::vector<TerrainMaterial*> Terrain::materials_for(InstanceId material) const {
    std::vector<TerrainMaterial*> out;
    for (TerrainMaterial* entry : materials()) {
        if (entry->material_instance() == material) {
            out.push_back(entry);
        }
    }
    return out;
}

int Terrain::free_id() const {
    std::bitset<TerrainMaterial::kMaxId + 1> held;
    for (InstanceId child = first_child(id()); child != 0; child = next_sibling(child)) {
        if (const auto* entry = dynamic_cast<const TerrainMaterial*>(instance(child))) {
            held.set(static_cast<std::size_t>(entry->material_id()));
        }
    }
    for (int candidate = 1; candidate <= TerrainMaterial::kMaxId; ++candidate) {
        if (!held.test(static_cast<std::size_t>(candidate))) {
            return candidate;
        }
    }
    return 0;
}

std::optional<std::string> Terrain::add_material(InstanceId material, TerrainMaterial*& out) {
    require_thread(*this);
    out = nullptr;
    const int free = free_id();
    if (free == 0) {
        return std::string("Terrain can hold at most 255 Materials");
    }
    // Checked before anything is made, so a refusal leaves nothing behind.
    if (material != 0) {
        const DataModel* target = alive(material) ? instance(material) : nullptr;
        if (target == nullptr || !lua_class_inherits(target->class_name(), "Material")) {
            return std::string("Material must be a Material");
        }
    }
    TerrainMaterial& entry = create<TerrainMaterial>();
    set_name(entry.id(), material != 0 ? name(material) : std::string("TerrainMaterial"));
    if (entry.set_material_id(free)) {
        contract_fail("add_material picked an Id set_material_id refuses");
    }
    if (material != 0) {
        LuaSlot slot;
        slot.kind = LuaSlot::Kind::Instance;
        slot.id = material;
        if (entry.set_material(slot)) {
            contract_fail("add_material checked a Material set_material refuses");
        }
    }
    set_parent(entry.id(), id());
    out = &entry;
    return std::nullopt;
}

void Terrain::on_reuse() {
    transform_ = matrix4_identity();
    can_collide_ = true;
    data_path_.clear();
    volume_ = terrain::VoxelVolume{};
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

Terrain* terrain_of(DataModel& object) { return dynamic_cast<Terrain*>(&object); }

bool read_transform(DataModel&, DataModel& object, LuaSlot& out) {
    const Terrain* terrain = terrain_of(object);
    if (terrain == nullptr) {
        return false;
    }
    out = matrix_slot(terrain->transform());
    return true;
}

bool write_transform(DataModel&, DataModel& object, LuaSlot& in) {
    Terrain* terrain = terrain_of(object);
    return terrain != nullptr && refuse(in, terrain->set_transform(in.transform));
}

bool read_voxel_size(DataModel&, DataModel& object, LuaSlot& out) {
    const Terrain* terrain = terrain_of(object);
    if (terrain == nullptr) {
        return false;
    }
    out = number_slot(terrain->voxel_size());
    return true;
}

bool read_can_collide(DataModel&, DataModel& object, LuaSlot& out) {
    const Terrain* terrain = terrain_of(object);
    if (terrain == nullptr) {
        return false;
    }
    out = bool_slot(terrain->can_collide());
    return true;
}

bool write_can_collide(DataModel&, DataModel& object, LuaSlot& in) {
    Terrain* terrain = terrain_of(object);
    return terrain != nullptr && refuse(in, terrain->set_can_collide(in.flag));
}

bool read_data_path(DataModel&, DataModel& object, LuaSlot& out) {
    const Terrain* terrain = terrain_of(object);
    if (terrain == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = terrain->data_path();
    return true;
}

bool write_data_path(DataModel&, DataModel& object, LuaSlot& in) {
    Terrain* terrain = terrain_of(object);
    if (terrain == nullptr) {
        return false;
    }
    terrain->set_data_path(in.text);
    return true;
}

ANARCHY_LUA_REGISTER(register_terrain_lua) {
    static const std::string identity = [] {
        const Matrix4 value = matrix4_identity();
        return write_json(json_floats(value.m, 16));
    }();
    LuaField data_path = lua_hidden(lua_saved_property("DataPath", "string", read_data_path, write_data_path, "\"\""));
    data_path.writable = false;
    const LuaField fields[] = {
        lua_saved_property("Transform", "Matrix4", read_transform, write_transform, identity.c_str()),
        lua_property("VoxelSize", "number", false, read_voxel_size, nullptr),
        lua_saved_property("CanCollide", "boolean", read_can_collide, write_can_collide, "true"),
        data_path,
    };
    register_lua_class("Terrain", "PVInstance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Terrain", {"Workspace"});
}

}  // namespace

}  // namespace engine_core
