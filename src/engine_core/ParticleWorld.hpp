#pragma once

#include "Enum.hpp"
#include "Matrix4.hpp"
#include "Vector3.hpp"
#include "particles/EmitterSim.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {

class DataModel;
class ParticleEmitter;

// One emitter's drawable particles for a frame. The particle list is
// immutable and shared, so the render snapshot copies a pointer.
struct ParticleDraw {
    InstanceId emitter = 0;
    std::shared_ptr<const std::vector<particles::ParticleVertex>> particles;
    std::string texture;   // the Texture's path; empty draws the built-in dot
    bool texture_flip_y = false;
    bool texture_always_loaded = false;
    int grid_x = 1;
    int grid_y = 1;
    ParticleBlendMode blend = ParticleBlendMode::Normal;
    ParticleFacing facing = ParticleFacing::Camera;
    Vec3 facing_axis{0.f, 1.f, 0.f};   // world, unit
    float velocity_stretch = 0.1f;
    float soft_edge = 0.5f;
    float light_influence = 1.f;
    float brightness = 1.f;
    int draw_order = 0;
    // A world box around every particle, padded by the largest one's size.
    Vec3 bounds_min{0.f, 0.f, 0.f};
    Vec3 bounds_max{0.f, 0.f, 0.f};
};

// Simulates every ParticleEmitter in Workspace and not in a ViewportPane,
// stopped or playing, once per engine frame (spec
// docs/superpowers/specs/2026-10-12-particle-emitter-design.md). A dt of 0
// freezes them, as paused play does. A Stop (a new world generation) drops
// every particle play spawned. Runs on the engine thread under the write lock.
class ParticleWorld {
public:
    ParticleWorld();
    ~ParticleWorld();
    ParticleWorld(const ParticleWorld&) = delete;
    ParticleWorld& operator=(const ParticleWorld&) = delete;

    void step(DataModel& game, double dt);

    // This frame's drawn emitters: those with live particles.
    const std::vector<ParticleDraw>& draws() const { return draws_; }
    std::size_t live_total() const { return live_total_; }
    std::size_t emitter_count() const { return emitters_.size(); }
    // Box3D rays World collision cast last step, for tests.
    int rays_last_step() const;

private:
    struct Entry;
    struct Colliders;
    struct Pending;

    std::unordered_map<InstanceId, std::unique_ptr<Entry>> emitters_;
    std::unique_ptr<Colliders> colliders_;
    std::vector<ParticleDraw> draws_;
    std::vector<InstanceId> ids_;
    std::vector<Pending> pending_;
    std::uint64_t pass_ = 0;
    std::uint64_t generation_ = ~0ull;
    std::size_t live_total_ = 0;
    std::uint32_t seed_counter_ = 0x9e3779b9u;
};

// The box an emitter under parent emits from: the frame at its centre and
// half its size. PhysicsObject: its Size. Brush: its mesh's box. GameObject:
// its Prefab's box times its Scale. False for any other parent, which gives
// a point at the parent's position (or the origin).
bool emitter_extent(const DataModel& game, InstanceId parent, Matrix4& frame, Vec3& half);

}  // namespace engine_core
