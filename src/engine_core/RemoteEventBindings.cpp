// RemoteEvent's methods (docs/superpowers/specs/2026-10-20-remote-events-design.md).
// RemoteEvent.cpp declares the class, Unreliable, and the two events; these
// send. The world's role, read at the call, picks the way: a standalone world
// loops back through its event queue, a Server or Client world hands the
// message to its session's IRemoteSink.

#include "ScriptBindings.hpp"

#include "ConnectionInstance.hpp"
#include "DataModel.hpp"
#include "LuaApi.hpp"
#include "RemoteEvent.hpp"
#include "RemoteSink.hpp"
#include "RemoteValues.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

// self as a RemoteEvent, or raises.
RemoteEvent& remote_self(lua_State* state) {
    luaL_checkudata(state, 1, kInstanceMeta);
    auto* remote = dynamic_cast<RemoteEvent*>(ScriptBindings::instance_arg(state, 1));
    if (remote == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *remote;
}

DataModel& game_of(lua_State* state) {
    DataModel* game = ScriptBindings::game_of(state);
    if (game == nullptr) {
        luaL_error(state, "RemoteEvent has no data model");
    }
    return *game;
}

void refuse_on_server(lua_State* state, const DataModel& game) {
    if (game.world_role() == WorldRole::Server) {
        luaL_error(state, "FireServer is for clients; the server sends with FireClient, FireClientExt or FireClientAll");
    }
}

void refuse_on_client(lua_State* state, const DataModel& game, const char* method) {
    if (game.world_role() == WorldRole::Client) {
        luaL_error(state, "%s is for the server; a client sends with FireServer", method);
    }
}

// stack[first..top] as a payload, or raises why it cannot be sent.
std::shared_ptr<const RemoteValues> payload(lua_State* state, int first, const char* method) {
    auto values = std::make_shared<RemoteValues>();
    std::string error;
    if (!remote_values_from_lua(state, first, method, *values, error)) {
        luaL_error(state, "%s", error.c_str());
    }
    return values;
}

LuaSlot values_slot(std::shared_ptr<const RemoteValues> values) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Values;
    slot.values = std::move(values);
    return slot;
}

IRemoteSink& sink_of(lua_State* state, const DataModel& game) {
    IRemoteSink* sink = game.remote_sink();
    if (sink == nullptr) {
        luaL_error(state, "Only a networked world can send this");
    }
    return *sink;
}

// A sink names why without the method: "message is over 262144".
void raise_if(lua_State* state, bool sent, const char* method, const std::string& error) {
    if (!sent) {
        luaL_error(state, "%s %s", method, error.c_str());
    }
}

// The live Connection of this server at index, or 0.
InstanceId connection_at(lua_State* state, const DataModel& game, int index) {
    const DataModel* object = ScriptBindings::instance_arg(state, index);
    if (object == nullptr || dynamic_cast<const ConnectionInstance*>(object) == nullptr ||
        game.parent(object->id()) != game.connections()) {
        return 0;
    }
    return object->id();
}

// Standalone: OnClientEvent, once for this call.
void loop_to_client(DataModel& game, const RemoteEvent& remote, std::shared_ptr<const RemoteValues> values) {
    EventArgs args;
    args.push_back(values_slot(std::move(values)));
    game.fire_event(remote.id(), "OnClientEvent", std::move(args));
}

int fire_server(lua_State* state) {
    return lua_guard(state, [&] {
        RemoteEvent& remote = remote_self(state);
        DataModel& game = game_of(state);
        refuse_on_server(state, game);
        std::shared_ptr<const RemoteValues> values = payload(state, 2, "FireServer");
        if (game.world_role() == WorldRole::Standalone) {
            EventArgs args;
            args.push_back(LuaSlot{});
            args.push_back(values_slot(std::move(values)));
            game.fire_event(remote.id(), "OnServerEvent", std::move(args));
            return 0;
        }
        std::string error;
        raise_if(state, sink_of(state, game).fire_server(remote.id(), std::move(values), remote.unreliable(), error),
                 "FireServer", error);
        return 0;
    });
}

int fire_client(lua_State* state) {
    return lua_guard(state, [&] {
        RemoteEvent& remote = remote_self(state);
        DataModel& game = game_of(state);
        refuse_on_client(state, game, "FireClient");
        if (game.world_role() == WorldRole::Standalone) {
            // Whatever connection was given, even nil: the one world is the client.
            loop_to_client(game, remote, payload(state, 3, "FireClient"));
            return 0;
        }
        const InstanceId connection = connection_at(state, game, 2);
        if (connection == 0) {
            luaL_error(state, "FireClient needs a Connection of this server");
        }
        std::shared_ptr<const RemoteValues> values = payload(state, 3, "FireClient");
        std::string error;
        const std::vector<InstanceId> connections{connection};
        raise_if(state,
                 sink_of(state, game).fire_clients(remote.id(), connections, false, std::move(values),
                                                   remote.unreliable(), error),
                 "FireClient", error);
        return 0;
    });
}

int fire_client_ext(lua_State* state) {
    return lua_guard(state, [&] {
        RemoteEvent& remote = remote_self(state);
        DataModel& game = game_of(state);
        refuse_on_client(state, game, "FireClientExt");
        luaL_checktype(state, 2, LUA_TTABLE);
        if (game.world_role() == WorldRole::Standalone) {
            loop_to_client(game, remote, payload(state, 3, "FireClientExt"));
            return 0;
        }
        // Entries that are not a live Connection of this server are skipped, and
        // one listed twice is sent once.
        std::vector<InstanceId> connections;
        const int length = lua_objlen(state, 2);
        for (int element = 1; element <= length; ++element) {
            lua_rawgeti(state, 2, element);
            const InstanceId connection = connection_at(state, game, lua_gettop(state));
            lua_pop(state, 1);
            if (connection != 0 && std::find(connections.begin(), connections.end(), connection) == connections.end()) {
                connections.push_back(connection);
            }
        }
        std::shared_ptr<const RemoteValues> values = payload(state, 3, "FireClientExt");
        std::string error;
        raise_if(state,
                 sink_of(state, game).fire_clients(remote.id(), connections, false, std::move(values),
                                                   remote.unreliable(), error),
                 "FireClientExt", error);
        return 0;
    });
}

int fire_client_all(lua_State* state) {
    return lua_guard(state, [&] {
        RemoteEvent& remote = remote_self(state);
        DataModel& game = game_of(state);
        refuse_on_client(state, game, "FireClientAll");
        std::shared_ptr<const RemoteValues> values = payload(state, 2, "FireClientAll");
        if (game.world_role() == WorldRole::Standalone) {
            loop_to_client(game, remote, std::move(values));
            return 0;
        }
        std::string error;
        raise_if(state,
                 sink_of(state, game).fire_clients(remote.id(), {}, true, std::move(values), remote.unreliable(),
                                                   error),
                 "FireClientAll", error);
        return 0;
    });
}

ANARCHY_LUA_REGISTER(register_remote_event_methods) {
    const LuaField methods[] = {
        lua_method("FireServer", "nil", reinterpret_cast<void*>(&fire_server)),
        lua_method("FireClient", "nil", reinterpret_cast<void*>(&fire_client)),
        lua_method("FireClientExt", "nil", reinterpret_cast<void*>(&fire_client_ext)),
        lua_method("FireClientAll", "nil", reinterpret_cast<void*>(&fire_client_all)),
    };
    register_lua_class("RemoteEvent", nullptr, methods, static_cast<int>(std::size(methods)));
}

}  // namespace

void ScriptBindings::link_remote_event_methods() {}

}  // namespace engine_core
