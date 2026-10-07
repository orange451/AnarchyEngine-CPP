// Workspace:Raycast and the datatypes it takes and gives: RaycastParams, which
// a script fills in, and RaycastResult, which it only reads.

#include "Enum.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "PhysicsWorld.hpp"
#include "SceneService.hpp"
#include "ScriptBindings.hpp"
#include "ScriptRuntime.hpp"

#include "lua.h"
#include "lualib.h"

#include <cstring>
#include <new>
#include <vector>

namespace engine_core {
namespace {

const char* kRaycastParamsMeta = "AE.RaycastParams";
const char* kRaycastResultMeta = "AE.RaycastResult";

// Lives in a Luau userdata with a destructor, so its vector is freed with it.
struct RaycastParamsUd {
    int filter_type = 0;
    std::vector<InstanceId> instances;
};

struct RaycastResultUd {
    RayHit hit;
};

void destroy_params(lua_State* /*state*/, void* data) { static_cast<RaycastParamsUd*>(data)->~RaycastParamsUd(); }

RaycastParamsUd* push_params(lua_State* state) {
    void* data = lua_newuserdatadtor(state, sizeof(RaycastParamsUd), destroy_params);
    auto* params = new (data) RaycastParamsUd();
    luaL_getmetatable(state, kRaycastParamsMeta);
    lua_setmetatable(state, -2);
    return params;
}

RaycastParamsUd* check_params(lua_State* state, int index) {
    return static_cast<RaycastParamsUd*>(luaL_checkudata(state, index, kRaycastParamsMeta));
}

int params_new(lua_State* state) {
    push_params(state);
    return 1;
}

int params_newindex(lua_State* state) {
    RaycastParamsUd* params = check_params(state, 1);
    const char* key = luaL_checkstring(state, 2);
    if (std::strcmp(key, "FilterType") == 0) {
        params->filter_type = check_enum_arg(state, 3, raycast_filter_type_enum());
        return 0;
    }
    if (std::strcmp(key, "FilterDescendantsInstances") == 0) {
        luaL_checktype(state, 3, LUA_TTABLE);
        std::vector<InstanceId> instances;
        const int count = lua_objlen(state, 3);
        instances.reserve(static_cast<std::size_t>(count));
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(state, 3, i);
            auto* ud = static_cast<InstanceUd*>(test_userdata(state, -1, kInstanceMeta));
            if (ud == nullptr) {
                luaL_error(state, "FilterDescendantsInstances must hold only Instances");
            }
            instances.push_back(ud->id);
            lua_pop(state, 1);
        }
        params->instances = std::move(instances);
        return 0;
    }
    luaL_error(state, "%s is not a valid member of RaycastParams", key);
}

void push_result(lua_State* state, const RayHit& hit) {
    auto* result = static_cast<RaycastResultUd*>(lua_newuserdata(state, sizeof(RaycastResultUd)));
    new (result) RaycastResultUd{hit};
    luaL_getmetatable(state, kRaycastResultMeta);
    lua_setmetatable(state, -2);
}

int result_newindex(lua_State* state) {
    luaL_checkudata(state, 1, kRaycastResultMeta);
    luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2));
}

// The metatable's own __type: "RaycastParams" or "RaycastResult", whichever
// install registered it with.
int type_tostring(lua_State* state) {
    lua_getmetatable(state, 1);
    lua_getfield(state, -1, "__type");
    lua_remove(state, -2);
    return 1;
}

void install(lua_State* state, const char* meta, const char* type_name, lua_CFunction index, lua_CFunction newindex) {
    luaL_newmetatable(state, meta);
    lua_pushcfunction(state, index, "index");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, newindex, "newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushstring(state, type_name);
    lua_setfield(state, -2, "__type");
    lua_pushcfunction(state, type_tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
}

Vec3 check_vector(lua_State* state, int index, const char* name) {
    const float* v = lua_tovector(state, index);
    if (v == nullptr) {
        luaL_error(state, "%s must be a Vector3", name);
    }
    return Vec3{v[0], v[1], v[2]};
}

}  // namespace

// Reads FilterType (an Enum.RaycastFilterType item) and FilterDescendantsInstances
// (a table of the Instances still alive, pushed fresh each read).
int ScriptBindings::raycast_params_index(lua_State* state) {
    RaycastParamsUd* params = check_params(state, 1);
    const char* key = luaL_checkstring(state, 2);
    if (std::strcmp(key, "FilterType") == 0) {
        push_enum_item(state, raycast_filter_type_enum(), params->filter_type);
        return 1;
    }
    if (std::strcmp(key, "FilterDescendantsInstances") == 0) {
        ScriptRuntime* runtime = runtime_from(state);
        lua_createtable(state, static_cast<int>(params->instances.size()), 0);
        int slot = 1;
        for (InstanceId id : params->instances) {
            if (runtime != nullptr && runtime->game_ != nullptr && runtime->game_->instance(id) != nullptr) {
                runtime->push_instance(state, id);
                lua_rawseti(state, -2, slot++);
            }
        }
        return 1;
    }
    luaL_error(state, "%s is not a valid member of RaycastParams", key);
}

// Reads Instance (nil when it is gone since the raycast), Position, Normal,
// Distance, and Material (always nil: nothing but Terrain, not in this
// sub-project, fills has_material).
int ScriptBindings::raycast_result_index(lua_State* state) {
    auto* result = static_cast<RaycastResultUd*>(luaL_checkudata(state, 1, kRaycastResultMeta));
    const char* key = luaL_checkstring(state, 2);
    const RayHit& hit = result->hit;
    if (std::strcmp(key, "Instance") == 0) {
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr || runtime->game_->instance(hit.instance) == nullptr) {
            lua_pushnil(state);
        } else {
            runtime->push_instance(state, hit.instance);
        }
        return 1;
    }
    if (std::strcmp(key, "Position") == 0) {
        lua_pushvector(state, hit.position.x, hit.position.y, hit.position.z);
        return 1;
    }
    if (std::strcmp(key, "Normal") == 0) {
        lua_pushvector(state, hit.normal.x, hit.normal.y, hit.normal.z);
        return 1;
    }
    if (std::strcmp(key, "Distance") == 0) {
        lua_pushnumber(state, hit.distance);
        return 1;
    }
    if (std::strcmp(key, "Material") == 0) {
        // Terrain fills this in (sub-project 1); nothing else has a material yet.
        lua_pushnil(state);
        return 1;
    }
    luaL_error(state, "%s is not a valid member of RaycastResult", key);
}

// Runs on whichever thread holds the VM, always under the write lock (a step,
// the tool step, a paused edit, or the render window), as PhysicsWorld::raycast needs.
int ScriptBindings::workspace_raycast(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        ScriptRuntime* runtime = runtime_from(state);
        auto* workspace = runtime == nullptr ? nullptr : dynamic_cast<Workspace*>(runtime->resolve_id(ud->id, ud->world));
        if (workspace == nullptr) {
            luaL_error(state, "Raycast must be called on Workspace");
        }
        const Vec3 origin = check_vector(state, 2, "origin");
        const Vec3 direction = check_vector(state, 3, "direction");
        RayFilter filter;
        if (!lua_isnoneornil(state, 4)) {
            auto* params = static_cast<RaycastParamsUd*>(test_userdata(state, 4, kRaycastParamsMeta));
            if (params == nullptr) {
                luaL_error(state, "params must be a RaycastParams");
            }
            filter.include = params->filter_type == static_cast<int>(RaycastFilterType::Include);
            filter.instances = params->instances;
        }
        PhysicsWorld* physics = runtime->game_->physics();
        if (physics == nullptr) {
            luaL_error(state, "Raycast needs a running engine");
        }
        const std::optional<RayHit> hit = physics->raycast(*runtime->game_, origin, direction, filter);
        if (!hit) {
            lua_pushnil(state);
        } else {
            push_result(state, *hit);
        }
        return 1;
    });
}

void open_raycast(lua_State* state) {
    install(state, kRaycastParamsMeta, "RaycastParams", &ScriptBindings::raycast_params_index, params_newindex);
    install(state, kRaycastResultMeta, "RaycastResult", &ScriptBindings::raycast_result_index, result_newindex);
    lua_createtable(state, 0, 1);
    lua_pushcfunction(state, params_new, "new");
    lua_setfield(state, -2, "new");
    lua_setreadonly(state, -1, true);
    lua_setglobal(state, "RaycastParams");
}

namespace {

ANARCHY_LUA_REGISTER(register_raycast_lua) {
    const LuaField workspace = lua_method("Raycast", "RaycastResult?", reinterpret_cast<void*>(&ScriptBindings::workspace_raycast));
    register_lua_class("Workspace", nullptr, &workspace, 1);

    const LuaField params[] = {
        lua_property("FilterType", "Enum.RaycastFilterType", true, nullptr, nullptr),
        lua_property("FilterDescendantsInstances", "{Instance}", true, nullptr, nullptr),
    };
    register_lua_class("RaycastParams", nullptr, params, static_cast<int>(sizeof(params) / sizeof(params[0])));
    lua_note_result("RaycastParams", "new", "RaycastParams", false);

    const LuaField result[] = {
        lua_property("Instance", "Instance", false, nullptr, nullptr),
        lua_property("Position", "Vector3", false, nullptr, nullptr),
        lua_property("Normal", "Vector3", false, nullptr, nullptr),
        lua_property("Distance", "number", false, nullptr, nullptr),
        lua_property("Material", "Material?", false, nullptr, nullptr),
    };
    register_lua_class("RaycastResult", nullptr, result, static_cast<int>(sizeof(result) / sizeof(result[0])));
}

}  // namespace

}  // namespace engine_core
