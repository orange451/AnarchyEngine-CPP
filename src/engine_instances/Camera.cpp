#include "Camera.hpp"

#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

}  // namespace

std::optional<std::string> Camera::set_field_of_view(double degrees) {
    if (!on_gameplay_thread()) {
        contract_fail("set_field_of_view runs on SimulationThread");
    }
    if (!std::isfinite(degrees)) {
        return std::string("FieldOfView must be a finite number");
    }
    degrees = std::clamp(degrees, kMinFieldOfView, kMaxFieldOfView);
    if (degrees == field_of_view_) {
        return std::nullopt;
    }
    const double previous = field_of_view_;
    field_of_view_ = degrees;
    note_property_change("FieldOfView", number_slot(previous), number_slot(degrees));
    note_visual(VisualField::Camera);
    return std::nullopt;
}

void Camera::on_reuse() {
    GameObject::on_reuse();
    field_of_view_ = kDefaultFieldOfView;
}

namespace {

bool read_field_of_view(DataModel&, DataModel& object, LuaSlot& out) {
    auto* camera = dynamic_cast<Camera*>(&object);
    if (camera == nullptr) {
        return false;
    }
    out = number_slot(camera->field_of_view());
    return true;
}

bool write_field_of_view(DataModel&, DataModel& object, LuaSlot& in) {
    auto* camera = dynamic_cast<Camera*>(&object);
    if (camera == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = camera->set_field_of_view(in.number)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

ANARCHY_LUA_REGISTER(register_camera_lua) {
    // The default, as a file would hold it, from the class's own constant.
    static const std::string field_of_view = write_json(JsonValue::number(Camera::kDefaultFieldOfView));
    const LuaField fields[] = {
        lua_slider(lua_saved_property("FieldOfView", "number", read_field_of_view, write_field_of_view,
                                      field_of_view.c_str()),
                   Camera::kMinFieldOfView, Camera::kMaxFieldOfView),
    };
    register_lua_class("Camera", "GameObject", fields, 1);
}

}  // namespace

}  // namespace engine_core
