#include "SceneService.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <cctype>
#include <cmath>
#include <string>

namespace engine_core {

void Service::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Paste, false});
}

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

namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

}  // namespace

std::optional<std::string> Workspace::set_gravity(double value) {
    if (!on_gameplay_thread()) {
        contract_fail("Workspace setters run on SimulationThread");
    }
    if (!std::isfinite(value)) {
        return std::string("Gravity must be a finite number");
    }
    if (gravity_ == value) {
        return std::nullopt;
    }
    const double previous = gravity_;
    gravity_ = value;
    note_property_change("Gravity", number_slot(previous), number_slot(value));
    return std::nullopt;
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

bool read_gravity(DataModel&, DataModel& object, LuaSlot& out) {
    auto* workspace = dynamic_cast<Workspace*>(&object);
    if (workspace == nullptr) {
        return false;
    }
    out = number_slot(workspace->gravity());
    return true;
}

bool write_gravity(DataModel&, DataModel& object, LuaSlot& in) {
    auto* workspace = dynamic_cast<Workspace*>(&object);
    if (workspace == nullptr) {
        return false;
    }
    if (const auto error = workspace->set_gravity(in.number)) {
        in.error = *error;
        return false;
    }
    return true;
}

ANARCHY_LUA_REGISTER(register_scene_service_lua) {
    register_lua_class("Service", "DataModel", nullptr, 0);
    register_lua_class("SceneService", "Service", nullptr, 0);
    // The default, as a file would hold it, from the class's own constant.
    static const std::string gravity = write_json(JsonValue::number(Workspace::kDefaultGravity));
    const LuaField workspace[] = {
        lua_property("CurrentCamera", "Camera?", true, read_current_camera, write_current_camera),
        lua_saved_property("Gravity", "number", read_gravity, write_gravity, gravity.c_str()),
    };
    register_lua_class("Workspace", "SceneService", workspace, static_cast<int>(sizeof(workspace) / sizeof(workspace[0])));
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
