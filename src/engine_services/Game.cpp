#include "Game.hpp"

#include "ChangeHistoryService.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "SceneService.hpp"

namespace engine_core {
namespace {

const char* const kClassName = "Game";

template <typename T>
void add_scene_service(Game& game) {
    T& service = game.create<T>();
    game.set_guid(service.id(), scene_service_guid(service.class_name()));
    game.set_parent(service.id(), game.id());
}

}  // namespace

Game::Game() : DataModel(kClassName) {
    // A new place starts with them; there is nothing to undo.
    ChangeHistoryService& changes = history();
    const bool enabled = changes.enabled();
    changes.set_enabled(false);
    add_scene_service<Workspace>(*this);
    add_scene_service<Lighting>(*this);
    add_scene_service<Storage>(*this);
    add_scene_service<Scripts>(*this);
    changes.set_enabled(enabled);
}

const char* Game::class_name() const { return kClassName; }

namespace {

// ScriptRuntime adds GetService, since that call needs the script VM.
ANARCHY_LUA_REGISTER(register_game_lua) { register_lua_class(kClassName, "DataModel", nullptr, 0); }

}  // namespace

}  // namespace engine_core
