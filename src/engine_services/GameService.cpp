#include "GameService.hpp"

#include "LuaApi.hpp"

namespace engine_core {

const char* Assets::class_name() const { return "Assets"; }
const char* Materials::class_name() const { return "Materials"; }
const char* Prefabs::class_name() const { return "Prefabs"; }
const char* Meshes::class_name() const { return "Meshes"; }
const char* Textures::class_name() const { return "Textures"; }
const char* Audio::class_name() const { return "Audio"; }
const char* Animations::class_name() const { return "Animations"; }

namespace {

ANARCHY_LUA_REGISTER(register_game_service_lua) {
    register_lua_class("GameService", "Service", nullptr, 0);
    for (const char* name : {"Assets", "Materials", "Prefabs", "Meshes", "Textures", "Audio", "Animations"}) {
        register_lua_class(name, "GameService", nullptr, 0);
    }
    // GetService finds a service directly under game.
    register_lua_service("Assets");
}

}  // namespace

}  // namespace engine_core
