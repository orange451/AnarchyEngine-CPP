// CollectionService: the place's tags, found and watched from Lua
// (docs/superpowers/specs/2026-11-23-tweens-tags-gamepad-saves-design.md).
// The tags are each instance's (DataModel::tags); the signals are the world's
// (DataModel::tag_signal). Observe is a pair of handlers on those signals,
// connected as Connect connects, so they belong to the calling script.

#include "ScriptBindings.hpp"

#include "DataModel.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"
#include "Tags.hpp"

#include "lua.h"
#include "lualib.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

// Observe's state table, which each of its closures holds as upvalue 1.
constexpr int kCallback = 1;
// Instance -> true while it is observed, so callback runs once each time it gains the tag.
constexpr int kSeen = 2;
constexpr int kTag = 3;
constexpr int kAddedConnection = 4;
constexpr int kRemovedConnection = 5;
// True once Disconnect has run.
constexpr int kClosed = 6;

// The tag at index, refused as AddTag refuses it.
std::string_view check_tag(lua_State* state, int index) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, index, &length);
    const std::string_view tag(text, length);
    if (const auto error = tag_error(tag)) {
        luaL_error(state, "%s", error->c_str());
    }
    return tag;
}

// The tag, kept for the process so a signal userdata can point at it.
const char* tag_signal_name(std::string_view tag) {
    static std::mutex mutex;
    static std::unordered_set<std::string> names;
    const std::lock_guard<std::mutex> lock(mutex);
    return names.emplace(tag).first->c_str();
}

void push_tag_signal(lua_State* state, std::string_view tag, bool added) {
    auto* ud = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
    *ud = SignalUd{};
    ud->kind = kSignalTag;
    ud->event_name = tag_signal_name(tag);
    ud->phase = added ? 1 : 0;
    luaL_getmetatable(state, kSignalMeta);
    lua_setmetatable(state, -2);
}

DataModel& service_world(lua_State* state) {
    luaL_checkudata(state, 1, kServiceMeta);
    DataModel* game = ScriptBindings::game_of(state);
    if (game == nullptr) {
        luaL_error(state, "CollectionService has no place");
    }
    return *game;
}

// Calls fn(arg) on a thread of its own, as task.spawn does, so an error in it
// is reported and ends only that thread. fn and arg are stack indexes.
void spawn(lua_State* state, int fn, int arg) {
    lua_pushcfunction(state, &ScriptBindings::task_spawn, "spawn");
    lua_pushvalue(state, fn);
    lua_pushvalue(state, arg);
    lua_call(state, 2, 1);
    lua_pop(state, 1);
}

// Connects handler, a closure on the observer table at index, to tag's signal,
// keeping the connection in the table at slot.
void connect_observer(lua_State* state, int observer, std::string_view tag, bool added, lua_CFunction handler,
                      int slot) {
    lua_pushcfunction(state, &ScriptBindings::signal_connect, "Connect");
    push_tag_signal(state, tag, added);
    lua_pushvalue(state, observer);
    lua_pushcclosure(state, handler, added ? "ObserveAdded" : "ObserveRemoved", 1);
    lua_call(state, 2, 1);
    lua_rawseti(state, observer, slot);
}

bool observer_closed(lua_State* state) {
    lua_rawgeti(state, lua_upvalueindex(1), kClosed);
    const bool closed = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return closed;
}

ANARCHY_LUA_REGISTER(register_collection_service_lua) {
    const LuaField methods[] = {
        lua_method("GetTagged", "Instance", reinterpret_cast<void*>(&ScriptBindings::collection_get_tagged), false,
                   false, true),
        lua_method("GetInstanceAddedSignal", "Signal",
                   reinterpret_cast<void*>(&ScriptBindings::collection_added_signal)),
        lua_method("GetInstanceRemovedSignal", "Signal",
                   reinterpret_cast<void*>(&ScriptBindings::collection_removed_signal)),
        lua_method("Observe", "ScriptConnection", reinterpret_cast<void*>(&ScriptBindings::collection_observe)),
    };
    register_lua_class("CollectionService", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
    register_lua_service("CollectionService");
}

}  // namespace

void ScriptBindings::link_collection_service_methods() {}

int ScriptBindings::collection_get_tagged(lua_State* state) {
    return lua_guard(state, [&] {
        DataModel& game = service_world(state);
        std::size_t length = 0;
        const char* text = luaL_checklstring(state, 2, &length);
        ScriptRuntime& runtime = *runtime_from(state);
        lua_newtable(state);
        int index = 1;
        for (const InstanceId id : game.tagged(std::string_view(text, length))) {
            if (hidden_from_play(state, runtime, id)) {
                continue;
            }
            runtime.push_instance(state, id);
            lua_rawseti(state, -2, index++);
        }
        return 1;
    });
}

int ScriptBindings::collection_added_signal(lua_State* state) {
    return lua_guard(state, [&] {
        service_world(state);
        push_tag_signal(state, check_tag(state, 2), true);
        return 1;
    });
}

int ScriptBindings::collection_removed_signal(lua_State* state) {
    return lua_guard(state, [&] {
        service_world(state);
        push_tag_signal(state, check_tag(state, 2), false);
        return 1;
    });
}

int ScriptBindings::collection_observe(lua_State* state) {
    return lua_guard(state, [&] {
        DataModel& game = service_world(state);
        const std::string_view tag = check_tag(state, 2);
        luaL_checktype(state, 3, LUA_TFUNCTION);
        ScriptRuntime& runtime = *runtime_from(state);
        lua_createtable(state, kClosed, 0);
        const int observer = lua_gettop(state);
        lua_pushvalue(state, 3);
        lua_rawseti(state, observer, kCallback);
        lua_newtable(state);
        lua_rawseti(state, observer, kSeen);
        lua_pushvalue(state, 2);
        lua_rawseti(state, observer, kTag);
        connect_observer(state, observer, tag, true, &collection_observe_added, kAddedConnection);
        connect_observer(state, observer, tag, false, &collection_observe_removed, kRemovedConnection);
        // Every instance tagged now, each on its own thread. An added event
        // already queued for one of them finds it observed and does nothing.
        lua_pushvalue(state, observer);
        lua_pushcclosure(state, &collection_observe_added, "ObserveAdded", 1);
        const int added = lua_gettop(state);
        for (const InstanceId id : game.tagged(tag)) {
            if (hidden_from_play(state, runtime, id)) {
                continue;
            }
            runtime.push_instance(state, id);
            spawn(state, added, lua_gettop(state));
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
        // What Observe returns: a connection's Connected and Disconnect.
        lua_createtable(state, 0, 2);
        lua_pushboolean(state, 1);
        lua_setfield(state, -2, "Connected");
        lua_pushvalue(state, observer);
        lua_pushvalue(state, -2);
        lua_pushcclosure(state, &collection_observe_disconnect, "Disconnect", 2);
        lua_setfield(state, -2, "Disconnect");
        return 1;
    });
}

// instance gained the tag: callback(instance) runs once while it keeps it.
int ScriptBindings::collection_observe_added(lua_State* state) {
    return lua_guard(state, [&] {
        const int observer = lua_upvalueindex(1);
        if (observer_closed(state)) {
            return 0;
        }
        lua_settop(state, 1);
        lua_rawgeti(state, observer, kTag);
        std::size_t length = 0;
        const char* tag = lua_tolstring(state, -1, &length);
        DataModel* object = instance_arg(state, 1);
        // An event that comes after the tag went again, or after the instance left the place.
        if (object == nullptr || tag == nullptr || !object->holds_tag(std::string_view(tag, length)) ||
            !object->in_game(object->id())) {
            return 0;
        }
        lua_rawgeti(state, observer, kSeen);
        const int seen = lua_gettop(state);
        lua_pushvalue(state, 1);
        lua_rawget(state, seen);
        if (!lua_isnil(state, -1)) {
            return 0;
        }
        lua_pop(state, 1);
        lua_pushvalue(state, 1);
        lua_pushboolean(state, 1);
        lua_rawset(state, seen);
        // On a thread of its own, so callback may yield.
        lua_rawgeti(state, observer, kCallback);
        spawn(state, lua_gettop(state), 1);
        return 0;
    });
}

// instance lost the tag, or left the place: gaining it again is new.
int ScriptBindings::collection_observe_removed(lua_State* state) {
    return lua_guard(state, [&] {
        const int observer = lua_upvalueindex(1);
        lua_settop(state, 1);
        lua_rawgeti(state, observer, kSeen);
        lua_pushvalue(state, 1);
        lua_pushnil(state);
        lua_rawset(state, 2);
        return 0;
    });
}

// Stops observing.
int ScriptBindings::collection_observe_disconnect(lua_State* state) {
    return lua_guard(state, [&] {
        const int observer = lua_upvalueindex(1);
        if (observer_closed(state)) {
            return 0;
        }
        lua_pushboolean(state, 1);
        lua_rawseti(state, observer, kClosed);
        lua_pushboolean(state, 0);
        lua_setfield(state, lua_upvalueindex(2), "Connected");
        for (const int slot : {kAddedConnection, kRemovedConnection}) {
            lua_rawgeti(state, observer, slot);
            static_cast<Connection*>(luaL_checkudata(state, -1, kConnectionMeta))->disconnect();
            lua_pop(state, 1);
        }
        return 0;
    });
}

}  // namespace engine_core
