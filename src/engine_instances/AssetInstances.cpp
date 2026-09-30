#include "AssetInstances.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "amesh.hpp"

#include <algorithm>
#include <atomic>
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

ANARCHY_LUA_REGISTER(register_asset_instances_lua) {
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
    };
    register_lua_class("Material", "ReferenceAsset", material_fields, 4);
    const LuaField model_fields[] = {
        lua_saved_property("Mesh", "Mesh?", read_reference<0>, write_reference<0>, "null"),
        lua_saved_property("Material", "Material?", read_reference<1>, write_reference<1>, "null"),
    };
    register_lua_class("Model", "ReferenceAsset", model_fields, 2);
    register_lua_class("Prefab", "Instance", nullptr, 0);
}

}  // namespace

}  // namespace engine_core
