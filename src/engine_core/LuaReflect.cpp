#include "LuaApi.hpp"
#include "LuauSandbox.hpp"

#include "lualib.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine_core {
namespace {

void* reflect_alloc(void*, void* pointer, std::size_t, std::size_t size) {
    if (size == 0) {
        std::free(pointer);
        return nullptr;
    }
    return std::realloc(pointer, size);
}

struct CloseState {
    void operator()(lua_State* state) const { lua_close(state); }
};

// Reflection keeps one state per thread, so completion on one thread and
// analysis or tests on another never share a lua_State.
lua_State* reflect_state() {
    thread_local std::unique_ptr<lua_State, CloseState> state;
    if (!state) {
        state.reset(lua_newstate(reflect_alloc, nullptr));
        if (state) {
            open_host_libraries(state.get());
        }
    }
    return state.get();
}

std::string lua_type_name(lua_State* state, int index) {
    switch (lua_type(state, index)) {
    case LUA_TFUNCTION:
        return "function";
    case LUA_TTABLE:
        return "table";
    case LUA_TNUMBER:
        return "number";
    case LUA_TSTRING:
        return "string";
    case LUA_TBOOLEAN:
        return "boolean";
    case LUA_TNIL:
        return "nil";
    default:
        break;
    }
    const char* name = luaL_typename(state, index);
    return name != nullptr ? name : "value";
}

void walk_table(lua_State* state, int index, std::vector<LuaSymbol>& out, bool methods) {
    const int table = lua_absindex(state, index);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(state, -2);
            if (key != nullptr && key[0] != '\0') {
                LuaSymbol symbol;
                symbol.name = key;
                symbol.type_name = lua_type_name(state, -1);
                symbol.call = symbol.type_name == "function";
                symbol.method = methods && symbol.call;
                out.push_back(std::move(symbol));
            }
        }
        lua_pop(state, 1);
    }
}

void sort_symbols(std::vector<LuaSymbol>& out) {
    std::sort(out.begin(), out.end(), [](const LuaSymbol& a, const LuaSymbol& b) { return a.name < b.name; });
}

}  // namespace

void lua_library_globals(std::vector<LuaSymbol>& out) {
    out.clear();
    lua_State* state = reflect_state();
    if (state == nullptr) {
        return;
    }
    walk_table(state, LUA_GLOBALSINDEX, out, false);
    sort_symbols(out);
}

bool lua_library_members(std::string_view global_name, std::vector<LuaSymbol>& out) {
    out.clear();
    lua_State* state = reflect_state();
    if (state == nullptr) {
        return false;
    }
    // A dotted name is a table inside a library, such as Enum.KeyCode.
    const std::size_t dot = global_name.find('.');
    const std::string_view head = global_name.substr(0, dot);
    lua_pushlstring(state, head.data(), head.size());
    lua_rawget(state, LUA_GLOBALSINDEX);
    std::size_t start = dot;
    while (start != std::string_view::npos && lua_istable(state, -1)) {
        const std::size_t next = global_name.find('.', start + 1);
        const std::string_view part =
            global_name.substr(start + 1, next == std::string_view::npos ? std::string_view::npos : next - start - 1);
        lua_pushlstring(state, part.data(), part.size());
        lua_rawget(state, -2);
        lua_remove(state, -2);
        start = next;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    walk_table(state, -1, out, false);
    lua_pop(state, 1);
    sort_symbols(out);
    return true;
}

bool lua_value_members(std::string_view value_type, std::vector<LuaSymbol>& out) {
    out.clear();
    lua_State* state = reflect_state();
    if (state == nullptr) {
        return false;
    }
    if (value_type == "string") {
        lua_pushliteral(state, "");
        if (!lua_getmetatable(state, -1)) {
            lua_pop(state, 1);
            return false;
        }
        lua_getfield(state, -1, "__index");
        if (!lua_istable(state, -1)) {
            lua_pop(state, 3);
            return false;
        }
        walk_table(state, -1, out, true);
        lua_pop(state, 3);
        sort_symbols(out);
        return true;
    }
    if (value_type == "vector") {
        lua_pushvector(state, 0.0f, 0.0f, 0.0f);
        if (!lua_getmetatable(state, -1)) {
            lua_pop(state, 1);
            return false;
        }
        lua_getfield(state, -1, "__index");
        if (!lua_isfunction(state, -1)) {
            lua_pop(state, 3);
            return false;
        }
        const char* names[] = {"x", "y", "z", "w"};
        for (const char* name : names) {
            lua_pushvalue(state, -1);
            lua_pushvalue(state, -4);
            lua_pushstring(state, name);
            if (lua_pcall(state, 2, 1, 0) == 0) {
                LuaSymbol symbol;
                symbol.name = name;
                symbol.type_name = "number";
                out.push_back(std::move(symbol));
                lua_pop(state, 1);
            } else {
                lua_pop(state, 1);
            }
        }
        lua_pop(state, 3);
        return !out.empty();
    }
    if (lua_class_known(std::string(value_type).c_str())) {
        std::vector<LuaField> fields;
        lua_class_members(std::string(value_type).c_str(), fields);
        for (const LuaField& field : fields) {
            if (field.name == nullptr) {
                continue;
            }
            LuaSymbol symbol;
            symbol.name = field.name;
            symbol.type_name = field.type_name != nullptr ? field.type_name : "";
            symbol.call = field.method;
            symbol.method = field.method;
            out.push_back(std::move(symbol));
        }
        return true;
    }
    return false;
}

}  // namespace engine_core
