#include "AssetInstances.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "amesh.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace engine_core {

std::optional<std::string> resource_path_error(std::string_view path) {
    const std::string refused = "Path must be relative to the resources folder";
    if (path.empty()) {
        return std::nullopt;
    }
    if (path.front() == '/' || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos) {
        return refused;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if (path.substr(start, end - start) == "..") {
            return refused;
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> FileAsset::set_path(std::string path) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (std::optional<std::string> error = resource_path_error(path)) {
        return error;
    }
    if (path == path_) {
        return std::nullopt;
    }
    LuaSlot before;
    before.kind = LuaSlot::Kind::String;
    before.text = path_;
    path_ = std::move(path);
    LuaSlot after;
    after.kind = LuaSlot::Kind::String;
    after.text = path_;
    note_property_change("Path", before, after);
    return std::nullopt;
}

const char* Texture::class_name() const { return "Texture"; }
const char* Mesh::class_name() const { return "Mesh"; }

std::optional<std::string> Mesh::read_file(const std::filesystem::path& root, const std::string& path,
                                           anarchy::amesh::Data& out) const {
    out = anarchy::amesh::Data{};
    if (root.empty() || path.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path file = root / std::filesystem::u8path(path);
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return std::nullopt;
    }
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    std::vector<std::byte> bytes(error ? 0 : static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (error || size > anarchy::amesh::kMaxFileSize ||
        !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return "Could not read " + path;
    }
    try {
        out = anarchy::amesh::read(bytes);
    } catch (const std::exception& failure) {
        return path + " is not an AMESH file, so it is left as it is: " + failure.what();
    }
    // New triangles would fall past the last LOD's range.
    if (out.lods.size() >= 2) {
        return path + " has LODs, and shapes are added only to a mesh without them";
    }
    return std::nullopt;
}

std::optional<std::string> Mesh::edit_geometry(const std::function<void(anarchy::amesh::Data&)>& edit) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (simulation_running()) {
        // Built on this session's copy, or on the file the first time.
        anarchy::amesh::Data data;
        if (const SessionGeometry current = session_geometry(); current.data) {
            data = *current.data;
        } else if (std::optional<std::string> error = read_file(resources_root(), path(), data)) {
            return error;
        }
        edit(data);
        data.lods.clear();
        if (data.vertices.size() > anarchy::amesh::kMaxVertices || data.indices.size() / 3 > anarchy::amesh::kMaxTriangles) {
            return std::string("The shapes are more than one mesh can hold");
        }
        // Unique across every Mesh, so a renderer can tell any two copies apart.
        static std::atomic<std::uint64_t> next_revision{1};
        session_.data = std::make_shared<const anarchy::amesh::Data>(std::move(data));
        session_.revision = next_revision.fetch_add(1);
        session_generation_ = world_generation();
        return std::nullopt;
    }

    const std::filesystem::path root = resources_root();
    if (root.empty()) {
        return std::string("Open or save a project first: a Mesh's shapes are written to its resources folder");
    }
    // The GUID keeps two Meshes with one Name from sharing a file.
    const std::string path =
        !this->path().empty() ? this->path() : "meshes/" + sanitize_file_name(name(id())) + "." + guid(id()) + ".amesh";
    const std::filesystem::path file = root / std::filesystem::u8path(path);
    anarchy::amesh::Data data;
    if (std::optional<std::string> error = read_file(root, path, data)) {
        return error;
    }
    edit(data);
    // One LOD over every triangle, whatever the edit added.
    data.lods.clear();
    std::vector<std::byte> bytes;
    try {
        bytes = anarchy::amesh::write(data);
    } catch (const std::exception& failure) {
        return std::string("The shapes do not fit in an AMESH file: ") + failure.what();
    }

    // Written beside the file and renamed over it, so a reader never sees half a mesh.
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::filesystem::path partial = file;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            return "Could not write " + path;
        }
    }
    std::filesystem::rename(partial, file, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return "Could not write " + path;
    }
    if (this->path() != path) {
        return set_path(path);
    }
    return std::nullopt;
}

Mesh::SessionGeometry Mesh::session_geometry() const {
    if (session_.data == nullptr || session_generation_ != world_generation() || !simulation_running()) {
        return SessionGeometry{};
    }
    return session_;
}

std::optional<std::string> Mesh::vertex_positions(std::vector<Vec3>& out) const {
    out.clear();
    anarchy::amesh::Data file;
    const anarchy::amesh::Data* data = nullptr;
    if (const SessionGeometry current = session_geometry(); current.data) {
        data = current.data.get();
    } else {
        if (path().empty()) {
            return std::string("the Mesh has no Path");
        }
        // A file with LODs still has its points.
        const std::optional<std::string> error = read_file(resources_root(), path(), file);
        if (error && file.vertices.empty()) {
            return error;
        }
        data = &file;
    }
    out.reserve(data->vertices.size());
    for (const anarchy::amesh::Vertex& vertex : data->vertices) {
        out.push_back(Vec3{vertex.p[0], vertex.p[1], vertex.p[2]});
    }
    if (out.empty()) {
        return path() + " has no vertices";
    }
    return std::nullopt;
}

void Mesh::on_reuse() {
    FileAsset::on_reuse();
    session_ = SessionGeometry{};
    session_generation_ = 0;
}

const char* Sound::class_name() const { return "Sound"; }
const char* Material::class_name() const { return "Material"; }
const char* Model::class_name() const { return "Model"; }
const char* Prefab::class_name() const { return "Prefab"; }

void Prefab::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Edit, true});
    DataModel::context_actions(out);
}

namespace {

constexpr ReferenceSpec kMaterialRefs[] = {
    {"DiffuseTexture", "Texture"},
    {"NormalTexture", "Texture"},
    {"RoughnessTexture", "Texture"},
    {"MetalnessTexture", "Texture"},
};

constexpr ReferenceSpec kModelRefs[] = {
    {"Mesh", "Mesh"},
    {"Material", "Material"},
};

}  // namespace

const ReferenceSpec* Material::reference_specs(std::size_t& count) const {
    count = std::size(kMaterialRefs);
    return kMaterialRefs;
}

const ReferenceSpec* Model::reference_specs(std::size_t& count) const {
    count = std::size(kModelRefs);
    return kModelRefs;
}

LuaSlot ReferenceAsset::reference(std::size_t index) const {
    std::size_t count = 0;
    const ReferenceSpec* specs = reference_specs(count);
    if (index >= count || index >= kMaxReferences) {
        return LuaSlot();
    }
    return instance_reference_slot(refs_[index], specs[index].klass);
}

std::optional<std::string> ReferenceAsset::set_reference(std::size_t index, const LuaSlot& value) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    std::size_t count = 0;
    const ReferenceSpec* specs = reference_specs(count);
    if (index >= count || index >= kMaxReferences) {
        contract_fail("no such reference");
    }
    const ReferenceSpec& spec = specs[index];
    return set_instance_reference(spec.property, spec.klass, refs_[index], value);
}

void ReferenceAsset::on_reuse() {
    for (InstanceRef& ref : refs_) {
        ref.set_guid(std::string());
    }
}

namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot color_slot(ColorRgb color) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Color;
    slot.color = color;
    return slot;
}

}  // namespace

std::optional<std::string> Material::set_number(const char* property, double& slot, double value) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> Material::set_reflectivity(double value) {
    return set_number("Reflectivity", reflectivity_, value);
}

std::optional<std::string> Material::set_transparency(double value) {
    return set_number("Transparency", transparency_, value);
}

std::optional<std::string> Material::set_metalness(double value) {
    return set_number("Metalness", metalness_, value);
}

std::optional<std::string> Material::set_roughness(double value) {
    return set_number("Roughness", roughness_, value);
}

std::optional<std::string> Material::set_color3(const char* property, ColorRgb& slot, ColorRgb color) {
    if (!on_gameplay_thread()) {
        contract_fail("asset setters run on SimulationThread");
    }
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) {
        return std::string(property) + " must be finite";
    }
    // A Color3 has no alpha.
    color.a = 1.f;
    if (same_color(slot, color)) {
        return std::nullopt;
    }
    const ColorRgb previous = slot;
    slot = color;
    note_property_change(property, color_slot(previous), color_slot(color));
    return std::nullopt;
}

std::optional<std::string> Material::set_color(ColorRgb color) { return set_color3("Color", color_, color); }

std::optional<std::string> Material::set_emissive(ColorRgb color) { return set_color3("Emissive", emissive_, color); }

void Material::on_reuse() {
    ReferenceAsset::on_reuse();
    reflectivity_ = kDefaultReflectivity;
    transparency_ = kDefaultTransparency;
    metalness_ = kDefaultMetalness;
    roughness_ = kDefaultRoughness;
    color_ = kDefaultColor;
    emissive_ = kDefaultEmissive;
}

namespace {

bool read_path(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* asset = dynamic_cast<const FileAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = asset->path();
    return true;
}

bool write_path(DataModel&, DataModel& object, LuaSlot& in) {
    auto* asset = dynamic_cast<FileAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = asset->set_path(in.text)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

template <std::size_t Index>
bool read_reference(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* asset = dynamic_cast<const ReferenceAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    out = asset->reference(Index);
    return true;
}

template <std::size_t Index>
bool write_reference(DataModel&, DataModel& object, LuaSlot& in) {
    auto* asset = dynamic_cast<ReferenceAsset*>(&object);
    if (asset == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = asset->set_reference(Index, in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

Material* material_of(DataModel& object) { return dynamic_cast<Material*>(&object); }

template <double (Material::*Get)() const>
bool read_material_number(DataModel&, DataModel& object, LuaSlot& out) {
    const Material* material = material_of(object);
    if (material == nullptr) {
        return false;
    }
    out = number_slot((material->*Get)());
    return true;
}

template <std::optional<std::string> (Material::*Set)(double)>
bool write_material_number(DataModel&, DataModel& object, LuaSlot& in) {
    Material* material = material_of(object);
    if (material == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = (material->*Set)(in.number)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

template <ColorRgb (Material::*Get)() const>
bool read_material_color(DataModel&, DataModel& object, LuaSlot& out) {
    const Material* material = material_of(object);
    if (material == nullptr) {
        return false;
    }
    out = color_slot((material->*Get)());
    return true;
}

template <std::optional<std::string> (Material::*Set)(ColorRgb)>
bool write_material_color(DataModel&, DataModel& object, LuaSlot& in) {
    Material* material = material_of(object);
    if (material == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = (material->*Set)(in.color)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

std::string color_json(ColorRgb color) {
    const float channels[3] = {color.r, color.g, color.b};
    return write_json(json_floats(channels, 3));
}

ANARCHY_LUA_REGISTER(register_asset_instances_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string reflectivity = write_json(JsonValue::number(Material::kDefaultReflectivity));
    static const std::string transparency = write_json(JsonValue::number(Material::kDefaultTransparency));
    static const std::string metalness = write_json(JsonValue::number(Material::kDefaultMetalness));
    static const std::string roughness = write_json(JsonValue::number(Material::kDefaultRoughness));
    static const std::string color = color_json(Material::kDefaultColor);
    static const std::string emissive = color_json(Material::kDefaultEmissive);
    const LuaField file_fields[] = {
        lua_saved_property("Path", "string", read_path, write_path, "\"\""),
    };
    register_lua_class("FileAsset", "Instance", nullptr, 0);
    register_lua_class("Texture", "FileAsset", file_fields, 1);
    register_lua_class("Mesh", "FileAsset", file_fields, 1);
    register_lua_class("Sound", "FileAsset", file_fields, 1);
    register_lua_class("ReferenceAsset", "Instance", nullptr, 0);
    const LuaField material_fields[] = {
        lua_saved_property("DiffuseTexture", "Texture?", read_reference<0>, write_reference<0>, "null"),
        lua_saved_property("NormalTexture", "Texture?", read_reference<1>, write_reference<1>, "null"),
        lua_saved_property("RoughnessTexture", "Texture?", read_reference<2>, write_reference<2>, "null"),
        lua_saved_property("MetalnessTexture", "Texture?", read_reference<3>, write_reference<3>, "null"),
        lua_saved_property("Color", "Color3", read_material_color<&Material::color>,
                           write_material_color<&Material::set_color>, color.c_str()),
        lua_saved_property("Emissive", "Color3", read_material_color<&Material::emissive>,
                           write_material_color<&Material::set_emissive>, emissive.c_str()),
        lua_slider(lua_saved_property("Metalness", "number", read_material_number<&Material::metalness>,
                                      write_material_number<&Material::set_metalness>, metalness.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("Roughness", "number", read_material_number<&Material::roughness>,
                                      write_material_number<&Material::set_roughness>, roughness.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("Reflectivity", "number", read_material_number<&Material::reflectivity>,
                                      write_material_number<&Material::set_reflectivity>, reflectivity.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("Transparency", "number", read_material_number<&Material::transparency>,
                                      write_material_number<&Material::set_transparency>, transparency.c_str()),
                   0.0, 1.0),
    };
    register_lua_class("Material", "ReferenceAsset", material_fields,
                       static_cast<int>(sizeof(material_fields) / sizeof(material_fields[0])));
    const LuaField model_fields[] = {
        lua_saved_property("Mesh", "Mesh?", read_reference<0>, write_reference<0>, "null"),
        lua_saved_property("Material", "Material?", read_reference<1>, write_reference<1>, "null"),
    };
    register_lua_class("Model", "ReferenceAsset", model_fields, 2);
    register_lua_class("Prefab", "Instance", nullptr, 0);
}

}  // namespace

}  // namespace engine_core
