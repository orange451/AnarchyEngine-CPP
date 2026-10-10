#include "PlayerController.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <cmath>
#include <iterator>

namespace engine_core {

using namespace physics_detail;

Matrix4 upright_transform(const Matrix4& transform) {
    const float* x = transform.m;
    const float* z = transform.m + 8;
    double yaw = 0.0;
    if (std::hypot(z[0], z[2]) > 1e-6) {
        // A turn of yaw about Y takes +Z to (sin yaw, 0, cos yaw).
        yaw = std::atan2(z[0], z[2]);
    } else if (std::hypot(x[0], x[2]) > 1e-6) {
        // And +X to (cos yaw, 0, -sin yaw).
        yaw = std::atan2(-x[2], x[0]);
    }
    Matrix4 out = matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, yaw);
    out.m[12] = transform.m[12];
    out.m[13] = transform.m[13];
    out.m[14] = transform.m[14];
    return out;
}

std::optional<std::string> PlayerController::set_transform(const Matrix4& transform) {
    for (float value : transform.m) {
        if (!std::isfinite(value)) {
            return std::string("Transform must be finite");
        }
    }
    return PhysicsBase::set_transform(upright_transform(transform));
}

std::optional<std::string> PlayerController::set_friction(double friction) {
    return set_number("Friction", friction_, std::isfinite(friction) ? std::max(friction, 0.0) : friction, 0);
}

std::optional<std::string> PlayerController::set_radius(double radius) {
    return set_number("Radius", radius_, std::isfinite(radius) ? std::max(radius, double(kMinSize)) : radius,
                      kDirtyShape);
}

std::optional<std::string> PlayerController::set_height(double height) {
    return set_number("Height", height_, std::isfinite(height) ? std::max(height, double(kMinSize)) : height,
                      kDirtyShape);
}

std::optional<std::string> PlayerController::set_step_height(double step_height) {
    return set_number("StepHeight", step_height_,
                      std::isfinite(step_height) ? std::max(step_height, 0.0) : step_height, kDirtyShape);
}

std::optional<std::string> PlayerController::set_max_slope(double degrees) {
    return set_number("MaxSlope", max_slope_,
                      std::isfinite(degrees) ? std::min(std::max(degrees, 0.0), kMaxSlopeLimit) : degrees, 0);
}

void PlayerController::on_reuse() {
    PhysicsBase::on_reuse();
    friction_ = kDefaultFriction;
    radius_ = kDefaultRadius;
    height_ = kDefaultHeight;
    step_height_ = kDefaultStepHeight;
    max_slope_ = kDefaultMaxSlope;
    on_ground_ = false;
    sliding_ = false;
}

namespace {

template <bool (PlayerController::*Get)() const>
bool read_flag(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* body = dynamic_cast<const PlayerController*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = bool_slot((body->*Get)());
    return true;
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_player_controller_lua) {
    using C = PlayerController;
    static const std::string friction = number_json(C::kDefaultFriction);
    static const std::string radius = number_json(C::kDefaultRadius);
    static const std::string height = number_json(C::kDefaultHeight);
    static const std::string step = number_json(C::kDefaultStepHeight);
    static const std::string slope = number_json(C::kDefaultMaxSlope);
    const LuaField fields[] = {
        lua_group("Physics"),
        lua_saved_property("Friction", "number", read_number<C, &C::friction>, write_number<C, &C::set_friction>,
                           friction.c_str()),
        lua_group("Character"),
        lua_saved_property("Radius", "number", read_number<C, &C::radius>, write_number<C, &C::set_radius>,
                           radius.c_str()),
        lua_saved_property("Height", "number", read_number<C, &C::height>, write_number<C, &C::set_height>,
                           height.c_str()),
        lua_saved_property("StepHeight", "number", read_number<C, &C::step_height>,
                           write_number<C, &C::set_step_height>, step.c_str()),
        lua_slider(lua_saved_property("MaxSlope", "number", read_number<C, &C::max_slope>,
                                      write_number<C, &C::set_max_slope>, slope.c_str()),
                   0.0, C::kMaxSlopeLimit),
        lua_property("OnGround", "boolean", false, read_flag<&C::on_ground>, nullptr),
        lua_property("IsSliding", "boolean", false, read_flag<&C::is_sliding>, nullptr),
    };
    register_lua_class("PlayerController", "PhysicsBase", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("PlayerController", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
