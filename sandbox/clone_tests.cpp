// Instance:Clone(): a copy of an instance and everything under it, with no
// parent, whose references inside the copied set point at the copies.

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

bool has(const std::string& output, const std::string& text) { return output.find(text) != std::string::npos; }

std::string run(ScriptRig& rig, const char* source) {
    rig.runtime.drain_output();
    rig.runtime.run_chunk(source);
    rig.frames(1);
    std::string out;
    for (const auto& line : rig.runtime.drain_output().lines) {
        out += line.text;
        out += '\n';
    }
    return out;
}

}  // namespace

TEST_CASE("CL1 Clone copies a tree with no parent, and points its references at the copies", "[clone][lua]") {
    ScriptRig rig;
    const std::string out = run(rig, R"(
        local car = Instance.new("Folder", workspace)
        car.Name = "Car"
        local a = Instance.new("PhysicsObject", car)
        a.Name = "A"
        local b = Instance.new("PhysicsObject", car)
        b.Name = "B"
        local axle = Instance.new("WheelConstraint", car)
        axle.PVInstance1 = a
        axle.PVInstance2 = b
        axle.SpinMotorSpeed = 90
        local outside = Instance.new("PhysicsObject", workspace)
        local tie = Instance.new("WeldConstraint", car)
        tie.PVInstance1 = a
        tie.PVInstance2 = outside
        local copy = car:Clone()
        print("parent=" .. tostring(copy.Parent))
        print("name=" .. copy.Name .. " " .. #copy:GetChildren())
        local ca = copy:FindFirstChild("A")
        local cb = copy:FindFirstChild("B")
        local caxle = copy:FindFirstChild("WheelConstraint")
        print("ends=" .. tostring(caxle.PVInstance1 == ca) .. tostring(caxle.PVInstance2 == cb))
        print("speed=" .. tostring(caxle.SpinMotorSpeed))
        print("distinct=" .. tostring(ca ~= a))
        local ctie = copy:FindFirstChild("WeldConstraint")
        print("outside=" .. tostring(ctie.PVInstance1 == ca) .. tostring(ctie.PVInstance2 == outside))
        print("original=" .. tostring(axle.PVInstance1 == a))
        copy.Parent = workspace
        print("placed=" .. tostring(copy.Parent == workspace))
    )");
    INFO(out);
    REQUIRE(has(out, "parent=nil"));
    REQUIRE(has(out, "name=Car 4"));
    REQUIRE(has(out, "ends=truetrue"));
    REQUIRE(has(out, "speed=90"));
    REQUIRE(has(out, "distinct=true"));
    REQUIRE(has(out, "outside=truetrue"));
    REQUIRE(has(out, "original=true"));
    REQUIRE(has(out, "placed=true"));
}
