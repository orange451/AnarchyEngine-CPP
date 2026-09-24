#include "GameObject.hpp"

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
    size_[0] = x;
    size_[1] = y;
    size_[2] = z;
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

}  // namespace engine_core
