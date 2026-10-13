// ParticleEmitter: its properties, their checks, undo, save, and Stop; and
// Workspace.ParticleBudget.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "ParticleEmitter.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SceneService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <optional>
#include <string>

using namespace engine_core;

namespace {

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

ParticleEmitter& add_emitter(DataModel& game, InstanceId parent) {
    ParticleEmitter& emitter = game.create<ParticleEmitter>();
    game.set_parent(emitter.id(), parent);
    return emitter;
}

}  // namespace

TEST_CASE("PE1 a new ParticleEmitter has the spec's defaults and saves nothing", "[particles]") {
    SimRole role;
    Game game;
    REQUIRE(project_class_known("ParticleEmitter"));
    ParticleEmitter& e = add_emitter(game, workspace_of(game));
    PropertyBag saved;
    e.save_properties(saved);
    REQUIRE(saved.empty());
    REQUIRE(e.playing());
    REQUIRE(e.rate() == NumberCurve::constant(20.f));
    REQUIRE(e.bursts().empty());
    REQUIRE(e.duration() == 5.0);
    REQUIRE(e.looping());
    REQUIRE_FALSE(e.prewarm());
    REQUIRE(e.max_particles() == 1000);
    REQUIRE(e.seed() == 0);
    REQUIRE(e.shape() == ParticleShape::Box);
    REQUIRE(e.emit_from() == ParticleEmitFrom::Volume);
    REQUIRE(e.arc() == 360.0);
    REQUIRE(e.emit_direction().y == 1.f);
    REQUIRE(e.emit_direction_space() == TransformSpace::Local);
    REQUIRE(e.emit_spread().x == 0.f);
    REQUIRE(e.lifetime() == NumberCurve::range(1.f, 2.f));
    REQUIRE(e.emit_speed() == NumberCurve::constant(5.f));
    REQUIRE(e.simulation_space() == TransformSpace::World);
    REQUIRE(e.acceleration_space() == TransformSpace::World);
    REQUIRE(e.drag() == NumberCurve::constant(0.f));
    REQUIRE(e.orbit_axis().y == 1.f);
    REQUIRE(e.collision() == ParticleCollision::None);
    REQUIRE(e.collision_response() == ParticleCollisionResponse::Bounce);
    REQUIRE(e.bounciness() == 0.5);
    REQUIRE(e.friction() == 0.2);
    REQUIRE(e.collision_group() == "Default");
    REQUIRE(e.texture_mode() == ParticleTextureMode::Static);
    REQUIRE(e.flipbook_grid().x == 1.f);
    REQUIRE(e.color() == ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, 0.f));
    REQUIRE(e.size() == NumberCurve::constant(1.f));
    REQUIRE(e.brightness() == 1.0);
    REQUIRE(e.facing() == ParticleFacing::Camera);
    REQUIRE(e.velocity_stretch() == 0.1);
    REQUIRE(e.blend_mode() == ParticleBlendMode::Normal);
    REQUIRE(e.soft_edge() == 0.5);
    REQUIRE(e.light_influence() == 1.0);
    REQUIRE(e.draw_order() == 0);
    REQUIRE(e.particle_count() == 0);
}

TEST_CASE("PE2 writes are checked, undo as one step, save, and come back at Stop", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& e = add_emitter(game, workspace_of(game));
    begin_step(game, "Set Rate");
    REQUIRE_FALSE(e.set_rate(NumberCurve::range(5.f, 10.f)).has_value());
    end_step(game);
    REQUIRE(e.rate() == NumberCurve::range(5.f, 10.f));
    game.history().undo();
    REQUIRE(e.rate() == NumberCurve::constant(20.f));
    game.history().redo();
    REQUIRE(e.rate() == NumberCurve::range(5.f, 10.f));

    REQUIRE(reason(e.set_duration(0.0)) == "Duration must be above 0");
    REQUIRE(reason(e.set_rate(NumberCurve::constant(-1.f))) == "Rate must not be negative");
    REQUIRE(reason(e.set_emit_direction(Vec3{0, 0, 0})) == "EmitDirection must not be zero");
    REQUIRE(reason(e.set_max_particles(1.5)) == "MaxParticles must be a whole number, 0 or more");
    REQUIRE(reason(e.set_flipbook_grid(Vec2{0.f, 2.f})) == "FlipbookGrid must be whole numbers, 1 or more");
    REQUIRE(reason(e.set_lifetime(NumberCurve::range(0.f, 1.f))) == "Lifetime must be above 0");
    REQUIRE(reason(e.set_arc(400.0)) == "Arc must be from 0 to 360");
    REQUIRE(reason(e.set_emit_spread(Vec2{10.f, 200.f})) == "EmitSpread must be from 0 to 180 on each axis");
    REQUIRE(reason(e.set_size(NumberCurve::constant(-1.f))) == "Size must not be negative");
    REQUIRE(reason(e.set_bounciness(2.0)) == "Bounciness must be from 0 to 1");
    REQUIRE(reason(e.set_soft_edge(std::nan(""))) == "SoftEdge must be a finite number");
    REQUIRE(e.duration() == 5.0);

    REQUIRE_FALSE(e.set_emit_direction(Vec3{0, 0, 5}).has_value());
    REQUIRE(e.emit_direction().z == 1.f);

    PropertyBag saved;
    e.save_properties(saved);
    REQUIRE(bag_find(saved, "Rate") != nullptr);
    REQUIRE(bag_find(saved, "EmitDirection") != nullptr);

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(99.f)).has_value());
    REQUIRE_FALSE(e.set_playing(false).has_value());
    game.stop_simulation();
    REQUIRE(e.rate() == NumberCurve::range(5.f, 10.f));
    REQUIRE(e.playing());
}

TEST_CASE("PE3 Workspace.ParticleBudget defaults to 20000 and refuses negatives", "[particles]") {
    SimRole role;
    Game game;
    auto* workspace = dynamic_cast<Workspace*>(game.instance(workspace_of(game)));
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->particle_budget() == 20000);
    REQUIRE(reason(workspace->set_particle_budget(-1)) == "ParticleBudget must be a whole number, 0 or more");
    REQUIRE_FALSE(workspace->set_particle_budget(0).has_value());
    REQUIRE(workspace->particle_budget() == 0);
}

TEST_CASE("PE4 Emit and Clear requests are taken once; Playing from false to true asks for a restart", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& e = add_emitter(game, workspace_of(game));
    (void)e.take_dirty();
    e.request_emit(5);
    e.request_emit(3);
    e.request_emit(-2);
    REQUIRE(e.take_emit_requests() == 8);
    REQUIRE(e.take_emit_requests() == 0);
    e.request_clear();
    REQUIRE(e.take_clear());
    REQUIRE_FALSE(e.take_clear());
    REQUIRE_FALSE(e.set_playing(false).has_value());
    (void)e.take_dirty();
    REQUIRE_FALSE(e.set_playing(true).has_value());
    REQUIRE((e.take_dirty() & ParticleEmitter::kDirtyRestart) != 0);
    REQUIRE_FALSE(e.set_playing(true).has_value());
    REQUIRE((e.take_dirty() & ParticleEmitter::kDirtyRestart) == 0);
    e.store_finished_cycle();
    REQUIRE_FALSE(e.playing());
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("PE5 Bursts save as a JSON array, undo, and refuse bad rows", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& e = add_emitter(game, workspace_of(game));
    begin_step(game, "Set Bursts");
    REQUIRE_FALSE(e.set_bursts({ParticleBurst{1.5f, NumberCurve::constant(5.f)},
                                ParticleBurst{0.f, NumberCurve::range(10.f, 20.f)}})
                      .has_value());
    end_step(game);
    REQUIRE(e.bursts().size() == 2);
    REQUIRE(e.bursts()[0].time == 0.f);   // sorted by time
    PropertyBag saved;
    e.save_properties(saved);
    const JsonValue* bursts = bag_find(saved, "Bursts");
    REQUIRE(bursts != nullptr);
    REQUIRE(compact_json(*bursts) ==
            R"([{"count":{"max":20,"min":10,"mode":"Range"},"time":0},{"count":{"mode":"Constant","value":5},"time":1.5}])");
    game.history().undo();
    REQUIRE(e.bursts().empty());
    REQUIRE(reason(e.set_bursts({ParticleBurst{-1.f, NumberCurve::constant(1.f)}})) ==
            "a burst's Time must be 0 or more");
    std::string error;
    const auto keyed = NumberCurve::keys({{0.f, 1.f}}, error);
    REQUIRE(reason(e.set_bursts({ParticleBurst{0.f, *keyed}})) ==
            "a burst's Count must be a Constant or a Range, 0 or more");
    REQUIRE(reason(e.set_bursts({ParticleBurst{0.f, NumberCurve::constant(-3.f)}})) ==
            "a burst's Count must be a Constant or a Range, 0 or more");

    // A saved list loads back.
    game.history().redo();
    PropertyBag again;
    e.save_properties(again);
    Game other;
    ParticleEmitter& copy = add_emitter(other, workspace_of(other));
    std::string why;
    REQUIRE(copy.load_property("Bursts", *bag_find(again, "Bursts"), why));
    REQUIRE(copy.bursts() == e.bursts());
}

TEST_CASE("PE6 a Color that is not finite is refused", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& e = add_emitter(game, workspace_of(game));
    REQUIRE(reason(e.set_color(ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, std::nanf("")))) ==
            "Color must be finite");
    REQUIRE(reason(e.set_color(ColorCurve::between(ColorRgb{INFINITY, 0.f, 0.f, 1.f}, 0.f, ColorRgb{}, 0.f))) ==
            "Color must be finite");
    REQUIRE(e.color() == ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, 0.f));
}

TEST_CASE("PE7 Emit requests saturate rather than overflow", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& e = add_emitter(game, workspace_of(game));
    e.request_emit(INT_MAX);
    e.request_emit(INT_MAX);
    REQUIRE(e.take_emit_requests() == INT_MAX);
}
