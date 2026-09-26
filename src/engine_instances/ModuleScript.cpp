#include "ModuleScript.hpp"

#include "LuaApi.hpp"

namespace engine_core {

namespace {
constexpr char kModuleScriptSource[] = "local module = {}\n\nreturn module\n";
}

ModuleScript::ModuleScript(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
    : LuaSource(tag, state, id) {
    source_ = kModuleScriptSource;
}

void ModuleScript::on_reuse() {
    LuaSource::on_reuse();
    source_ = kModuleScriptSource;
}

namespace {

ANARCHY_LUA_REGISTER(register_module_script_lua) { register_lua_class("ModuleScript", "LuaSource", nullptr, 0); }

}  // namespace

}  // namespace engine_core
