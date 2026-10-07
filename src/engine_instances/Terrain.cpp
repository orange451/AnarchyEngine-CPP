#include "Terrain.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Contract.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "TerrainMaterial.hpp"
#include "terrain/AvoxFile.hpp"
#include "terrain/TerrainStash.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

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

// True when the matrix's axes are unit length and at right angles, and keep
// their handedness: a mirror is a scale of -1.
bool rigid(const Matrix4& m) {
    const float* a = m.m;
    auto dot = [&](int i, int j) { return a[i] * a[j] + a[i + 1] * a[j + 1] + a[i + 2] * a[j + 2]; };
    constexpr float tolerance = 1e-3f;
    // The first axis dotted with the cross of the other two.
    const float determinant = a[0] * (a[5] * a[10] - a[6] * a[9]) - a[1] * (a[4] * a[10] - a[6] * a[8]) +
                              a[2] * (a[4] * a[9] - a[5] * a[8]);
    return std::fabs(dot(0, 0) - 1.f) < tolerance && std::fabs(dot(4, 4) - 1.f) < tolerance &&
           std::fabs(dot(8, 8) - 1.f) < tolerance && std::fabs(dot(0, 4)) < tolerance &&
           std::fabs(dot(0, 8)) < tolerance && std::fabs(dot(4, 8)) < tolerance && determinant > 0.f;
}

// Terrain's place bytes: [u32 base length][DataModel's bytes][u64 token].
struct PlaceParts {
    const std::byte* base = nullptr;
    std::size_t base_size = 0;
    // 0 when the bytes are too short to hold one.
    std::uint64_t token = 0;
};

PlaceParts split_place(const std::byte* data, std::size_t size) {
    PlaceParts parts;
    std::uint32_t length = 0;
    if (data != nullptr && size >= sizeof(length)) {
        std::memcpy(&length, data, sizeof(length));
        if (length <= size - sizeof(length)) {
            parts.base = data + sizeof(length);
            parts.base_size = length;
            if (size - sizeof(length) - length >= sizeof(parts.token)) {
                std::memcpy(&parts.token, parts.base + length, sizeof(parts.token));
            }
        }
    }
    return parts;
}

// The DataPath in the base bytes, which are the saved properties as JSON.
std::string place_data_path(const PlaceParts& parts) {
    if (parts.base == nullptr || parts.base_size == 0) {
        return std::string();
    }
    JsonValue values;
    std::string message;
    if (!parse_json(std::string_view(reinterpret_cast<const char*>(parts.base), parts.base_size), values, message) ||
        !values.is_object()) {
        return std::string();
    }
    const JsonValue* path = values.find("DataPath");
    return path != nullptr && path->is_string() ? path->as_string() : std::string();
}

// A Terrain's DataPath and voxels as Play's capture holds them. False when
// there is no capture, or its stash token is gone.
bool captured_voxels(const std::vector<std::byte>* captured, std::string& path, terrain::ChunkMap& chunks,
                     float& voxel_size) {
    if (captured == nullptr) {
        return false;
    }
    const PlaceParts parts = split_place(captured->data(), captured->size());
    if (!terrain::TerrainStash::get(parts.token, chunks, voxel_size)) {
        return false;
    }
    path = place_data_path(parts);
    return true;
}

// Writes chunks as the .avox file at path under root.
std::optional<std::string> write_avox(const std::filesystem::path& root, const std::string& name,
                                      const std::string& path, const terrain::ChunkMap& chunks, float voxel_size) {
    if (std::optional<std::string> error = resource_path_error(path)) {
        return "Terrain " + name + ": DataPath " + path + ": " + *error;
    }
    terrain::VoxelVolume volume(voxel_size);
    volume.set_chunks(chunks);
    return write_resource_file(root, path, terrain::encode_avox(volume));
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

void Terrain::load_data_path(std::string path) {
    if (restoring_ || path.empty() || path == data_path_) {
        data_path_ = std::move(path);
        return;
    }
    Terrain* holder = nullptr;
    for_each_instance([&](DataModel& object) {
        auto* other = dynamic_cast<Terrain*>(&object);
        if (holder == nullptr && other != nullptr && other != this && other->data_path_ == path) {
            holder = other;
        }
    });
    terrain::ChunkMap chunks;
    bool found = false;
    if (holder != nullptr) {
        chunks = holder->volume_.chunks();
        found = true;
    } else if (history().enabled()) {
        // A source that was cut: its delete's undo record holds the path's
        // latest voxels. A load builds with history off and never asks, so
        // reopening a project keeps each Terrain's file.
        float stashed_size = volume_.voxel_size();
        found = terrain::TerrainStash::get(terrain::TerrainStash::latest(path), chunks, stashed_size);
    }
    if (!found) {
        read_data_file(std::move(path));
        return;
    }
    // A paste: the copy starts with its source's voxels and its own file.
    volume_.set_chunks(std::move(chunks));
    data_path_ = own_data_path(std::string());
    emit_property("DataPath");
    note_unrecorded_edit(id());
    // The paste's step recorded this Terrain empty when it was made.
    refresh_created_record(id());
}

void Terrain::read_data_file(std::string path) {
    data_path_ = std::move(path);
    const std::filesystem::path root = resources_root();
    // No project, so no resources folder: there is no file to read.
    if (root.empty()) {
        return;
    }
    std::optional<std::string> damage = resource_path_error(data_path_);
    const std::filesystem::path file = root / std::filesystem::u8path(data_path_);
    std::error_code error;
    if (!damage && !std::filesystem::is_regular_file(file, error)) {
        volume_.set_chunks(terrain::ChunkMap{});
        warn("Terrain " + name(id()) + ": its voxel file " + data_path_ + " is missing, so it is empty");
        // Empty is what it loaded: a save writes nothing until it is edited.
        saved_ = true;
        saved_path_ = data_path_;
        saved_chunks_.clear();
        return;
    }
    terrain::VoxelVolume loaded(volume_.voxel_size());
    if (!damage) {
        std::ifstream in(file, std::ios::binary);
        std::vector<char> bytes;
        if (in) {
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (!in.is_open() || in.bad()) {
            damage = std::string("it could not be read");
        } else {
            damage = terrain::decode_avox(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size(), loaded);
        }
    }
    if (damage) {
        volume_.set_chunks(terrain::ChunkMap{});
        warn("Terrain " + name(id()) + ": its voxel file " + data_path_ + " is damaged (" + *damage +
             "), so it is empty");
        // The damaged file stays as it is, for recovery by hand: the next
        // save writes this Terrain's voxels to a file of its own.
        data_path_ = own_data_path(data_path_);
        emit_property("DataPath");
        note_unrecorded_edit(id());
        return;
    }
    volume_.set_chunks(loaded.chunks());
    saved_ = true;
    saved_path_ = data_path_;
    saved_chunks_ = volume_.chunks();
}

std::string Terrain::own_data_path(const std::string& avoid) const {
    const std::string stem = "terrain/" + sanitize_file_name(name(id())) + "." + guid(id());
    std::string path = stem + ".avox";
    for (int suffix = 2; path == avoid; ++suffix) {
        path = stem + "." + std::to_string(suffix) + ".avox";
    }
    return path;
}

std::optional<std::string> Terrain::edit_volume(
    const std::function<std::optional<std::string>(terrain::VoxelVolume&)>& edit) {
    require_thread(*this);
    if (std::optional<std::string> error = edit(volume_)) {
        return error;
    }
    // Edits during play are the session's: Stop drops them, and no save
    // writes them.
    if (simulation_running()) {
        return std::nullopt;
    }
    // The first authored voxels: the file's path is in every snapshot from now on.
    if (data_path_.empty()) {
        data_path_ = own_data_path(std::string());
        emit_property("DataPath");
    }
    note_unrecorded_edit(id());
    refresh_creation();
    return std::nullopt;
}

void Terrain::replace_material_everywhere(int from, int to) {
    edit_volume([&](terrain::VoxelVolume& volume) -> std::optional<std::string> {
        volume.replace_everywhere(static_cast<std::uint8_t>(from), static_cast<std::uint8_t>(to));
        return std::nullopt;
    });
}

void Terrain::refresh_creation() {
    // Known limitation: a creation committed in an earlier step keeps the
    // record it was made with, so undo then redo of it brings the Terrain
    // back without the edits made after it. That needs voxel undo
    // (sub-project 2).
    const std::vector<std::byte>* before = open_created_place(id());
    if (before == nullptr) {
        return;
    }
    // A script may edit thousands of times in one recording: once this
    // Terrain's own refresh has put an entry in the record, later ones
    // overwrite that entry rather than putting one per edit.
    const std::uint64_t held = split_place(before->data(), before->size()).token;
    reuse_token_ = held != 0 && held == refreshed_token_ ? held : 0;
    refresh_created_record(id());
    reuse_token_ = 0;
    if (const std::vector<std::byte>* after = open_created_place(id())) {
        refreshed_token_ = split_place(after->data(), after->size()).token;
    }
}

std::optional<std::string> Terrain::save_resources(const std::filesystem::path& root) {
    require_thread(*this);
    std::string path = data_path_;
    terrain::ChunkMap chunks;
    float voxel_size = volume_.voxel_size();
    if (simulation_running()) {
        // A save during play writes what Stop will restore: the voxels and
        // DataPath this Terrain had at Play's capture.
        if (!captured_voxels(captured_place_bytes(id()), path, chunks, voxel_size)) {
            return std::nullopt;
        }
    } else {
        chunks = volume_.chunks();
    }
    // No DataPath: this Terrain has never had authored voxels.
    if (path.empty()) {
        return std::nullopt;
    }
    // Unchanged since the last save or load. A Terrain that loaded empty
    // because its file was missing writes nothing until it is edited, so a
    // file the user puts back is not written over.
    std::error_code error;
    if (saved_ && saved_path_ == path && saved_chunks_ == chunks &&
        (chunks.empty() || std::filesystem::is_regular_file(root / std::filesystem::u8path(path), error))) {
        return std::nullopt;
    }
    if (std::optional<std::string> failure = write_avox(root, name(id()), path, chunks, voxel_size)) {
        return failure;
    }
    saved_ = true;
    saved_path_ = std::move(path);
    saved_chunks_ = std::move(chunks);
    return std::nullopt;
}

std::optional<std::string> Terrain::save_captured(const std::filesystem::path& root, const std::string& name,
                                                  const std::vector<std::byte>* captured) {
    std::string path;
    terrain::ChunkMap chunks;
    float voxel_size = 1.f;
    if (!captured_voxels(captured, path, chunks, voxel_size) || path.empty()) {
        return std::nullopt;
    }
    // Nothing remembers what this Terrain last wrote, so its file is written.
    return write_avox(root, name, path, chunks, voxel_size);
}

void Terrain::write_place(std::vector<std::byte>& out) const {
    std::vector<std::byte> base;
    DataModel::write_place(base);
    const std::uint32_t length = static_cast<std::uint32_t>(base.size());
    const auto* l = reinterpret_cast<const std::byte*>(&length);
    out.insert(out.end(), l, l + sizeof(length));
    out.insert(out.end(), base.begin(), base.end());
    std::uint64_t token = reuse_token_;
    if (token == 0 || !terrain::TerrainStash::replace(token, volume_.chunks(), volume_.voxel_size(), data_path_)) {
        token = terrain::TerrainStash::put(volume_.chunks(), volume_.voxel_size(), data_path_);
    }
    const auto* t = reinterpret_cast<const std::byte*>(&token);
    out.insert(out.end(), t, t + sizeof(token));
}

void Terrain::read_place(const std::byte* data, std::size_t size) {
    const PlaceParts parts = split_place(data, size);
    terrain::ChunkMap chunks;
    // VoxelSize is always 1; the stash carries it for when it may vary.
    float stashed_size = volume_.voxel_size();
    const bool known = parts.token != 0 && terrain::TerrainStash::get(parts.token, chunks, stashed_size);
    // A known token supplies the voxels, so DataPath is only stored; with an
    // unknown one DataPath is written as a load writes it, file read and all.
    restoring_ = known;
    DataModel::read_place(parts.base, parts.base_size);
    restoring_ = false;
    if (known) {
        volume_.set_chunks(std::move(chunks));
    }
}

void Terrain::on_reuse() {
    transform_ = matrix4_identity();
    can_collide_ = true;
    data_path_.clear();
    volume_ = terrain::VoxelVolume{};
    restoring_ = false;
    refreshed_token_ = 0;
    reuse_token_ = 0;
    saved_ = false;
    saved_path_.clear();
    saved_chunks_.clear();
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
    terrain->load_data_path(in.text);
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
