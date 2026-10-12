// The H1Z1 car (spec test 15), on true cylinder wheels: driven straight for
// about 120 units, it drifts under 0.5 units, and its body rolls under 0.05
// degrees RMS. Hidden ([.car]): it needs the project on this machine.

#include "physics_rig.hpp"

#include "JointConstraint.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace physics_rig;

TEST_CASE("CY30 the H1Z1 car on cylinder wheels drives straight and level", "[cylinder][.car]") {
    const std::filesystem::path project = "C:/Users/Andrew/Documents/Anarchy Engine Projects/H1Z1 Z1";
    if (!std::filesystem::exists(project)) {
        SKIP("the H1Z1 project is not on this machine");
    }
    TempDir dir;
    std::filesystem::copy(project, dir.path, std::filesystem::copy_options::recursive);
    PhysicsRig rig;
    engine_core::Project loaded = engine_core::Project::load(dir.path, rig.game);
    for (const InstanceId child : rig.game.get_children(workspace_of(rig.game))) {
        if (rig.game.name(child) != "Car") {
            rig.game.destroy(child);
        }
    }
    const InstanceId car = rig.game.find_first_child(workspace_of(rig.game), "Car");
    REQUIRE(car != 0);
    auto* chassis = dynamic_cast<engine_core::PhysicsBase*>(rig.game.instance(rig.game.find_first_child(car, "Chassis")));
    REQUIRE(chassis != nullptr);
    // The axles, and each wheel's Collider, wherever under the car they sit.
    std::vector<engine_core::JointConstraint*> axles;
    int colliders = 0;
    std::vector<InstanceId> open{car};
    while (!open.empty()) {
        const InstanceId at = open.back();
        open.pop_back();
        for (const InstanceId child : rig.game.get_children(at)) {
            open.push_back(child);
            if (auto* axle = dynamic_cast<engine_core::JointConstraint*>(rig.game.instance(child))) {
                axles.push_back(axle);
            }
            auto* collider = dynamic_cast<PhysicsObject*>(rig.game.instance(child));
            if (collider != nullptr && rig.game.name(child) == "Collider" &&
                rig.game.name(at).find("Wheel") != std::string::npos) {
                REQUIRE_FALSE(collider->set_shape(static_cast<int>(PhysicsObject::Shape::Cylinder)));
                REQUIRE_FALSE(collider->set_size(Vec3{1.1f, 0.45f, 1.1f}));
                ++colliders;
            }
        }
    }
    REQUIRE(colliders == 4);
    REQUIRE(axles.size() == 4);
    // A wide floor under the wheels' bottoms (36.425 - 0.55).
    rig.body(at(982.f, 35.875f - 0.5f, -661.4f), Vec3{600.f, 1.f, 600.f}, true);
    rig.play();
    auto wheels = [&](double speed, double torque) {
        for (engine_core::JointConstraint* axle : axles) {
            axle->set_param(engine_core::JointParam::SpinMotorSpeed, speed);
            axle->set_param(engine_core::JointParam::MaxSpinTorque, torque);
        }
    };
    wheels(0.0, 2500.0);
    rig.engine_steps(240);
    const Matrix4 start = chassis->transform();
    wheels(1500.0, 900.0);
    rig.engine_steps(240 * 3);
    double roll = 0.0;
    double rollSquared = 0.0;
    int samples = 0;
    for (int i = 0; i < 240 * 6; ++i) {
        rig.engine_steps(1);
        const double lean = chassis->transform().m[4];  // up's sideways part
        roll += lean;
        rollSquared += lean * lean;
        ++samples;
    }
    const Matrix4 end = chassis->transform();
    const double mean = roll / samples;
    const double rms = std::sqrt(std::max(0.0, rollSquared / samples - mean * mean)) * 57.2958;
    const float drift = x_of(end) - x_of(start);
    INFO("travelled " << (z_of(end) - z_of(start)) << ", drift " << drift << ", roll RMS " << rms << " deg"
                      << ", yaw " << std::atan2(end.m[8], end.m[10]) * 57.2958 - std::atan2(start.m[8], start.m[10]) * 57.2958
                      << " deg");
    REQUIRE(z_of(end) - z_of(start) > 100.f);
    REQUIRE(std::fabs(drift) < 0.5f);
    REQUIRE(rms < 0.05);
}
