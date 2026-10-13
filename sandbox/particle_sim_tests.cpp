// EmitterSim: one emitter's particles, spawned, moved, collided, and packed,
// with no DataModel in sight.

#include "Matrix4.hpp"
#include "particles/EmitterSim.hpp"
#include "particles/ParticleMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <optional>
#include <vector>

using namespace engine_core;
using namespace engine_core::particles;
using Catch::Approx;

namespace {

constexpr int kAll = INT_MAX;

CurveTable table(const NumberCurve& curve) {
    CurveTable t;
    t.bake(curve);
    return t;
}

ParticleSettings basic() {
    ParticleSettings s;
    s.rate = table(NumberCurve::constant(10.f));
    s.lifetime = table(NumberCurve::constant(100.f));
    s.emit_speed = table(NumberCurve::constant(0.f));
    s.emit_rotation = table(NumberCurve::constant(0.f));
    s.rotation_speed = table(NumberCurve::constant(0.f));
    s.drag = table(NumberCurve::constant(0.f));
    s.orbit_speed = table(NumberCurve::constant(0.f));
    s.size = table(NumberCurve::constant(1.f));
    s.stretch = table(NumberCurve::constant(0.f));
    s.color.bake(ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, 0.f));
    s.shape = ParticleShape::Point;
    s.seed = 1;
    return s;
}

ParticleSettings one_burst(int count) {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.f, table(NumberCurve::constant(static_cast<float>(count)))}};
    return s;
}

EmitterFrame still(float dt) {
    EmitterFrame f;
    f.previous = matrix4_identity();
    f.current = matrix4_identity();
    f.dt = dt;
    return f;
}

void run(EmitterSim& sim, float seconds, float dt, bool playing = true) {
    const int steps = static_cast<int>(std::lround(seconds / dt));
    for (int i = 0; i < steps; ++i) {
        sim.step(still(dt), playing, 0, kAll, nullptr);
    }
}

// A floor at y = 0 that a particle hits on its way down.
struct Floor : ParticleCollider {
    int tests = 0;
    void begin(std::size_t) override {}
    void moved(std::size_t, std::size_t) override {}
    std::optional<ParticleHit> test(std::size_t, Vec3 from, Vec3 to) override {
        ++tests;
        if (from.y >= 0.f && to.y < 0.f) {
            const float f = from.y / (from.y - to.y);
            return ParticleHit{{from.x + (to.x - from.x) * f, 0.f, from.z + (to.z - from.z) * f}, {0.f, 1.f, 0.f}};
        }
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("SIM1 Rate spawns rate times time, carrying fractions across steps", "[particle-sim]") {
    EmitterSim sim;
    sim.configure(basic());
    sim.restart(0);
    run(sim, 1.f, 1.f / 60.f);
    REQUIRE(sim.count() == 10);
}

TEST_CASE("SIM2 a burst fires once at its time and adds to Rate", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(0.f));
    s.bursts = {{0.5f, table(NumberCurve::constant(25.f))}};
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    run(sim, 0.49f, 0.01f);
    REQUIRE(sim.count() == 0);
    run(sim, 0.02f, 0.01f);
    REQUIRE(sim.count() == 25);
    run(sim, 1.f, 0.01f);
    REQUIRE(sim.count() == 25);
}

TEST_CASE("SIM3 a one-shot ends its cycle once and stops; a loop wraps and fires bursts again", "[particle-sim]") {
    ParticleSettings s = basic();
    s.duration = 1.f;
    s.looping = false;
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    run(sim, 1.05f, 0.05f);
    REQUIRE(sim.take_cycle_ended());
    REQUIRE_FALSE(sim.take_cycle_ended());
    const std::size_t n = sim.count();
    REQUIRE(n == 10);
    run(sim, 1.f, 0.05f);
    REQUIRE(sim.count() == n);

    ParticleSettings looping = one_burst(3);
    looping.duration = 1.f;
    EmitterSim loop;
    loop.configure(looping);
    loop.restart(0);
    run(loop, 2.5f, 0.05f);
    REQUIRE(loop.count() == 9);   // at t = 0, 1, and 2
    REQUIRE_FALSE(loop.take_cycle_ended());
}

TEST_CASE("SIM4 particles die at their lifetime", "[particle-sim]") {
    ParticleSettings s = one_burst(5);
    s.lifetime = table(NumberCurve::constant(0.5f));
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    run(sim, 0.4f, 0.1f);
    REQUIRE(sim.count() == 5);
    run(sim, 0.2f, 0.1f);
    REQUIRE(sim.count() == 0);
}

TEST_CASE("SIM5 acceleration and drag integrate as written", "[particle-sim]") {
    ParticleSettings s = one_burst(1);
    s.acceleration = Vec3{0.f, -10.f, 0.f};
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    run(sim, 1.f, 1.f / 240.f);
    REQUIRE(sim.velocity(0).y == Approx(-10.f).margin(0.1f));
    REQUIRE(sim.position(0).y == Approx(-5.f).margin(0.1f));

    s.acceleration = Vec3{0.f, 0.f, 0.f};
    s.emit_speed = table(NumberCurve::constant(10.f));
    s.drag = table(NumberCurve::constant(1.f));
    EmitterSim dragged;
    dragged.configure(s);
    dragged.restart(0);
    run(dragged, 1.f, 1.f / 60.f);
    REQUIRE(length(dragged.velocity(0)) == Approx(10.f * std::exp(-1.f)).margin(0.1f));
}

TEST_CASE("SIM6 drag is frame-rate independent", "[particle-sim]") {
    ParticleSettings s = one_burst(1);
    s.emit_speed = table(NumberCurve::constant(10.f));
    s.drag = table(NumberCurve::constant(2.f));
    EmitterSim a;
    EmitterSim b;
    a.configure(s);
    a.restart(0);
    run(a, 1.f, 1.f / 30.f);
    b.configure(s);
    b.restart(0);
    run(b, 1.f, 1.f / 240.f);
    REQUIRE(length(a.velocity(0)) == Approx(length(b.velocity(0))).margin(0.05f));
}

TEST_CASE("SIM7 orbit swings particles about the emitter and keeps their distance", "[particle-sim]") {
    ParticleSettings s = one_burst(1);
    s.shape = ParticleShape::Box;
    s.emit_from = ParticleEmitFrom::Surface;
    s.orbit_speed = table(NumberCurve::constant(90.f));
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    EmitterFrame f = still(0.01f);
    f.half = Vec3{1.f, 0.f, 1.f};
    sim.step(f, true, 0, kAll, nullptr);
    const Vec3 start = sim.position(0);
    for (int i = 0; i < 100; ++i) {
        sim.step(f, true, 0, kAll, nullptr);
    }
    const Vec3 end = sim.position(0);
    const float r0 = std::hypot(start.x, start.z);
    const float r1 = std::hypot(end.x, end.z);
    REQUIRE(r1 == Approx(r0).margin(1e-3));
    // A second at 90 degrees a second turns it a quarter.
    const float turned = std::acos(std::clamp((start.x * end.x + start.z * end.z) / (r0 * r1), -1.f, 1.f));
    REQUIRE(turned == Approx(3.14159265f / 2.f).margin(0.03f));
}

TEST_CASE("SIM8 a fast emitter spreads its spawns along its path", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(100.f));
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    EmitterFrame f;
    f.previous = matrix4_identity();
    f.current = matrix4_translation(10.f, 0.f, 0.f);
    f.dt = 0.1f;   // ten particles across x = 0 to 10
    sim.step(f, true, 0, kAll, nullptr);
    REQUIRE(sim.count() == 10);
    float lo = 1e9f;
    float hi = -1e9f;
    for (std::size_t i = 0; i < sim.count(); ++i) {
        lo = std::min(lo, sim.position(i).x);
        hi = std::max(hi, sim.position(i).x);
    }
    REQUIRE(lo < 1.5f);
    REQUIRE(hi > 8.5f);
}

TEST_CASE("SIM9 the same seed replays identically; a Range picks per particle", "[particle-sim]") {
    ParticleSettings s = basic();
    s.shape = ParticleShape::Sphere;
    s.emit_speed = table(NumberCurve::range(1.f, 5.f));
    EmitterSim a;
    EmitterSim b;
    EmitterFrame f = still(1.f / 60.f);
    f.half = Vec3{1.f, 1.f, 1.f};
    a.configure(s);
    a.restart(0);
    b.configure(s);
    b.restart(12345);   // the settings' seed wins over a fresh one
    for (int i = 0; i < 60; ++i) {
        a.step(f, true, 0, kAll, nullptr);
        b.step(f, true, 0, kAll, nullptr);
    }
    REQUIRE(a.count() == b.count());
    bool varied = false;
    for (std::size_t i = 0; i < a.count(); ++i) {
        REQUIRE(a.position(i).x == b.position(i).x);
        varied = varied || std::fabs(length(a.velocity(i)) - length(a.velocity(0))) > 0.1f;
    }
    REQUIRE(varied);

    // Seed 0: a fresh seed each restart.
    s.seed = 0;
    EmitterSim c;
    EmitterSim d;
    c.configure(s);
    c.restart(1);
    d.configure(s);
    d.restart(2);
    c.step(f, true, 5, kAll, nullptr);
    d.step(f, true, 5, kAll, nullptr);
    REQUIRE(c.position(0).x != d.position(0).x);
}

TEST_CASE("SIM10 allowance caps spawns and MaxParticles caps the live count", "[particle-sim]") {
    ParticleSettings s = basic();
    s.rate = table(NumberCurve::constant(1e9f));
    s.max_particles = 50;
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    REQUIRE(sim.wanted_spawns(1.f, true, 0) >= 50);
    sim.step(still(1.f), true, 0, 20, nullptr);
    REQUIRE(sim.count() == 20);
    sim.step(still(1.f), true, 0, kAll, nullptr);
    REQUIRE(sim.count() == 50);
    s.max_particles = 0;
    sim.configure(s);
    sim.step(still(1.f), true, 9, kAll, nullptr);
    REQUIRE(sim.count() == 50);   // nothing new; the live ones stay
}

TEST_CASE("SIM11 local-space particles follow the emitter; world-space ones stay", "[particle-sim]") {
    std::vector<ParticleVertex> out;
    for (bool local : {false, true}) {
        ParticleSettings s = one_burst(1);
        s.local_space = local;
        EmitterSim sim;
        sim.configure(s);
        sim.restart(0);
        sim.step(still(0.01f), true, 0, kAll, nullptr);
        sim.pack(out, matrix4_translation(5.f, 0.f, 0.f), Vec3{0.f, 0.f, 10.f}, false);
        REQUIRE(out.size() == 1);
        REQUIRE(out[0].position[0] == Approx(local ? 5.f : 0.f));
    }
}

TEST_CASE("SIM12 pack sorts back to front from the eye and fills each vertex", "[particle-sim]") {
    ParticleSettings s = one_burst(50);
    s.shape = ParticleShape::Box;
    s.size = table(NumberCurve::constant(2.f));
    s.color.bake(ColorCurve::constant(ColorRgb{1.f, 0.5f, 0.f, 1.f}, 0.25f));
    s.frames = 4;
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    EmitterFrame f = still(0.01f);
    f.half = Vec3{5.f, 5.f, 5.f};
    sim.step(f, true, 0, kAll, nullptr);
    std::vector<ParticleVertex> out;
    const Vec3 eye{0.f, 0.f, 20.f};
    sim.pack(out, matrix4_identity(), eye, true);
    REQUIRE(out.size() == 50);
    for (std::size_t i = 1; i < out.size(); ++i) {
        const Vec3 a{out[i - 1].position[0], out[i - 1].position[1], out[i - 1].position[2]};
        const Vec3 b{out[i].position[0], out[i].position[1], out[i].position[2]};
        REQUIRE(length(sub(a, eye)) >= length(sub(b, eye)) - 1e-4f);
    }
    REQUIRE(out[0].size == 2.f);
    REQUIRE(out[0].color[1] == Approx(0.5f));
    REQUIRE(out[0].color[3] == Approx(0.75f));
    REQUIRE(out[0].frame == 0.f);   // a young particle shows the first cell
}

TEST_CASE("SIM13 Emit spawns now, even when not playing", "[particle-sim]") {
    EmitterSim sim;
    sim.configure(basic());
    sim.restart(0);
    sim.step(still(0.01f), false, 7, kAll, nullptr);
    REQUIRE(sim.count() == 7);
    sim.clear();
    REQUIRE(sim.count() == 0);
}

TEST_CASE("SIM14 bounce, stick, and die against a floor", "[particle-sim]") {
    for (auto response :
         {ParticleCollisionResponse::Bounce, ParticleCollisionResponse::Stick, ParticleCollisionResponse::Die}) {
        ParticleSettings s = one_burst(1);
        s.acceleration = Vec3{0.f, -20.f, 0.f};
        s.collision = ParticleCollision::Terrain;
        s.response = response;
        s.bounciness = 0.5f;
        EmitterSim sim;
        sim.configure(s);
        sim.restart(0);
        Floor floor;
        EmitterFrame f;
        f.previous = matrix4_translation(0.f, 5.f, 0.f);
        f.current = f.previous;
        f.dt = 1.f / 120.f;
        float highest_after_hit = 0.f;
        for (int i = 0; i < 240; ++i) {
            sim.step(f, true, 0, kAll, &floor);
            if (i > 120 && sim.count() == 1) {
                highest_after_hit = std::max(highest_after_hit, sim.position(0).y);
            }
        }
        if (response == ParticleCollisionResponse::Die) {
            REQUIRE(sim.count() == 0);
        } else {
            REQUIRE(sim.count() == 1);
            REQUIRE(sim.position(0).y >= -1e-3f);
            if (response == ParticleCollisionResponse::Stick) {
                REQUIRE(length(sim.velocity(0)) == 0.f);
                REQUIRE(sim.position(0).y == Approx(0.f).margin(1e-3));
            } else {
                REQUIRE(highest_after_hit > 0.1f);   // it came back up
            }
        }
    }
}

TEST_CASE("SIM15 prewarm fills a looping emitter as if one cycle had run", "[particle-sim]") {
    ParticleSettings s = basic();
    s.duration = 2.f;
    s.lifetime = table(NumberCurve::constant(10.f));
    EmitterSim sim;
    sim.configure(s);
    sim.restart(0);
    sim.prewarm(still(0.f));
    REQUIRE(sim.count() == 20);
    REQUIRE(sim.cycle_time() == 0.f);
}
