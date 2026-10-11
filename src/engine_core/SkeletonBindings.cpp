// GameObject's bone methods. Every binding runs on SimulationThread, as every
// script does. GetBone makes the Bone for a bone the first time it is asked.

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

// The Bone child for the bone called name, made the first time it is asked
// for: an ordinary child, saved with the place and undone as any instance
// made. nil when the skeleton has no such bone.
int ScriptBindings::game_object_get_bone(lua_State* state) {
    return lua_guard(state, [&] {
        GameObject& object = game_object_self(state);
        const std::string name = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (const InstanceId found = find_bone(object, name); found != 0) {
            runtime->push_instance(state, found);
            return 1;
        }
        const std::shared_ptr<const Skeleton> skeleton = prefab_skeleton(object, object.prefab_guid());
        if (skeleton == nullptr || skeleton->find(name) < 0) {
            lua_pushnil(state);
            return 1;
        }
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
