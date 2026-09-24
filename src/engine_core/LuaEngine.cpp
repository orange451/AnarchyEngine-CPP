#include "LuaEngine.hpp"

#include "lualib.h"
#include "luacode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>

namespace engine_core {
namespace {

bool isIdentifier(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    const unsigned char head = static_cast<unsigned char>(name[0]);
    const bool headOk = (head >= 'A' && head <= 'Z') || (head >= 'a' && head <= 'z') || head == '_';
    if (!headOk) {
        return false;
    }
    for (char character : name) {
        const unsigned char value = static_cast<unsigned char>(character);
        const bool letter = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
        const bool digit = value >= '0' && value <= '9';
        if (!(letter || digit || value == '_')) {
            return false;
        }
    }
    return true;
}

std::string formatNumber(double value) {
    if (std::isfinite(value)) {
        const double truncated = std::trunc(value);
        if (value == truncated && value >= -1.0e15 && value <= 1.0e15) {
            return std::to_string(static_cast<long long>(truncated));
        }
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.14g", value);
    return buffer;
}

// Cleared on the way out, including when luaL_error throws, so the next safepoint can stop again.
struct InterruptGuard {
    bool& flag;
    explicit InterruptGuard(bool& flag) : flag(flag) { flag = true; }
    ~InterruptGuard() { flag = false; }
};

struct RunningGuard {
    bool& flag;
    explicit RunningGuard(bool& flag) : flag(flag) { flag = true; }
    ~RunningGuard() { flag = false; }
};

struct PopThread {
    lua_State* state;
    ~PopThread() { lua_pop(state, 1); }
};

// Base language, tables, strings, math, coroutines, utf8, bit32, buffer, and vector.
// debug and os stay closed: debug can see through the sandbox, and os reads the host clock.
const luaL_Reg kLibraries[] = {
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
const char* kRemoved[] = {
    "getfenv", "setfenv", "loadstring", "dofile", "loadfile", "load", "require", "collectgarbage", "module",
    "debug",   "os",      "io",         "package", nullptr,
};

}  // namespace

HostArgs::HostArgs(lua_State* state) : state_(state), argumentCount_(lua_gettop(state)) {}

bool HostArgs::isNil(int index) const {
    return lua_type(state_, index) == LUA_TNIL;
}

bool HostArgs::isBoolean(int index) const {
    return lua_type(state_, index) == LUA_TBOOLEAN;
}

bool HostArgs::isNumber(int index) const {
    const int type = lua_type(state_, index);
    return type == LUA_TNUMBER || type == LUA_TINTEGER;
}

bool HostArgs::isString(int index) const {
    return lua_type(state_, index) == LUA_TSTRING;
}

bool HostArgs::boolean(int index) const {
    if (!isBoolean(index)) {
        luaL_typeerror(state_, index, "boolean");
    }
    return lua_toboolean(state_, index) != 0;
}

double HostArgs::number(int index) const {
    if (!isNumber(index)) {
        luaL_typeerror(state_, index, "number");
    }
    if (lua_type(state_, index) == LUA_TINTEGER) {
        return static_cast<double>(lua_tointeger64(state_, index, nullptr));
    }
    return lua_tonumber(state_, index);
}

std::string_view HostArgs::string(int index) const {
    if (!isString(index)) {
        luaL_typeerror(state_, index, "string");
    }
    size_t length = 0;
    const char* text = lua_tolstring(state_, index, &length);
    return std::string_view(text != nullptr ? text : "", length);
}

void HostArgs::pushNil() {
    lua_pushnil(state_);
    ++results_;
}

void HostArgs::pushBoolean(bool value) {
    lua_pushboolean(state_, value ? 1 : 0);
    ++results_;
}

void HostArgs::pushNumber(double value) {
    lua_pushnumber(state_, value);
    ++results_;
}

void HostArgs::pushString(std::string_view value) {
    const char* text = value.data() != nullptr ? value.data() : "";
    lua_pushlstring(state_, text, value.size());
    ++results_;
}

void HostArgs::error(std::string_view message) {
    const char* text = message.data() != nullptr ? message.data() : "";
    lua_pushlstring(state_, text, message.size());
    lua_error(state_);
}

LuaEngine::~LuaEngine() {
    if (state_ != nullptr && !executing_) {
        lua_State* state = state_;
        state_ = nullptr;
        lua_close(state);
    }
}

void LuaEngine::bind(std::string name, HostFunction function) {
    if (state_ != nullptr) {
        throw std::logic_error("host functions must be bound before the Lua engine starts");
    }
    if (!isIdentifier(name)) {
        throw std::invalid_argument("host function name must be a Lua identifier");
    }
    if (!function) {
        throw std::invalid_argument("host function is empty");
    }
    for (const Binding& binding : bindings_) {
        if (binding.name == name) {
            throw std::invalid_argument("host function '" + name + "' is already bound");
        }
    }
    bindings_.push_back(Binding{std::move(name), std::move(function)});
}

void LuaEngine::setPrintHandler(PrintHandler handler) {
    printHandler_ = std::move(handler);
}

void LuaEngine::setExecutionBudget(std::uint64_t interrupts) {
    if (interrupts == 0) {
        throw std::invalid_argument("execution budget must be positive");
    }
    executionBudget_ = interrupts;
}

void LuaEngine::setMemoryLimit(std::size_t bytes) {
    if (bytes == 0) {
        throw std::invalid_argument("memory limit must be positive");
    }
    memoryLimit_ = bytes;
}

void LuaEngine::start() {
    if (state_ != nullptr) {
        throw std::logic_error("Lua engine already started");
    }

    lua_State* state = lua_newstate(&LuaEngine::allocate, this);
    if (state == nullptr) {
        throw std::runtime_error("could not create the Luau state");
    }

    try {
        lua_Callbacks* callbacks = lua_callbacks(state);
        callbacks->userdata = this;
        callbacks->interrupt = &LuaEngine::interrupt;
        callbacks->panic = &LuaEngine::panic;

        for (const luaL_Reg* library = kLibraries; library->func != nullptr; ++library) {
            lua_pushcfunction(state, library->func, nullptr);
            lua_pushstring(state, library->name);
            lua_call(state, 1, 0);
        }

        // The stock print writes to stdout. This one can only reach the handler we installed.
        lua_pushcfunction(state, &LuaEngine::print, "print");
        lua_setglobal(state, "print");

        for (const char* const* name = kRemoved; *name != nullptr; ++name) {
            lua_pushnil(state);
            lua_setglobal(state, *name);
        }

        for (std::size_t index = 0; index < bindings_.size(); ++index) {
            const Binding& binding = bindings_[index];
            lua_getglobal(state, binding.name.c_str());
            const bool exists = !lua_isnil(state, -1);
            lua_pop(state, 1);
            if (exists) {
                throw std::invalid_argument("host function replaces Lua global '" + binding.name + "'");
            }
            lua_pushinteger(state, static_cast<int>(index));
            lua_pushcclosure(state, &LuaEngine::dispatchHost, binding.name.c_str(), 1);
            lua_setglobal(state, binding.name.c_str());
        }

        // Freezes every library table, the string metatable, and the shared globals.
        luaL_sandbox(state);
        state_ = state;
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        lua_close(state);
        throw std::runtime_error(message);
    } catch (...) {
        lua_close(state);
        throw;
    }
}

void LuaEngine::stop() {
    if (state_ == nullptr) {
        return;
    }
    if (executing_) {
        throw std::logic_error("cannot stop the Lua engine while a script is running");
    }
    lua_State* state = state_;
    state_ = nullptr;
    lua_close(state);
}

LuaEngine::ScriptResult LuaEngine::execute(std::string_view chunkName, std::string_view source) {
    ScriptResult result;
    if (state_ == nullptr) {
        result.error = "Lua engine is not running";
        return result;
    }
    if (executing_) {
        result.error = "a script is already running";
        return result;
    }

    RunningGuard running(executing_);
    steps_ = 0;

    const std::string chunk = chunkName.empty() ? std::string("=script") : std::string(chunkName);
    const char* text = source.data() != nullptr ? source.data() : "";
    const std::size_t length = source.size();

    lua_CompileOptions options = {};
    options.optimizationLevel = 1;
    options.debugLevel = 1;

    std::size_t bytecodeSize = 0;
    std::unique_ptr<char, void (*)(void*)> bytecode(luau_compile(text, length, &options, &bytecodeSize), std::free);
    if (bytecode == nullptr || bytecodeSize == 0) {
        result.error = "could not compile script";
        return result;
    }

    // The child thread keeps the sealed state untouched. Its globals can read the sealed
    // environment and cannot write it. User code never runs on the main state.
    lua_State* script = lua_newthread(state_);
    if (script == nullptr) {
        result.error = "could not create a script thread";
        return result;
    }
    PopThread pop{state_};
    luaL_sandboxthread(script);

    const int loaded = luau_load(script, chunk.c_str(), bytecode.get(), bytecodeSize, 0);
    if (loaded != 0) {
        result.error = stackText(script, -1);
        return result;
    }

    const int status = lua_pcall(script, 0, LUA_MULTRET, 0);
    if (status == LUA_YIELD) {
        result.error = "script yielded";
        return result;
    }
    if (status != LUA_OK) {
        result.error = stackText(script, -1);
        return result;
    }

    const int count = lua_gettop(script);
    result.values.reserve(static_cast<std::size_t>(count));
    for (int index = 1; index <= count; ++index) {
        result.values.push_back(stackText(script, index));
    }
    result.ok = true;
    return result;
}

std::string LuaEngine::stackText(lua_State* state, int index) const {
    if (lua_gettop(state) == 0 || lua_isnone(state, index)) {
        return "script error";
    }
    const int type = lua_type(state, index);
    std::string text;
    if (type == LUA_TNIL) {
        text = "nil";
    } else if (type == LUA_TBOOLEAN) {
        text = lua_toboolean(state, index) != 0 ? "true" : "false";
    } else if (type == LUA_TINTEGER) {
        text = std::to_string(lua_tointeger64(state, index, nullptr));
    } else if (type == LUA_TNUMBER) {
        text = formatNumber(lua_tonumber(state, index));
    } else if (type == LUA_TSTRING) {
        std::size_t length = 0;
        const char* bytes = lua_tolstring(state, index, &length);
        text.assign(bytes != nullptr ? bytes : "", length);
    } else {
        const char* name = lua_typename(state, type);
        text = name != nullptr ? name : "error";
    }
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    if (text.empty()) {
        return "script error";
    }
    return text;
}

void* LuaEngine::allocate(void* userdata, void* pointer, std::size_t oldSize, std::size_t newSize) {
    auto* engine = static_cast<LuaEngine*>(userdata);
    if (pointer == nullptr) {
        oldSize = 0;
    }
    if (newSize == 0) {
        if (oldSize <= engine->memoryUsed_) {
            engine->memoryUsed_ -= oldSize;
        } else {
            engine->memoryUsed_ = 0;
        }
        std::free(pointer);
        return nullptr;
    }
    if (oldSize > engine->memoryUsed_) {
        return nullptr;
    }
    // Refuse before the OS allocates, so a script cannot grow the process past the limit.
    const std::size_t retained = engine->memoryUsed_ - oldSize;
    if (retained > engine->memoryLimit_ || newSize > engine->memoryLimit_ - retained) {
        return nullptr;
    }
    void* block = std::realloc(pointer, newSize);
    if (block == nullptr) {
        return nullptr;
    }
    engine->memoryUsed_ = retained + newSize;
    return block;
}

void LuaEngine::interrupt(lua_State* state, int gc) {
    // Instruction safepoints pass -1. Garbage-collection calls are not script progress.
    if (gc >= 0) {
        return;
    }
    auto* engine = static_cast<LuaEngine*>(lua_callbacks(state)->userdata);
    if (engine == nullptr || engine->interrupting_) {
        return;
    }
    if (engine->steps_ < engine->executionBudget_) {
        ++engine->steps_;
        return;
    }
    InterruptGuard guard(engine->interrupting_);
    luaL_error(state, "script exceeded the execution budget");
}

void LuaEngine::panic(lua_State* state, int) {
    const char* message = lua_tostring(state, -1);
    throw std::runtime_error(message != nullptr ? message : "Luau panic");
}

int LuaEngine::print(lua_State* state) {
    auto* engine = static_cast<LuaEngine*>(lua_callbacks(state)->userdata);
    const int count = lua_gettop(state);
    std::string line;
    for (int index = 1; index <= count; ++index) {
        std::size_t length = 0;
        const char* text = luaL_tolstring(state, index, &length);
        if (index > 1) {
            line.push_back('\t');
        }
        if (text != nullptr && length > 0) {
            line.append(text, length);
        }
        lua_pop(state, 1);
    }
    line.push_back('\n');
    if (engine != nullptr && engine->printHandler_) {
        engine->printHandler_(line);
    }
    return 0;
}

int LuaEngine::dispatchHost(lua_State* state) {
    auto* engine = static_cast<LuaEngine*>(lua_callbacks(state)->userdata);
    const int index = lua_tointeger(state, lua_upvalueindex(1));
    if (engine == nullptr || index < 0 || static_cast<std::size_t>(index) >= engine->bindings_.size()) {
        luaL_error(state, "host function is missing");
    }
    HostArgs args(state);
    engine->bindings_[static_cast<std::size_t>(index)].function(args);
    return args.resultCount();
}

}  // namespace engine_core
