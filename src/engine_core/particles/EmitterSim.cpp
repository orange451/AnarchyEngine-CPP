#include "particles/EmitterSim.hpp"

#include "particles/ParticleMath.hpp"
#include "particles/ParticleShapes.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <numeric>

namespace engine_core::particles {
namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.f;
// Rate's carry loses a little to float sums; this keeps 10 a second at 60
// steps a second from giving 9.
constexpr double kCarrySlack = 1e-4;
// Far above any budget, so a huge Rate never overflows an int.
constexpr double kMostSpawns = 1.0e7;

Vec3 direction_in(const Matrix4& m, Vec3 d) { return normalize(matrix4_vector(m, d)); }

int burst_count(const CurveTable& count, float pick) {
    return static_cast<int>(std::max(0.f, count.sample(0.f, pick)) + 0.5f);
}

}  // namespace

void EmitterSim::configure(const ParticleSettings& settings) {
    const bool space_changed = settings.local_space != s_.local_space;
    s_ = settings;
    if (s_.duration <= 0.f) {
        s_.duration = 1e-3f;
    }
    if (space_changed) {
        clear();   // the stored positions would mean something else
    }
}

void EmitterSim::restart(std::uint32_t fresh_seed) {
    rng_.reseed(s_.seed != 0 ? s_.seed : fresh_seed);
    cycle_time_ = 0.f;
    rate_carry_ = 0.0;
    next_burst_ = 0;
    cycle_over_ = false;
    cycle_ended_ = false;
    rate_blend_ = rng_.uniform();
}

int EmitterSim::wanted_spawns(float dt, bool playing, int pending_emits) const {
    double want = std::max(0, pending_emits);
    if (playing && !cycle_over_ && dt > 0.f) {
        const float t = std::min(cycle_time_ / s_.duration, 1.f);
        want += std::floor(rate_carry_ + std::max(0.f, s_.rate.sample(t, rate_blend_)) * dt + kCarrySlack);
        const float end = cycle_time_ + dt;
        for (std::size_t b = next_burst_; b < s_.bursts.size() && s_.bursts[b].time < end; ++b) {
            // The Range's middle; the step draws the real count.
            want += burst_count(s_.bursts[b].count, 0.5f);
        }
    }
    return static_cast<int>(std::min(want, kMostSpawns));
}

void EmitterSim::step(const EmitterFrame& frame, bool playing, int pending_emits, int allowance,
                      ParticleCollider* collider) {
    const float dt = std::max(0.f, frame.dt);
    const Matrix4& m = frame.current;
    const Matrix4 inverse = s_.local_space ? matrix4_inverse(m) : matrix4_identity();

    // 1. Move the particles alive before this step.
    Vec3 accel = s_.acceleration;
    if (s_.local_space && !s_.acceleration_local) {
        accel = matrix4_vector(inverse, accel);
    } else if (!s_.local_space && s_.acceleration_local) {
        accel = matrix4_vector(m, accel);
    }
    const Vec3 centre = s_.local_space ? Vec3{0.f, 0.f, 0.f} : matrix4_position(m);
    const Vec3 orbit_axis = s_.local_space ? normalize(s_.orbit_axis) : direction_in(m, s_.orbit_axis);
    const bool colliding = collider != nullptr && s_.collision != ParticleCollision::None;
    if (colliding) {
        collider->begin(count());
    }
    if (dt > 0.f) {
        for (std::size_t i = 0; i < count();) {
            age_[i] += dt;
            if (age_[i] >= life_[i]) {
                kill(i, collider);
                continue;
            }
            if (stuck_[i] != 0) {
                ++i;
                continue;
            }
            const Vec3 from = pos_[i];
            integrate(i, dt, accel, centre, orbit_axis);
            if (colliding) {
                const Vec3 world_from = s_.local_space ? matrix4_point(m, from) : from;
                const Vec3 world_to = s_.local_space ? matrix4_point(m, pos_[i]) : pos_[i];
                if (std::optional<ParticleHit> hit = collider->test(i, world_from, world_to)) {
                    if (s_.response == ParticleCollisionResponse::Die) {
                        kill(i, collider);
                        continue;
                    }
                    const Vec3 point = s_.local_space ? matrix4_point(inverse, hit->point) : hit->point;
                    const Vec3 normal = s_.local_space ? direction_in(inverse, hit->normal) : hit->normal;
                    if (s_.response == ParticleCollisionResponse::Stick) {
                        pos_[i] = point;
                        vel_[i] = Vec3{0.f, 0.f, 0.f};
                        stuck_[i] = 1;
                        ++i;
                        continue;
                    }
                    const float into = dot(vel_[i], normal);
                    if (into < 0.f) {
                        const Vec3 normal_part = scale(normal, into);
                        const Vec3 along = sub(vel_[i], normal_part);
                        vel_[i] = sub(scale(along, 1.f - s_.friction), scale(normal_part, s_.bounciness));
                    }
                    pos_[i] = add(point, scale(normal, 1e-3f));
                }
            }
            ++i;
        }
    }

    // 2. Spawn: Emit's now, bursts at their times, Rate spread across the step.
    int room = std::max(0, std::min(allowance, s_.max_particles - static_cast<int>(count())));
    const int emits = std::min(std::max(0, pending_emits), room);
    if (emits > 0) {
        spawn(frame, emits, false, 0.f);
        room -= emits;
    }
    if (!playing || cycle_over_ || !(dt > 0.f)) {
        return;
    }
    const float t = std::min(cycle_time_ / s_.duration, 1.f);
    rate_carry_ += static_cast<double>(std::max(0.f, s_.rate.sample(t, rate_blend_))) * dt;
    const double whole = std::floor(rate_carry_ + kCarrySlack);
    rate_carry_ = std::max(0.0, rate_carry_ - whole);
    const int from_rate = static_cast<int>(std::min(whole, kMostSpawns));
    float end = cycle_time_ + dt;
    while (next_burst_ < s_.bursts.size() && s_.bursts[next_burst_].time < end) {
        const ParticleSettings::Burst& burst = s_.bursts[next_burst_++];
        const int n = std::min(burst_count(burst.count, rng_.uniform()), room);
        if (n > 0) {
            spawn(frame, n, false, std::clamp(end - burst.time, 0.f, dt));
            room -= n;
        }
    }
    const int rate_n = std::min(from_rate, room);
    if (rate_n > 0) {
        spawn(frame, rate_n, true, 0.f);
        room -= rate_n;
    }
    cycle_time_ = end;
    if (cycle_time_ >= s_.duration) {
        if (!s_.looping) {
            cycle_over_ = true;
            cycle_ended_ = true;
            return;
        }
        // A new cycle: its bursts the wrapped time has passed fire now.
        cycle_time_ = std::fmod(cycle_time_, s_.duration);
        rate_blend_ = rng_.uniform();
        next_burst_ = 0;
        end = cycle_time_;
        while (next_burst_ < s_.bursts.size() && s_.bursts[next_burst_].time < end) {
            const ParticleSettings::Burst& burst = s_.bursts[next_burst_++];
            const int n = std::min(burst_count(burst.count, rng_.uniform()), room);
            if (n > 0) {
                spawn(frame, n, false, std::clamp(end - burst.time, 0.f, dt));
                room -= n;
            }
        }
    }
}

void EmitterSim::integrate(std::size_t i, float dt, Vec3 accel, Vec3 centre, Vec3 orbit_axis) {
    const float t = std::min(age_[i] / life_[i], 1.f);
    vel_[i] = add(vel_[i], scale(accel, dt));
    const float drag = std::max(0.f, s_.drag.sample(t, random_[i]));
    if (drag > 0.f) {
        vel_[i] = scale(vel_[i], std::exp(-drag * dt));
    }
    Vec3 to = add(pos_[i], scale(vel_[i], dt));
    const float orbit = s_.orbit_speed.sample(t, random_[i]) * kDegToRad * dt;
    if (orbit != 0.f) {
        to = add(centre, rotate(sub(to, centre), orbit_axis, orbit));
    }
    pos_[i] = to;
    rot_[i] += spin_[i] * dt;
}

void EmitterSim::spawn(const EmitterFrame& frame, int count, bool spread, float birth_ago) {
    const float dt = std::max(0.f, frame.dt);
    const float cycle_t = std::min(cycle_time_ / s_.duration, 1.f);
    const Matrix4 inverse = s_.local_space ? matrix4_inverse(frame.current) : matrix4_identity();
    Vec3 accel = s_.acceleration;
    if (s_.local_space && !s_.acceleration_local) {
        accel = matrix4_vector(inverse, accel);
    } else if (!s_.local_space && s_.acceleration_local) {
        accel = matrix4_vector(frame.current, accel);
    }
    const Vec3 centre = s_.local_space ? Vec3{0.f, 0.f, 0.f} : matrix4_position(frame.current);
    const Vec3 orbit_axis =
        s_.local_space ? normalize(s_.orbit_axis) : direction_in(frame.current, s_.orbit_axis);
    for (int k = 0; k < count; ++k) {
        // Where in the step it was born: spread oldest first, or birth_ago before the end.
        const float ago = spread ? dt * (1.f - (static_cast<float>(k) + rng_.uniform()) / static_cast<float>(count))
                                 : birth_ago;
        const float f = dt > 0.f ? 1.f - ago / dt : 1.f;
        const Matrix4 at = f >= 1.f ? frame.current : matrix4_lerp(frame.previous, frame.current, f);
        const ShapeSample sample = sample_shape(s_.shape, s_.emit_from, frame.half, s_.arc, rng_);
        Vec3 dir = s_.emit_direction_local ? direction_in(at, s_.emit_direction) : normalize(s_.emit_direction);
        dir = spread_direction(dir, s_.spread_x, s_.spread_y, rng_);
        Vec3 p = matrix4_point(at, sample.position);
        Vec3 v = scale(dir, s_.emit_speed.sample(cycle_t, rng_.uniform()));
        if (!s_.local_space) {
            v = add(v, scale(frame.inherited, s_.inherit_velocity));
        } else {
            p = matrix4_point(inverse, p);
            v = matrix4_vector(inverse, v);
        }
        pos_.push_back(p);
        vel_.push_back(v);
        age_.push_back(0.f);
        life_.push_back(std::max(1e-3f, s_.lifetime.sample(cycle_t, rng_.uniform())));
        random_.push_back(rng_.uniform());
        rot_.push_back(s_.emit_rotation.sample(cycle_t, rng_.uniform()) * kDegToRad);
        spin_.push_back(s_.rotation_speed.sample(cycle_t, rng_.uniform()) * kDegToRad);
        stuck_.push_back(0);
        // Born mid-step: it has lived the rest of the step already.
        if (ago > 0.f) {
            const std::size_t i = pos_.size() - 1;
            age_[i] = ago;
            integrate(i, ago, accel, centre, orbit_axis);
        }
    }
}

void EmitterSim::kill(std::size_t i, ParticleCollider* collider) {
    const std::size_t last = count() - 1;
    if (i != last) {
        pos_[i] = pos_[last];
        vel_[i] = vel_[last];
        age_[i] = age_[last];
        life_[i] = life_[last];
        random_[i] = random_[last];
        rot_[i] = rot_[last];
        spin_[i] = spin_[last];
        stuck_[i] = stuck_[last];
        if (collider != nullptr) {
            collider->moved(last, i);
        }
    }
    pos_.pop_back();
    vel_.pop_back();
    age_.pop_back();
    life_.pop_back();
    random_.pop_back();
    rot_.pop_back();
    spin_.pop_back();
    stuck_.pop_back();
}

void EmitterSim::clear() {
    pos_.clear();
    vel_.clear();
    age_.clear();
    life_.clear();
    random_.clear();
    rot_.clear();
    spin_.clear();
    stuck_.clear();
}

void EmitterSim::prewarm(const EmitterFrame& frame) {
    EmitterFrame step_frame = frame;
    step_frame.previous = frame.current;
    step_frame.dt = 1.f / 30.f;
    const int steps = static_cast<int>(std::ceil(s_.duration * 30.f - 1e-3f));
    for (int i = 0; i < steps; ++i) {
        step(step_frame, true, 0, INT_MAX, nullptr);
    }
    cycle_time_ = 0.f;
    next_burst_ = 0;
    cycle_over_ = false;
    cycle_ended_ = false;
    rate_carry_ = 0.0;
}

bool EmitterSim::take_cycle_ended() {
    const bool out = cycle_ended_;
    cycle_ended_ = false;
    return out;
}

void EmitterSim::pack(std::vector<ParticleVertex>& out, const Matrix4& emitter_world, Vec3 eye, bool sort) const {
    out.resize(count());
    for (std::size_t i = 0; i < count(); ++i) {
        const float t = std::min(age_[i] / life_[i], 1.f);
        const Vec3 p = s_.local_space ? matrix4_point(emitter_world, pos_[i]) : pos_[i];
        const Vec3 v = s_.local_space ? matrix4_vector(emitter_world, vel_[i]) : vel_[i];
        const ColorRgb c = s_.color.sample(t, random_[i]);
        ParticleVertex& out_v = out[i];
        out_v.position[0] = p.x;
        out_v.position[1] = p.y;
        out_v.position[2] = p.z;
        out_v.size = std::max(0.f, s_.size.sample(t, random_[i]));
        out_v.color[0] = c.r;
        out_v.color[1] = c.g;
        out_v.color[2] = c.b;
        out_v.color[3] = std::clamp(c.a, 0.f, 1.f);
        out_v.velocity[0] = v.x;
        out_v.velocity[1] = v.y;
        out_v.velocity[2] = v.z;
        out_v.rotation = rot_[i];
        out_v.stretch = s_.stretch.sample(t, random_[i]);
        const int frames = std::max(1, s_.frames);
        out_v.frame = static_cast<float>(std::min(frames - 1, static_cast<int>(t * static_cast<float>(frames))));
    }
    if (sort) {
        const auto far = [&eye](const ParticleVertex& v) {
            const float dx = v.position[0] - eye.x;
            const float dy = v.position[1] - eye.y;
            const float dz = v.position[2] - eye.z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::sort(out.begin(), out.end(),
                  [&far](const ParticleVertex& a, const ParticleVertex& b) { return far(a) > far(b); });
    }
}

}  // namespace engine_core::particles
