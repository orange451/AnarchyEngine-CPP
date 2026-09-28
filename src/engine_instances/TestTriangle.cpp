#include "TestTriangle.hpp"

#include "LuaApi.hpp"

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
    const Vec3 previous{x_.load(), y_.load(), z_.load()};
    if (previous.x == x && previous.y == y && previous.z == z) {
        return;
    }
    x_.store(x);
    y_.store(y);
    z_.store(z);
    record_position(id(), previous, Vec3{x, y, z});
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

void TestTriangle::save_properties(PropertyBag& out) const {
    DataModel::save_properties(out);
    const float position[3] = {x_.load(), y_.load(), z_.load()};
    if (position[0] != 0.f || position[1] != 0.f || position[2] != 0.f) {
        bag_set(out, "Position", json_floats(position, 3));
    }
}

void TestTriangle::default_properties(PropertyBag& out) const {
    DataModel::default_properties(out);
    const float origin[3] = {0.f, 0.f, 0.f};
    bag_set(out, "Position", json_floats(origin, 3));
}

bool TestTriangle::load_property(const std::string& key, const JsonValue& value, std::string& error) {
    if (key == "Position") {
        std::vector<float> floats;
        if (!read_json_floats(value, 3, 3, floats)) {
            error = "Position must be 3 numbers";
            return true;
        }
        set_position(floats[0], floats[1], floats[2]);
        return true;
    }
    return DataModel::load_property(key, value, error);
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

namespace {

bool read_lua_position(DataModel&, DataModel& object, LuaSlot& out) {
    auto* triangle = dynamic_cast<TestTriangle*>(&object);
    if (triangle == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Vec3;
    out.vec = triangle->position();
    return true;
}

bool write_lua_position(DataModel&, DataModel& object, LuaSlot& in) {
    auto* triangle = dynamic_cast<TestTriangle*>(&object);
    if (triangle == nullptr) {
        return false;
    }
    triangle->set_position(in.vec.x, in.vec.y, in.vec.z);
    return true;
}

ANARCHY_LUA_REGISTER(register_test_triangle_lua) {
    const LuaField fields[] = {
        lua_property("Position", "Vector3", true, read_lua_position, write_lua_position),
    };
    register_lua_class("TestTriangle", "Instance", fields, 1);
}

}  // namespace

}  // namespace engine_core
