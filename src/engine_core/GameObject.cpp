#include "GameObject.hpp"

#include "LuaApi.hpp"

#include <cstring>
#include <type_traits>

namespace engine_core {

void GameObject::set_transform(const Transform& transform) { apply_transform(id_, transform, false); }

void GameObject::set_transform(const Transform& transform, ForceSimWrite) { apply_transform(id_, transform, true); }

void GameObject::set_color(ColorRgb color) { apply_color(id_, color, false); }

void GameObject::set_color(ColorRgb color, ForceSimWrite) { apply_color(id_, color, true); }

void GameObject::set_size(float x, float y, float z) {
    require_simulation_thread("set_size runs on SimulationThread");
    Slot* part = slot(id_);
    if (part == nullptr) {
        contract_fail("size write on a dead instance");
    }
    if (part->instance != this) {
        contract_fail("size write on an instance that is not a GameObject");
    }
    if (size_[0] == x && size_[1] == y && size_[2] == z) {
        return;
    }
    const float previous_x = size_[0];
    const float previous_y = size_[1];
    const float previous_z = size_[2];
    size_[0] = x;
    size_[1] = y;
    size_[2] = z;
    record_size(id_, previous_x, previous_y, previous_z, x, y, z);
    const WriteOrigin origin = current_origin();
    note(id_, VisualField::Size, origin);
    emit_change(id_, Field::Size, origin);
}

void GameObject::set_linear_velocity(float x, float y, float z) {
    require_simulation_thread("set_linear_velocity runs on SimulationThread");
    Slot* part = slot(id_);
    if (part == nullptr) {
        contract_fail("velocity write on a dead instance");
    }
    if (part->instance != this) {
        contract_fail("velocity write on an instance that is not a GameObject");
    }
    if (velocity_[0] == x && velocity_[1] == y && velocity_[2] == z) {
        return;
    }
    velocity_[0] = x;
    velocity_[1] = y;
    velocity_[2] = z;
    emit_change(id_, Field::LinearVelocity, current_origin());
}

Transform GameObject::transform() const {
    if (!alive(id_)) {
        return Transform{};
    }
    return transform_;
}

ColorRgb GameObject::color() const {
    if (!alive(id_)) {
        return ColorRgb{};
    }
    return color_;
}

bool GameObject::copy_size(float out[3]) const {
    if (!alive(id_)) {
        return false;
    }
    out[0] = size_[0];
    out[1] = size_[1];
    out[2] = size_[2];
    return true;
}

void GameObject::on_release() { clear_spatial(); }

void GameObject::on_reuse() { reset_spatial(); }

void GameObject::reset_spatial() {
    transform_ = transform_identity();
    color_ = ColorRgb{};
    size_[0] = size_[1] = size_[2] = 1.f;
    velocity_[0] = velocity_[1] = velocity_[2] = 0.f;
}

void GameObject::clear_spatial() {
    transform_ = Transform{};
    color_ = ColorRgb{};
    size_[0] = size_[1] = size_[2] = 0.f;
    velocity_[0] = velocity_[1] = velocity_[2] = 0.f;
}

namespace {

struct SpatialPlace {
    Transform transform = transform_identity();
    ColorRgb color{};
    float size[3] = {1.f, 1.f, 1.f};
};

}  // namespace

void GameObject::write_place(std::vector<std::byte>& out) const {
    static_assert(std::is_trivially_copyable<SpatialPlace>::value, "place blob must be memcpy-safe");
    SpatialPlace pod;
    pod.transform = transform_;
    pod.color = color_;
    pod.size[0] = size_[0];
    pod.size[1] = size_[1];
    pod.size[2] = size_[2];
    const auto* bytes = reinterpret_cast<const std::byte*>(&pod);
    out.insert(out.end(), bytes, bytes + sizeof(pod));
}

void GameObject::read_place(const std::byte* data, std::size_t size) {
    // Velocity is session-only. A place restore always clears it.
    velocity_[0] = velocity_[1] = velocity_[2] = 0.f;
    if (data == nullptr || size < sizeof(SpatialPlace)) {
        reset_spatial();
        return;
    }
    SpatialPlace pod;
    std::memcpy(&pod, data, sizeof(pod));
    transform_ = pod.transform;
    color_ = pod.color;
    size_[0] = pod.size[0];
    size_[1] = pod.size[1];
    size_[2] = pod.size[2];
}

namespace {

bool read_lua_color(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Color;
    out.color = body->color();
    return true;
}

bool write_lua_color(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    body->set_color(in.color);
    return true;
}

bool read_lua_transform(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Transform;
    out.transform = body->transform();
    return true;
}

bool write_lua_transform(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    body->set_transform(in.transform);
    return true;
}

ANARCHY_LUA_REGISTER(register_game_object_lua) {
    const LuaField fields[] = {
        lua_property("Color", "Color", true, read_lua_color, write_lua_color),
        lua_property("Transform", "Transform", true, read_lua_transform, write_lua_transform),
        lua_property("CFrame", "Transform", true, read_lua_transform, write_lua_transform),
    };
    register_lua_class("GameObject", "DataModel", fields, 3);
}

}  // namespace

}  // namespace engine_core
