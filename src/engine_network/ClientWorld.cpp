#include "ClientWorld.hpp"

#include "Connections.hpp"
#include "Project.hpp"

#include <optional>
#include <string>
#include <utility>
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
    // A snapshot's service records carry only the values that differ from the
    // defaults, so a value the client's project set would outlive one the server
    // put back to its default.
    engine_core::Project::reset_services(game);
}

void swap_for_client(engine_core::DataModel& game, const std::function<void(const std::string&)>& warn) {
    // The same walk as clear_for_client: down through services, Core skipped.
    // Each other child of a service tops a tree. A Persistent top survives
    // whole; any other is searched for its topmost Persistent descendants,
    // which move up to that service, and then it is destroyed.
    const InstanceId core = game.core();
    std::vector<InstanceId> services{0};
    std::vector<InstanceId> doomed;
    std::vector<InstanceId> survivors;
    std::vector<std::pair<InstanceId, InstanceId>> rescued;   // survivor, the service it moves to
    while (!services.empty()) {
        const InstanceId at = services.back();
        services.pop_back();
        for (const InstanceId child : game.get_children(at)) {
            if (child == core) {
                continue;
            }
            if (game.instance(child)->is_service()) {
                services.push_back(child);
                continue;
            }
            if (game.persistent(child)) {
                survivors.push_back(child);
                continue;
            }
            doomed.push_back(child);
            std::vector<InstanceId> pending = game.get_children(child);
            while (!pending.empty()) {
                const InstanceId next = pending.back();
                pending.pop_back();
                if (game.persistent(next)) {
                    rescued.emplace_back(next, at);
                    continue;
                }
                for (const InstanceId below : game.get_children(next)) {
                    pending.push_back(below);
                }
            }
        }
    }
    // Out first, so destroying what held them leaves them alone.
    for (const auto& [id, service] : rescued) {
        if (!game.alive(id)) {
            continue;
        }
        if (const std::optional<std::string> refused = game.parent_error(id, service)) {
            if (warn) {
                warn("Persistent " + std::string(game.instance(id)->class_name()) + " \"" + game.name(id) +
                     "\" could not move to " + game.name(service) + " for the join, so it was destroyed: " +
                     *refused);
            }
            continue;
        }
        game.set_parent(id, service);
        survivors.push_back(id);
    }
    for (const InstanceId id : doomed) {
        if (game.alive(id) && !game.destroy_error(id)) {
            game.destroy_tree(id);
        }
    }
    // After the swap the survivors are this client's own: new GUIDs, the way
    // Instance.new makes them.
    std::vector<InstanceId> pending = survivors;
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        game.set_guid(next, engine_core::make_guid());
        for (const InstanceId below : game.get_children(next)) {
            pending.push_back(below);
        }
    }
    engine_core::Project::reset_services(game);
}

void clear_connections(engine_core::DataModel& game) {
    // The server put them there and only a session takes them out; a Connection
    // is never a script's to keep, so none refuses to go.
    for (const InstanceId id : game.get_children(game.connections())) {
        if (game.alive(id)) {
            game.destroy_tree(id);
        }
    }
    if (auto* list = dynamic_cast<engine_core::Connections*>(game.instance(game.connections()))) {
        list->set_local_connection(0);
    }
}

}  // namespace engine_network
