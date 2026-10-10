// GameObject's bone methods. Every binding runs on SimulationThread, as every
// script does. Nothing here makes a Bone but AddBone: GetBone only finds one.

#include "ScriptBindings.hpp"

#include "Bone.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Skeleton.hpp"
#include "Skinning.hpp"

#include "lua.h"
#include "lualib.h"

#include <string>

namespace engine_core {

using namespace script_internal;

namespace {

// The Bone under object that poses the bone called name: the first Bone so
// named, as GameObject::pose counts them. 0 for none.
InstanceId find_bone(const GameObject& object, const std::string& name) {
    for (InstanceId child = object.first_child(object.id()); child != 0; child = object.next_sibling(child)) {
        if (dynamic_cast<const Bone*>(object.instance(child)) != nullptr && object.name(child) == name) {
            return child;
        }
    }
    return 0;
}

ANARCHY_LUA_REGISTER(register_skeleton_methods) {
    const LuaField methods[] = {
        lua_method("GetBoneNames", "string", reinterpret_cast<void*>(&ScriptBindings::game_object_get_bone_names),
                   false, false, true),
        lua_method("GetBone", "Bone?", reinterpret_cast<void*>(&ScriptBindings::game_object_get_bone)),
        lua_method("AddBone", "Bone", reinterpret_cast<void*>(&ScriptBindings::game_object_add_bone)),
    };
    register_lua_class("GameObject", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
}

}  // namespace

void ScriptBindings::link_skeleton_methods() {}

GameObject& ScriptBindings::game_object_self(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* object = runtime == nullptr ? nullptr : dynamic_cast<GameObject*>(runtime->resolve_id(ud->id, ud->world));
    if (object == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *object;
}

int ScriptBindings::game_object_get_bone_names(lua_State* state) {
    return lua_guard(state, [&] {
        const GameObject& object = game_object_self(state);
        const std::shared_ptr<const Skeleton> skeleton = prefab_skeleton(object, object.prefab_guid());
        const int count = skeleton != nullptr ? static_cast<int>(skeleton->bones.size()) : 0;
        lua_createtable(state, count, 0);
        for (int b = 0; b < count; ++b) {
            const std::string& name = skeleton->bones[static_cast<std::size_t>(b)].name;
            lua_pushlstring(state, name.data(), name.size());
            lua_rawseti(state, -2, b + 1);
        }
        return 1;
    });
}

int ScriptBindings::game_object_get_bone(lua_State* state) {
    return lua_guard(state, [&] {
        const GameObject& object = game_object_self(state);
        const InstanceId found = find_bone(object, luaL_checkstring(state, 2));
        if (found == 0) {
            lua_pushnil(state);
        } else {
            runtime_from(state)->push_instance(state, found);
        }
        return 1;
    });
}

int ScriptBindings::game_object_add_bone(lua_State* state) {
    return lua_guard(state, [&] {
        GameObject& object = game_object_self(state);
        const std::string name = luaL_checkstring(state, 2);
        const std::shared_ptr<const Skeleton> skeleton = prefab_skeleton(object, object.prefab_guid());
        if (skeleton == nullptr || skeleton->find(name) < 0) {
            luaL_error(state, "%s has no bone named %s", object.name(object.id()).c_str(), name.c_str());
        }
        if (find_bone(object, name) != 0) {
            luaL_error(state, "%s already has a Bone for %s; use GetBone", object.name(object.id()).c_str(),
                       name.c_str());
        }
        ScriptRuntime* runtime = runtime_from(state);
        DataModel* made = lua_create_instance(*runtime->game_, "Bone");
        if (made == nullptr) {
            luaL_error(state, "unknown class Bone");
        }
        runtime->game_->set_name(made->id(), name);
        runtime->game_->set_parent(made->id(), object.id());
        runtime->push_instance(state, made->id());
        return 1;
    });
}

}  // namespace engine_core
