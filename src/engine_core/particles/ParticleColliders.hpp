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

    // How many particles a step tries to give a ray to, starting where the
    // last step stopped.
    static constexpr std::size_t kWindow = 512;

private:
    struct Plane {
        Vec3 normal{0.f, 1.f, 0.f};
        float offset = 0.f;
        bool valid = false;
    };
    DataModel* game_ = nullptr;
    TerrainCollider* terrain_ = nullptr;
    std::string group_;
    int* budget_ = nullptr;
    int* cast_ = nullptr;
    std::vector<Plane> planes_;
    std::size_t count_ = 0;
    std::size_t cursor_ = 0;
};

}  // namespace engine_core::particles
