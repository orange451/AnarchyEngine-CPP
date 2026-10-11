// Touched and TouchEnded: when two bodies start and stop touching, with a
// TouchObject (docs/superpowers/specs/2026-10-11-touched-design.md).

#include "alloc_counter.hpp"
#include "physics_rig.hpp"
#include "support.hpp"

#include "Brush.hpp"
#include "CollisionGroups.hpp"
#include "Constraint.hpp"
#include "AssetInstances.hpp"
#include "Events.hpp"
#include "MeshShapes.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "TouchRecord.hpp"
#include "brush/BrushGeometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

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

namespace {

struct Heard {
    engine_core::InstanceId on = 0;
    bool ended = false;
    engine_core::TouchRecord touch;
};

// Connects both signals of id; what fires lands in out.
void listen(engine_core::DataModel& game, engine_core::InstanceId id, std::vector<Heard>& out) {
    for (const char* name : {"Touched", "TouchEnded"}) {
        const bool ended = std::string(name) == "TouchEnded";
        game.event_signal(id, name).connect([&game, &out, id, ended](engine_core::InstanceId, engine_core::Field) {
            const engine_core::EventArgs* args = game.events().current_args();
            REQUIRE(args != nullptr);
            REQUIRE(args->size() == 1);
            out.push_back(Heard{id, ended, (*args)[0].touch});
        });
    }
}

// Steps as the engine does: physics, then the events it queued.
void run_for(PhysicsRig& rig, double seconds) {
    const int count = static_cast<int>(seconds / kStep + 0.5);
    for (int i = 0; i < count; ++i) {
        rig.physics.step(rig.game, kStep);
        rig.game.events().drain();
    }
}

std::size_t count(const std::vector<Heard>& heard, engine_core::InstanceId on, bool ended) {
    std::size_t n = 0;
    for (const Heard& h : heard) {
        n += h.on == on && h.ended == ended ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("T4 a box landing on a floor fires Touched once on each listener, with a TouchObject",
          "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    listen(rig.game, floor.id(), heard);
    rig.play();
    run_for(rig, 3.0);
    REQUIRE(count(heard, box.id(), false) == 1);
    REQUIRE(count(heard, floor.id(), false) == 1);
    REQUIRE(count(heard, box.id(), true) == 0);
    for (const Heard& h : heard) {
        const bool on_box = h.on == box.id();
        REQUIRE(h.touch.instance == (on_box ? floor.id() : box.id()));
        REQUIRE(near(h.touch.point.y, 0.f, 0.1f));
        REQUIRE(near(h.touch.normal.y, on_box ? -1.f : 1.f, 1e-3f));
        // The other's velocity relative to this one: the box was falling at about 7 units/s.
        REQUIRE((on_box ? h.touch.relative_velocity.y > 5.f : h.touch.relative_velocity.y < -5.f));
        REQUIRE(h.touch.time > 0.5);
    }
}

TEST_CASE("T5 lifting it off fires TouchEnded once on each listener", "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    listen(rig.game, floor.id(), heard);
    rig.play();
    run_for(rig, 1.0);
    REQUIRE_FALSE(box.set_transform(at(0.f, 20.f, 0.f)));
    run_for(rig, 0.1);
    REQUIRE(count(heard, box.id(), true) == 1);
    REQUIRE(count(heard, floor.id(), true) == 1);
    for (const Heard& h : heard) {
        if (h.ended) {
            REQUIRE(h.touch.instance == (h.on == box.id() ? floor.id() : box.id()));
        }
    }
}

TEST_CASE("T6 a Brush in several pieces fires one Touched", "[touch][physics]") {
    PhysicsRig rig;
    rig.floor();
    engine_core::Brush& ball = rig.game.create<engine_core::Brush>();
    REQUIRE_FALSE(ball.set_faces(engine_core::brush::make_sphere({2, 2, 2}, 3)));
    REQUIRE_FALSE(ball.set_transform(at(0.f, 3.f, 0.f)));
    rig.game.set_parent(ball.id(), workspace_of(rig.game));
    ball.set_anchored(false);
    std::vector<Heard> heard;
    listen(rig.game, ball.id(), heard);
    rig.play();
    run_for(rig, 3.0);
    REQUIRE(rig.physics.shape_count(ball.id()) > 1);
    REQUIRE(count(heard, ball.id(), false) == 1);
}

TEST_CASE("T7 a welded pair fires on the member that hit", "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    PhysicsObject& low = rig.body(at(0.f, 2.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    PhysicsObject& high = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    engine_core::WeldConstraint& weld = rig.game.create<engine_core::WeldConstraint>();
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = low.id();
    REQUIRE_FALSE(weld.set_pv_instance1(slot));
    slot.id = high.id();
    REQUIRE_FALSE(weld.set_pv_instance2(slot));
    rig.game.set_parent(weld.id(), workspace_of(rig.game));
    std::vector<Heard> heard;
    listen(rig.game, low.id(), heard);
    listen(rig.game, high.id(), heard);
    listen(rig.game, floor.id(), heard);
    rig.play();
    run_for(rig, 3.0);
    REQUIRE(count(heard, low.id(), false) == 1);
    REQUIRE(count(heard, high.id(), false) == 0);
    for (const Heard& h : heard) {
        if (h.on == floor.id()) {
            REQUIRE(h.touch.instance == low.id());
        }
    }
}

TEST_CASE("T8 a trigger zone fires as a box falls through it, on both", "[touch][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& zone = rig.body(at(0.f, 5.f, 0.f), Vec3{4.f, 2.f, 4.f}, true);
    REQUIRE_FALSE(zone.set_can_collide(false));
    PhysicsObject& box = rig.body(at(0.f, 9.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    std::vector<Heard> heard;
    listen(rig.game, zone.id(), heard);
    listen(rig.game, box.id(), heard);
    rig.play();
    run_for(rig, 3.0);
    REQUIRE(near(y_of(box.transform()), 0.5f, 0.05f));
    REQUIRE(count(heard, zone.id(), false) == 1);
    REQUIRE(count(heard, zone.id(), true) == 1);
    REQUIRE(count(heard, box.id(), false) == 2);  // the zone, then the floor
    REQUIRE(count(heard, box.id(), true) == 1);
    for (const Heard& h : heard) {
        if (h.on == zone.id()) {
            REQUIRE(h.touch.instance == box.id());
            REQUIRE(h.touch.normal.x == 0.f);
            REQUIRE(h.touch.normal.y == 0.f);
        }
    }
}

TEST_CASE("T9 collision groups and a NoCollisionConstraint keep a pair from touching", "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    PhysicsObject& block = rig.body(at(0.f, 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, true);
    PhysicsObject& box = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    engine_core::NoCollisionConstraint& apart = rig.game.create<engine_core::NoCollisionConstraint>();
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = block.id();
    REQUIRE_FALSE(apart.set_pv_instance1(slot));
    slot.id = box.id();
    REQUIRE_FALSE(apart.set_pv_instance2(slot));
    rig.game.set_parent(apart.id(), workspace_of(rig.game));
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    rig.play();
    run_for(rig, 3.0);
    REQUIRE(count(heard, box.id(), false) == 1);
    REQUIRE(heard[0].touch.instance == floor.id());
}

TEST_CASE("T10 a body nobody listens to asks for no contact events; connecting turns them on",
          "[touch][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 3.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    run_for(rig, 0.1);
    REQUIRE_FALSE(rig.physics.contact_events_enabled(box.id()));
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    run_for(rig, 0.1);
    REQUIRE(rig.physics.contact_events_enabled(box.id()));
}

TEST_CASE("T11 a body destroyed while touching ends the touch once for the other", "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    std::vector<Heard> heard;
    listen(rig.game, floor.id(), heard);
    rig.play();
    run_for(rig, 1.0);
    REQUIRE(count(heard, floor.id(), false) == 1);
    rig.game.destroy(box.id());
    run_for(rig, 0.2);
    REQUIRE(count(heard, floor.id(), true) == 1);
}

TEST_CASE("T12 a shape remade while touching fires nothing more", "[touch][physics]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    rig.play();
    run_for(rig, 1.0);
    REQUIRE(count(heard, box.id(), false) == 1);
    REQUIRE_FALSE(box.set_size(Vec3{1.f, 1.2f, 1.f}));
    run_for(rig, 0.5);
    REQUIRE(count(heard, box.id(), false) == 1);
    REQUIRE(count(heard, box.id(), true) == 0);
}

TEST_CASE("T13 a PlayerController standing on the floor touches it, once, until it leaves it",
          "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    engine_core::PlayerController& player = rig.controller(at(0.f, 0.f, 0.f));
    std::vector<Heard> heard;
    listen(rig.game, player.id(), heard);
    listen(rig.game, floor.id(), heard);
    rig.play();
    run_for(rig, 1.0);
    REQUIRE(count(heard, player.id(), false) == 1);
    REQUIRE(count(heard, floor.id(), false) == 1);
    for (const Heard& h : heard) {
        if (h.on == player.id()) {
            REQUIRE(h.touch.instance == floor.id());
            REQUIRE(near(h.touch.normal.y, -1.f, 1e-3f));
            REQUIRE(near(h.touch.point.y, 0.f, 0.05f));
        } else {
            REQUIRE(h.touch.instance == player.id());
            REQUIRE(near(h.touch.normal.y, 1.f, 1e-3f));
        }
    }
    // Standing on, and walking across, it fires nothing more.
    REQUIRE_FALSE(player.set_transform(at(3.f, 0.f, 0.f)));
    run_for(rig, 0.5);
    REQUIRE(count(heard, player.id(), false) == 1);
    REQUIRE(count(heard, player.id(), true) == 0);
    // Lifted off, it stops touching.
    REQUIRE_FALSE(player.set_transform(at(0.f, 20.f, 0.f)));
    run_for(rig, 0.1);
    REQUIRE(count(heard, player.id(), true) == 1);
    REQUIRE(count(heard, floor.id(), true) == 1);
}

TEST_CASE("T14 a PlayerController moving onto another body ends one touch and begins the next",
          "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& left = rig.body(at(-5.f, -0.5f, 0.f), Vec3{10.f, 1.f, 10.f}, true);
    PhysicsObject& right = rig.body(at(5.f, -0.5f, 0.f), Vec3{10.f, 1.f, 10.f}, true);
    engine_core::PlayerController& player = rig.controller(at(-5.f, 0.f, 0.f));
    std::vector<Heard> heard;
    listen(rig.game, player.id(), heard);
    rig.play();
    run_for(rig, 0.5);
    REQUIRE(count(heard, player.id(), false) == 1);
    REQUIRE(heard.back().touch.instance == left.id());
    REQUIRE_FALSE(player.set_transform(at(5.f, 0.f, 0.f)));
    run_for(rig, 0.5);
    REQUIRE(count(heard, player.id(), true) == 1);
    REQUIRE(count(heard, player.id(), false) == 2);
    REQUIRE(heard.back().touch.instance == right.id());
}

TEST_CASE("T15 a body lifted off an anchored Custom mesh ends its touch", "[touch][physics]") {
    PhysicsRig rig;
    engine_core::Mesh& mesh = rig.game.create<engine_core::Mesh>();
    PhysicsObject& ground = rig.body(at(0.f, -0.5f, 0.f), Vec3{20.f, 1.f, 20.f}, true);
    REQUIRE_FALSE(ground.set_shape(static_cast<int>(PhysicsObject::Shape::Custom)));
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = mesh.id();
    REQUIRE_FALSE(ground.set_mesh(slot));
    PhysicsObject& box = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    rig.play();
    // Playing, the Mesh's geometry is the session's own (as P16 does).
    REQUIRE_FALSE(edit_geometry(rig.game, mesh, [](anarchy::amesh::Data& data) {
        engine_core::add_box(data, Vec3{20.f, 1.f, 20.f}, Vec3{0.f, 0.f, 0.f});
    }));
    run_for(rig, 1.0);
    REQUIRE(count(heard, box.id(), false) == 1);
    REQUIRE(heard.back().touch.instance == ground.id());
    REQUIRE_FALSE(box.set_transform(at(0.f, 20.f, 0.f)));
    run_for(rig, 0.1);
    REQUIRE(count(heard, box.id(), true) == 1);
}

TEST_CASE("T16 writing CollisionGroup or Anchored on a touching body keeps the touch", "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    rig.play();
    REQUIRE_FALSE(rig.game.collision_groups().register_group("Crates", engine_core::CollisionGroups::FilterType::Exclude));
    run_for(rig, 1.0);
    REQUIRE(count(heard, box.id(), false) == 1);
    // A filter change resets the contact in Box3D: still touching, nothing fires.
    REQUIRE_FALSE(box.set_collision_group("Crates"));
    run_for(rig, 0.2);
    floor.set_anchored(false);
    floor.set_anchored(true);
    run_for(rig, 0.2);
    REQUIRE(count(heard, box.id(), false) == 1);
    REQUIRE(count(heard, box.id(), true) == 0);
    REQUIRE_FALSE(box.set_transform(at(0.f, 20.f, 0.f)));
    run_for(rig, 0.1);
    REQUIRE(count(heard, box.id(), true) == 1);
}

TEST_CASE("T17 remaking the anchored body a box rests on keeps the touch", "[touch][physics]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    PhysicsObject& box = rig.body(at(0.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    rig.play();
    // Long enough for the box to fall asleep: its proxy no longer moves.
    run_for(rig, 3.0);
    REQUIRE(count(heard, box.id(), false) == 1);
    REQUIRE_FALSE(floor.set_size(Vec3{30.f, 1.f, 30.f}));
    run_for(rig, 0.5);
    REQUIRE(count(heard, box.id(), false) == 1);
    REQUIRE(count(heard, box.id(), true) == 0);
}

TEST_CASE("T18 connecting mid-contact fires nothing for it, and its later end is still heard",
          "[touch][physics]") {
    PhysicsRig rig;
    rig.floor();
    // A small anchored platform the box rests on, 2 units above the floor.
    PhysicsObject& platform = rig.body(at(0.f, 1.5f, 0.f), Vec3{2.f, 1.f, 2.f}, true);
    PhysicsObject& box = rig.body(at(0.f, 2.5f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    run_for(rig, 1.0);
    // The contact was made with nobody listening.
    std::vector<Heard> heard;
    listen(rig.game, box.id(), heard);
    run_for(rig, 0.2);
    REQUIRE(count(heard, box.id(), false) == 0);
    // Slid off the platform's edge, the box stops touching it.
    REQUIRE_FALSE(box.set_velocity(Vec3{6.f, 0.f, 0.f}));
    run_for(rig, 1.5);
    bool ended_platform = false;
    for (const Heard& h : heard) {
        ended_platform = ended_platform || (h.ended && h.touch.instance == platform.id());
    }
    REQUIRE(ended_platform);
}

TEST_CASE("T19 with a listener connected, a step of moving bodies allocates nothing", "[touch][alloc]") {
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    SKIP("Debug containers allocate proxies; run in Release");
#endif
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    // Boxes sliding across the floor: moving bodies, contacts that persist.
    std::vector<PhysicsObject*> boxes;
    for (int i = 0; i < 8; ++i) {
        boxes.push_back(&rig.body(at(-10.f + 2.5f * static_cast<float>(i), 0.5f, 0.f), Vec3{1.f, 1.f, 1.f}, false));
    }
    std::vector<Heard> heard;
    listen(rig.game, floor.id(), heard);
    heard.reserve(1024);
    rig.play();
    run_for(rig, 0.5);
    for (PhysicsObject* box : boxes) {
        REQUIRE_FALSE(box->set_velocity(Vec3{0.f, 0.f, 2.f}));
    }
    run_for(rig, 0.1);
    alloc_counter::arm();
    run_for(rig, 0.5);
    alloc_counter::disarm();
    REQUIRE(alloc_counter::count() == 0u);
}
