#pragma once

#include "Matrix4.hpp"
#include "particles/EmitterSim.hpp"
#include "types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine_core {
class DataModel;
}

namespace engine_core::particles {

// Particles against the voxel terrain of every collidable Terrain in
// Workspace: one that ends a step inside solid ground is put back on the
// surface along the distance field's gradient. One lookup per particle.
class TerrainCollider : public ParticleCollider {
public:
    TerrainCollider();
    ~TerrainCollider() override;
    // Once per step, before any test: the Terrains as they are now.
    void prepare(DataModel& game);
    bool empty() const;
    void begin(std::size_t) override {}
    std::optional<ParticleHit> test(std::size_t slot, Vec3 from, Vec3 to) override;
    void moved(std::size_t, std::size_t) override {}

private:
    struct Island;
    std::vector<std::unique_ptr<Island>> islands_;
};

// The plane each particle's last ray hit, by slot: a particle between rays
// tests its plane instead. Slots follow the simulation's swap-remove kills.
class PlaneCache {
public:
    // count particles this step; slots past the old count start with no plane.
    void begin(std::size_t count);
    void set(std::size_t slot, Vec3 normal, float offset);
    void clear(std::size_t slot);
    // Where from-to crosses slot's plane from its front, or nothing.
    std::optional<Vec3> crossing(std::size_t slot, Vec3 from, Vec3 to) const;
    Vec3 normal(std::size_t slot) const;
    // The particle in slot from moved to slot to.
    void moved(std::size_t from, std::size_t to);

private:
    struct Plane {
        Vec3 normal{0.f, 1.f, 0.f};
        float offset = 0.f;
        bool valid = false;
    };
    std::vector<Plane> planes_;
};

// Terrain first, then Box3D rays against physics bodies and Brushes, from a
// budget every World emitter shares each step. A particle whose turn for a
// ray has not come tests the plane its last ray hit, so most steps cost a
// dot product, not a cast. One per emitter.
class WorldCollider : public ParticleCollider {
public:
    void prepare(DataModel& game, TerrainCollider& terrain, const std::string& collision_group, int* ray_budget,
                 int* rays_cast);
    void begin(std::size_t count) override;
    std::optional<ParticleHit> test(std::size_t slot, Vec3 from, Vec3 to) override;
    void moved(std::size_t from, std::size_t to) override;


private:
    DataModel* game_ = nullptr;
    TerrainCollider* terrain_ = nullptr;
    std::string group_;
    int* budget_ = nullptr;
    int* cast_ = nullptr;
    PlaneCache planes_;
    std::size_t count_ = 0;
    // This step's rays go to share_ particles from cursor_ on; each looks
    // ahead reach_ steps, until that particle's next turn.
    std::size_t cursor_ = 0;
    std::size_t share_ = 0;
    float reach_ = 1.f;
};

}  // namespace engine_core::particles
