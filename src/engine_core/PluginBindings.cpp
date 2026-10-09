// The plugin global a plugin VM thread sees, and what it makes: toolbars and
// their buttons. Each is a handle on ScriptRuntime's PluginUi, by id, so a
// handle whose plugin has unloaded fails cleanly instead of dangling.

#include "ScriptBindings.hpp"

#include <cstring>
#include <string>

namespace engine_core {

using namespace script_internal;

namespace {

int plugin_newindex(lua_State* state) {
    luaL_checkudata(state, 1, kPluginMeta);
    luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2));
}

int plugin_tostring(lua_State* state) {
    luaL_checkudata(state, 1, kPluginMeta);
    lua_pushstring(state, "Plugin");
    return 1;
}

// A metatable whose __index and __newindex are given, read-only, named type_name for tostring.
void install(lua_State* state, const char* meta, lua_CFunction index, lua_CFunction newindex,
             lua_CFunction tostring) {
    luaL_newmetatable(state, meta);
    lua_pushcfunction(state, index, "index");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, newindex, "newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushstring(state, "The metatable is locked");
    lua_setfield(state, -2, "__metatable");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
}

}  // namespace

void push_plugin_signal(lua_State* state, std::uint32_t key, const char* cause) {
    auto* ud = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
    *ud = SignalUd{};
    ud->kind = kSignalPlugin;
    ud->id = key;
    ud->event_name = cause;
    luaL_getmetatable(state, kSignalMeta);
    lua_setmetatable(state, -2);
}

int ScriptBindings::plugin_index(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginUd*>(luaL_checkudata(state, 1, kPluginMeta));
        const char* key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        const std::string* name = runtime != nullptr ? runtime->plugin_ui().plugin_name(ud->serial) : nullptr;
        if (name == nullptr) {
            luaL_error(state, "the plugin has unloaded");
        }
        if (std::strcmp(key, "Name") == 0) {
            lua_pushstring(state, name->c_str());
            return 1;
        }
        if (std::strcmp(key, "Unloading") == 0) {
            push_plugin_signal(state, runtime->plugin_ui().unloading_key(ud->serial), "Unloading");
            return 1;
        }
        luaL_error(state, "%s is not a valid member of Plugin", key);
    });
}

void open_plugin_api(lua_State* state) {
    install(state, kPluginMeta, &ScriptBindings::plugin_index, plugin_newindex, plugin_tostring);
    // One userdata per plugin, so plugin == plugin holds across its threads.
    lua_newtable(state);
    lua_newtable(state);
    lua_pushstring(state, "v");
    lua_setfield(state, -2, "__mode");
    lua_setreadonly(state, -1, 1);
    lua_setmetatable(state, -2);
    lua_setfield(state, LUA_REGISTRYINDEX, kPluginCache);
}

void ScriptRuntime::set_plugin_global(lua_State* co, std::uint32_t serial) {
    if (plugin_ui_.plugin_name(serial) == nullptr) {
        lua_pushnil(co);
        lua_setglobal(co, "plugin");
        return;
    }
    lua_getfield(co, LUA_REGISTRYINDEX, kPluginCache);
    lua_pushnumber(co, static_cast<double>(serial));
    lua_rawget(co, -2);
    if (test_userdata(co, -1, kPluginMeta) == nullptr) {
        lua_pop(co, 1);
        auto* ud = static_cast<PluginUd*>(lua_newuserdata(co, sizeof(PluginUd)));
        ud->serial = serial;
        luaL_getmetatable(co, kPluginMeta);
        lua_setmetatable(co, -2);
        lua_pushnumber(co, static_cast<double>(serial));
        lua_pushvalue(co, -2);
        lua_rawset(co, -4);
    }
    lua_remove(co, -2);
    lua_setglobal(co, "plugin");
}

}  // namespace engine_core
