#include "ParticleWorld.hpp"

#include "Assemblies.hpp"
#include "AssetInstances.hpp"
#include "Brush.hpp"
#include "DataModel.hpp"
#include "GameObject.hpp"
#include "ParticleEmitter.hpp"
#include "PhysicsBase.hpp"
#include "PhysicsObject.hpp"
#include "PVInstance.hpp"
#include "SceneService.hpp"
#include "particles/ParticleColliders.hpp"
#include "particles/ParticleMath.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

namespace engine_core {

using particles::EmitterFrame;
using particles::EmitterSim;
using particles::ParticleSettings;
using particles::ParticleVertex;

struct ParticleWorld::Entry {
    EmitterSim sim;
    Matrix4 last_frame = matrix4_identity();
    bool seen = false;
    InstanceId last_parent = 0;   // a new parent starts the shape's motion over
    std::uint64_t pass = 0;
    bool finished_pending = false;   // a one-shot ended; Finished fires once its last particle dies
    bool replay_pending = false;     // stopped: a one-shot ended, and replays once its last particle dies
    int ray_budget = 0;              // its share of this step's World rays
    particles::WorldCollider world;   // its ray turns and hit planes, for World collision
};

// What particles hit this step: the Terrains, prepared on first need, and the
// ray budget World emitters share.
struct ParticleWorld::Colliders {
    particles::TerrainCollider terrain;
    bool terrain_ready = false;
    int rays = 0;

    particles::TerrainCollider& ground(DataModel& game) {
        if (!terrain_ready) {
            terrain.prepare(game);
            terrain_ready = true;
        }
        return terrain;
    }
};

namespace {

CurveTable bake(const NumberCurve& curve) {
    CurveTable table;
    table.bake(curve);
    return table;
}

ParticleSettings build_settings(const ParticleEmitter& e) {
    ParticleSettings s;
    s.rate = bake(e.rate());
    for (const ParticleBurst& burst : e.bursts()) {
        s.bursts.push_back(ParticleSettings::Burst{burst.time, bake(burst.count)});
    }
    s.duration = static_cast<float>(e.duration());
    s.looping = e.looping();
    s.max_particles = e.max_particles();
    s.seed = static_cast<std::uint32_t>(e.seed());
    s.shape = e.shape();
    s.emit_from = e.emit_from();
    s.arc = static_cast<float>(e.arc());
    s.emit_direction = e.emit_direction();
    s.emit_direction_local = e.emit_direction_space() == TransformSpace::Local;
    s.spread_x = e.emit_spread().x;
    s.spread_y = e.emit_spread().y;
    s.lifetime = bake(e.lifetime());
    s.emit_speed = bake(e.emit_speed());
    s.emit_rotation = bake(e.emit_rotation());
    s.rotation_speed = bake(e.rotation_speed());
    s.local_space = e.simulation_space() == TransformSpace::Local;
    s.inherit_velocity = static_cast<float>(e.inherit_velocity());
    s.acceleration = e.acceleration();
    s.acceleration_local = e.acceleration_space() == TransformSpace::Local;
    s.drag = bake(e.drag());
    s.orbit_speed = bake(e.orbit_speed());
    s.orbit_axis = e.orbit_axis();
    s.collision = e.collision();
    s.response = e.collision_response();
    s.bounciness = static_cast<float>(e.bounciness());
    s.friction = static_cast<float>(e.friction());
    s.color.bake(e.color());
    s.size = bake(e.size());
    s.stretch = bake(e.stretch());
    s.frames = e.texture_mode() == ParticleTextureMode::Flipbook
                   ? static_cast<int>(e.flipbook_grid().x) * static_cast<int>(e.flipbook_grid().y)
                   : 1;
    return s;
}

Vec3 camera_position(const DataModel& game) {
    const auto* workspace = dynamic_cast<const Workspace*>(game.instance(game.service("Workspace")));
    const InstanceId camera = workspace != nullptr ? workspace->current_camera() : 0;
    const auto* eye = camera != 0 ? dynamic_cast<const PVInstance*>(game.instance(camera)) : nullptr;
    return eye != nullptr ? matrix4_position(eye->transform()) : Vec3{0.f, 0.f, 0.f};
}

// The velocity at point of the body the parent's assembly moves as: linear
// plus spin. None outside an assembly.
Vec3 inherited_velocity(const DataModel& game, InstanceId parent, Vec3 point) {
    if (parent == DataModel::kNoParent) {
        return Vec3{0.f, 0.f, 0.f};
    }
    const InstanceId root = game.assemblies().root_of(game, parent);
    const auto* body = root != 0 ? dynamic_cast<const PhysicsBase*>(game.instance(root)) : nullptr;
    if (body == nullptr) {
        return Vec3{0.f, 0.f, 0.f};
    }
    Vec3 spin{0.f, 0.f, 0.f};
    if (const auto* rigid = dynamic_cast<const PhysicsObject*>(body)) {
        spin = rigid->angular_velocity();
    } else if (const auto* brush = dynamic_cast<const Brush*>(body)) {
        spin = brush->angular_velocity();
    }
    const Vec3 r = particles::sub(point, matrix4_position(body->transform()));
    return particles::add(body->velocity(), particles::cross(spin, r));
}

ParticleDraw make_draw(const DataModel& game, const ParticleEmitter& e, const EmitterSim& sim, const Matrix4& frame,
                       Vec3 eye) {
    ParticleDraw draw;
    draw.emitter = e.id();
    auto list = std::make_shared<std::vector<ParticleVertex>>();
    sim.pack(*list, frame, eye, e.blend_mode() == ParticleBlendMode::Normal);
    float biggest = 0.f;
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 high{-low.x, -low.y, -low.z};
    for (const ParticleVertex& v : *list) {
        low = Vec3{std::min(low.x, v.position[0]), std::min(low.y, v.position[1]), std::min(low.z, v.position[2])};
        high = Vec3{std::max(high.x, v.position[0]), std::max(high.y, v.position[1]), std::max(high.z, v.position[2])};
        biggest = std::max(biggest, v.size * (1.f + std::fabs(v.stretch)));
    }
    if (e.facing() == ParticleFacing::Velocity) {
        biggest *= 4.f;   // a fast streak reaches past its centre
    }
    draw.bounds_min = Vec3{low.x - biggest, low.y - biggest, low.z - biggest};
    draw.bounds_max = Vec3{high.x + biggest, high.y + biggest, high.z + biggest};
    draw.particles = std::move(list);

    const LuaSlot slot = e.texture();
    const auto* texture = slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Texture*>(game.instance(slot.id)) : nullptr;
    if (texture != nullptr) {
        draw.texture = texture->path();
        draw.texture_flip_y = texture->flip_y();
        draw.texture_always_loaded = texture->streaming() == TextureStreaming::AlwaysLoaded;
    }
    if (e.texture_mode() == ParticleTextureMode::Flipbook) {
        draw.grid_x = static_cast<int>(e.flipbook_grid().x);
        draw.grid_y = static_cast<int>(e.flipbook_grid().y);
    }
    draw.blend = e.blend_mode();
    draw.facing = e.facing();
    draw.facing_axis = e.facing_axis_space() == TransformSpace::Local
                           ? particles::normalize(matrix4_vector(frame, e.facing_axis()))
                           : e.facing_axis();
    draw.velocity_stretch = static_cast<float>(e.velocity_stretch());
    draw.soft_edge = static_cast<float>(e.soft_edge());
    draw.light_influence = static_cast<float>(e.light_influence());
    draw.brightness = static_cast<float>(e.brightness());
    draw.draw_order = e.draw_order();
    return draw;
}

}  // namespace

bool emitter_extent(const DataModel& game, InstanceId parent, Matrix4& frame, Vec3& half) {
    const DataModel* object = parent != DataModel::kNoParent ? game.instance(parent) : nullptr;
    const auto* pv = dynamic_cast<const PVInstance*>(object);
    half = Vec3{0.f, 0.f, 0.f};
    if (pv == nullptr) {
        frame = matrix4_identity();
        return false;
    }
    frame = pv->transform();
    if (const auto* body = dynamic_cast<const PhysicsObject*>(object)) {
        const Vec3 size = body->size();
        half = Vec3{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
        return true;
    }
    if (const auto* brush = dynamic_cast<const Brush*>(object)) {
        const auto mesh = brush->mesh();
        if (mesh != nullptr && mesh->max[0] > mesh->min[0]) {
            const Vec3 centre{(mesh->min[0] + mesh->max[0]) * 0.5f, (mesh->min[1] + mesh->max[1]) * 0.5f,
                              (mesh->min[2] + mesh->max[2]) * 0.5f};
            half = Vec3{(mesh->max[0] - mesh->min[0]) * 0.5f, (mesh->max[1] - mesh->min[1]) * 0.5f,
                        (mesh->max[2] - mesh->min[2]) * 0.5f};
            frame = matrix4_multiply(frame, matrix4_translation(centre.x, centre.y, centre.z));
            return true;
        }
        return false;
    }
    if (const auto* go = dynamic_cast<const GameObject*>(object)) {
        const LuaSlot slot = go->prefab();
        const auto* prefab =
            slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Prefab*>(game.instance(slot.id)) : nullptr;
        Vec3 low;
        Vec3 high;
        if (prefab != nullptr && prefab->bounds(low, high)) {
            const float s = static_cast<float>(go->scale());
            half = Vec3{(high.x - low.x) * 0.5f * s, (high.y - low.y) * 0.5f * s, (high.z - low.z) * 0.5f * s};
            frame = matrix4_multiply(frame, matrix4_translation((low.x + high.x) * 0.5f * s,
                                                                (low.y + high.y) * 0.5f * s,
                                                                (low.z + high.z) * 0.5f * s));
            return true;
        }
    }
    return false;
}

ParticleWorld::ParticleWorld() : colliders_(std::make_unique<Colliders>()) {}

ParticleWorld::~ParticleWorld() = default;

int ParticleWorld::rays_last_step() const { return colliders_->rays; }

// One emitter this step: where it is, how many it asks for, and how many it gets.
struct ParticleWorld::Pending {
    InstanceId id = 0;
    InstanceId parent = 0;
    ParticleEmitter* emitter = nullptr;
    Entry* entry = nullptr;
    EmitterFrame frame;
    int emits = 0;
    int want = 0;
    int allowance = 0;
    float distance2 = 0.f;
};

void ParticleWorld::step(DataModel& game, double dt) {
    if (generation_ != game.world_generation()) {
        emitters_.clear();   // a Stop: everything play spawned goes
        generation_ = game.world_generation();
    }
    ++pass_;
    game.particle_emitters(ids_);
    const Vec3 eye = camera_position(game);
    const float step_dt = static_cast<float>(std::max(0.0, dt));

    const auto* workspace = dynamic_cast<const Workspace*>(game.instance(game.service("Workspace")));
    const long long budget = workspace != nullptr ? workspace->particle_budget() : Workspace::kDefaultParticleBudget;

    // Pass 1: settings, where each emitter is, and what it wants to spawn.
    pending_.clear();
    std::size_t live = 0;
    int world_emitters = 0;
    for (InstanceId id : ids_) {
        auto* e = dynamic_cast<ParticleEmitter*>(game.instance(id));
        if (e == nullptr) {
            continue;
        }
        std::unique_ptr<Entry>& slot = emitters_[id];
        if (!slot) {
            slot = std::make_unique<Entry>();
        }
        Entry& entry = *slot;
        entry.pass = pass_;
        const std::uint32_t dirty = e->take_dirty();
        if ((dirty & ParticleEmitter::kDirtySettings) != 0) {
            entry.sim.configure(build_settings(*e));
        }
        const bool restart = (dirty & ParticleEmitter::kDirtyRestart) != 0;
        if (restart) {
            seed_counter_ += 0x6d2b79f5u;
            entry.sim.restart(seed_counter_);
            entry.finished_pending = false;
        }
        if (e->take_clear()) {
            entry.sim.clear();
        }
        const InstanceId parent = game.parent(id);
        Matrix4 shape_frame;
        Vec3 half;
        emitter_extent(game, parent, shape_frame, half);
        Pending p;
        p.id = id;
        p.parent = parent;
        p.emitter = e;
        p.entry = &entry;
        p.frame.current = shape_frame;
        p.frame.previous = entry.seen && entry.last_parent == parent ? entry.last_frame : shape_frame;
        p.frame.half = half;
        p.frame.inherited = inherited_velocity(game, parent, matrix4_position(shape_frame));
        p.frame.dt = step_dt;
        if (restart && e->prewarm() && e->looping()) {
            const long long room = std::clamp<long long>(budget - static_cast<long long>(live), 0, INT_MAX);
            entry.sim.prewarm(p.frame, static_cast<int>(room));
        }
        p.emits = e->take_emit_requests();
        p.want = entry.sim.wanted_spawns(step_dt, e->playing(), p.emits);
        p.allowance = p.want;
        const Vec3 to_eye = particles::sub(matrix4_position(shape_frame), eye);
        p.distance2 = particles::dot(to_eye, to_eye);
        world_emitters += e->collision() == ParticleCollision::World ? 1 : 0;
        live += entry.sim.count();
        pending_.push_back(p);
    }

    // The budget: over it, the farthest emitters give up their spawns first.
    // Live particles are never removed.
    long long wanted = 0;
    for (const Pending& p : pending_) {
        wanted += p.want;
    }
    long long over = static_cast<long long>(live) + wanted - budget;
    if (over > 0) {
        std::vector<std::size_t> order(pending_.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [this](std::size_t a, std::size_t b) { return pending_[a].distance2 > pending_[b].distance2; });
        for (std::size_t i : order) {
            if (over <= 0) {
                break;
            }
            const long long cut = std::min<long long>(over, pending_[i].allowance);
            pending_[i].allowance -= static_cast<int>(cut);
            over -= cut;
        }
    }

    // Pass 2: step, store what scripts read, fire Finished, and pack draws.
    draws_.clear();
    live_total_ = 0;
    colliders_->terrain_ready = false;
    // Every World emitter gets an even share of the rays, so none starves another.
    const int ray_share = world_emitters > 0 ? std::max(1, kRaysPerStep / world_emitters) : 0;
    colliders_->rays = 0;
    for (Pending& p : pending_) {
        ParticleEmitter& e = *p.emitter;
        Entry& entry = *p.entry;
        // An Emit that cannot fit is dropped, not carried.
        const int emits = std::min(p.emits, p.allowance);
        particles::ParticleCollider* collider = nullptr;
        if (e.collision() == ParticleCollision::Terrain) {
            collider = &colliders_->ground(game);
        } else if (e.collision() == ParticleCollision::World) {
            entry.ray_budget = ray_share;
            entry.world.prepare(game, colliders_->ground(game), e.collision_group(), &entry.ray_budget,
                                &colliders_->rays);
            collider = &entry.world;
        }
        entry.sim.step(p.frame, e.playing(), emits, p.allowance, collider);
        entry.last_frame = p.frame.current;
        entry.last_parent = p.parent;
        entry.seen = true;
        if (entry.sim.take_cycle_ended()) {
            if (game.simulation_running()) {
                e.store_finished_cycle();
                entry.finished_pending = true;
            } else {
                // Stopped, a one-shot previews again rather than saving Playing false.
                entry.replay_pending = true;
            }
        }
        const int count = static_cast<int>(entry.sim.count());
        e.store_particle_count(count);
        if (entry.replay_pending && count == 0) {
            entry.replay_pending = false;
            seed_counter_ += 0x6d2b79f5u;
            entry.sim.restart(seed_counter_);
        }
        if (entry.finished_pending && count == 0) {
            entry.finished_pending = false;
            game.fire_event(p.id, "Finished", {});
        }
        live_total_ += entry.sim.count();
        if (count > 0) {
            draws_.push_back(make_draw(game, e, entry.sim, p.frame.current, eye));
        }
    }
    // Gone, moved into a ViewportPane, or out of Workspace: dropped now.
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        it = it->second->pass == pass_ ? std::next(it) : emitters_.erase(it);
    }
}

}  // namespace engine_core
