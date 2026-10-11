// PhysicsService: the collision groups scripts register while the place runs
// (docs/superpowers/specs/2026-10-11-collision-groups-design.md §3). The
// registry is the DataModel's (CollisionGroups); this is its Lua face.

#include "ScriptBindings.hpp"

#include "CollisionGroups.hpp"
#include "DataModel.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"

#include "lua.h"
#include "lualib.h"

#include <cstring>
#include <optional>
#include <string>

namespace engine_core {

using namespace script_internal;

namespace {

CollisionGroups& groups_of(lua_State* state) { return ScriptBindings::physics_service_groups(state); }

// Enum.CollisionFilterType at index (an item, its name, or its value);
// Exclude when optional and absent.
CollisionGroups::FilterType check_filter_type(lua_State* state, int index, bool optional) {
    if (optional && lua_isnoneornil(state, index)) {
        return CollisionGroups::FilterType::Exclude;
    }
    return static_cast<CollisionGroups::FilterType>(check_enum_arg(state, index, collision_filter_type_enum()));
}

void raise(lua_State* state, const std::optional<std::string>& refused) {
    if (refused) {
        luaL_error(state, "%s", refused->c_str());
    }
}

int register_group(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        const std::string name = luaL_checkstring(state, 2);
        raise(state, groups.register_group(name, check_filter_type(state, 3, true)));
        return 0;
    });
}

int unregister_group(lua_State* state) {
    return lua_guard(state, [&] {
        raise(state, groups_of(state).unregister_group(luaL_checkstring(state, 2)));
        return 0;
    });
}

int set_filter_type(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        const std::string name = luaL_checkstring(state, 2);
        raise(state, groups.set_filter_type(name, check_filter_type(state, 3, false)));
        return 0;
    });
}

int set_collidable(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        const std::string a = luaL_checkstring(state, 2);
        const std::string b = luaL_checkstring(state, 3);
        luaL_checktype(state, 4, LUA_TBOOLEAN);
        raise(state, groups.set_collidable(a, b, lua_toboolean(state, 4) != 0));
        return 0;
    });
}

int set_collidable_bulk(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        const std::string name = luaL_checkstring(state, 2);
        luaL_checktype(state, 3, LUA_TTABLE);
        luaL_checktype(state, 4, LUA_TBOOLEAN);
        const bool collidable = lua_toboolean(state, 4) != 0;
        const int count = lua_objlen(state, 3);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(state, 3, i);
            const std::string other = luaL_checkstring(state, -1);
            lua_pop(state, 1);
            raise(state, groups.set_collidable(name, other, collidable));
        }
        return 0;
    });
}

int are_collidable(lua_State* state) {
    return lua_guard(state, [&] {
        CollisionGroups& groups = groups_of(state);
        lua_pushboolean(state, groups.collidable(luaL_checkstring(state, 2), luaL_checkstring(state, 3)) ? 1 : 0);
        return 1;
    });
}

int is_registered(lua_State* state) {
    return lua_guard(state, [&] {
        lua_pushboolean(state, groups_of(state).registered(luaL_checkstring(state, 2)) ? 1 : 0);
        return 1;
    });
}

int registered_groups(lua_State* state) {
    return lua_guard(state, [&] {
        const std::vector<CollisionGroups::Info> groups = groups_of(state).groups();
        lua_createtable(state, static_cast<int>(groups.size()), 0);
        int index = 1;
        for (const CollisionGroups::Info& info : groups) {
            lua_createtable(state, 0, 3);
            lua_pushlstring(state, info.name.data(), info.name.size());
            lua_setfield(state, -2, "name");
            push_enum_item(state, collision_filter_type_enum(), static_cast<int>(info.filter_type));
            lua_setfield(state, -2, "filterType");
            lua_createtable(state, static_cast<int>(info.list.size()), 0);
            int at = 1;
            for (const std::string& other : info.list) {
                lua_pushlstring(state, other.data(), other.size());
                lua_rawseti(state, -2, at++);
            }
            lua_setfield(state, -2, "list");
            lua_rawseti(state, -2, index++);
        }
        return 1;
    });
}

int max_groups(lua_State* state) {
    return lua_guard(state, [&] {
        groups_of(state);
        lua_pushnumber(state, CollisionGroups::kMaxGroups);
        return 1;
    });
}

ANARCHY_LUA_REGISTER(register_physics_service_lua) {
    const LuaField methods[] = {
        lua_method("RegisterCollisionGroup", "nil", reinterpret_cast<void*>(&register_group)),
        lua_method("UnregisterCollisionGroup", "nil", reinterpret_cast<void*>(&unregister_group)),
        lua_method("SetCollisionGroupFilterType", "nil", reinterpret_cast<void*>(&set_filter_type)),
        lua_method("CollisionGroupSetCollidable", "nil", reinterpret_cast<void*>(&set_collidable)),
        lua_method("CollisionGroupSetCollidableBulk", "nil", reinterpret_cast<void*>(&set_collidable_bulk)),
        lua_method("CollisionGroupsAreCollidable", "boolean", reinterpret_cast<void*>(&are_collidable)),
        lua_method("IsCollisionGroupRegistered", "boolean", reinterpret_cast<void*>(&is_registered)),
        lua_method("GetRegisteredCollisionGroups", "table", reinterpret_cast<void*>(&registered_groups)),
        lua_method("GetMaxCollisionGroups", "number", reinterpret_cast<void*>(&max_groups)),
    };
    register_lua_class("PhysicsService", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
    register_lua_service("PhysicsService");
}

}  // namespace

void ScriptBindings::link_physics_service_methods() {}

CollisionGroups& ScriptBindings::physics_service_groups(lua_State* state) {
    luaL_checkudata(state, 1, kServiceMeta);
    ScriptRuntime* runtime = runtime_from(state);
    if (runtime == nullptr || runtime->game_ == nullptr) {
        luaL_error(state, "PhysicsService is not available");
    }
    return runtime->game_->collision_groups();
}

}  // namespace engine_core
