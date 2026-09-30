#include "AssetInstances.hpp"

#include "LuaApi.hpp"

namespace engine_core {

const char* Texture::class_name() const { return "Texture"; }
const char* Mesh::class_name() const { return "Mesh"; }
const char* Sound::class_name() const { return "Sound"; }
const char* Material::class_name() const { return "Material"; }
const char* Model::class_name() const { return "Model"; }
const char* Prefab::class_name() const { return "Prefab"; }

namespace {

ANARCHY_LUA_REGISTER(register_asset_instances_lua) {
    register_lua_class("FileAsset", "Instance", nullptr, 0);
    register_lua_class("Texture", "FileAsset", nullptr, 0);
    register_lua_class("Mesh", "FileAsset", nullptr, 0);
    register_lua_class("Sound", "FileAsset", nullptr, 0);
    register_lua_class("ReferenceAsset", "Instance", nullptr, 0);
    register_lua_class("Material", "ReferenceAsset", nullptr, 0);
    register_lua_class("Model", "ReferenceAsset", nullptr, 0);
    register_lua_class("Prefab", "Instance", nullptr, 0);
}

}  // namespace

}  // namespace engine_core
