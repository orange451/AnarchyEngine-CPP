#include "AssetInstances.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"

#include <algorithm>
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
const char* Sound::class_name() const { return "Sound"; }
const char* Material::class_name() const { return "Material"; }
const char* Model::class_name() const { return "Model"; }
const char* Prefab::class_name() const { return "Prefab"; }

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
    LuaSlot slot;
    if (index >= kMaxReferences) {
        return slot;
    }
    slot.text = refs_[index].guid();
    slot.id = refs_[index].resolve(*this);
    slot.kind = slot.id != 0 ? LuaSlot::Kind::Instance : LuaSlot::Kind::Nil;
    return slot;
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
    const std::string refused = std::string(spec.property) + " must be a " + spec.klass;
    std::string guid;
    if (value.kind == LuaSlot::Kind::Instance && value.id != 0 && alive(value.id)) {
        const DataModel* target = instance(value.id);
        if (target == nullptr || !lua_class_inherits(target->class_name(), spec.klass)) {
            return refused;
        }
        guid = this->guid(value.id);
    } else if (value.kind == LuaSlot::Kind::Nil) {
        guid = value.text;
    } else if (value.kind == LuaSlot::Kind::Instance && !value.text.empty()) {
        guid = value.text;
    } else if (value.kind == LuaSlot::Kind::Instance) {
        return std::string("That instance no longer exists");
    } else {
        return refused;
    }
    if (guid == refs_[index].guid()) {
        return std::nullopt;
    }
    const LuaSlot before = reference(index);
    refs_[index].set_guid(std::move(guid));
    note_property_change(spec.property, before, reference(index));
    return std::nullopt;
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
