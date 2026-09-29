#pragma once

// Internal to ScriptRuntime.cpp and ScriptBindings.cpp: the userdata layouts,
// metatable names, and helpers both use, and the Lua bindings that reach into
// ScriptRuntime as a friend. Nothing else includes this.

#include "Contract.hpp"
#include "Events.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "UserInputService.hpp"

#include "lua.h"
#include "lualib.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>

namespace engine_core {
namespace script_internal {


struct InstanceUd {
    InstanceId id = 0;
    std::uint32_t world = 0;
};

struct SignalUd {
    int kind = 0;
    InstanceId id = 0;
    std::uint32_t world = 0;
    int phase = 0;
    bool blocked = false;
    // The registered field's name, which lives as long as the class registry.
    const char* blocked_name = nullptr;
};

inline constexpr const char* kInstanceMeta = "AE.Instance";
// Registry table: instance id -> its one userdata in this state, weak values.
inline constexpr const char* kInstanceCache = "AE.InstanceCache";
inline constexpr const char* kSignalMeta = "AE.Signal";
inline constexpr const char* kConnectionMeta = "AE.Connection";
inline constexpr const char* kThreadMeta = "AE.Thread";
inline constexpr const char* kServiceMeta = "AE.Service";
inline constexpr const char* kInputObjectMeta = "AE.InputObject";

// Which service a GetService userdata stands for. The name is also its class.
struct ServiceUd {
    int kind = 0;
};

inline constexpr const char* kServiceClasses[] = {"RunService", "Selection", "UserInputService"};
constexpr int kUserInputServiceKind = 2;

// SignalUd kinds. An instance's Changed, a RunService phase, or an UserInputService signal.
constexpr int kSignalChanged = 0;
constexpr int kSignalPhase = 1;
constexpr int kSignalInput = 2;
constexpr int kServiceKinds = static_cast<int>(sizeof(kServiceClasses) / sizeof(kServiceClasses[0]));

inline int service_kind(const char* name) {
    for (int kind = 0; kind < kServiceKinds; ++kind) {
        if (std::strcmp(kServiceClasses[kind], name) == 0) {
            return kind;
        }
    }
    return -1;
}

inline const char* field_name(Field field) {
    switch (field) {
    case Field::Transform:
        return "Transform";
    case Field::Color:
        return "Color";
    case Field::Size:
        return "Size";
    case Field::LinearVelocity:
        return "LinearVelocity";
    case Field::Simulated:
        return "Simulated";
    case Field::VisualOnly:
        return "VisualOnly";
    case Field::Parent:
        return "Parent";
    case Field::Name:
        return "Name";
    case Field::Source:
        return "Source";
    case Field::Enabled:
        return "Enabled";
    case Field::Position:
        return "Position";
    case Field::Count:
        break;
    }
    return "Unknown";
}

inline ScriptRuntime* runtime_from(lua_State* state) {
    return static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
}

// A thread's serial, kept in its coroutine's thread data. 0 is no thread.
inline void* serial_data(std::uint64_t serial) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(serial)); }

inline std::uint64_t data_serial(void* data) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data)); }

template <typename Fn>
inline int lua_guard(lua_State* state, Fn fn) {
    try {
        return fn();
    } catch (const ContractViolation& ex) {
        luaL_error(state, "%s", ex.what());
    }
}

inline bool read_color3(lua_State* state, int index, ColorRgb& color) {
    const Color3* rgb = to_color3(state, index);
    if (rgb == nullptr) {
        return false;
    }
    color = ColorRgb{rgb->r, rgb->g, rgb->b, 1.f};
    return true;
}

inline bool is_a(const DataModel& object, const char* name) {
    if (name == nullptr) {
        return false;
    }
    if (std::strcmp(object.class_name(), name) == 0) {
        return true;
    }
    // Instance and DataModel, and any other registered base.
    if (lua_class_inherits(object.class_name(), name)) {
        return true;
    }
    if (std::strcmp(name, "DataModel") == 0) {
        return true;
    }
    if (std::strcmp(name, "GameObject") == 0) {
        return dynamic_cast<const GameObject*>(&object) != nullptr;
    }
    if (std::strcmp(name, "Script") == 0) {
        return dynamic_cast<const Script*>(&object) != nullptr;
    }
    if (std::strcmp(name, "ModuleScript") == 0) {
        return dynamic_cast<const ModuleScript*>(&object) != nullptr;
    }
    if (std::strcmp(name, "LuaSource") == 0) {
        return dynamic_cast<const LuaSource*>(&object) != nullptr;
    }
    return false;
}

constexpr std::size_t kMaxOutputBytes = 16 * 1024;
constexpr std::size_t kMaxOutputLines = 1024;

// Calls read with _G[name] on top of the stack, then pops it. Does nothing
// without a state or a name.
template <typename Read>
inline void with_global(lua_State* state, const char* name, Read&& read) {
    if (state == nullptr || name == nullptr) {
        return;
    }
    lua_getglobal(state, "_G");
    lua_getfield(state, -1, name);
    read(state);
    lua_pop(state, 2);
}


inline void push_input_object(lua_State* state, const InputRecord& record) {
    auto* ud = static_cast<InputRecord*>(lua_newuserdata(state, sizeof(InputRecord)));
    new (ud) InputRecord(record);
    luaL_getmetatable(state, kInputObjectMeta);
    lua_setmetatable(state, -2);
}

}  // namespace script_internal

using namespace script_internal;

// The last copy of a handler lets its reference go. A VM that has closed took
// every reference with it, so then there is nothing to release.
struct ScriptRuntime::HeldRef {
    HeldRef(ScriptRuntime& runtime, int ref) : runtime(&runtime), vm(runtime.vm_token_), ref(ref) {}
    HeldRef(const HeldRef&) = delete;
    HeldRef& operator=(const HeldRef&) = delete;
    ~HeldRef() {
        if (!vm.expired() && runtime->state_ != nullptr) {
            lua_unref(runtime->state_, ref);
        }
    }

    ScriptRuntime* runtime;
    std::weak_ptr<void> vm;
    int ref;
};

template <typename Fn>
void ScriptRuntime::guarded(Fn&& fn) {
    try {
        fn();
    } catch (const ContractViolation&) {
        throw;
    } catch (const std::exception& error) {
        halt(error.what());
    }
}

struct ScriptBindings {
    // task.spawn, task.defer, and task.delay. The caller must be a script
    // thread. task_thread moves the function at first, and the arguments after
    // it, to a new thread for the caller's script. The handle is what
    // task.cancel takes.
    static ScriptRuntime::Thread& task_caller(lua_State* state, const char* name);
    static ScriptRuntime::Thread& task_thread(lua_State* state, const ScriptRuntime::Thread& caller, int first);
    static int push_task_handle(lua_State* state, const ScriptRuntime::Thread& thread);
    static int task_wait(lua_State* state);
    static int task_spawn(lua_State* state);
    static int task_defer(lua_State* state);
    static int task_delay(lua_State* state);
    static int task_cancel(lua_State* state);
    static int instance_new(lua_State* state);
    static int require(lua_State* state);
    static int instance_index(lua_State* state);
    static int instance_newindex(lua_State* state);
    static int instance_destroy(lua_State* state);
    static int instance_children(lua_State* state);
    static int instance_find(lua_State* state);
    static int instance_wait_child(lua_State* state);
    static int instance_isa(lua_State* state);
    static int instance_tostring(lua_State* state);
    static int instance_service(lua_State* state);
    // What a signal userdata names: an instance's Changed, an UserInputService
    // signal, or a simulation phase on RunService. Callers refuse a blocked
    // signal, such as a render phase, first. Raises when the instance is gone.
    static Signal& signal_of(lua_State* state, ScriptRuntime& runtime, const SignalUd& ud);
    static int signal_connect(lua_State* state);
    static int signal_wait(lua_State* state);
    static int signal_index(lua_State* state);
    static int connection_disconnect(lua_State* state);
    static int connection_index(lua_State* state);
    static int service_index(lua_State* state);
    static int selection_get(lua_State* state);
    static int selection_set(lua_State* state);
    // The keys and buttons are the service's, read on the simulation thread.
    static UserInputService* input_service(lua_State* state);
    static int input_is_key_down(lua_State* state);
    static int input_is_mouse_button_pressed(lua_State* state);
    static int input_get_keys_pressed(lua_State* state);
    static int input_get_mouse_buttons_pressed(lua_State* state);
    static int input_get_mouse_location(lua_State* state);
    static int input_object_index(lua_State* state);
    static int input_object_tostring(lua_State* state);
    static int thread_index(lua_State* state);
};

}  // namespace engine_core
