#include "Folder.hpp"

#include "LuaApi.hpp"

namespace engine_core {

const char* Folder::class_name() const { return "Folder"; }

namespace {

ANARCHY_LUA_REGISTER(register_folder_lua) { register_lua_class("Folder", "Instance", nullptr, 0); }

}  // namespace

}  // namespace engine_core
