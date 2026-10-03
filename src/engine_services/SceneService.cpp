#include "SceneService.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"

#include <cctype>

namespace engine_core {

void Service::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Paste, false});
}

bool is_scene_service_class(std::string_view class_name) {
    for (const char* name : kSceneServiceClasses) {
        if (class_name == name) {
            return true;
        }
    }
    return false;
}

std::string scene_service_guid(std::string_view class_name) { return service_guid(class_name); }

const char* Workspace::class_name() const { return "Workspace"; }

InstanceId Workspace::current_camera() const {
    if (current_camera_ == 0 || !alive(current_camera_)) {
        return 0;
    }
    const DataModel* target = instance(current_camera_);
    return target != nullptr && lua_class_inherits(target->class_name(), "Camera") ? current_camera_ : 0;
}

bool Workspace::set_current_camera(InstanceId id) {
    if (id != 0) {
        const DataModel* target = alive(id) ? instance(id) : nullptr;
        if (target == nullptr || !lua_class_inherits(target->class_name(), "Camera")) {
            return false;
        }
    }
    if (id == current_camera_) {
        return true;
    }
    current_camera_ = id;
    emit_property("CurrentCamera");
    return true;
}

const char* Storage::class_name() const { return "Storage"; }

const char* Scripts::class_name() const { return "Scripts"; }

const char* GuiService::class_name() const { return "Gui"; }
const char* Core::class_name() const { return kCoreClass; }

namespace {

bool read_current_camera(DataModel&, DataModel& object, LuaSlot& out) {
    auto* workspace = dynamic_cast<Workspace*>(&object);
    if (workspace == nullptr) {
        return false;
    }
    out.id = workspace->current_camera();
    out.kind = out.id != 0 ? LuaSlot::Kind::Instance : LuaSlot::Kind::Nil;
    return true;
}

// Not an edit: no history, no change to the place.
bool write_current_camera(DataModel&, DataModel& object, LuaSlot& in) {
    auto* workspace = dynamic_cast<Workspace*>(&object);
    if (workspace == nullptr) {
        return false;
    }
    const bool shaped = in.kind == LuaSlot::Kind::Nil || in.kind == LuaSlot::Kind::Instance;
    if (!shaped || !workspace->set_current_camera(in.kind == LuaSlot::Kind::Instance ? in.id : 0)) {
        in.error = "CurrentCamera must be a Camera";
        return false;
    }
    return true;
}

ANARCHY_LUA_REGISTER(register_scene_service_lua) {
    register_lua_class("Service", "DataModel", nullptr, 0);
    register_lua_class("SceneService", "Service", nullptr, 0);
    const LuaField workspace[] = {
        lua_property("CurrentCamera", "Camera?", true, read_current_camera, write_current_camera),
    };
    register_lua_class("Workspace", "SceneService", workspace, 1);
    register_lua_class("Storage", "SceneService", nullptr, 0);
    register_lua_class("Scripts", "SceneService", nullptr, 0);
    register_lua_class("Gui", "SceneService", nullptr, 0);
    // Not a registered service: completion does not offer it to GetService.
    register_lua_class("Core", "Service", nullptr, 0);
    // So completion offers them to GetService.
    for (const char* name : kSceneServiceClasses) {
        register_lua_service(name);
    }
}

}  // namespace

}  // namespace engine_core
