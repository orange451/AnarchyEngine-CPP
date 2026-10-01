#pragma once

// The engine's one include of flecs. Only engine_core and engine_instances
// sources include this: flecs ids never leave them. Hot paths use the C API
// with the ids in EcsIds; the C++ API is for setup (the world, registration,
// query building), since its inline wrappers are slow in Debug builds.

#pragma warning(push, 0)
#include "flecs.h"
#pragma warning(pop)

#include "Matrix4.hpp"
#include "types.hpp"

namespace engine_core {
namespace ecs {

// GameObject's spatial data. Transform is a component as it is.
struct Velocity {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

// The instance an entity belongs to.
struct Instance {
    InstanceId id = 0;
};

// Under game, under the Workspace service, and under the Lighting service.
struct InGame {};
struct InWorkspace {};
struct InLighting {};
// A rigid body: its class's physics_body() is true (PhysicsObject).
struct PhysicsBody {};
// Heartbeat steps it: its class's steps() is true.
struct Steps {};
// DataModel::set_simulated and set_visual_only.
struct Simulated {};
struct VisualOnly {};

}  // namespace ecs

// The ids one world gave the components and tags above.
struct EcsIds {
    ecs_id_t transform = 0;
    ecs_id_t velocity = 0;
    ecs_id_t instance = 0;
    ecs_id_t in_game = 0;
    ecs_id_t in_workspace = 0;
    ecs_id_t in_lighting = 0;
    ecs_id_t steps = 0;
    ecs_id_t physics_body = 0;
    ecs_id_t simulated = 0;
    ecs_id_t visual_only = 0;
};

// Registers every component and tag with world and returns their ids.
EcsIds register_ecs(flecs::world& world);

// Process-wide flecs settings, applied once before the first world. Declared
// ahead of each world so it is constructed first. It turns off flecs' 1 ms
// Windows timer resolution: the engine paces frames around the default tick,
// and a storage library should not change it.
struct EcsProcessSetup {
    EcsProcessSetup();
};

// A component of e, or null when e is 0 or does not have it.
template <typename T>
const T* read_component(ecs_world_t* world, ecs_entity_t e, ecs_id_t id) {
    return e == 0 ? nullptr : static_cast<const T*>(ecs_get_id(world, e, id));
}

// Writes nothing when e is 0: a released instance has no entity.
template <typename T>
void write_component(ecs_world_t* world, ecs_entity_t e, ecs_id_t id, const T& value) {
    if (e != 0) {
        ecs_set_id(world, e, id, sizeof(T), &value);
    }
}

inline bool has_tag(ecs_world_t* world, ecs_entity_t e, ecs_id_t tag) {
    return e != 0 && ecs_has_id(world, e, tag);
}

inline void set_tag(ecs_world_t* world, ecs_entity_t e, ecs_id_t tag, bool on) {
    if (e == 0) {
        return;
    }
    if (on) {
        ecs_add_id(world, e, tag);
    } else {
        ecs_remove_id(world, e, tag);
    }
}

}  // namespace engine_core
