#include "Ecs.hpp"

#include <mutex>

namespace engine_core {

EcsProcessSetup::EcsProcessSetup() {
    static std::once_flag once;
    std::call_once(once, [] { ecs_os_api.flags_ &= ~static_cast<ecs_flags32_t>(EcsOsApiHighResolutionTimer); });
}

EcsIds register_ecs(flecs::world& world) {
    EcsIds ids;
    ids.transform = world.component<Transform>().id();
    ids.color = world.component<ColorRgb>().id();
    ids.size = world.component<ecs::Size>().id();
    ids.velocity = world.component<ecs::Velocity>().id();
    ids.instance = world.component<ecs::Instance>().id();
    ids.in_game = world.component<ecs::InGame>().id();
    ids.in_workspace = world.component<ecs::InWorkspace>().id();
    ids.steps = world.component<ecs::Steps>().id();
    ids.simulated = world.component<ecs::Simulated>().id();
    ids.visual_only = world.component<ecs::VisualOnly>().id();
    return ids;
}

}  // namespace engine_core
