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

class ChangeHistoryService;
class Mesh;
class SoundEmitter;
enum class FinishRecordingOperation;

// Enum.FinishRecordingOperation's values, which are Roblox's (Cancel 0,
// Commit 1), to and from the C++ enum, whatever order that has.
FinishRecordingOperation finish_operation_from_lua(int value);
int finish_operation_to_lua(FinishRecordingOperation op);

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
    // kSignalEvent: the event's name, the registered field's, like blocked_name.
    const char* event_name = nullptr;
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

inline constexpr const char* kServiceClasses[] = {"RunService", "Selection", "UserInputService",
                                                 "ChangeHistoryService"};
constexpr int kUserInputServiceKind = 2;

// SignalUd kinds. An instance's Changed, a RunService phase, an UserInputService
// signal, or an event the instance's class declares (lua_event).
constexpr int kSignalChanged = 0;
constexpr int kSignalPhase = 1;
constexpr int kSignalInput = 2;
constexpr int kSignalEvent = 3;
// A service's own signal, found by its HostSignal tag in phase through
// ScriptRuntime::host_signal; its handlers get whatever values the event carries.
constexpr int kSignalHost = 4;
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
    case Field::Reflected:
    case Field::Count:
        break;
    }
    return "Unknown";
}

// The name Changed passes. A registry property's id comes with its event.
inline const char* changed_name(Field field, std::uint64_t payload) {
    return field == Field::Reflected ? lua_property_name(static_cast<std::uint32_t>(payload)) : field_name(field);
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
    } catch (const InstanceCapacityError& ex) {
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
    HeldRef(Vm& owner, int held) : vm(&owner), token(owner.token), ref(held) {}
    HeldRef(const HeldRef&) = delete;
    HeldRef& operator=(const HeldRef&) = delete;
    ~HeldRef() {
        if (!token.expired() && vm->state != nullptr) {
            lua_unref(vm->state, ref);
        }
    }

    Vm* vm;
    std::weak_ptr<void> token;
    int ref;
};

template <typename Fn>
void ScriptRuntime::guarded(Vm& vm, Fn&& fn) {
    try {
        fn();
    } catch (const ContractViolation&) {
        throw;
    } catch (const std::exception& error) {
        halt(vm, error.what());
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
    // A game script never sees Core or what is in it. The command line and plugins do.
    static bool hidden_from_play(lua_State* state, ScriptRuntime& runtime, InstanceId id);
    static int instance_new(lua_State* state);
    static int require(lua_State* state);
    static int instance_index(lua_State* state);
    static int instance_newindex(lua_State* state);
    // In the window, raises the refusal a property write just deferred, with
    // authorize's reason, when there was none before it (deferred_before).
    static void raise_window_refusal(lua_State* state, ScriptRuntime& runtime, const char* key, bool deferred_before);
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
    // Play and plugin handlers run in the render window; the console VM keeps
    // the sim-side delivery, since its command line enters it without the
    // write lock.
    static bool render_window_routed(const SignalUd& ud, ScriptRuntime::VmKind vm_kind);
    // The name the profiler gives what this signal resumes, such as InputBegan.
    static const char* signal_cause(const SignalUd& ud);
    static int signal_connect(lua_State* state);
    static int signal_wait(lua_State* state);
    static int signal_index(lua_State* state);
    static int connection_disconnect(lua_State* state);
    static int connection_index(lua_State* state);
    static int service_index(lua_State* state);
    static int service_newindex(lua_State* state);
    static int selection_get(lua_State* state);
    static int selection_set(lua_State* state);
    // ChangeHistoryService's methods, each the C++ method of the same name.
    // Raises when no place is attached.
    static ChangeHistoryService& history_service(lua_State* state);
    static int history_try_begin_recording(lua_State* state);
    static int history_finish_recording(lua_State* state);
    static int history_is_recording_in_progress(lua_State* state);
    static int history_set_waypoint(lua_State* state);
    static int history_undo(lua_State* state);
    static int history_redo(lua_State* state);
    static int history_get_can_undo(lua_State* state);
    static int history_get_can_redo(lua_State* state);
    static int history_reset_waypoints(lua_State* state);
    // A Mesh's shape methods. Each adds to the Mesh's AMESH file; see Mesh::edit_geometry.
    static Mesh& mesh_self(lua_State* state);
    static int mesh_add_box(lua_State* state);
    static int mesh_add_sphere(lua_State* state);
    static int mesh_add_cylinder(lua_State* state);
    static int mesh_add_cone(lua_State* state);
    static int mesh_add_plane(lua_State* state);
    static int mesh_add_teapot(lua_State* state);
    static int mesh_clear(lua_State* state);
    static SoundEmitter& emitter_self(lua_State* state);
    static int emitter_play(lua_State* state);
    static int emitter_stop(lua_State* state);
    // A Prefab's bounding box size, from Prefab::bounds.
    static int prefab_get_bounding_box(lua_State* state);
    // The keys and buttons are the service's, read on the simulation thread.
    static UserInputService* input_service(lua_State* state);
    static int input_is_key_down(lua_State* state);
    static int input_is_mouse_button_pressed(lua_State* state);
    static int input_get_keys_pressed(lua_State* state);
    static int input_get_mouse_buttons_pressed(lua_State* state);
    static int input_get_mouse_location(lua_State* state);
    static int input_get_mouse_delta(lua_State* state);
    static int run_is_running(lua_State* state);
    static int input_object_index(lua_State* state);
    static int input_object_tostring(lua_State* state);
    static int thread_index(lua_State* state);
};

}  // namespace engine_core
