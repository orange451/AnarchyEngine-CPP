#pragma once

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string_view>

// What ScriptRuntime, LuaEngine, and LuaReflect share when they open a Luau
// state or compile a chunk. Each keeps its own interrupt and execution budget.
namespace engine_core {

// Opens base, coroutines, tables, strings, math, utf8, bit32, buffer, and
// vector, then installs the host's print. debug and os stay closed: debug can
// see through the sandbox, and os reads the host clock. The stock print writes
// to stdout; print here can only reach the host.
inline void open_sandbox_libraries(lua_State* state, lua_CFunction print) {
    static const luaL_Reg kLibraries[] = {
        {"", luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_BITLIBNAME, luaopen_bit32},
        {LUA_BUFFERLIBNAME, luaopen_buffer},
        {LUA_VECLIBNAME, luaopen_vector},
        {nullptr, nullptr},
    };
    // Luau already omits most of these. Clearing the names means a later library open cannot put them back.
    static const char* const kRemoved[] = {
        "getfenv", "setfenv", "loadstring", "dofile", "loadfile", "load", "require", "collectgarbage", "module",
        "debug",   "os",      "io",         "package", nullptr,
    };
    for (const luaL_Reg* library = kLibraries; library->func != nullptr; ++library) {
        lua_pushcfunction(state, library->func, nullptr);
        lua_pushstring(state, library->name);
        lua_call(state, 1, 0);
    }
    lua_pushcfunction(state, print, "print");
    lua_setglobal(state, "print");
    for (const char* const* name = kRemoved; *name != nullptr; ++name) {
        lua_pushnil(state);
        lua_setglobal(state, *name);
    }
}

// Bytecode for luau_load. A syntax error still compiles, to bytecode that
// raises the error when loaded, so an empty result means compiling failed.
struct Bytecode {
    std::unique_ptr<char, void (*)(void*)> data{nullptr, std::free};
    std::size_t size = 0;

    explicit operator bool() const { return data != nullptr && size != 0; }
};

// Compiled with optimization level 1 and line information for error messages.
inline Bytecode compile_luau(std::string_view source) {
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    Bytecode out;
    out.data.reset(luau_compile(source.data() != nullptr ? source.data() : "", source.size(), &options, &out.size));
    return out;
}

// One Luau allocator call under a byte budget. used is what the state holds.
// A request that would pass limit fails before the OS allocates, so a script
// cannot grow the process past it.
inline void* budget_realloc(std::size_t& used, std::size_t limit, void* pointer, std::size_t old_size,
                            std::size_t new_size) {
    if (pointer == nullptr) {
        old_size = 0;
    }
    if (new_size == 0) {
        if (old_size <= used) {
            used -= old_size;
        } else {
            used = 0;
        }
        std::free(pointer);
        return nullptr;
    }
    if (old_size > used) {
        return nullptr;
    }
    const std::size_t retained = used - old_size;
    if (retained > limit || new_size > limit - retained) {
        return nullptr;
    }
    void* block = std::realloc(pointer, new_size);
    if (block == nullptr) {
        return nullptr;
    }
    used = retained + new_size;
    return block;
}

}  // namespace engine_core
