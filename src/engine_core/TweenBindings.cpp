// TweenService and the Tween datatype: TweenWorld's Lua face
// (docs/superpowers/specs/2026-11-23-tweens-tags-gamepad-saves-design.md).
// Every binding runs on SimulationThread, as every script does. A Tween is a
// userdata naming its tween in the runtime's TweenWorld; one the world has
// forgotten, as a Stop does, raises "Tween is gone" at its next use.

#include "ScriptBindings.hpp"

#include "DataModel.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"
#include "TweenWorld.hpp"

#include "lua.h"
#include "lualib.h"

#include <climits>
#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

constexpr const char* kTweenMeta = "AE.Tween";

// The runtime, so the destructor finds the world after a VM's callbacks are cleared.
struct TweenUd {
    ScriptRuntime* runtime = nullptr;
    std::uint32_t id = 0;
};

// The property types a tween moves, and the value each takes.
struct TweenType {
    const char* name;
    LuaSlot::Kind kind;
};

constexpr TweenType kTweenTypes[] = {
    {"number", LuaSlot::Kind::Number}, {"Vector2", LuaSlot::Kind::Vec2},   {"Vector3", LuaSlot::Kind::Vec3},
    {"Color3", LuaSlot::Kind::Color},  {"Matrix4", LuaSlot::Kind::Matrix4},
};

const TweenType* tween_type(const char* name) {
    for (const TweenType& type : kTweenTypes) {
        if (name != nullptr && std::strcmp(type.name, name) == 0) {
            return &type;
        }
    }
    return nullptr;
}

double finite_option(lua_State* state, int index, const char* name) {
    if (lua_type(state, index) != LUA_TNUMBER || !std::isfinite(lua_tonumber(state, index))) {
        luaL_error(state, "%s must be a finite number", name);
    }
    return lua_tonumber(state, index);
}

// The options table at index, or the defaults when it is absent.
TweenOptions read_options(lua_State* state, int index) {
    TweenOptions options;
    if (lua_isnoneornil(state, index)) {
        return options;
    }
    luaL_checktype(state, index, LUA_TTABLE);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        const int value = lua_gettop(state);
        const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : nullptr;
        if (key == nullptr) {
            luaL_error(state, "tween options are named, such as Time");
        } else if (std::strcmp(key, "Time") == 0) {
            options.time = finite_option(state, value, "Time");
        } else if (std::strcmp(key, "Style") == 0) {
            options.style = static_cast<anarchy::aanim::EasingStyle>(check_enum_arg(state, value, easing_style_enum()));
        } else if (std::strcmp(key, "Direction") == 0) {
            options.direction =
                static_cast<anarchy::aanim::EasingDirection>(check_enum_arg(state, value, easing_direction_enum()));
        } else if (std::strcmp(key, "Delay") == 0) {
            options.delay = finite_option(state, value, "Delay");
            if (options.delay < 0) {
                luaL_error(state, "Delay must be 0 or more");
            }
        } else if (std::strcmp(key, "Repeat") == 0) {
            const double repeat = finite_option(state, value, "Repeat");
            if (repeat != std::floor(repeat) || repeat < -1 || repeat > INT_MAX) {
                luaL_error(state, "Repeat must be a whole number, -1 or more");
            }
            options.repeat = static_cast<int>(repeat);
        } else if (std::strcmp(key, "Reverses") == 0) {
            if (!lua_isboolean(state, value)) {
                luaL_error(state, "Reverses must be true or false");
            }
            options.reverses = lua_toboolean(state, value) != 0;
        } else {
            luaL_error(state, "unknown tween option %s", key);
        }
        lua_pop(state, 1);
    }
    return options;
}

void push_tween_signal(lua_State* state, std::uint32_t id, const char* name) {
    auto* ud = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
    *ud = SignalUd{};
    ud->kind = kSignalTween;
    ud->track = id;
    ud->event_name = name;
    luaL_getmetatable(state, kSignalMeta);
    lua_setmetatable(state, -2);
}

// A handle on tween id, which the world keeps while the handle lives.
void push_tween(lua_State* state, ScriptRuntime& runtime, TweenWorld& tweens, std::uint32_t id) {
    auto* ud = static_cast<TweenUd*>(lua_newuserdatadtor(state, sizeof(TweenUd), &ScriptBindings::tween_release));
    *ud = TweenUd{&runtime, id};
    luaL_getmetatable(state, kTweenMeta);
    lua_setmetatable(state, -2);
    tweens.retain(id);
}

int tween_tostring(lua_State* state) {
    lua_pushliteral(state, "Tween");
    return 1;
}

int tween_eq(lua_State* state) {
    const auto* a = static_cast<TweenUd*>(test_userdata(state, 1, kTweenMeta));
    const auto* b = static_cast<TweenUd*>(test_userdata(state, 2, kTweenMeta));
    lua_pushboolean(state, a != nullptr && b != nullptr && a->id == b->id);
    return 1;
}

ANARCHY_LUA_REGISTER(register_tween_service_lua) {
    const LuaField methods[] = {
        lua_method("Tween", "Tween", reinterpret_cast<void*>(&ScriptBindings::tween_service_tween)),
        lua_method("Value", "Tween", reinterpret_cast<void*>(&ScriptBindings::tween_service_value)),
    };
    register_lua_class("TweenService", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
    register_lua_service("TweenService");

    // The datatype, for completion and the type checker; the metatable does the work.
    static const LuaParam kCompletedArgs[] = {{"completed", "boolean"}};
    const LuaField tween_fields[] = {
        lua_property("Playing", "boolean", false, nullptr, nullptr),
        lua_method("Pause", "nil", nullptr),
        lua_method("Resume", "nil", nullptr),
        lua_method("Cancel", "nil", nullptr),
        lua_method("Wait", "boolean", nullptr),
        lua_event("Completed", kCompletedArgs, 1),
    };
    register_lua_class("Tween", nullptr, tween_fields, static_cast<int>(sizeof(tween_fields) / sizeof(tween_fields[0])));
}

}  // namespace

void ScriptBindings::link_tween_methods() {}

ScriptRuntime& ScriptBindings::tween_service_runtime(lua_State* state) {
    luaL_checkudata(state, 1, kServiceMeta);
    ScriptRuntime* runtime = runtime_from(state);
    if (runtime == nullptr || runtime->game_ == nullptr || !runtime->open_) {
        luaL_error(state, "TweenService tweens only while the place is playing");
    }
    return *runtime;
}

int ScriptBindings::tween_service_tween(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime& runtime = tween_service_runtime(state);
        DataModel* object = instance_arg(state, 2);
        if (object == nullptr) {
            luaL_error(state, "Tween takes a live Instance");
        }
        luaL_checktype(state, 3, LUA_TTABLE);
        const TweenOptions options = read_options(state, 4);
        std::vector<TweenGoal> goals;
        lua_pushnil(state);
        while (lua_next(state, 3) != 0) {
            const char* name = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : nullptr;
            if (name == nullptr) {
                luaL_error(state, "Tween's goals are property names");
            }
            const LuaField* field = lua_class_find(object->class_name(), name);
            const TweenType* type = field != nullptr ? tween_type(field->type_name) : nullptr;
            if (type == nullptr || field->method || field->event || field->blocked || !field->writable ||
                field->read == nullptr || field->write == nullptr || field->enum_type != nullptr) {
                luaL_error(state, "%s cannot be tweened", name);
            }
            TweenGoal goal;
            goal.field = field;
            if (!attribute_from_stack(state, runtime, lua_gettop(state), goal.to) || goal.to.kind != type->kind) {
                luaL_error(state, "%s expects a %s", name, type->name);
            }
            if (!field->read(*runtime.game_, *object, goal.from) || goal.from.kind != type->kind) {
                luaL_error(state, "%s cannot be tweened", name);
            }
            goals.push_back(std::move(goal));
            lua_pop(state, 1);
        }
        if (goals.empty()) {
            luaL_error(state, "Tween needs at least one goal");
        }
        const std::uint32_t id = runtime.tweens_.start(*runtime.game_, object->id(), std::move(goals), options);
        push_tween(state, runtime, runtime.tweens_, id);
        return 1;
    });
}

int ScriptBindings::tween_service_value(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime& runtime = tween_service_runtime(state);
        LuaSlot from;
        LuaSlot to;
        if (!attribute_from_stack(state, runtime, 2, from) || !attribute_from_stack(state, runtime, 3, to) ||
            !tween_kind(from.kind) || to.kind != from.kind) {
            luaL_error(state, "Value tweens a number, Vector2, Vector3, Color3, or Matrix4 to another of its kind");
        }
        const TweenOptions options = read_options(state, 4);
        luaL_checktype(state, 5, LUA_TFUNCTION);
        const std::uint32_t id = runtime.tweens_.start_value(std::move(from), std::move(to), options);
        push_tween(state, runtime, runtime.tweens_, id);
        // The callback connects as any handler does, so the calling script owns it.
        lua_pushcfunction(state, &ScriptBindings::signal_connect, "Connect");
        push_tween_signal(state, id, "Stepped");
        lua_pushvalue(state, 5);
        lua_call(state, 2, 1);
        lua_pop(state, 1);
        return 1;
    });
}

ScriptRuntime& ScriptBindings::tween_of(lua_State* state, std::uint32_t& id) {
    auto* ud = static_cast<TweenUd*>(luaL_checkudata(state, 1, kTweenMeta));
    if (ud->runtime == nullptr || ud->runtime->game_ == nullptr || !ud->runtime->tweens_.known(ud->id)) {
        luaL_error(state, "Tween is gone");
    }
    id = ud->id;
    return *ud->runtime;
}

int ScriptBindings::tween_index(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<TweenUd*>(luaL_checkudata(state, 1, kTweenMeta));
        const char* key = luaL_checkstring(state, 2);
        if (std::strcmp(key, "Playing") == 0) {
            lua_pushboolean(state, ud->runtime != nullptr && ud->runtime->tweens_.playing(ud->id));
        } else if (std::strcmp(key, "Completed") == 0) {
            push_tween_signal(state, ud->id, "Completed");
        } else {
            lua_pushvalue(state, lua_upvalueindex(1));
            lua_pushvalue(state, 2);
            lua_rawget(state, -2);
            if (!lua_isfunction(state, -1)) {
                luaL_error(state, "%s is not a valid member of Tween", key);
            }
        }
        return 1;
    });
}

int ScriptBindings::tween_pause(lua_State* state) {
    return lua_guard(state, [&] {
        std::uint32_t id = 0;
        tween_of(state, id).tweens_.pause(id);
        return 0;
    });
}

int ScriptBindings::tween_resume(lua_State* state) {
    return lua_guard(state, [&] {
        std::uint32_t id = 0;
        tween_of(state, id).tweens_.resume(id);
        return 0;
    });
}

int ScriptBindings::tween_cancel(lua_State* state) {
    return lua_guard(state, [&] {
        std::uint32_t id = 0;
        ScriptRuntime& runtime = tween_of(state, id);
        runtime.tweens_.cancel(*runtime.game_, id);
        return 0;
    });
}

int ScriptBindings::tween_wait(lua_State* state) {
    return lua_guard(state, [&] {
        std::uint32_t id = 0;
        ScriptRuntime& runtime = tween_of(state, id);
        if (runtime.tweens_.ended(id)) {
            lua_pushboolean(state, runtime.tweens_.completed(id));
            return 1;
        }
        // Completed's Wait: the thread resumes with the boolean it fires with.
        lua_settop(state, 0);
        push_tween_signal(state, id, "Completed");
        return signal_wait(state);
    });
}

void ScriptBindings::tween_release(lua_State*, void* data) {
    const auto* ud = static_cast<TweenUd*>(data);
    if (ud->runtime != nullptr && ud->runtime->game_ != nullptr) {
        ud->runtime->tweens_.release(*ud->runtime->game_, ud->id);
    }
}

void open_tween(lua_State* state) {
    static const luaL_Reg kMethods[] = {
        {"Pause", &ScriptBindings::tween_pause},
        {"Resume", &ScriptBindings::tween_resume},
        {"Cancel", &ScriptBindings::tween_cancel},
        {"Wait", &ScriptBindings::tween_wait},
        {nullptr, nullptr},
    };
    luaL_newmetatable(state, kTweenMeta);
    lua_createtable(state, 0, 4);
    for (const luaL_Reg* entry = kMethods; entry->name != nullptr; ++entry) {
        lua_pushcfunction(state, entry->func, entry->name);
        lua_setfield(state, -2, entry->name);
    }
    lua_setreadonly(state, -1, true);
    lua_pushcclosure(state, &ScriptBindings::tween_index, "index", 1);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, tween_tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, tween_eq, "eq");
    lua_setfield(state, -2, "__eq");
    lua_pushliteral(state, "Tween");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
}

}  // namespace engine_core
