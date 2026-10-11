#include "Game.hpp"

#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "GameService.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "SceneService.hpp"

namespace engine_core {
namespace {

const char* const kClassName = "Game";

// Under its table parent, with its fixed GUID.
template <typename T>
void add_service(Game& game) {
    T& service = game.create<T>();
    const ServiceSpec* spec = find_service(service.class_name());
    if (spec == nullptr) {
        contract_fail("a service class is missing from kServices");
    }
    game.set_guid(service.id(), service_guid(service.class_name()));
    game.set_parent(service.id(), spec->parent_class == nullptr ? 0 : game.service(spec->parent_class));
}

}  // namespace

Game::Game() : DataModel(kClassName) {
    // A new place starts with them; there is nothing to undo.
    ChangeHistoryService& changes = history();
    const bool enabled = changes.enabled();
    changes.set_enabled(false);
    // kServices order, parents first.
    add_service<Workspace>(*this);
    add_service<Lighting>(*this);
    add_service<Storage>(*this);
    add_service<Scripts>(*this);
    add_service<GuiService>(*this);
    add_service<Assets>(*this);
    add_service<Materials>(*this);
    add_service<Prefabs>(*this);
    add_service<Meshes>(*this);
    add_service<Textures>(*this);
    add_service<Audio>(*this);
    add_service<Animations>(*this);
    // Not one of the place's services, so not in kServices: it holds the studio's own tools.
    Core& core = create<Core>();
    set_guid(core.id(), service_guid(kCoreClass));
    set_parent(core.id(), 0);
    changes.set_enabled(enabled);
}

const char* Game::class_name() const { return kClassName; }

namespace {

// ScriptRuntime adds GetService, since that call needs the script VM.
ANARCHY_LUA_REGISTER(register_game_lua) { register_lua_class(kClassName, "DataModel", nullptr, 0); }

}  // namespace

}  // namespace engine_core
