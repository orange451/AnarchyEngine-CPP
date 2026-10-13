#pragma once

#include "Curve.hpp"
#include "DataModel.hpp"
#include "Enum.hpp"
#include "InstanceRef.hpp"
#include "Vector2.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {

// One row of ParticleEmitter.Bursts: Count particles at Time seconds into each cycle.
struct ParticleBurst {
    float time = 0.f;
    NumberCurve count = NumberCurve::constant(10.f);   // a Constant or a Range

    bool operator==(const ParticleBurst& other) const { return time == other.time && count == other.count; }
};

// Emits particles from its parent PVInstance, whose transform it follows and
// whose size, if it has one, gives its Shape a size (spec
// docs/superpowers/specs/2026-10-12-particle-emitter-design.md). ParticleWorld
// simulates every one in Workspace, stopped or playing. Properties are saved
// registry properties; each setter refuses a bad value, saying why. Writes
// the engine makes (Playing at a one-shot's end, ParticleCount) go through
// store_* and record nothing.
class ParticleEmitter : public DataModel {
public:
    enum Dirty : std::uint32_t {
        kDirtySettings = 1u << 0,   // a property the simulation reads changed
        kDirtyRestart = 1u << 1,    // Playing went true, or Seed changed: start a cycle
        kDirtyAll = 0x3u,
    };

    ParticleEmitter(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const char* class_name() const override { return "ParticleEmitter"; }
    bool particle_emitter() const override { return true; }

    // Emission
    bool playing() const { return playing_; }
    std::optional<std::string> set_playing(bool value);
    const NumberCurve& rate() const { return rate_; }
    std::optional<std::string> set_rate(NumberCurve value);
    const std::vector<ParticleBurst>& bursts() const { return bursts_; }
    std::optional<std::string> set_bursts(std::vector<ParticleBurst> value);
    double duration() const { return duration_; }
    std::optional<std::string> set_duration(double value);
    bool looping() const { return looping_; }
    std::optional<std::string> set_looping(bool value);
    bool prewarm() const { return prewarm_; }
    std::optional<std::string> set_prewarm(bool value);
    int max_particles() const { return static_cast<int>(max_particles_); }
    std::optional<std::string> set_max_particles(double value);
    int seed() const { return static_cast<int>(seed_); }
    std::optional<std::string> set_seed(double value);

    // Shape
    ParticleShape shape() const { return static_cast<ParticleShape>(shape_); }
    std::optional<std::string> set_shape(int value);
    ParticleEmitFrom emit_from() const { return static_cast<ParticleEmitFrom>(emit_from_); }
    std::optional<std::string> set_emit_from(int value);
    double arc() const { return arc_; }
    std::optional<std::string> set_arc(double value);
    Vec3 emit_direction() const { return emit_direction_; }
    std::optional<std::string> set_emit_direction(Vec3 value);
    TransformSpace emit_direction_space() const { return static_cast<TransformSpace>(emit_direction_space_); }
    std::optional<std::string> set_emit_direction_space(int value);
    Vec2 emit_spread() const { return emit_spread_; }
    std::optional<std::string> set_emit_spread(Vec2 value);

    // Particle
    const NumberCurve& lifetime() const { return lifetime_; }
    std::optional<std::string> set_lifetime(NumberCurve value);
    const NumberCurve& emit_speed() const { return emit_speed_; }
    std::optional<std::string> set_emit_speed(NumberCurve value);
    const NumberCurve& emit_rotation() const { return emit_rotation_; }
    std::optional<std::string> set_emit_rotation(NumberCurve value);
    const NumberCurve& rotation_speed() const { return rotation_speed_; }
    std::optional<std::string> set_rotation_speed(NumberCurve value);

    // Motion
    TransformSpace simulation_space() const { return static_cast<TransformSpace>(simulation_space_); }
    std::optional<std::string> set_simulation_space(int value);
    double inherit_velocity() const { return inherit_velocity_; }
    std::optional<std::string> set_inherit_velocity(double value);
    Vec3 acceleration() const { return acceleration_; }
    std::optional<std::string> set_acceleration(Vec3 value);
    TransformSpace acceleration_space() const { return static_cast<TransformSpace>(acceleration_space_); }
    std::optional<std::string> set_acceleration_space(int value);
    const NumberCurve& drag() const { return drag_; }
    std::optional<std::string> set_drag(NumberCurve value);
    const NumberCurve& orbit_speed() const { return orbit_speed_; }
    std::optional<std::string> set_orbit_speed(NumberCurve value);
    Vec3 orbit_axis() const { return orbit_axis_; }
    std::optional<std::string> set_orbit_axis(Vec3 value);

    // Collision
    ParticleCollision collision() const { return static_cast<ParticleCollision>(collision_); }
    std::optional<std::string> set_collision(int value);
    ParticleCollisionResponse collision_response() const {
        return static_cast<ParticleCollisionResponse>(collision_response_);
    }
    std::optional<std::string> set_collision_response(int value);
    double bounciness() const { return bounciness_; }
    std::optional<std::string> set_bounciness(double value);
    double friction() const { return friction_; }
    std::optional<std::string> set_friction(double value);
    const std::string& collision_group() const { return collision_group_; }
    std::optional<std::string> set_collision_group(std::string value);

    // Appearance
    // The Texture, held by GUID: a Texture? reference, as SoundEmitter's Sound.
    LuaSlot texture() const;
    std::optional<std::string> set_texture(const LuaSlot& value);
    ParticleTextureMode texture_mode() const { return static_cast<ParticleTextureMode>(texture_mode_); }
    std::optional<std::string> set_texture_mode(int value);
    Vec2 flipbook_grid() const { return flipbook_grid_; }
    std::optional<std::string> set_flipbook_grid(Vec2 value);
    const ColorCurve& color() const { return color_; }
    std::optional<std::string> set_color(ColorCurve value);
    const NumberCurve& size() const { return size_; }
    std::optional<std::string> set_size(NumberCurve value);
    double brightness() const { return brightness_; }
    std::optional<std::string> set_brightness(double value);
    const NumberCurve& stretch() const { return stretch_; }
    std::optional<std::string> set_stretch(NumberCurve value);

    // Rendering
    ParticleFacing facing() const { return static_cast<ParticleFacing>(facing_); }
    std::optional<std::string> set_facing(int value);
    Vec3 facing_axis() const { return facing_axis_; }
    std::optional<std::string> set_facing_axis(Vec3 value);
    TransformSpace facing_axis_space() const { return static_cast<TransformSpace>(facing_axis_space_); }
    std::optional<std::string> set_facing_axis_space(int value);
    double velocity_stretch() const { return velocity_stretch_; }
    std::optional<std::string> set_velocity_stretch(double value);
    ParticleBlendMode blend_mode() const { return static_cast<ParticleBlendMode>(blend_mode_); }
    std::optional<std::string> set_blend_mode(int value);
    double soft_edge() const { return soft_edge_; }
    std::optional<std::string> set_soft_edge(double value);
    double light_influence() const { return light_influence_; }
    std::optional<std::string> set_light_influence(double value);
    int draw_order() const { return static_cast<int>(draw_order_); }
    std::optional<std::string> set_draw_order(double value);

    int particle_count() const { return particle_count_; }

    // ParticleWorld's side.
    std::uint32_t take_dirty() {
        const std::uint32_t out = dirty_;
        dirty_ = 0;
        return out;
    }
    // Emit's counts not yet spawned, and whether Clear was called, each taken once.
    int take_emit_requests() {
        const int out = emit_requests_;
        emit_requests_ = 0;
        return out;
    }
    bool take_clear() {
        const bool out = clear_requested_;
        clear_requested_ = false;
        return out;
    }
    void request_emit(int count);
    void request_clear();
    // A one-shot's cycle ended: Playing becomes false, Changed fires, nothing is recorded.
    void store_finished_cycle();
    // Fires Changed for ParticleCount only when the count moved.
    void store_particle_count(int count);

protected:
    void on_reuse() override;
    void read_place(const std::byte* data, std::size_t size) override;

private:
    // The shared checks behind each setter: refuse, or store, mark dirty, and record.
    std::optional<std::string> set_number_property(const char* name, double& slot, double value, double low,
                                                   double high, bool whole, const char* range_text);
    std::optional<std::string> set_bool_property(const char* name, bool& slot, bool value);
    std::optional<std::string> set_enum_property(const char* name, const EnumType& type, int& slot, int value);
    std::optional<std::string> set_axis_property(const char* name, Vec3& slot, Vec3 value);
    std::optional<std::string> set_vector_property(const char* name, Vec3& slot, Vec3 value);
    // What a curve's values must be.
    enum class CurveRule { Any, NotNegative, AboveZero };
    std::optional<std::string> set_curve(const char* name, NumberCurve& slot, NumberCurve value, CurveRule rule);
    void reset();

    bool playing_ = true;
    NumberCurve rate_ = NumberCurve::constant(20.f);
    std::vector<ParticleBurst> bursts_;
    double duration_ = 5.0;
    bool looping_ = true;
    bool prewarm_ = false;
    double max_particles_ = 1000.0;
    double seed_ = 0.0;
    int shape_ = static_cast<int>(ParticleShape::Box);
    int emit_from_ = static_cast<int>(ParticleEmitFrom::Volume);
    double arc_ = 360.0;
    Vec3 emit_direction_{0.f, 1.f, 0.f};
    int emit_direction_space_ = static_cast<int>(TransformSpace::Local);
    Vec2 emit_spread_{0.f, 0.f};
    NumberCurve lifetime_ = NumberCurve::range(1.f, 2.f);
    NumberCurve emit_speed_ = NumberCurve::constant(5.f);
    NumberCurve emit_rotation_ = NumberCurve::constant(0.f);
    NumberCurve rotation_speed_ = NumberCurve::constant(0.f);
    int simulation_space_ = static_cast<int>(TransformSpace::World);
    double inherit_velocity_ = 0.0;
    Vec3 acceleration_{0.f, 0.f, 0.f};
    int acceleration_space_ = static_cast<int>(TransformSpace::World);
    NumberCurve drag_ = NumberCurve::constant(0.f);
    NumberCurve orbit_speed_ = NumberCurve::constant(0.f);
    Vec3 orbit_axis_{0.f, 1.f, 0.f};
    int collision_ = static_cast<int>(ParticleCollision::None);
    int collision_response_ = static_cast<int>(ParticleCollisionResponse::Bounce);
    double bounciness_ = 0.5;
    double friction_ = 0.2;
    std::string collision_group_ = "Default";
    InstanceRef texture_ref_;
    int texture_mode_ = static_cast<int>(ParticleTextureMode::Static);
    Vec2 flipbook_grid_{1.f, 1.f};
    ColorCurve color_ = ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, 0.f);
    NumberCurve size_ = NumberCurve::constant(1.f);
    double brightness_ = 1.0;
    NumberCurve stretch_ = NumberCurve::constant(0.f);
    int facing_ = static_cast<int>(ParticleFacing::Camera);
    Vec3 facing_axis_{0.f, 1.f, 0.f};
    int facing_axis_space_ = static_cast<int>(TransformSpace::Local);
    double velocity_stretch_ = 0.1;
    int blend_mode_ = static_cast<int>(ParticleBlendMode::Normal);
    double soft_edge_ = 0.5;
    double light_influence_ = 1.0;
    double draw_order_ = 0.0;

    std::uint32_t dirty_ = kDirtyAll;
    int emit_requests_ = 0;
    bool clear_requested_ = false;
    int particle_count_ = 0;
};

}  // namespace engine_core
