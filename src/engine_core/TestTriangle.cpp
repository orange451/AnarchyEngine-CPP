#include "TestTriangle.hpp"

#include <cmath>

namespace engine_core {
namespace {

// A full turn takes four seconds of Heartbeat time.
constexpr double kDegreesPerSecond = 90.0;

}  // namespace

void TestTriangle::step(double dt) {
    if (!alive(id())) {
        return;
    }
    double next = std::fmod(angle_.load() + dt * kDegreesPerSecond, 360.0);
    if (next < 0.0) {
        next += 360.0;
    }
    angle_.store(next);
}

void TestTriangle::set_position(float x, float y, float z) {
    if (!alive(id())) {
        return;
    }
    x_.store(x);
    y_.store(y);
    z_.store(z);
}

double TestTriangle::angle_degrees() const {
    if (!alive(id())) {
        return 0.0;
    }
    return angle_.load();
}

Vec3 TestTriangle::position() const {
    if (!alive(id())) {
        return {};
    }
    return {x_.load(), y_.load(), z_.load()};
}

void TestTriangle::clear_pose() {
    angle_.store(0.0);
    x_.store(0.f);
    y_.store(0.f);
    z_.store(0.f);
}

void TestTriangle::on_release() { clear_pose(); }

void TestTriangle::on_reuse() { clear_pose(); }

}  // namespace engine_core
