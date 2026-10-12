// PhysicsService: the place's collision groups
// (docs/superpowers/specs/2026-10-11-collision-groups-design.md §3). The
// registry is the DataModel's (CollisionGroups); this is its Lua face. Every
// change goes through edit_collision_groups, so it is saved with the place and
// undoable (docs/superpowers/specs/2026-10-12-collision-groups-editor-design.md).

#include "ScriptBindings.hpp"

#include "CollisionGroupEdits.hpp"
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
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

CollisionGroups& groups_of(lua_State* state) { return ScriptBindings::physics_service_groups(state); }

// Applies change through the place's Workspace, raising its refusal.
void edit(lua_State* state, const CollisionGroupsEdit& change) {
    DataModel& world = ScriptBindings::physics_service_world(state);
    if (std::optional<std::string> refused = edit_collision_groups(world, change)) {
        luaL_error(state, "%s", refused->c_str());
    }
}

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
        const std::string name = luaL_checkstring(state, 2);
        const CollisionGroups::FilterType filter_type = check_filter_type(state, 3, true);
        edit(state, [&](CollisionGroups& groups) { return groups.register_group(name, filter_type); });
        return 0;
    });
}

int unregister_group(lua_State* state) {
    return lua_guard(state, [&] {
        const std::string name = luaL_checkstring(state, 2);
        edit(state, [&](CollisionGroups& groups) { return groups.unregister_group(name); });
        return 0;
    });
}

int rename_group(lua_State* state) {
    return lua_guard(state, [&] {
        const std::string from = luaL_checkstring(state, 2);
        const std::string to = luaL_checkstring(state, 3);
        raise(state, rename_collision_group(ScriptBindings::physics_service_world(state), from, to));
        return 0;
    });
}

int set_filter_type(lua_State* state) {
    return lua_guard(state, [&] {
        const std::string name = luaL_checkstring(state, 2);
        const CollisionGroups::FilterType filter_type = check_filter_type(state, 3, false);
        edit(state, [&](CollisionGroups& groups) { return groups.set_filter_type(name, filter_type); });
        return 0;
    });
}

int set_collidable(lua_State* state) {
    return lua_guard(state, [&] {
        const std::string a = luaL_checkstring(state, 2);
        const std::string b = luaL_checkstring(state, 3);
        luaL_checktype(state, 4, LUA_TBOOLEAN);
        const bool collidable = lua_toboolean(state, 4) != 0;
        edit(state, [&](CollisionGroups& groups) { return groups.set_collidable(a, b, collidable); });
        return 0;
    });
}

int set_collidable_bulk(lua_State* state) {
    return lua_guard(state, [&] {
        const std::string name = luaL_checkstring(state, 2);
        luaL_checktype(state, 3, LUA_TTABLE);
        luaL_checktype(state, 4, LUA_TBOOLEAN);
        const bool collidable = lua_toboolean(state, 4) != 0;
        const int count = lua_objlen(state, 3);
        std::vector<std::string> others;
        others.reserve(static_cast<std::size_t>(count));
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(state, 3, i);
            others.emplace_back(luaL_checkstring(state, -1));
            lua_pop(state, 1);
        }
        // All or nothing: one edit, refused whole by the first bad name.
        edit(state, [&](CollisionGroups& groups) -> std::optional<std::string> {
            for (const std::string& other : others) {
                if (std::optional<std::string> refused = groups.set_collidable(name, other, collidable)) {
                    return refused;
                }
            }
            return std::nullopt;
        });
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
        lua_method("RenameCollisionGroup", "nil", reinterpret_cast<void*>(&rename_group)),
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
    return physics_service_world(state).collision_groups();
}

DataModel& ScriptBindings::physics_service_world(lua_State* state) {
    luaL_checkudata(state, 1, kServiceMeta);
    ScriptRuntime* runtime = runtime_from(state);
    if (runtime == nullptr || runtime->game_ == nullptr) {
        luaL_error(state, "PhysicsService is not available");
    }
    return *runtime->game_;
}

}  // namespace engine_core
