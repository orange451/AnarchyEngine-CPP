#include "ClientWorld.hpp"

#include <vector>

namespace engine_network {

using engine_core::InstanceId;

void clear_for_client(engine_core::DataModel& game) {
    // Services nest (Storage holds ServerStorage and ClientStorage), so the walk
    // goes down through services and destroys each other instance whole, as one
    // tree, deepest first. Core is the engine's and never replicates.
    const InstanceId core = game.core();
    std::vector<InstanceId> services{0};
    std::vector<InstanceId> doomed;
    while (!services.empty()) {
        const InstanceId at = services.back();
        services.pop_back();
        for (const InstanceId child : game.get_children(at)) {
            if (child == core) {
                continue;
            }
            if (game.instance(child)->is_service()) {
                services.push_back(child);
            } else {
                doomed.push_back(child);
            }
        }
    }
    for (const InstanceId id : doomed) {
        // Each is the top of its own tree under a service, so none dies with another.
        if (game.alive(id) && !game.destroy_error(id)) {
            game.destroy_tree(id);
        }
    }
}

}  // namespace engine_network
