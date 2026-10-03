#include "Ecs.hpp"

#include <mutex>

namespace engine_core {

EcsProcessSetup::EcsProcessSetup() {
    static std::once_flag once;
    std::call_once(once, [] { ecs_os_api.flags_ &= ~static_cast<ecs_flags32_t>(EcsOsApiHighResolutionTimer); });
}

EcsIds register_ecs(flecs::world& world) {
    EcsIds ids;
    ids.transform = world.component<Matrix4>().id();
    ids.velocity = world.component<ecs::Velocity>().id();
    ids.instance = world.component<ecs::Instance>().id();
    ids.in_game = world.component<ecs::InGame>().id();
    ids.in_workspace = world.component<ecs::InWorkspace>().id();
    ids.in_lighting = world.component<ecs::InLighting>().id();
    ids.in_core = world.component<ecs::InCore>().id();
    ids.steps = world.component<ecs::Steps>().id();
    ids.physics_body = world.component<ecs::PhysicsBody>().id();
    ids.sound_source = world.component<ecs::SoundSource>().id();
    ids.dragger = world.component<ecs::DraggerTag>().id();
    ids.simulated = world.component<ecs::Simulated>().id();
    ids.visual_only = world.component<ecs::VisualOnly>().id();
    return ids;
}

}  // namespace engine_core
