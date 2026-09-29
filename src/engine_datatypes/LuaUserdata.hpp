#pragma once

#include "lua.h"
#include "lualib.h"

// Userdata whose type is the metatable registered under a name, as
// luaL_newmetatable makes.
namespace engine_core {

// The userdata at index when its metatable is the one registered as meta, else null.
inline void* test_userdata(lua_State* state, int index, const char* meta) {
    void* data = lua_touserdata(state, index);
    if (data == nullptr || !lua_getmetatable(state, index)) {
        return nullptr;
    }
    luaL_getmetatable(state, meta);
    const bool match = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return match ? data : nullptr;
}

// A new userdata holding a copy of value, with the metatable registered as meta.
template <typename T>
void push_userdata(lua_State* state, const T& value, const char* meta) {
    auto* data = static_cast<T*>(lua_newuserdata(state, sizeof(T)));
    *data = value;
    luaL_getmetatable(state, meta);
    lua_setmetatable(state, -2);
}

// The value at index, or a type error that names type_name.
template <typename T>
const T& check_userdata(lua_State* state, int index, const char* meta, const char* type_name) {
    const auto* value = static_cast<const T*>(test_userdata(state, index, meta));
    if (value == nullptr) {
        luaL_typeerrorL(state, index, type_name);
    }
    return *value;
}

}  // namespace engine_core
