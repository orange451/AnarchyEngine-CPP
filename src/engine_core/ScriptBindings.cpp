#include "ScriptBindings.hpp"

#include "AssetInstances.hpp"
#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "Contract.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "Light.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "LuauSandbox.hpp"
#include "Matrix4.hpp"
#include "MeshShapes.hpp"
#include "ModuleScript.hpp"
#include "PhysicsObject.hpp"
#include "PropertyReflection.hpp"
#include "Script.hpp"
#include "SelectionService.hpp"
#include "Skybox.hpp"
#include "Dragger.hpp"
#include "SoundEmitter.hpp"
#include "UserInputService.hpp"
#include "Vector2.hpp"
#include "Vector3.hpp"

#include "lualib.h"
#include "luacode.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace engine_core {

int ScriptBindings::task_wait(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* thread = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || thread == nullptr || thread->co != state) {
            luaL_error(state, "task.wait yields the running script thread");
        }
        if (thread->dead) {
            luaL_error(state, "script is dead");
        }
        double dt = 0;
        if (lua_gettop(state) >= 1 && !lua_isnoneornil(state, 1)) {
            dt = luaL_checknumber(state, 1);
        }
        if (dt < 0) {
            dt = 0;
        }
        thread->park = ScriptRuntime::Thread::Park::Sleep;
        thread->due = thread->vm->clock + dt;
        thread->vm->sleep.push_back(thread);
        return lua_yield(state, 0);
    });
}

ScriptRuntime::Thread& ScriptBindings::task_caller(lua_State* state, const char* name) {
    ScriptRuntime::Thread* caller = ScriptRuntime::thread_from(state);
    if (caller == nullptr) {
        luaL_error(state, "%s runs inside a script", name);
    }
    return *caller;
}

ScriptRuntime::Thread& ScriptBindings::task_thread(lua_State* state, const ScriptRuntime::Thread& caller, int first) {
    luaL_checktype(state, first, LUA_TFUNCTION);
    ScriptRuntime::Thread& child = runtime_from(state)->new_thread(*caller.vm, caller.script, caller.generation);
    const int count = lua_gettop(state);
    for (int index = first; index <= count; ++index) {
        lua_pushvalue(state, index);
    }
    lua_xmove(state, child.co, count - first + 1);
    child.nargs = count - first;
    return child;
}

int ScriptBindings::push_task_handle(lua_State* state, const ScriptRuntime::Thread& thread) {
    auto* ud = static_cast<std::uint64_t*>(lua_newuserdata(state, sizeof(std::uint64_t)));
    *ud = thread.serial;
    luaL_getmetatable(state, kThreadMeta);
    lua_setmetatable(state, -2);
    return 1;
}

int ScriptBindings::task_spawn(lua_State* state) {
    return lua_guard(state, [&] {
        const ScriptRuntime::Thread& caller = task_caller(state, "task.spawn");
        ScriptRuntime::Thread& child = task_thread(state, caller, 1);
        runtime_from(state)->ready(child);
        return push_task_handle(state, child);
    });
}

int ScriptBindings::task_defer(lua_State* state) {
    return lua_guard(state, [&] {
        const ScriptRuntime::Thread& caller = task_caller(state, "task.defer");
        ScriptRuntime::Thread& child = task_thread(state, caller, 1);
        child.park = ScriptRuntime::Thread::Park::Defer;
        child.vm->defer.push_back(&child);
        return push_task_handle(state, child);
    });
}

int ScriptBindings::task_delay(lua_State* state) {
    return lua_guard(state, [&] {
        const ScriptRuntime::Thread& caller = task_caller(state, "task.delay");
        const double dt = luaL_checknumber(state, 1);
        ScriptRuntime::Thread& child = task_thread(state, caller, 2);
        child.park = ScriptRuntime::Thread::Park::Sleep;
        child.due = child.vm->clock + (dt < 0 ? 0 : dt);
        child.vm->sleep.push_back(&child);
        return push_task_handle(state, child);
    });
}

int ScriptBindings::task_cancel(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        const auto* ud = static_cast<const std::uint64_t*>(test_userdata(state, 1, kThreadMeta));
        if (runtime == nullptr || ud == nullptr) {
            luaL_error(state, "task.cancel expects a thread");
        }
        ScriptRuntime::Thread* thread = runtime->find_thread(*ud);
        if (thread == nullptr) {
            // It finished, and its thread is gone already.
            return 0;
        }
        thread->dead = true;
        thread->vm->ready.remove(thread);
        thread->vm->sleep.remove(thread);
        thread->vm->defer.remove(thread);
        runtime->forget_child_wait(*thread);
        if (ScriptRuntime::thread_from(state) == thread) {
            luaL_error(state, "cancelled");
        }
        return 0;
    });
}

namespace {

DataModel& create_game_object(DataModel& world) { return world.create<GameObject>(); }

DataModel& create_camera(DataModel& world) { return world.create<Camera>(); }

DataModel& create_point_light(DataModel& world) { return world.create<PointLight>(); }

DataModel& create_spot_light(DataModel& world) { return world.create<SpotLight>(); }

DataModel& create_directional_light(DataModel& world) { return world.create<DirectionalLight>(); }

DataModel& create_script(DataModel& world) { return world.create<Script>(); }

DataModel& create_module_script(DataModel& world) { return world.create<ModuleScript>(); }

DataModel& create_folder(DataModel& world) { return world.create<Folder>(); }

DataModel& create_physics_object(DataModel& world) { return world.create<PhysicsObject>(); }

DataModel& create_sound_emitter(DataModel& world) { return world.create<SoundEmitter>(); }
DataModel& create_dragger(DataModel& world) { return world.create<Dragger>(); }
DataModel& create_skybox(DataModel& world) { return world.create<Skybox>(); }
DataModel& create_screen_gui(DataModel& world) { return world.create<ScreenGui>(); }
DataModel& create_pane(DataModel& world) { return world.create<Pane>(); }
DataModel& create_hbox(DataModel& world) { return world.create<HBox>(); }
DataModel& create_vbox(DataModel& world) { return world.create<VBox>(); }
DataModel& create_label(DataModel& world) { return world.create<Label>(); }
DataModel& create_button(DataModel& world) { return world.create<Button>(); }
DataModel& create_text_field(DataModel& world) { return world.create<TextField>(); }
DataModel& create_css(DataModel& world) { return world.create<Css>(); }

DataModel& create_texture(DataModel& world) { return world.create<Texture>(); }
DataModel& create_mesh(DataModel& world) { return world.create<Mesh>(); }
DataModel& create_sound(DataModel& world) { return world.create<Sound>(); }
DataModel& create_material(DataModel& world) { return world.create<Material>(); }
DataModel& create_model(DataModel& world) { return world.create<Model>(); }
DataModel& create_prefab(DataModel& world) { return world.create<Prefab>(); }

// The factories stay here, which ScriptRuntime.cpp links, so each class's
// object file stays linked.
// Completion reads the same names Instance.new will construct.
ANARCHY_LUA_REGISTER(register_creatable_instances) {
    register_lua_creatable("GameObject", create_game_object);
    register_lua_creatable("Camera", create_camera);
    register_lua_creatable("PointLight", create_point_light);
    register_lua_creatable("SpotLight", create_spot_light);
    register_lua_creatable("DirectionalLight", create_directional_light);
    register_lua_creatable("Script", create_script);
    register_lua_creatable("ModuleScript", create_module_script);
    register_lua_creatable("Folder", create_folder);
    register_lua_creatable("PhysicsObject", create_physics_object);
    register_lua_creatable("SoundEmitter", create_sound_emitter);
    register_lua_creatable("Dragger", create_dragger);
    register_lua_creatable("Skybox", create_skybox);
    register_lua_creatable("ScreenGui", create_screen_gui);
    register_lua_creatable("Pane", create_pane);
    register_lua_creatable("HBox", create_hbox);
    register_lua_creatable("VBox", create_vbox);
    register_lua_creatable("Label", create_label);
    register_lua_creatable("Button", create_button);
    register_lua_creatable("TextField", create_text_field);
    register_lua_creatable("CSS", create_css);
    register_lua_creatable("Texture", create_texture);
    register_lua_creatable("Mesh", create_mesh);
    register_lua_creatable("Sound", create_sound);
    register_lua_creatable("Material", create_material);
    register_lua_creatable("Model", create_model);
    register_lua_creatable("Prefab", create_prefab);
}

}  // namespace

int ScriptBindings::instance_new(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr) {
            luaL_error(state, "Instance.new has no data model");
        }
        const char* name = luaL_checkstring(state, 1);
        // The second argument is the parent, as in Instance.new("Script", game).
        // Checked before create so a bad parent does not leave an instance behind.
        InstanceId parent_id = DataModel::kNoParent;
        if (lua_gettop(state) >= 2 && !lua_isnil(state, 2)) {
            auto* parent = static_cast<InstanceUd*>(luaL_checkudata(state, 2, kInstanceMeta));
            if (parent == nullptr || runtime->resolve_id(parent->id, parent->world) == nullptr) {
                luaL_error(state, "instance is gone");
            }
            parent_id = parent->id;
            // Checked by class, before create, for the same reason.
            if (lua_creatable_known(name)) {
                if (std::optional<std::string> refused = runtime->game_->placement_error_for_class(parent_id, name)) {
                    luaL_error(state, "%s", refused->c_str());
                }
            }
        }
        DataModel* created = lua_create_instance(*runtime->game_, name);
        if (created == nullptr) {
            luaL_error(state, "unknown class %s", name);
        }
        if (parent_id != DataModel::kNoParent) {
            runtime->game_->set_parent(created->id(), parent_id);
        }
        runtime->push_instance(state, created->id());
        return 1;
    });
}

int ScriptBindings::require(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        if (runtime == nullptr || ud == nullptr) {
            luaL_error(state, "require expects a ModuleScript");
        }
        DataModel* object = runtime->resolve_id(ud->id, ud->world);
        if (object == nullptr) {
            luaL_error(state, "instance is gone");
        }
        return runtime->require_module(state, object->id());
    });
}

int ScriptBindings::instance_tostring(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    const char* fallback = "Instance";
    if (runtime == nullptr || runtime->game_ == nullptr || ud == nullptr) {
        lua_pushstring(state, fallback);
        return 1;
    }
    DataModel* object = runtime->resolve_id(ud->id, ud->world);
    if (object == nullptr) {
        lua_pushstring(state, fallback);
        return 1;
    }
    const std::string name = runtime->game_->name(object->id());
    if (!name.empty()) {
        lua_pushlstring(state, name.data(), name.size());
        return 1;
    }
    const char* class_name = object->class_name();
    lua_pushstring(state, class_name != nullptr ? class_name : fallback);
    return 1;
}

void push_registered(lua_State* state, ScriptRuntime* runtime, const LuaSlot& slot, InstanceId id, std::uint32_t world) {
    switch (slot.kind) {
    case LuaSlot::Kind::Nil:
        lua_pushnil(state);
        return;
    case LuaSlot::Kind::Bool:
        lua_pushboolean(state, slot.flag ? 1 : 0);
        return;
    case LuaSlot::Kind::Number:
        lua_pushnumber(state, slot.number);
        return;
    case LuaSlot::Kind::String:
        lua_pushlstring(state, slot.text.data(), slot.text.size());
        return;
    case LuaSlot::Kind::Instance:
        runtime->push_instance(state, slot.id);
        return;
    case LuaSlot::Kind::Vec3:
        lua_pushvector(state, slot.vec.x, slot.vec.y, slot.vec.z);
        return;
    case LuaSlot::Kind::Vec2:
        push_vector2(state, Vec2{slot.vec.x, slot.vec.y});
        return;
    case LuaSlot::Kind::Color:
        push_color3(state, Color3{slot.color.r, slot.color.g, slot.color.b});
        return;
    case LuaSlot::Kind::Matrix4:
        push_matrix4(state, slot.transform);
        return;
    case LuaSlot::Kind::Enum:
        if (slot.enum_type == nullptr) {
            lua_pushnil(state);
            return;
        }
        push_enum_item(state, *slot.enum_type, static_cast<int>(slot.number));
        return;
    case LuaSlot::Kind::InputObject:
        push_input_object(state, slot.input);
        return;
    case LuaSlot::Kind::Signal: {
        auto* signal = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
        *signal = SignalUd{};
        signal->kind = kSignalChanged;
        signal->id = id;
        signal->world = world;
        luaL_getmetatable(state, kSignalMeta);
        lua_setmetatable(state, -2);
        return;
    }
    }
    lua_pushnil(state);
}

void push_method(lua_State* state, const LuaField& field) {
    if (field.call == nullptr) {
        lua_pushnil(state);
        return;
    }
    lua_pushcfunction(state, reinterpret_cast<lua_CFunction>(field.call), field.name);
}

int ScriptBindings::instance_index(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        DataModel* object = runtime->resolve_id(ud->id, ud->world);
        if (object == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        const LuaField* field = lua_class_find(object->class_name(), key != nullptr ? key : "");
        if (field == nullptr) {
            // Not a property or method: a child by that name, the first in sibling
            // order, like FindFirstChild. A property of the same name wins.
            const InstanceId child = runtime->game_->find_first_child(object->id(), key != nullptr ? key : "");
            if (child != 0 && !hidden_from_play(state, *runtime, child)) {
                runtime->push_instance(state, child);
                return 1;
            }
            const std::string name = runtime->game_->name(object->id());
            luaL_error(state, "%s is not a valid member of %s \"%s\"", key != nullptr ? key : "",
                       object->class_name() != nullptr ? object->class_name() : "Instance", name.c_str());
        }
        if (field->method) {
            push_method(state, *field);
            return 1;
        }
        if (field->event) {
            auto* signal = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
            *signal = SignalUd{};
            signal->kind = kSignalEvent;
            signal->id = object->id();
            signal->world = ud->world;
            signal->event_name = field->name;
            luaL_getmetatable(state, kSignalMeta);
            lua_setmetatable(state, -2);
            return 1;
        }
        LuaSlot slot;
        if (field->read == nullptr || !field->read(*runtime->game_, *object, slot)) {
            lua_pushnil(state);
            return 1;
        }
        push_registered(state, runtime, slot, object->id(), ud->world);
        return 1;
    });
}

int ScriptBindings::instance_newindex(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr) {
            luaL_error(state, "instance is gone");
        }
        DataModel* object = runtime->resolve_id(ud->id, ud->world);
        if (object == nullptr) {
            luaL_error(state, "instance is gone");
        }
        const LuaField* field = lua_class_find(object->class_name(), key != nullptr ? key : "");
        if (field == nullptr || !field->writable || field->write == nullptr || field->type_name == nullptr) {
            luaL_error(state, "cannot set %s", key);
        }
        LuaSlot slot;
        const std::string_view type = field->type_name;
        if (field->enum_type != nullptr) {
            // The EnumItem, its name, or its value; anything else raises.
            slot.kind = LuaSlot::Kind::Enum;
            slot.enum_type = field->enum_type;
            slot.number = check_enum_arg(state, 3, *field->enum_type);
        } else if (type == "string") {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 3, &length);
            slot.kind = LuaSlot::Kind::String;
            slot.text.assign(text != nullptr ? text : "", length);
        } else if (type == "boolean") {
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = lua_toboolean(state, 3) != 0;
        } else if (type == "number") {
            slot.kind = LuaSlot::Kind::Number;
            slot.number = luaL_checknumber(state, 3);
        } else if (type == "Instance" || type == "Instance?" || type == "DataModel" || type == "DataModel?" ||
                   !reference_class(type).empty()) {
            if (lua_isnil(state, 3)) {
                slot.kind = LuaSlot::Kind::Nil;
            } else {
                auto* parent = static_cast<InstanceUd*>(luaL_checkudata(state, 3, kInstanceMeta));
                if (runtime->resolve_id(parent->id, parent->world) == nullptr) {
                    luaL_error(state, "instance is gone");
                }
                slot.kind = LuaSlot::Kind::Instance;
                slot.id = parent->id;
            }
        } else if (type == "Vector3") {
            const float* components = lua_tovector(state, 3);
            if (components == nullptr) {
                luaL_error(state, "%s expects a Vector3", field->name);
            }
            slot.kind = LuaSlot::Kind::Vec3;
            slot.vec = Vec3{components[0], components[1], components[2]};
        } else if (type == "Vector2") {
            const Vec2* value = to_vector2(state, 3);
            if (value == nullptr) {
                luaL_error(state, "%s expects a Vector2", field->name);
            }
            slot.kind = LuaSlot::Kind::Vec2;
            slot.vec = Vec3{value->x, value->y, 0.f};
        } else if (type == "Color3") {
            if (!read_color3(state, 3, slot.color)) {
                luaL_error(state, "%s expects a Color3", field->name);
            }
            slot.kind = LuaSlot::Kind::Color;
        } else if (type == "Matrix4") {
            const Matrix4* value = to_matrix4(state, 3);
            if (value == nullptr) {
                luaL_error(state, "%s expects a Matrix4", field->name);
            }
            slot.kind = LuaSlot::Kind::Matrix4;
            slot.transform = *value;
        } else {
            luaL_error(state, "cannot set %s", key);
        }
        if (!field->write(*runtime->game_, *object, slot)) {
            luaL_error(state, "%s", slot.error.empty() ? "property is not available" : slot.error.c_str());
        }
        return 0;
    });
}

int ScriptBindings::instance_destroy(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->resolve_id(ud->id, ud->world) == nullptr) {
            luaL_error(state, "instance is gone");
        }
        if (const std::optional<std::string> error = runtime->game_->destroy_error(ud->id)) {
            luaL_error(state, "%s", error->c_str());
        }
        runtime->game_->destroy(ud->id);
        return 0;
    });
}

int ScriptBindings::instance_children(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->resolve_id(ud->id, ud->world) == nullptr) {
            lua_newtable(state);
            return 1;
        }
        const std::vector<InstanceId> children = runtime->game_->get_children(ud->id);
        lua_newtable(state);
        int index = 1;
        for (InstanceId child : children) {
            if (hidden_from_play(state, *runtime, child)) {
                continue;
            }
            runtime->push_instance(state, child);
            lua_rawseti(state, -2, index);
            ++index;
        }
        return 1;
    });
}

int ScriptBindings::instance_find(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* name = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->resolve_id(ud->id, ud->world) == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        const InstanceId child = runtime->game_->find_first_child(ud->id, name != nullptr ? name : "");
        if (child == 0 || hidden_from_play(state, *runtime, child)) {
            lua_pushnil(state);
        } else {
            runtime->push_instance(state, child);
        }
        return 1;
    });
}

int ScriptBindings::instance_wait_child(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* name = luaL_checkstring(state, 2);
        double timeout = -1;
        if (lua_gettop(state) >= 3 && !lua_isnoneornil(state, 3)) {
            timeout = luaL_checknumber(state, 3);
            if (timeout < 0) {
                timeout = 0;
            }
        }
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->resolve_id(ud->id, ud->world) == nullptr) {
            luaL_error(state, "WaitForChild on an instance that is gone");
        }
        const std::string wanted = name != nullptr ? name : "";
        const InstanceId child = runtime->game_->find_first_child(ud->id, wanted);
        if (child != 0 && !hidden_from_play(state, *runtime, child)) {
            runtime->push_instance(state, child);
            return 1;
        }
        ScriptRuntime::Thread* thread = ScriptRuntime::thread_from(state);
        if (thread == nullptr || thread->co != state) {
            luaL_error(state, "WaitForChild yields the running script thread");
        }
        if (thread->dead) {
            luaL_error(state, "script is dead");
        }
        constexpr double kInfiniteYieldNotice = 5.0;
        thread->wait_parent = ud->id;
        thread->wait_world = ud->world;
        thread->wait_name = wanted;
        thread->due = timeout < 0 ? std::numeric_limits<double>::infinity() : thread->vm->clock + timeout;
        thread->wait_warn_at = thread->vm->clock + kInfiniteYieldNotice;
        // A timeout means the caller expects nil back, so there is no notice.
        thread->wait_warned = timeout >= 0;
        runtime->park_child_wait(*thread);
        return lua_yield(state, 0);
    });
}

int ScriptBindings::instance_isa(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* name = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        DataModel* object = runtime != nullptr ? runtime->resolve_id(ud->id, ud->world) : nullptr;
        if (object == nullptr) {
            lua_pushboolean(state, 0);
            return 1;
        }
        lua_pushboolean(state, is_a(*object, name) ? 1 : 0);
        return 1;
    });
}

bool ScriptBindings::hidden_from_play(lua_State* state, ScriptRuntime& runtime, InstanceId id) {
    const ScriptRuntime::Vm* vm = runtime.vm_from(state);
    return vm != nullptr && vm->kind == ScriptRuntime::VmKind::Play && runtime.game_->core_holds(id);
}

int ScriptBindings::instance_service(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* name = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || ud->id != 0 || runtime->resolve_id(0, ud->world) == nullptr) {
            luaL_error(state, "GetService is on game");
        }
        // A service directly under game is in the tree: GetService gives the instance itself.
        const InstanceId found = name != nullptr ? runtime->game_->service(name) : 0;
        if (found != 0 && hidden_from_play(state, *runtime, found)) {
            luaL_error(state, "%s is not available to game scripts", name);
        }
        if (found != 0 && runtime->game_->parent(found) == 0) {
            runtime->push_instance(state, found);
            return 1;
        }
        const int kind = name != nullptr && lua_service_known(name) ? service_kind(name) : -1;
        if (kind < 0) {
            luaL_error(state, "unknown service");
        }
        auto* service = static_cast<ServiceUd*>(lua_newuserdata(state, sizeof(ServiceUd)));
        service->kind = kind;
        luaL_getmetatable(state, kServiceMeta);
        lua_setmetatable(state, -2);
        return 1;
    });
}

Signal& ScriptBindings::signal_of(lua_State* state, ScriptRuntime& runtime, const SignalUd& ud) {
    Signal* signal = nullptr;
    if (ud.kind == kSignalChanged) {
        if (runtime.resolve_id(ud.id, ud.world) == nullptr) {
            luaL_error(state, "instance is gone");
        }
        signal = &runtime.game_->changed(ud.id);
    } else if (ud.kind == kSignalEvent) {
        if (runtime.resolve_id(ud.id, ud.world) == nullptr) {
            luaL_error(state, "instance is gone");
        }
        signal = &runtime.game_->event_signal(ud.id, ud.event_name != nullptr ? ud.event_name : "");
    } else if (ud.kind == kSignalInput) {
        signal = runtime.game_->input().signal(static_cast<UserInputService::Kind>(ud.phase));
    } else if (ud.kind == kSignalHost) {
        signal = runtime.host_signal(static_cast<HostSignal>(ud.phase));
    } else {
        signal = runtime.run_service_.signal(static_cast<Phase>(ud.phase));
    }
    if (signal == nullptr) {
        luaL_error(state, "signal is not available");
    }
    return *signal;
}

int ScriptBindings::signal_connect(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<SignalUd*>(luaL_checkudata(state, 1, kSignalMeta));
        luaL_checktype(state, 2, LUA_TFUNCTION);
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* caller = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr || caller == nullptr) {
            luaL_error(state, "Connect runs inside a script");
        }
        if (ud->blocked) {
            luaL_error(state, "%s is not available to scripts", ud->blocked_name);
        }
        ScriptRuntime::Vm& vm = *caller->vm;
        if (!runtime->owner_ok(vm, caller->script, caller->generation)) {
            luaL_error(state, "script is dead");
        }
        lua_pushvalue(state, 2);
        const auto held = std::make_shared<const ScriptRuntime::HeldRef>(vm, lua_ref(state, -1));
        lua_pop(state, 1);
        Signal* signal = &signal_of(state, *runtime, *ud);
        const InstanceId script = caller->script;
        const std::uint32_t generation = caller->generation;
        // The handler owns the callback's reference; Disconnect drops the handler.
        Handler handler = [runtime, held, script, generation, kind = ud->kind,
                           phase = static_cast<Phase>(ud->phase)](InstanceId, Field field) {
            ScriptRuntime::Vm& owner = *held->vm;
            if (kind == kSignalChanged) {
                runtime->invoke_listener(owner, held->ref, script, generation,
                                         changed_name(field, runtime->game_->events().payload()), false, 0);
            } else if (kind == kSignalInput || kind == kSignalEvent || kind == kSignalHost) {
                runtime->invoke_listener_args(owner, held->ref, script, generation,
                                              runtime->game_->events().current_args());
            } else {
                runtime->invoke_listener(owner, held->ref, script, generation, nullptr, true,
                                         runtime->run_service_.dt(phase));
            }
        };
        // A play connection is tagged, so the queue's gate and Stop end it. The console's
        // and a plugin's outlive the play session; their VM ends them.
        Connection connection;
        if (vm.kind == ScriptRuntime::VmKind::Play) {
            connection = signal->connect_scripted(std::move(handler), script, generation, false);
        } else {
            connection = signal->connect_kept(std::move(handler), false);
            runtime->keep(vm, script, generation, connection);
        }
        auto* box = static_cast<Connection*>(lua_newuserdata(state, sizeof(Connection)));
        new (box) Connection(connection);
        luaL_getmetatable(state, kConnectionMeta);
        lua_setmetatable(state, -2);
        return 1;
    });
}

int ScriptBindings::signal_wait(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<SignalUd*>(luaL_checkudata(state, 1, kSignalMeta));
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* thread = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr || thread == nullptr) {
            luaL_error(state, "Wait yields the running script thread");
        }
        if (ud->blocked) {
            luaL_error(state, "%s is not available to scripts", ud->blocked_name);
        }
        Signal* signal = &signal_of(state, *runtime, *ud);
        const int kind = ud->kind;
        thread->park = ScriptRuntime::Thread::Park::Signal;
        // By serial: task.cancel can end the thread, and release it, before the signal fires.
        const Phase phase = static_cast<Phase>(ud->phase);
        Handler handler = [runtime, serial = thread->serial, kind, phase](InstanceId, Field field) {
            ScriptRuntime::Thread* waiting = runtime->find_thread(serial);
            if (waiting == nullptr || waiting->vm->closing || waiting->dead) {
                return;
            }
            runtime->guarded(*waiting->vm, [&] {
                if (kind == kSignalChanged) {
                    runtime->make_ready(*waiting, changed_name(field, runtime->game_->events().payload()));
                } else if (kind == kSignalInput || kind == kSignalEvent || kind == kSignalHost) {
                    runtime->make_ready_args(*waiting, runtime->game_->events().current_args());
                } else {
                    runtime->make_ready_number(*waiting, runtime->run_service_.dt(phase));
                }
            });
        };
        ScriptRuntime::Vm& vm = *thread->vm;
        if (vm.kind == ScriptRuntime::VmKind::Play) {
            signal->connect_scripted(std::move(handler), thread->script, thread->generation, true);
        } else {
            runtime->keep(vm, thread->script, thread->generation, signal->connect_kept(std::move(handler), true));
        }
        return lua_yield(state, 0);
    });
}

int ScriptBindings::connection_disconnect(lua_State* state) {
    return lua_guard(state, [&] {
        auto* connection = static_cast<Connection*>(luaL_checkudata(state, 1, kConnectionMeta));
        connection->disconnect();
        return 0;
    });
}

int ScriptBindings::connection_index(lua_State* state) {
    const char* key = luaL_checkstring(state, 2);
    const LuaField* field = lua_class_find("Connection", key != nullptr ? key : "");
    if (field == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    if (field->method) {
        push_method(state, *field);
        return 1;
    }
    if (field->tag == 1) {
        auto* connection = static_cast<Connection*>(luaL_checkudata(state, 1, kConnectionMeta));
        lua_pushboolean(state, connection->connected() ? 1 : 0);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

int ScriptBindings::signal_index(lua_State* state) {
    const char* key = luaL_checkstring(state, 2);
    const LuaField* field = lua_class_find("Signal", key != nullptr ? key : "");
    if (field == nullptr || !field->method) {
        lua_pushnil(state);
        return 1;
    }
    push_method(state, *field);
    return 1;
}

int ScriptBindings::service_index(lua_State* state) {
    const auto* service = static_cast<ServiceUd*>(luaL_checkudata(state, 1, kServiceMeta));
    const char* key = luaL_checkstring(state, 2);
    const char* class_name = service->kind >= 0 && service->kind < kServiceKinds ? kServiceClasses[service->kind] : "";
    const LuaField* field = lua_class_find(class_name, key != nullptr ? key : "");
    if (field == nullptr) {
        luaL_error(state, "unknown %s member", class_name);
    }
    if (field->method) {
        push_method(state, *field);
        return 1;
    }
    if (service->kind == kUserInputServiceKind && std::strcmp(field->name, "MouseBehavior") == 0) {
        ScriptRuntime* runtime = runtime_from(state);
        const int behavior = runtime != nullptr && runtime->game_ != nullptr ? runtime->game_->input().mouse_behavior()
                                                                             : UserInputService::kMouseBehaviorDefault;
        push_enum_item(state, mouse_behavior_enum(), behavior);
        return 1;
    }
    if (field->read != nullptr) {
        ScriptRuntime* runtime = runtime_from(state);
        LuaSlot slot;
        if (runtime == nullptr || runtime->game_ == nullptr || !field->read(*runtime->game_, *runtime->game_, slot)) {
            lua_pushnil(state);
            return 1;
        }
        push_registered(state, runtime, slot, 0, runtime->game_->world_generation());
        return 1;
    }
    auto* ud = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
    *ud = SignalUd{};
    if (field->host_signal) {
        ud->kind = kSignalHost;
    } else {
        ud->kind = service->kind == kUserInputServiceKind ? kSignalInput : kSignalPhase;
    }
    ud->phase = field->tag;
    ud->blocked = field->blocked;
    if (field->blocked) {
        ud->blocked_name = field->name != nullptr ? field->name : "signal";
    }
    luaL_getmetatable(state, kSignalMeta);
    lua_setmetatable(state, -2);
    return 1;
}

int ScriptBindings::service_newindex(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* service = static_cast<ServiceUd*>(luaL_checkudata(state, 1, kServiceMeta));
        const char* key = luaL_checkstring(state, 2);
        const char* class_name =
            service->kind >= 0 && service->kind < kServiceKinds ? kServiceClasses[service->kind] : "";
        const LuaField* field = lua_class_find(class_name, key != nullptr ? key : "");
        if (field == nullptr || field->method || !field->writable) {
            luaL_error(state, "%s cannot be assigned to", key != nullptr ? key : "");
        }
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr) {
            luaL_error(state, "%s is not available here", class_name);
        }
        if (service->kind == kUserInputServiceKind && std::strcmp(field->name, "MouseBehavior") == 0) {
            runtime->game_->input().set_mouse_behavior(check_enum_arg(state, 3, mouse_behavior_enum()));
            return 0;
        }
        LuaSlot slot;
        if (lua_isnumber(state, 3)) {
            slot.kind = LuaSlot::Kind::Number;
            slot.number = lua_tonumber(state, 3);
        } else if (lua_isboolean(state, 3)) {
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = lua_toboolean(state, 3) != 0;
        }
        if (field->write == nullptr || !field->write(*runtime->game_, *runtime->game_, slot)) {
            luaL_error(state, "%s", slot.error.empty() ? "invalid value" : slot.error.c_str());
        }
        return 0;
    });
}

int ScriptBindings::selection_get(lua_State* state) {
    return lua_guard(state, [&] {
        luaL_checkudata(state, 1, kServiceMeta);
        ScriptRuntime* runtime = runtime_from(state);
        lua_newtable(state);
        if (runtime == nullptr || runtime->game_ == nullptr) {
            return 1;
        }
        // The list may still name an instance destroyed since it was set.
        int index = 1;
        for (InstanceId id : runtime->game_->selection().get()) {
            if (runtime->game_->instance(id) == nullptr) {
                continue;
            }
            runtime->push_instance(state, id);
            lua_rawseti(state, -2, index);
            ++index;
        }
        return 1;
    });
}

int ScriptBindings::selection_set(lua_State* state) {
    return lua_guard(state, [&] {
        luaL_checkudata(state, 1, kServiceMeta);
        luaL_checktype(state, 2, LUA_TTABLE);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->game_ == nullptr) {
            luaL_error(state, "Selection is not available");
        }
        std::vector<InstanceId> ids;
        const int count = lua_objlen(state, 2);
        ids.reserve(static_cast<std::size_t>(count));
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(state, 2, i);
            const auto* ud = static_cast<InstanceUd*>(test_userdata(state, -1, kInstanceMeta));
            if (ud == nullptr) {
                luaL_error(state, "Set takes a list of instances");
            }
            // A gone instance, or one from before a Stop, cannot be selected.
            if (ud->id != 0 && runtime->resolve_id(ud->id, ud->world) != nullptr) {
                ids.push_back(ud->id);
            }
            lua_pop(state, 1);
        }
        runtime->game_->selection().set(std::move(ids));
        return 0;
    });
}

namespace {

// A size the shape methods take: a number above 0.
float shape_size(lua_State* state, int index, const char* what) {
    const double value = luaL_checknumber(state, index);
    if (!(value > 0.0) || !std::isfinite(value)) {
        luaL_error(state, "%s must be a number above 0", what);
    }
    return static_cast<float>(value);
}

// An optional Vector3 argument; nil or none is the origin.
Vec3 shape_position(lua_State* state, int index) {
    if (lua_isnoneornil(state, index)) {
        return Vec3{};
    }
    const float* components = lua_tovector(state, index);
    if (components == nullptr) {
        luaL_error(state, "position must be a Vector3");
    }
    return Vec3{components[0], components[1], components[2]};
}

int shape_segments(lua_State* state, int index, int fallback) {
    return lua_isnoneornil(state, index) ? fallback : static_cast<int>(luaL_checkinteger(state, index));
}

bool shape_flag(lua_State* state, int index, bool fallback) {
    return lua_isnoneornil(state, index) ? fallback : lua_toboolean(state, index) != 0;
}

void edit_mesh(lua_State* state, Mesh& mesh, const std::function<void(anarchy::amesh::Data&)>& edit) {
    if (const std::optional<std::string> error = mesh.edit_geometry(edit)) {
        luaL_error(state, "%s", error->c_str());
    }
}

}  // namespace

Mesh& ScriptBindings::mesh_self(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* mesh = runtime == nullptr ? nullptr : dynamic_cast<Mesh*>(runtime->resolve_id(ud->id, ud->world));
    if (mesh == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *mesh;
}

SoundEmitter& ScriptBindings::emitter_self(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* emitter = runtime == nullptr ? nullptr : dynamic_cast<SoundEmitter*>(runtime->resolve_id(ud->id, ud->world));
    if (emitter == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *emitter;
}

int ScriptBindings::emitter_play(lua_State* state) {
    return lua_guard(state, [&] {
        emitter_self(state).play();
        return 0;
    });
}

int ScriptBindings::emitter_stop(lua_State* state) {
    return lua_guard(state, [&] {
        emitter_self(state).stop();
        return 0;
    });
}

int ScriptBindings::mesh_add_box(lua_State* state) {
    return lua_guard(state, [&] {
        Mesh& mesh = mesh_self(state);
        const float* size = lua_tovector(state, 2);
        if (size == nullptr || !(size[0] > 0.f && size[1] > 0.f && size[2] > 0.f) ||
            !(std::isfinite(size[0]) && std::isfinite(size[1]) && std::isfinite(size[2]))) {
            luaL_error(state, "size must be a Vector3 above 0 on every axis");
        }
        const Vec3 extent{size[0], size[1], size[2]};
        const Vec3 at = shape_position(state, 3);
        edit_mesh(state, mesh, [&](anarchy::amesh::Data& data) { add_box(data, extent, at); });
        return 0;
    });
}

int ScriptBindings::mesh_add_sphere(lua_State* state) {
    return lua_guard(state, [&] {
        Mesh& mesh = mesh_self(state);
        const float radius = shape_size(state, 2, "radius");
        const int segments = shape_segments(state, 3, 24);
        const Vec3 at = shape_position(state, 4);
        edit_mesh(state, mesh, [&](anarchy::amesh::Data& data) { add_sphere(data, radius, segments, at); });
        return 0;
    });
}

int ScriptBindings::mesh_add_cylinder(lua_State* state) {
    return lua_guard(state, [&] {
        Mesh& mesh = mesh_self(state);
        const float radius = shape_size(state, 2, "radius");
        const float height = shape_size(state, 3, "height");
        const int segments = shape_segments(state, 4, 16);
        const bool capped = shape_flag(state, 5, true);
        const Vec3 at = shape_position(state, 6);
        edit_mesh(state, mesh,
                  [&](anarchy::amesh::Data& data) { add_cylinder(data, radius, height, segments, capped, at); });
        return 0;
    });
}

int ScriptBindings::mesh_add_cone(lua_State* state) {
    return lua_guard(state, [&] {
        Mesh& mesh = mesh_self(state);
        const float radius = shape_size(state, 2, "radius");
        const float height = shape_size(state, 3, "height");
        const int segments = shape_segments(state, 4, 16);
        const bool capped = shape_flag(state, 5, true);
        const Vec3 at = shape_position(state, 6);
        edit_mesh(state, mesh, [&](anarchy::amesh::Data& data) { add_cone(data, radius, height, segments, capped, at); });
        return 0;
    });
}

int ScriptBindings::mesh_add_plane(lua_State* state) {
    return lua_guard(state, [&] {
        Mesh& mesh = mesh_self(state);
        const float width = shape_size(state, 2, "width");
        const float depth = shape_size(state, 3, "depth");
        const Vec3 at = shape_position(state, 4);
        edit_mesh(state, mesh, [&](anarchy::amesh::Data& data) { add_plane(data, width, depth, at); });
        return 0;
    });
}

int ScriptBindings::mesh_add_teapot(lua_State* state) {
    return lua_guard(state, [&] {
        Mesh& mesh = mesh_self(state);
        const float size = shape_size(state, 2, "size");
        const Vec3 at = shape_position(state, 3);
        edit_mesh(state, mesh, [&](anarchy::amesh::Data& data) { add_teapot(data, size, at); });
        return 0;
    });
}

int ScriptBindings::mesh_clear(lua_State* state) {
    return lua_guard(state, [&] {
        Mesh& mesh = mesh_self(state);
        edit_mesh(state, mesh, [](anarchy::amesh::Data& data) { data = anarchy::amesh::Data{}; });
        return 0;
    });
}

UserInputService* ScriptBindings::input_service(lua_State* state) {
    luaL_checkudata(state, 1, kServiceMeta);
    ScriptRuntime* runtime = runtime_from(state);
    if (runtime == nullptr || runtime->game_ == nullptr) {
        return nullptr;
    }
    return &runtime->game_->input();
}

int ScriptBindings::input_is_key_down(lua_State* state) {
    return lua_guard(state, [&] {
        UserInputService* input = input_service(state);
        const int key = check_enum_arg(state, 2, key_code_enum());
        lua_pushboolean(state, input != nullptr && input->key_down(key) ? 1 : 0);
        return 1;
    });
}

int ScriptBindings::input_is_mouse_button_pressed(lua_State* state) {
    return lua_guard(state, [&] {
        UserInputService* input = input_service(state);
        const int type = check_enum_arg(state, 2, user_input_type_enum());
        const int button = type - UserInputService::kMouseButton1;
        lua_pushboolean(state, input != nullptr && input->button_down(button) ? 1 : 0);
        return 1;
    });
}

int ScriptBindings::input_get_keys_pressed(lua_State* state) {
    return lua_guard(state, [&] {
        UserInputService* input = input_service(state);
        lua_newtable(state);
        if (input == nullptr) {
            return 1;
        }
        const Vec3 mouse = input->mouse_location();
        int index = 1;
        for (int key : input->keys_down()) {
            InputRecord record;
            record.type = UserInputService::kKeyboard;
            record.state = UserInputService::kBegin;
            record.key = key;
            record.position = mouse;
            push_input_object(state, record);
            lua_rawseti(state, -2, index);
            ++index;
        }
        return 1;
    });
}

int ScriptBindings::input_get_mouse_buttons_pressed(lua_State* state) {
    return lua_guard(state, [&] {
        UserInputService* input = input_service(state);
        lua_newtable(state);
        if (input == nullptr) {
            return 1;
        }
        int index = 1;
        for (int button = 0; button < 3; ++button) {
            if (!input->button_down(button)) {
                continue;
            }
            InputRecord record;
            record.type = UserInputService::kMouseButton1 + button;
            record.state = UserInputService::kBegin;
            record.position = input->mouse_location();
            push_input_object(state, record);
            lua_rawseti(state, -2, index);
            ++index;
        }
        return 1;
    });
}

int ScriptBindings::input_get_mouse_location(lua_State* state) {
    return lua_guard(state, [&] {
        UserInputService* input = input_service(state);
        const Vec3 mouse = input != nullptr ? input->mouse_location() : Vec3{};
        push_vector2(state, Vec2{mouse.x, mouse.y});
        return 1;
    });
}

int ScriptBindings::input_get_mouse_delta(lua_State* state) {
    return lua_guard(state, [&] {
        UserInputService* input = input_service(state);
        Vec2 delta{0.f, 0.f};
        if (input != nullptr) {
            const Vec3 raw = input->mouse_delta();
            const float scale = static_cast<float>(input->mouse_delta_sensitivity());
            delta = Vec2{raw.x * scale, raw.y * scale};
        }
        push_vector2(state, delta);
        return 1;
    });
}

int ScriptBindings::run_is_running(lua_State* state) {
    return lua_guard(state, [&] {
        luaL_checkudata(state, 1, kServiceMeta);
        ScriptRuntime* runtime = runtime_from(state);
        lua_pushboolean(state, runtime != nullptr && runtime->vm_open() ? 1 : 0);
        return 1;
    });
}

int ScriptBindings::input_object_index(lua_State* state) {
    const auto* record = static_cast<const InputRecord*>(luaL_checkudata(state, 1, kInputObjectMeta));
    const char* key = luaL_checkstring(state, 2);
    const std::string_view name = key != nullptr ? key : "";
    if (name == "KeyCode") {
        push_enum_item(state, key_code_enum(), record->key);
    } else if (name == "UserInputType") {
        push_enum_item(state, user_input_type_enum(), record->type);
    } else if (name == "UserInputState") {
        push_enum_item(state, user_input_state_enum(), record->state);
    } else if (name == "Position") {
        lua_pushvector(state, record->position.x, record->position.y, record->position.z);
    } else if (name == "Delta") {
        lua_pushvector(state, record->delta.x, record->delta.y, record->delta.z);
    } else {
        luaL_error(state, "%s is not a valid member of InputObject", key != nullptr ? key : "");
    }
    return 1;
}

int ScriptBindings::input_object_tostring(lua_State* state) {
    luaL_checkudata(state, 1, kInputObjectMeta);
    lua_pushstring(state, "InputObject");
    return 1;
}

int ScriptBindings::thread_index(lua_State* state) {
    lua_pushnil(state);
    return 1;
}

ANARCHY_LUA_REGISTER(note_task_library) { lua_note_host_library("task"); }

ANARCHY_LUA_REGISTER(register_script_methods) {
    LuaField get_service =
        lua_method("GetService", "", reinterpret_cast<void*>(&ScriptBindings::instance_service), true, false, false);
    get_service.service_arg = true;
    const LuaField methods[] = {
        lua_method("Destroy", "nil", reinterpret_cast<void*>(&ScriptBindings::instance_destroy)),
        lua_method("GetChildren", "Instance", reinterpret_cast<void*>(&ScriptBindings::instance_children), false, false, true),
        lua_method("FindFirstChild", "Instance?", reinterpret_cast<void*>(&ScriptBindings::instance_find), false, true, false),
        lua_method("WaitForChild", "Instance", reinterpret_cast<void*>(&ScriptBindings::instance_wait_child), false, true,
                   false),
        lua_method("IsA", "boolean", reinterpret_cast<void*>(&ScriptBindings::instance_isa)),
    };
    register_lua_class("DataModel", nullptr, methods, 5);
    // Services hang off game alone. Game.cpp declares the class.
    register_lua_class("Game", nullptr, &get_service, 1);

    LuaField connect =
        lua_method("Connect", "Connection", reinterpret_cast<void*>(&ScriptBindings::signal_connect));
    connect.callback_arg = true;
    const LuaField signal[] = {
        connect,
        lua_method("Wait", "nil", reinterpret_cast<void*>(&ScriptBindings::signal_wait)),
    };
    register_lua_class("Signal", nullptr, signal, 2);

    LuaField connected = lua_property("Connected", "boolean", false, nullptr, nullptr);
    connected.tag = 1;
    const LuaField connection[] = {
        lua_method("Disconnect", "nil", reinterpret_cast<void*>(&ScriptBindings::connection_disconnect)),
        connected,
    };
    register_lua_class("Connection", nullptr, connection, 2);

    // SelectionService.cpp declares the class and the service.
    const LuaField selection[] = {
        lua_method("Get", "Instance", reinterpret_cast<void*>(&ScriptBindings::selection_get), false, false, true),
        lua_method("Set", "nil", reinterpret_cast<void*>(&ScriptBindings::selection_set)),
    };
    register_lua_class("Selection", nullptr, selection, 2);

    // AssetInstances.cpp declares the class and its Path.
    const LuaField mesh[] = {
        lua_method("AddBox", "nil", reinterpret_cast<void*>(&ScriptBindings::mesh_add_box)),
        lua_method("AddSphere", "nil", reinterpret_cast<void*>(&ScriptBindings::mesh_add_sphere)),
        lua_method("AddCylinder", "nil", reinterpret_cast<void*>(&ScriptBindings::mesh_add_cylinder)),
        lua_method("AddCone", "nil", reinterpret_cast<void*>(&ScriptBindings::mesh_add_cone)),
        lua_method("AddPlane", "nil", reinterpret_cast<void*>(&ScriptBindings::mesh_add_plane)),
        lua_method("AddTeapot", "nil", reinterpret_cast<void*>(&ScriptBindings::mesh_add_teapot)),
        lua_method("Clear", "nil", reinterpret_cast<void*>(&ScriptBindings::mesh_clear)),
    };
    register_lua_class("Mesh", nullptr, mesh, static_cast<int>(sizeof(mesh) / sizeof(mesh[0])));

    // SoundEmitter.cpp declares the class and its properties.
    const LuaField emitter[] = {
        lua_method("Play", "nil", reinterpret_cast<void*>(&ScriptBindings::emitter_play)),
        lua_method("Stop", "nil", reinterpret_cast<void*>(&ScriptBindings::emitter_stop)),
    };
    register_lua_class("SoundEmitter", nullptr, emitter, static_cast<int>(sizeof(emitter) / sizeof(emitter[0])));

    // UserInputService.cpp declares the class, its signals, and the service.
    const LuaField input[] = {
        lua_method("IsKeyDown", "boolean", reinterpret_cast<void*>(&ScriptBindings::input_is_key_down)),
        lua_method("IsMouseButtonPressed", "boolean",
                   reinterpret_cast<void*>(&ScriptBindings::input_is_mouse_button_pressed)),
        lua_method("GetKeysPressed", "InputObject", reinterpret_cast<void*>(&ScriptBindings::input_get_keys_pressed),
                   false, false, true),
        lua_method("GetMouseButtonsPressed", "InputObject",
                   reinterpret_cast<void*>(&ScriptBindings::input_get_mouse_buttons_pressed), false, false, true),
        lua_method("GetMouseLocation", "Vector2", reinterpret_cast<void*>(&ScriptBindings::input_get_mouse_location)),
        lua_method("GetMouseDelta", "Vector2", reinterpret_cast<void*>(&ScriptBindings::input_get_mouse_delta)),
    };
    register_lua_class("UserInputService", nullptr, input, static_cast<int>(sizeof(input) / sizeof(input[0])));

    // RunService.cpp declares the class, its signals, and the service.
    const LuaField run[] = {
        lua_method("IsRunning", "boolean", reinterpret_cast<void*>(&ScriptBindings::run_is_running)),
    };
    register_lua_class("RunService", nullptr, run, 1);
}

}  // namespace engine_core
