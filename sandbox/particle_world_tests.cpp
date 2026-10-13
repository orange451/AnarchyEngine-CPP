// ParticleWorld: the emitters in Workspace, simulated every frame stopped or
// playing, with their events, counts, budget, and draws.

#include "support.hpp"

#include "Attachment.hpp"
#include "Gui.hpp"
#include "Matrix4.hpp"
#include "ParticleEmitter.hpp"
#include "ParticleWorld.hpp"
#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "SceneService.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace engine_core;
using Catch::Approx;

namespace {

struct ParticleRig : ScriptRig {
    PhysicsWorld physics;
    ParticleWorld world;
    ParticleRig() { game.set_physics(&physics); }
    ~ParticleRig() { game.set_physics(nullptr); }

    // dt is what Engine::run_frame hands the particle world.
    void steps(int count, double dt = 1.0 / 60.0) {
        for (int i = 0; i < count; ++i) {
            world.step(game, dt);
            frames(1, dt);
        }
    }

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

    ParticleEmitter& emitter(InstanceId parent, const char* name = "Emitter") {
        ParticleEmitter& e = game.create<ParticleEmitter>();
        game.set_name(e.id(), name);
        game.set_parent(e.id(), parent);
        return e;
    }

    Attachment& attachment_at(float x, float y, float z) {
        Attachment& a = game.create<Attachment>();
        game.set_parent(a.id(), workspace_of(game));
        REQUIRE_FALSE(a.set_offset(matrix4_translation(x, y, z)).has_value());
        return a;
    }
};

}  // namespace

TEST_CASE("PW1 an emitter in Workspace simulates while stopped; in a ViewportPane it does not", "[particle-world]") {
    ParticleRig rig;
    ParticleEmitter& e = rig.emitter(workspace_of(rig.game));
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(60.f)).has_value());
    REQUIRE_FALSE(rig.game.simulation_running());
    rig.steps(60);
    REQUIRE(rig.world.emitter_count() == 1);
    REQUIRE(e.particle_count() > 40);
    REQUIRE(rig.world.draws().size() == 1);
    REQUIRE(rig.world.draws()[0].emitter == e.id());
    REQUIRE(rig.world.draws()[0].particles->size() == static_cast<std::size_t>(e.particle_count()));

    ScreenGui& screen = rig.game.create<ScreenGui>();
    rig.game.set_parent(screen.id(), rig.game.scene_service("Gui"));
    ViewportPane& pane = rig.game.create<ViewportPane>();
    rig.game.set_parent(pane.id(), screen.id());
    rig.game.set_parent(e.id(), pane.id());
    rig.steps(1);
    REQUIRE(rig.world.emitter_count() == 0);
    REQUIRE(rig.world.draws().empty());
}

TEST_CASE("PW2 a one-shot sets Playing false at its cycle's end and fires Finished when its last particle dies",
          "[particle-world]") {
    ParticleRig rig;
    ParticleEmitter& e = rig.emitter(workspace_of(rig.game));
    REQUIRE_FALSE(e.set_looping(false).has_value());
    REQUIRE_FALSE(e.set_duration(0.5).has_value());
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(0.5f)).has_value());
    rig.game.capture_place();
    rig.game.start_simulation();   // a one-shot ends only in play; stopped, it previews again
    rig.run(R"(
        _G.finished = 0
        _G.playing = nil
        workspace.Emitter.Finished:Connect(function() _G.finished += 1 end)
        workspace.Emitter.Changed:Connect(function(name) if name == "Playing" then _G.playing = workspace.Emitter.Playing end end)
    )");
    rig.steps(33);
    REQUIRE_FALSE(e.playing());
    REQUIRE(rig.run("print(_G.finished, _G.playing)") == "0\tfalse\n");
    rig.steps(40);
    REQUIRE(e.particle_count() == 0);
    REQUIRE(rig.run("print(_G.finished)") == "1\n");
    rig.steps(5);
    REQUIRE(rig.run("print(_G.finished)") == "1\n");   // once
    REQUIRE_FALSE(e.set_playing(true).has_value());   // replays
    rig.steps(5);
    REQUIRE(e.particle_count() > 0);
}

TEST_CASE("PW3 Emit and Clear, from C++ and from scripts, take effect on the next step", "[particle-world]") {
    ParticleRig rig;
    ParticleEmitter& e = rig.emitter(workspace_of(rig.game));
    REQUIRE_FALSE(e.set_playing(false).has_value());
    e.request_emit(25);
    rig.steps(1);
    REQUIRE(e.particle_count() == 25);
    e.request_clear();
    rig.steps(1);
    REQUIRE(e.particle_count() == 0);
    rig.run("workspace.Emitter:Emit(7)");
    rig.steps(1);
    REQUIRE(e.particle_count() == 7);
    REQUIRE(rig.run("print(workspace.Emitter.ParticleCount)") == "7\n");
    rig.run("workspace.Emitter:Clear()");
    rig.steps(1);
    REQUIRE(e.particle_count() == 0);
    REQUIRE(rig.run("print(pcall(function() workspace.Emitter:Emit(-1) end))").find("0 or more") !=
            std::string::npos);
}

TEST_CASE("PW4 particles spawn at the parent and follow it in Local space", "[particle-world]") {
    ParticleRig rig;
    Attachment& a = rig.attachment_at(10.f, 0.f, 0.f);
    ParticleEmitter& e = rig.emitter(a.id());
    REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)).has_value());
    rig.steps(30);
    REQUIRE(e.particle_count() > 0);
    const auto& world_draw = rig.world.draws().front();
    REQUIRE(world_draw.particles->front().position[0] == Approx(10.f).margin(1e-3));
    // World space: moving the parent leaves the old particles behind.
    REQUIRE_FALSE(a.set_offset(matrix4_translation(20.f, 0.f, 0.f)).has_value());
    rig.steps(1);
    bool left_behind = false;
    for (const auto& p : *rig.world.draws().front().particles) {
        left_behind = left_behind || std::fabs(p.position[0] - 10.f) < 1e-3f;
    }
    REQUIRE(left_behind);
    // Local space: they come along.
    REQUIRE_FALSE(e.set_simulation_space(static_cast<int>(TransformSpace::Local)).has_value());
    rig.steps(10);
    REQUIRE_FALSE(a.set_offset(matrix4_translation(30.f, 0.f, 0.f)).has_value());
    rig.steps(1);
    for (const auto& p : *rig.world.draws().front().particles) {
        REQUIRE(p.position[0] == Approx(30.f).margin(1e-3));
    }
}

TEST_CASE("PW5 InheritVelocity takes an assembly body's velocity, and nothing without one", "[particle-world]") {
    ParticleRig rig;
    PhysicsObject& body = rig.game.create<PhysicsObject>();
    rig.game.set_parent(body.id(), workspace_of(rig.game));
    body.store_simulated(matrix4_identity(), Vec3{20.f, 0.f, 0.f});
    ParticleEmitter& e = rig.emitter(body.id());
    REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)).has_value());
    REQUIRE_FALSE(e.set_inherit_velocity(1.0).has_value());
    REQUIRE_FALSE(e.set_playing(false).has_value());
    e.request_emit(1);
    rig.steps(1);
    REQUIRE(rig.world.draws().front().particles->front().velocity[0] == Approx(20.f));

    Attachment& a = rig.attachment_at(0.f, 0.f, 0.f);
    ParticleEmitter& lone = rig.emitter(a.id(), "Lone");
    REQUIRE_FALSE(lone.set_emit_speed(NumberCurve::constant(0.f)).has_value());
    REQUIRE_FALSE(lone.set_inherit_velocity(1.0).has_value());
    REQUIRE_FALSE(lone.set_playing(false).has_value());
    lone.request_emit(1);
    rig.steps(1);
    for (const auto& draw : rig.world.draws()) {
        if (draw.emitter == lone.id()) {
            REQUIRE(draw.particles->front().velocity[0] == 0.f);
        }
    }
}

TEST_CASE("PW6 Stop clears what play spawned; a destroyed emitter's draw goes that frame", "[particle-world]") {
    ParticleRig rig;
    ParticleEmitter& e = rig.emitter(workspace_of(rig.game));
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(60.f)).has_value());
    rig.game.capture_place();
    rig.game.start_simulation();
    REQUIRE_FALSE(e.set_playing(false).has_value());
    e.request_emit(50);
    rig.steps(1);
    REQUIRE(e.particle_count() == 50);
    rig.game.stop_simulation();
    REQUIRE(e.playing());   // restored
    rig.steps(1);
    REQUIRE(e.particle_count() <= 2);   // cleared, then at most a frame's worth
    const InstanceId id = e.id();
    rig.game.destroy(id);
    rig.steps(1);
    REQUIRE(rig.world.draws().empty());
    REQUIRE(rig.world.emitter_count() == 0);
}

TEST_CASE("PW7 a reparented emitter keeps its particles; a destroyed parent takes them", "[particle-world]") {
    ParticleRig rig;
    Attachment& a = rig.attachment_at(0.f, 5.f, 0.f);
    ParticleEmitter& e = rig.emitter(a.id());
    rig.steps(30);
    const int before = e.particle_count();
    REQUIRE(before > 0);
    rig.game.set_parent(e.id(), workspace_of(rig.game));
    rig.steps(1);
    REQUIRE(e.particle_count() >= before);
    rig.game.set_parent(e.id(), a.id());
    rig.steps(1);
    rig.game.destroy_tree(a.id());
    rig.steps(1);
    REQUIRE(rig.world.draws().empty());
}

TEST_CASE("PW8 a PhysicsObject gives the shape its size; a point parent gives none", "[particle-world]") {
    ParticleRig rig;
    PhysicsObject& body = rig.game.create<PhysicsObject>();
    rig.game.set_parent(body.id(), workspace_of(rig.game));
    REQUIRE_FALSE(body.set_size(Vec3{4.f, 2.f, 6.f}).has_value());
    REQUIRE_FALSE(body.set_transform(matrix4_translation(0.f, 10.f, 0.f)).has_value());
    ParticleEmitter& e = rig.emitter(body.id());
    REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)).has_value());
    REQUIRE_FALSE(e.set_playing(false).has_value());
    e.request_emit(200);
    rig.steps(1);
    float spread_x = 0.f;
    for (const auto& p : *rig.world.draws().front().particles) {
        REQUIRE(std::fabs(p.position[0]) <= 2.001f);
        REQUIRE(std::fabs(p.position[1] - 10.f) <= 1.001f);
        REQUIRE(std::fabs(p.position[2]) <= 3.001f);
        spread_x = std::max(spread_x, std::fabs(p.position[0]));
    }
    REQUIRE(spread_x > 1.f);

    Matrix4 frame;
    Vec3 half;
    REQUIRE(emitter_extent(rig.game, body.id(), frame, half));
    REQUIRE(half.x == Approx(2.f));
    REQUIRE_FALSE(emitter_extent(rig.game, workspace_of(rig.game), frame, half));
    REQUIRE(half.x == 0.f);
}

TEST_CASE("PW9 a frozen step (paused play) moves nothing and spawns nothing", "[particle-world]") {
    ParticleRig rig;
    ParticleEmitter& e = rig.emitter(workspace_of(rig.game));
    REQUIRE_FALSE(e.set_acceleration(Vec3{0.f, -10.f, 0.f}).has_value());
    rig.steps(10);
    const int count = e.particle_count();
    const float y = rig.world.draws().front().particles->front().position[1];
    rig.steps(10, 0.0);
    REQUIRE(e.particle_count() == count);
    REQUIRE(rig.world.draws().front().particles->front().position[1] == y);
}

#include "Engine.hpp"

TEST_CASE("PW10 Engine steps particles each frame: stopped, playing, and frozen while paused", "[particle-world]") {
    SimRole role;
    Engine engine;   // never started: run_frame runs here
    DataModel& game = engine.datamodel();
    ParticleEmitter& e = game.create<ParticleEmitter>();
    game.set_parent(e.id(), workspace_of(game));
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(60.f)).has_value());
    REQUIRE(engine.paused());   // a stopped place
    for (int i = 0; i < 30; ++i) {
        engine.run_frame(1.0 / 60.0);
    }
    REQUIRE(e.particle_count() > 10);
    REQUIRE(engine.particles().live_total() == static_cast<std::size_t>(e.particle_count()));

    game.start_simulation();
    engine.resume();
    const int before = e.particle_count();
    for (int i = 0; i < 30; ++i) {
        engine.run_frame(1.0 / 60.0);
    }
    REQUIRE(e.particle_count() > before);

    engine.pause();   // paused play: frozen
    const int frozen = e.particle_count();
    for (int i = 0; i < 10; ++i) {
        engine.run_frame(1.0 / 60.0);
    }
    REQUIRE(e.particle_count() == frozen);
}

#include "Camera.hpp"

TEST_CASE("PW11 over ParticleBudget, far emitters spawn fewer first; live particles are never culled",
          "[particle-world]") {
    ParticleRig rig;
    auto* workspace = dynamic_cast<Workspace*>(rig.game.instance(workspace_of(rig.game)));
    REQUIRE_FALSE(workspace->set_particle_budget(90).has_value());
    Camera& camera = rig.game.create<Camera>();
    rig.game.set_parent(camera.id(), workspace_of(rig.game));
    REQUIRE_FALSE(camera.set_pv_transform(matrix4_translation(0.f, 0.f, -10.f)).has_value());
    REQUIRE(workspace->set_current_camera(camera.id()));
    Attachment& near_point = rig.attachment_at(0.f, 0.f, 0.f);
    Attachment& far_point = rig.attachment_at(0.f, 0.f, 500.f);
    ParticleEmitter& near_emitter = rig.emitter(near_point.id(), "Near");
    ParticleEmitter& far_emitter = rig.emitter(far_point.id(), "Far");
    for (ParticleEmitter* e : {&near_emitter, &far_emitter}) {
        REQUIRE_FALSE(e->set_rate(NumberCurve::constant(1000.f)).has_value());
        REQUIRE_FALSE(e->set_lifetime(NumberCurve::constant(100.f)).has_value());
    }
    rig.steps(30);
    REQUIRE(rig.world.live_total() <= 90);
    REQUIRE(rig.world.live_total() >= 80);
    REQUIRE(near_emitter.particle_count() > far_emitter.particle_count());

    REQUIRE_FALSE(workspace->set_particle_budget(10).has_value());
    const int before = near_emitter.particle_count() + far_emitter.particle_count();
    rig.steps(1);
    REQUIRE(near_emitter.particle_count() + far_emitter.particle_count() == before);   // nothing killed
}

TEST_CASE("PW12 huge rates, tiny lifetimes, and zero budgets stay bounded", "[particle-world]") {
    ParticleRig rig;
    ParticleEmitter& e = rig.emitter(workspace_of(rig.game));
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(1e9f)).has_value());
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(1e-6f)).has_value());
    rig.steps(3);
    REQUIRE(e.particle_count() <= 1000);
    REQUIRE_FALSE(e.set_max_particles(0).has_value());
    rig.steps(1);
    REQUIRE(e.particle_count() == 0);
    auto* workspace = dynamic_cast<Workspace*>(rig.game.instance(workspace_of(rig.game)));
    REQUIRE_FALSE(workspace->set_particle_budget(0).has_value());
    REQUIRE_FALSE(e.set_max_particles(1000).has_value());
    REQUIRE_FALSE(e.set_playing(false).has_value());
    e.request_emit(500);
    rig.steps(1);
    REQUIRE(e.particle_count() == 0);
}

#include "Brush.hpp"
#include "CollisionGroups.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "terrain/ShapeDistance.hpp"

TEST_CASE("PW13 Terrain collision stops particles at voxel ground", "[particle-world]") {
    ParticleRig rig;
    Terrain& terrain = rig.game.create<Terrain>();
    rig.game.set_parent(terrain.id(), workspace_of(rig.game));
    TerrainMaterial* entry = nullptr;
    REQUIRE_FALSE(terrain.add_material(0, entry).has_value());
    terrain::Shape slab;
    slab.kind = terrain::Shape::Kind::Block;
    slab.size = Vec3{40.f, 10.f, 40.f};
    slab.frame = matrix4_translation(0.f, -5.f, 0.f);   // solid below y = 0
    terrain::prepare_shape(slab);
    REQUIRE_FALSE(terrain.volume().fill(slab, static_cast<std::uint8_t>(entry->material_id())).has_value());
    Attachment& above = rig.attachment_at(0.f, 6.f, 0.f);
    ParticleEmitter& e = rig.emitter(above.id());
    REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)).has_value());
    REQUIRE_FALSE(e.set_acceleration(Vec3{0.f, -50.f, 0.f}).has_value());
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(10.f)).has_value());
    REQUIRE_FALSE(e.set_collision(static_cast<int>(ParticleCollision::Terrain)).has_value());
    REQUIRE_FALSE(e.set_collision_response(static_cast<int>(ParticleCollisionResponse::Stick)).has_value());
    REQUIRE_FALSE(e.set_playing(false).has_value());
    e.request_emit(20);
    rig.steps(120);
    REQUIRE(e.particle_count() == 20);
    for (const auto& p : *rig.world.draws().front().particles) {
        REQUIRE(p.position[1] > -1.f);   // within a voxel of the surface
        REQUIRE(p.position[1] < 0.6f);
    }
    // Without collision they fall through.
    REQUIRE_FALSE(e.set_collision(static_cast<int>(ParticleCollision::None)).has_value());
    e.request_emit(1);
    rig.steps(60);
    float lowest = 0.f;
    for (const auto& p : *rig.world.draws().front().particles) {
        lowest = std::min(lowest, p.position[1]);
    }
    REQUIRE(lowest < -5.f);
}

TEST_CASE("PW14 World collision hits a Brush, obeys CollisionGroup, and keeps to the ray budget",
          "[particle-world]") {
    ParticleRig rig;
    Brush& floor = rig.game.create<Brush>();   // a 4-unit box: its top at y = 2
    rig.game.set_parent(floor.id(), workspace_of(rig.game));
    floor.set_anchored(true);
    Attachment& above = rig.attachment_at(0.f, 8.f, 0.f);
    ParticleEmitter& e = rig.emitter(above.id());
    REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)).has_value());
    REQUIRE_FALSE(e.set_acceleration(Vec3{0.f, -40.f, 0.f}).has_value());
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(10.f)).has_value());
    REQUIRE_FALSE(e.set_collision(static_cast<int>(ParticleCollision::World)).has_value());
    REQUIRE_FALSE(e.set_collision_response(static_cast<int>(ParticleCollisionResponse::Stick)).has_value());
    REQUIRE_FALSE(e.set_playing(false).has_value());
    e.request_emit(10);
    rig.steps(90);
    REQUIRE(e.particle_count() == 10);
    for (const auto& p : *rig.world.draws().front().particles) {
        REQUIRE(p.position[1] == Approx(2.f).margin(0.05f));
    }
    REQUIRE(rig.world.rays_last_step() >= 0);

    // A group that does not collide with the floor's: they fall through.
    CollisionGroups& groups = rig.game.collision_groups();
    REQUIRE_FALSE(groups.register_group("Particles", CollisionGroups::FilterType::Exclude).has_value());
    REQUIRE_FALSE(groups.set_collidable("Particles", "Default", false).has_value());
    e.request_clear();
    REQUIRE_FALSE(e.set_collision_group("Particles").has_value());
    e.request_emit(5);
    rig.steps(90);
    for (const auto& p : *rig.world.draws().front().particles) {
        REQUIRE(p.position[1] < -5.f);
    }

    // Thousands of particles in one step cast no more than the budget's rays.
    REQUIRE_FALSE(e.set_collision_group("Default").has_value());
    REQUIRE_FALSE(e.set_max_particles(5000).has_value());
    e.request_clear();
    e.request_emit(5000);
    rig.steps(2);
    REQUIRE(rig.world.rays_last_step() > 0);
    REQUIRE(rig.world.rays_last_step() <= ParticleWorld::kRaysPerStep);
}

#include "SnapshotPump.hpp"

TEST_CASE("PW15 the render snapshot carries each drawn emitter's particles by pointer", "[particle-world]") {
    ParticleRig rig;
    SnapshotPump pump;
    pump.reserve(DataModel::kMaxInstances);
    pump.set_particle_world(&rig.world);
    ParticleEmitter& e = rig.emitter(workspace_of(rig.game));
    REQUIRE_FALSE(e.set_blend_mode(static_cast<int>(ParticleBlendMode::Additive)).has_value());
    REQUIRE_FALSE(e.set_brightness(3.0).has_value());
    rig.steps(10);
    pump.prepare_copy(rig.game);
    pump.publish();
    const VisualSnapshot& snap = pump.front();
    REQUIRE(snap.particles.size() == 1);
    REQUIRE(snap.particles[0].particles.get() == rig.world.draws()[0].particles.get());   // shared, not copied
    REQUIRE(snap.particles[0].blend == ParticleBlendMode::Additive);
    REQUIRE(snap.particles[0].brightness == 3.f);
    pump.set_particle_world(nullptr);
    pump.prepare_copy(rig.game);
    pump.publish();
    REQUIRE(pump.front().particles.empty());
}

#include "particles/ParticleColliders.hpp"

TEST_CASE("PW16 an edit-mode one-shot replays and never saves Playing false", "[particle-world]") {
    ParticleRig rig;
    ParticleEmitter& e = rig.emitter(workspace_of(rig.game));
    REQUIRE_FALSE(e.set_looping(false).has_value());
    REQUIRE_FALSE(e.set_duration(0.3).has_value());
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(0.2f)).has_value());
    REQUIRE_FALSE(rig.game.simulation_running());
    rig.steps(60);   // a second: the cycle ended and its particles died, more than once
    REQUIRE(e.playing());
    rig.steps(5);
    REQUIRE(e.particle_count() > 0);   // the preview runs again
    PropertyBag saved;
    e.save_properties(saved);
    REQUIRE(bag_find(saved, "Playing") == nullptr);
}

TEST_CASE("PW17 a particle's hit plane goes with it when another dies, never to a newborn", "[particle-world]") {
    particles::PlaneCache planes;
    planes.begin(3);
    planes.set(2, Vec3{0.f, 1.f, 0.f}, 0.f);
    REQUIRE(planes.crossing(2, Vec3{0.f, 1.f, 0.f}, Vec3{0.f, -1.f, 0.f}).has_value());
    planes.moved(2, 0);   // slot 0 died; the particle in slot 2 moves there
    REQUIRE(planes.crossing(0, Vec3{0.f, 1.f, 0.f}, Vec3{0.f, -1.f, 0.f}).has_value());
    planes.begin(3);      // a newborn takes slot 2 again
    REQUIRE_FALSE(planes.crossing(2, Vec3{0.f, 1.f, 0.f}, Vec3{0.f, -1.f, 0.f}).has_value());
}

TEST_CASE("PW18 the ray budget is shared, so every World emitter collides", "[particle-world]") {
    ParticleRig rig;
    for (int i = 0; i < 2; ++i) {
        Brush& floor = rig.game.create<Brush>();   // a 4-unit box with its top at y = 2
        rig.game.set_parent(floor.id(), workspace_of(rig.game));
        floor.set_anchored(true);
        REQUIRE_FALSE(floor.set_transform(matrix4_translation(i * 20.f, 0.f, 0.f)).has_value());
    }
    std::vector<ParticleEmitter*> emitters;
    for (int i = 0; i < 2; ++i) {
        Attachment& above = rig.attachment_at(i * 20.f, 8.f, 0.f);
        ParticleEmitter& e = rig.emitter(above.id(), i == 0 ? "A" : "B");
        REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)).has_value());
        REQUIRE_FALSE(e.set_acceleration(Vec3{0.f, -40.f, 0.f}).has_value());
        REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(10.f)).has_value());
        REQUIRE_FALSE(e.set_max_particles(2000).has_value());
        REQUIRE_FALSE(e.set_collision(static_cast<int>(ParticleCollision::World)).has_value());
        REQUIRE_FALSE(e.set_collision_response(static_cast<int>(ParticleCollisionResponse::Stick)).has_value());
        REQUIRE_FALSE(e.set_playing(false).has_value());
        e.request_emit(1500);
        emitters.push_back(&e);
    }
    rig.steps(120);
    for (const auto& draw : rig.world.draws()) {
        int fell_through = 0;
        for (const auto& p : *draw.particles) {
            fell_through += p.position[1] < 1.f ? 1 : 0;
        }
        INFO("emitter " << draw.emitter);
        REQUIRE(fell_through == 0);
    }
}
