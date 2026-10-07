// workspace:Raycast from Luau, with RaycastParams and RaycastResult.

#include "physics_rig.hpp"
#include "support.hpp"

#include "PhysicsWorld.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

bool has_line(const std::string& output, const std::string& line) { return output.find(line) != std::string::npos; }

// A ScriptRig whose Game has a physics world, as an Engine's does.
struct RaycastRig : ScriptRig {
    engine_core::PhysicsWorld physics;
    RaycastRig() { game.set_physics(&physics); }
    ~RaycastRig() { game.set_physics(nullptr); }

    PhysicsObject& body(engine_core::Matrix4 where, Vec3 size, const char* name) {
        PhysicsObject& object = game.create<PhysicsObject>();
        REQUIRE_FALSE(object.set_transform(where));
        REQUIRE_FALSE(object.set_size(size));
        object.set_anchored(true);
        game.set_name(object.id(), name);
        game.set_parent(object.id(), workspace_of(game));
        return object;
    }

    // Runs source on the command line and returns what it printed, one line each.
    std::string run(const char* source) {
        runtime.drain_output();
        runtime.run_chunk(source);
        frames(1);
        std::string out;
        for (const auto& line : runtime.drain_output().lines) {
            out += line.text;
        }
        return out;
    }
};

}  // namespace

TEST_CASE("L1 Raycast returns what it hit", "[raycast][lua]") {
    RaycastRig rig;
    rig.body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, "Floor");
    const std::string out = rig.run(R"(
        local r = workspace:Raycast(Vector3.new(1, 10, 2), Vector3.new(0, -20, 0))
        print(typeof(r), r.Instance.Name, r.Position.Y, r.Normal.Y, r.Distance, r.Material)
    )");
    INFO(out);
    REQUIRE(has_line(out, "RaycastResult\tFloor\t0\t1\t10\tnil\n"));
}

TEST_CASE("L2 a miss is nil", "[raycast][lua]") {
    RaycastRig rig;
    rig.body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, "Floor");
    const std::string out = rig.run(R"(print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, 5, 0))))");
    REQUIRE(has_line(out, "nil\n"));
}

TEST_CASE("L3 RaycastParams filters", "[raycast][lua]") {
    RaycastRig rig;
    rig.body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, "Floor");
    rig.body(at(0.f, 5.f, 0.f), Vec3{2.f, 2.f, 2.f}, "Box");
    const std::string out = rig.run(R"(
        local p = RaycastParams.new()
        print(typeof(p), p.FilterType == Enum.RaycastFilterType.Exclude, #p.FilterDescendantsInstances)
        p.FilterDescendantsInstances = { workspace.Box }
        print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0), p).Instance.Name)
        p.FilterType = Enum.RaycastFilterType.Include
        print(workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -20, 0), p).Instance.Name)
    )");
    INFO(out);
    REQUIRE(has_line(out, "RaycastParams\ttrue\t0\n"));
    REQUIRE(has_line(out, "Floor\n"));
    REQUIRE(has_line(out, "Box\n"));
}

TEST_CASE("L4 bad arguments and read-only results raise", "[raycast][lua]") {
    RaycastRig rig;
    rig.body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, "Floor");
    const std::string out = rig.run(R"(
        print(pcall(function() return workspace:Raycast(1, Vector3.new(0, -1, 0)) end))
        print(pcall(function() return workspace:Raycast(Vector3.new(0, 1, 0), Vector3.new(0, -2, 0), 5) end))
        local r = workspace:Raycast(Vector3.new(0, 1, 0), Vector3.new(0, -2, 0))
        print(pcall(function() r.Distance = 3 end))
        local p = RaycastParams.new()
        print(pcall(function() p.FilterDescendantsInstances = { 5 } end))
    )");
    INFO(out);
    REQUIRE(has_line(out, "false\t"));
    REQUIRE(out.find("origin must be a Vector3") != std::string::npos);
    REQUIRE(out.find("params must be a RaycastParams") != std::string::npos);
    REQUIRE(out.find("Distance cannot be assigned to") != std::string::npos);
    REQUIRE(out.find("FilterDescendantsInstances must hold only Instances") != std::string::npos);
}

TEST_CASE("L5 Raycast without a physics world raises", "[raycast][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(print(pcall(function() return workspace:Raycast(Vector3.new(), Vector3.new(0, -1, 0)) end)))");
    rig.frames(1);
    std::string out;
    for (const auto& line : rig.runtime.drain_output().lines) {
        out += line.text;
    }
    INFO(out);
    REQUIRE(out.find("Raycast needs a running engine") != std::string::npos);
}
