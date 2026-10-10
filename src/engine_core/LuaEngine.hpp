#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace engine_core {

// Arguments for one call from a script into the host. Indexes are 1-based, matching Luau.
// A string view is valid only until the host function returns.
class HostArgs {
public:
    int count() const { return argumentCount_; }

    bool isBoolean(int index) const;
    bool isNumber(int index) const;
    bool isString(int index) const;

    bool boolean(int index) const;
    double number(int index) const;
    std::string_view string(int index) const;

    void pushNumber(double value);

    // Raises a script error and does not return.
    [[noreturn]] void error(std::string_view message);

private:
    friend class LuaEngine;

    explicit HostArgs(lua_State* state);

    int resultCount() const { return results_; }

    lua_State* state_ = nullptr;
    int argumentCount_ = 0;
    int results_ = 0;
};

// Sandboxed Luau state. Scripts will drive the data model from here, the way Roblox scripts do.
//
// start() seals the shared environment. Each script then gets its own globals, which can read
// that environment and cannot change it. The shared surface is the safe libraries (base,
// coroutine, table, string, math, utf8, bit32, buffer, and vector) plus host functions bound
// before start. print forwards to the handler installed here and nowhere else.
//
// debug, os, io, package, require, loadstring, and getfenv/setfenv are not part of that surface,
// so a script cannot inspect the VM, touch the process, or replace some other function's environment.
class LuaEngine {
public:
    static constexpr std::uint64_t kDefaultExecutionBudget = 1000000;
    static constexpr std::size_t kDefaultMemoryLimit = 64 * 1024 * 1024;

    using HostFunction = std::function<void(HostArgs&)>;
    using PrintHandler = std::function<void(std::string_view text)>;

    struct ScriptResult {
        bool ok = false;
        std::string error;
        std::vector<std::string> values;
    };

    LuaEngine() = default;
    ~LuaEngine();

    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    // Registers a global every script can call. Must happen before start().
    void bind(std::string name, HostFunction function);

    // print() calls this with one line, including the trailing newline.
    // The view is valid only for the call. With no handler, print succeeds and discards the line.
    void setPrintHandler(PrintHandler handler);

    // Interrupt checks per script. A tight loop trips this and stops, including inside pcall.
    void setExecutionBudget(std::uint64_t interrupts);
    // Bytes the state may allocate, including its own structures. A script cannot grow past it.
    void setMemoryLimit(std::size_t bytes);

    void start();
    void stop();
    bool running() const { return state_ != nullptr; }

    // Runs one script on a fresh environment. Return values are converted with a plain format:
    // nil, booleans, numbers, and strings keep their text, and everything else is its type name.
    ScriptResult execute(std::string_view chunkName, std::string_view source);

private:
    struct Binding {
        std::string name;
        HostFunction function;
    };

    static void* allocate(void* userdata, void* pointer, std::size_t oldSize, std::size_t newSize);
    static void interrupt(lua_State* state, int gc);
    static int print(lua_State* state);
    static int dispatchHost(lua_State* state);

    std::string stackText(lua_State* state, int index) const;

    lua_State* state_ = nullptr;
    std::vector<Binding> bindings_;
    PrintHandler printHandler_;
    std::uint64_t executionBudget_ = kDefaultExecutionBudget;
    std::uint64_t steps_ = 0;
    std::size_t memoryLimit_ = kDefaultMemoryLimit;
    std::size_t memoryUsed_ = 0;
    bool executing_ = false;
    bool interrupting_ = false;
};

}  // namespace engine_core
