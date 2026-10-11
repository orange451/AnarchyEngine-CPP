// Touched and TouchEnded: when two bodies start and stop touching, with a
// TouchObject (docs/superpowers/specs/2026-10-11-touched-design.md).

#include "physics_rig.hpp"
#include "support.hpp"

#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "TouchRecord.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::PhysicsObject;
using engine_core::Vec3;

using namespace physics_rig;

bool has(const std::string& output, const std::string& text) { return output.find(text) != std::string::npos; }

struct TouchRig : ScriptRig {
    engine_core::PhysicsWorld physics;
    TouchRig() { game.set_physics(&physics); }
    ~TouchRig() { game.set_physics(nullptr); }

    std::string run(const char* source) {
        runtime.drain_output();
        runtime.run_chunk(source);
        frames(1);
        return drain();
    }
    std::string drain() {
        std::string out;
        for (const auto& line : runtime.drain_output().lines) {
            out += line.text;
        }
        return out;
    }
};

engine_core::LuaSlot touch_slot(engine_core::InstanceId other) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::TouchObject;
    slot.touch.time = 1.5;
    slot.touch.instance = other;
    slot.touch.point = Vec3{1.f, 2.f, 3.f};
    slot.touch.normal = Vec3{0.f, -1.f, 0.f};
    slot.touch.relative_velocity = Vec3{0.f, 4.f, 0.f};
    return slot;
}

}  // namespace

TEST_CASE("T1 Touched and TouchEnded hand a script a read-only TouchObject", "[touch][lua]") {
    TouchRig rig;
    PhysicsObject& box = rig.game.create<PhysicsObject>();
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    PhysicsObject& floor = rig.game.create<PhysicsObject>();
    rig.game.set_name(floor.id(), "Floor");
    rig.game.set_parent(floor.id(), workspace_of(rig.game));
    rig.run(R"(
        workspace.Box.Touched:Connect(function(t)
            print(typeof(t), t.Instance.Name, t.Time, t.Point.Y, t.Normal.Y, t.RelativeVelocity.Y)
            print(pcall(function() t.Time = 3 end))
        end)
        workspace.Box.TouchEnded:Connect(function(t) print("ended", t.Instance.Name) end)
    )");
    rig.game.fire_event(box.id(), "Touched", {touch_slot(floor.id())});
    rig.game.fire_event(box.id(), "TouchEnded", {touch_slot(floor.id())});
    rig.frames(1);
    const std::string out = rig.drain();
    INFO(out);
    REQUIRE(has(out, "TouchObject\tFloor\t1.5\t2\t-1\t4\n"));
    REQUIRE(has(out, "Time cannot be assigned to"));
    REQUIRE(has(out, "ended\tFloor\n"));
}

TEST_CASE("T2 event_listened is true only while Touched or TouchEnded has a connection, and makes nothing",
          "[touch][lua]") {
    TouchRig rig;
    PhysicsObject& box = rig.game.create<PhysicsObject>();
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    REQUIRE_FALSE(rig.game.event_listened(box.id(), "Touched"));
    rig.run(R"(_G.c = workspace.Box.Touched:Connect(function() end))");
    REQUIRE(rig.game.event_listened(box.id(), "Touched"));
    REQUIRE_FALSE(rig.game.event_listened(box.id(), "TouchEnded"));
    rig.run(R"(_G.c:Disconnect())");
    REQUIRE_FALSE(rig.game.event_listened(box.id(), "Touched"));
}

TEST_CASE("T3 a CanCollide-off body is a sensor: still hit by rays, still falls, never collides",
          "[touch][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& zone = rig.body(at(0.f, 5.f, 0.f), Vec3{4.f, 4.f, 4.f}, true);
    REQUIRE_FALSE(zone.set_can_collide(false));
    PhysicsObject& ghost = rig.body(at(8.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    REQUIRE_FALSE(ghost.set_can_collide(false));
    rig.physics.sync(rig.game);
    REQUIRE(rig.physics.shapes_are_sensors(zone.id()));
    REQUIRE_FALSE(rig.physics.shapes_are_sensors(rig.body(at(0.f, 20.f, 0.f), Vec3{1.f, 1.f, 1.f}, true).id()));
    const auto hit = rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f},
                                         engine_core::RayFilter{});
    REQUIRE((hit && hit->instance == zone.id()));
    rig.play();
    rig.seconds(2.0);
    REQUIRE(y_of(ghost.transform()) < -5.f);
}
