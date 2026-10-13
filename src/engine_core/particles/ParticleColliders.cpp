#include "particles/ParticleColliders.hpp"

#include "DataModel.hpp"
#include "PhysicsWorld.hpp"
#include "Terrain.hpp"
#include "particles/ParticleMath.hpp"
#include "terrain/VoxelSampler.hpp"

namespace engine_core::particles {

struct TerrainCollider::Island {
    Matrix4 forward;
    Matrix4 inverse;
    terrain::VoxelSampler sampler;
    Island(const Matrix4& transform, const terrain::ChunkMap& chunks, float voxel_size)
        : forward(transform), inverse(matrix4_inverse(transform)), sampler(chunks, voxel_size) {}
};

TerrainCollider::TerrainCollider() = default;

TerrainCollider::~TerrainCollider() = default;

void TerrainCollider::prepare(DataModel& game) {
    islands_.clear();
    std::vector<InstanceId> ids;
    game.terrains(ids);
    for (InstanceId id : ids) {
        const auto* terrain = dynamic_cast<const Terrain*>(game.instance(id));
        if (terrain == nullptr || !terrain->can_collide()) {
            continue;
        }
        islands_.push_back(std::make_unique<Island>(terrain->transform(), terrain->volume().chunks(),
                                                    static_cast<float>(terrain->voxel_size())));
    }
}

bool TerrainCollider::empty() const { return islands_.empty(); }

std::optional<ParticleHit> TerrainCollider::test(std::size_t, Vec3, Vec3 to) {
    for (const auto& island : islands_) {
        const Vec3 local = matrix4_point(island->inverse, to);
        const float d = island->sampler.distance(local);
        if (!(d < 0.f)) {
            continue;
        }
        const Vec3 n = normalize(island->sampler.gradient(local));
        const Vec3 surface = sub(local, scale(n, d));   // d is negative: out along n
        return ParticleHit{matrix4_point(island->forward, surface), normalize(matrix4_vector(island->forward, n))};
    }
    return std::nullopt;
}

void WorldCollider::prepare(DataModel& game, TerrainCollider& terrain, const std::string& collision_group,
                            int* ray_budget, int* rays_cast) {
    game_ = &game;
    terrain_ = &terrain;
    group_ = collision_group;
    budget_ = ray_budget;
    cast_ = rays_cast;
    share_ = ray_budget != nullptr ? static_cast<std::size_t>(std::max(0, *ray_budget)) : 0;
}

void PlaneCache::begin(std::size_t count) { planes_.resize(count); }

void PlaneCache::set(std::size_t slot, Vec3 normal, float offset) {
    if (slot >= planes_.size()) {
        planes_.resize(slot + 1);
    }
    planes_[slot] = Plane{normal, offset, true};
}

void PlaneCache::clear(std::size_t slot) {
    if (slot < planes_.size()) {
        planes_[slot].valid = false;
    }
}

std::optional<Vec3> PlaneCache::crossing(std::size_t slot, Vec3 from, Vec3 to) const {
    if (slot >= planes_.size() || !planes_[slot].valid) {
        return std::nullopt;
    }
    const Plane& plane = planes_[slot];
    const float before = dot(plane.normal, from) - plane.offset;
    const float after = dot(plane.normal, to) - plane.offset;
    if (before >= 0.f && after < 0.f) {
        const float f = before / (before - after);
        return add(from, scale(sub(to, from), f));
    }
    return std::nullopt;
}

Vec3 PlaneCache::normal(std::size_t slot) const { return slot < planes_.size() ? planes_[slot].normal : Vec3{0.f, 1.f, 0.f}; }

void PlaneCache::moved(std::size_t from, std::size_t to) {
    if (from < planes_.size() && to < planes_.size()) {
        planes_[to] = planes_[from];
        // The slot it left goes to a newborn, which has hit nothing yet.
        planes_[from].valid = false;
    }
}

void WorldCollider::begin(std::size_t count) {
    planes_.begin(count);
    count_ = count;
    cursor_ = count > 0 ? (cursor_ + share_) % count : 0;
    // Steps until a particle's turn comes again, and one more.
    reach_ = share_ > 0 ? static_cast<float>((count + share_ - 1) / share_ + 1) : 1.f;
}

std::optional<ParticleHit> WorldCollider::test(std::size_t slot, Vec3 from, Vec3 to) {
    if (std::optional<ParticleHit> ground = terrain_->test(slot, from, to)) {
        return ground;
    }
    const Vec3 along = sub(to, from);
    if (!(dot(along, along) > 0.f)) {
        return std::nullopt;   // nothing moved: nothing to hit, and no ray spent
    }
    // In this step's window, and rays left: cast one.
    const std::size_t place = count_ > 0 ? (slot + count_ - cursor_) % count_ : 0;
    PhysicsWorld* physics = game_->physics();
    if (physics != nullptr && *budget_ > 0 && place < share_) {
        --*budget_;
        ++*cast_;
        RayFilter filter;
        filter.collision_group = group_;
        // Looks ahead to the particle's next turn, so its plane is known before it gets there.
        const std::optional<RayHit> hit = physics->raycast(*game_, from, scale(along, reach_), filter);
        if (!hit) {
            planes_.clear(slot);
            return std::nullopt;
        }
        const Vec3 normal = normalize(hit->normal);
        planes_.set(slot, normal, dot(normal, hit->position));
        if (hit->distance <= length(along)) {
            return ParticleHit{hit->position, normal};
        }
        return std::nullopt;
    }
    if (const std::optional<Vec3> point = planes_.crossing(slot, from, to)) {
        return ParticleHit{*point, planes_.normal(slot)};
    }
    return std::nullopt;
}

void WorldCollider::moved(std::size_t from, std::size_t to) { planes_.moved(from, to); }

}  // namespace engine_core::particles
