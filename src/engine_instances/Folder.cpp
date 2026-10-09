#include "Folder.hpp"

#include "LuaApi.hpp"

namespace engine_core {

const char* Folder::class_name() const { return "Folder"; }

void Folder::context_actions(std::vector<ContextAction>& out) const {
    DataModel::context_actions(out);
    out.push_back(ContextAction{InstanceAction::SaveAsPlugin, false});
}

namespace {

ANARCHY_LUA_REGISTER(register_folder_lua) { register_lua_class("Folder", "Instance", nullptr, 0); }

}  // namespace

}  // namespace engine_core
