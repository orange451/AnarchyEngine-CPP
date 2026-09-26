#include "Game.hpp"

#include "LuaApi.hpp"

namespace engine_core {
namespace {

const char* const kClassName = "Game";

}  // namespace

Game::Game() : DataModel(kClassName) {}

const char* Game::class_name() const { return kClassName; }

namespace {

// ScriptRuntime adds GetService, since that call needs the script VM.
ANARCHY_LUA_REGISTER(register_game_lua) { register_lua_class(kClassName, "DataModel", nullptr, 0); }

}  // namespace

}  // namespace engine_core
