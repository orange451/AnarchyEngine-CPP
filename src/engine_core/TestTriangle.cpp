#include "TestTriangle.hpp"

#include <cmath>
#include <cstring>
#include <type_traits>

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

namespace {

struct PosePlace {
    double angle = 0;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

}  // namespace

void TestTriangle::write_place(std::vector<std::byte>& out) const {
    static_assert(std::is_trivially_copyable<PosePlace>::value, "place blob must be memcpy-safe");
    PosePlace pose;
    pose.angle = angle_.load();
    pose.x = x_.load();
    pose.y = y_.load();
    pose.z = z_.load();
    const auto* bytes = reinterpret_cast<const std::byte*>(&pose);
    out.insert(out.end(), bytes, bytes + sizeof(pose));
}

void TestTriangle::read_place(const std::byte* data, std::size_t size) {
    if (data == nullptr || size < sizeof(PosePlace)) {
        clear_pose();
        return;
    }
    PosePlace pose;
    std::memcpy(&pose, data, sizeof(pose));
    angle_.store(pose.angle);
    x_.store(pose.x);
    y_.store(pose.y);
    z_.store(pose.z);
}

}  // namespace engine_core
