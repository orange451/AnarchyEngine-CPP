#include "SceneService.hpp"

#include "LuaApi.hpp"

#include <cctype>

namespace engine_core {

void SceneService::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Paste, false});
}

bool is_scene_service_class(std::string_view class_name) {
    for (const char* name : kSceneServiceClasses) {
        if (class_name == name) {
            return true;
        }
    }
    return false;
}

std::string scene_service_guid(std::string_view class_name) {
    std::string guid(class_name);
    for (char& c : guid) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return guid;
}

const char* Workspace::class_name() const { return "Workspace"; }

const char* Storage::class_name() const { return "Storage"; }

const char* Scripts::class_name() const { return "Scripts"; }

namespace {

ANARCHY_LUA_REGISTER(register_scene_service_lua) {
    register_lua_class("SceneService", "DataModel", nullptr, 0);
    register_lua_class("Workspace", "SceneService", nullptr, 0);
    register_lua_class("Storage", "SceneService", nullptr, 0);
    register_lua_class("Scripts", "SceneService", nullptr, 0);
    // So completion offers them to GetService.
    for (const char* name : kSceneServiceClasses) {
        register_lua_service(name);
    }
}

}  // namespace

}  // namespace engine_core
