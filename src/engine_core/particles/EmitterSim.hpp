#pragma once

#include "Curve.hpp"
#include "Enum.hpp"
#include "Matrix4.hpp"
#include "Vector3.hpp"
#include "particles/ParticleRandom.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine_core::particles {

// Everything a simulation reads from its emitter, curves already baked.
// ParticleWorld builds it whenever the emitter's settings change.
struct ParticleSettings {
    // Emission. Curves over the cycle: 0 at its start, 1 at Duration.
    CurveTable rate;
    struct Burst {
        float time = 0.f;
        CurveTable count;
    };
    std::vector<Burst> bursts;   // sorted by time
    float duration = 5.f;
    bool looping = true;
    int max_particles = 1000;
    std::uint32_t seed = 0;      // 0: the fresh seed restart is given
    // Shape
    ParticleShape shape = ParticleShape::Box;
    ParticleEmitFrom emit_from = ParticleEmitFrom::Volume;
    float arc = 360.f;
    Vec3 emit_direction{0.f, 1.f, 0.f};
    bool emit_direction_local = true;
    float spread_x = 0.f;
    float spread_y = 0.f;
    // Values at birth, over the cycle.
    CurveTable lifetime;
    CurveTable emit_speed;
    CurveTable emit_rotation;    // degrees
    CurveTable rotation_speed;   // degrees per second
    // Motion. Curves over each particle's life.
    bool local_space = false;
    float inherit_velocity = 0.f;
    Vec3 acceleration{0.f, 0.f, 0.f};
    bool acceleration_local = false;
    CurveTable drag;
    CurveTable orbit_speed;      // degrees per second
    Vec3 orbit_axis{0.f, 1.f, 0.f};
    // Collision
    ParticleCollision collision = ParticleCollision::None;
    ParticleCollisionResponse response = ParticleCollisionResponse::Bounce;
    float bounciness = 0.5f;
    float friction = 0.2f;
    // Appearance, over each particle's life.
    ColorTable color;
    CurveTable size;
    CurveTable stretch;
    int frames = 1;              // flipbook cells; 1 for a static texture
};

// Where the emitter is this step.
struct EmitterFrame {
    Matrix4 previous = matrix4_identity();   // the shape's frame last step (current on the first)
    Matrix4 current = matrix4_identity();
    Vec3 half{0.f, 0.f, 0.f};                // half the shape's box; zero is a point
    Vec3 inherited{0.f, 0.f, 0.f};           // the parent body's velocity at the emitter, world
    float dt = 0.f;
};

struct ParticleHit {
    Vec3 point;
    Vec3 normal;   // unit, toward the side the particle came from
};

// What particles hit. Asked once per moving, unstuck particle each step, in
// world space. slot is the particle's index for this step.
class ParticleCollider {
public:
    virtual ~ParticleCollider() = default;
    virtual void begin(std::size_t count) = 0;
    virtual std::optional<ParticleHit> test(std::size_t slot, Vec3 from, Vec3 to) = 0;
    // A dead particle's slot was filled by the one at from.
    virtual void moved(std::size_t from, std::size_t to) = 0;
};

// One drawn particle, in world space, as the renderer reads it: 16 floats.
struct ParticleVertex {
    float position[3];
    float size;        // units
    float color[4];    // straight RGBA
    float velocity[3]; // world, for Velocity facing
    float rotation;    // radians
    float stretch;
    float frame;       // the flipbook cell, a whole number
};

// One emitter's particles, kept as parallel arrays. Knows nothing of the
// DataModel, so it is tested alone.
class EmitterSim {
public:
    // Keeps live particles, unless the simulation space changed.
    void configure(const ParticleSettings& settings);
    // Cycle time 0, bursts rearmed, seeded from the settings or fresh_seed.
    void restart(std::uint32_t fresh_seed);
    // How many particles a step of dt would spawn, without taking it.
    int wanted_spawns(float dt, bool playing, int pending_emits) const;
    // One step. allowance caps this step's spawns; MaxParticles caps the live count.
    void step(const EmitterFrame& frame, bool playing, int pending_emits, int allowance, ParticleCollider* collider);
    void clear();
    // A looping emitter's state one cycle in, at 1/30 s steps, without
    // collision; then its cycle starts again.
    void prewarm(const EmitterFrame& frame);

    std::size_t count() const { return age_.size(); }
    float cycle_time() const { return cycle_time_; }
    bool cycle_over() const { return cycle_over_; }
    // True once, the step a non-looping cycle runs out.
    bool take_cycle_ended();

    // Every particle in world space; back to front from eye when sort is true.
    void pack(std::vector<ParticleVertex>& out, const Matrix4& emitter_world, Vec3 eye, bool sort) const;

    Vec3 position(std::size_t i) const { return pos_[i]; }
    Vec3 velocity(std::size_t i) const { return vel_[i]; }
    float age(std::size_t i) const { return age_[i]; }

private:
    // count particles born at fractions of the step: spread 0 spreads them
    // from its start to its end, else each is born at birth_ago seconds
    // before the step's end.
    void spawn(const EmitterFrame& frame, int count, bool spread, float birth_ago);
    void kill(std::size_t i, ParticleCollider* collider);
    // Moves particle i on by dt: acceleration, drag, orbit, spin.
    void integrate(std::size_t i, float dt, Vec3 accel, Vec3 centre, Vec3 orbit_axis);

    ParticleSettings s_;
    Pcg32 rng_;
    float cycle_time_ = 0.f;
    float rate_blend_ = 0.f;   // the cycle's pick for a Range Rate
    double rate_carry_ = 0.0;
    std::size_t next_burst_ = 0;
    bool cycle_over_ = false;
    bool cycle_ended_ = false;
    // pos_ and vel_ are world space, or the emitter's space when local_space.
    std::vector<Vec3> pos_;
    std::vector<Vec3> vel_;
    std::vector<float> age_;
    std::vector<float> life_;
    std::vector<float> random_;
    std::vector<float> rot_;
    std::vector<float> spin_;
    std::vector<std::uint8_t> stuck_;
};

}  // namespace engine_core::particles
