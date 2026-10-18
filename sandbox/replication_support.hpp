#pragma once

// Helpers the replication tests share: a server world and a client world on
// one thread, and a check that a client's tree is the server's, filtered.

#include "support.hpp"

#include "ConnectionInstance.hpp"
#include "LuaSource.hpp"
#include "ReplicaApplier.hpp"
#include "Replicator.hpp"
#include "Script.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace replication {

// A client world as a ClientSession leaves it before the snapshot: Client role, playing, empty.
inline void start_client(ScriptRig& client) {
    client.game.set_world_role(engine_core::WorldRole::Client);
    client.game.capture_place();
    client.game.start_simulation();
}

inline void start_server(ScriptRig& server) {
    server.game.set_world_role(engine_core::WorldRole::Server);
    server.game.start_simulation();
}

// Hands every message to the applier in order; the last result.
inline engine_network::ApplyResult deliver(engine_network::ReplicaApplier& applier,
                                           const std::vector<std::vector<std::uint8_t>>& messages) {
    engine_network::ApplyResult result = engine_network::ApplyResult::Ok;
    for (const auto& message : messages) {
        result = applier.receive_message(message.data(), message.size());
        if (result == engine_network::ApplyResult::Bad) {
            break;
        }
    }
    return result;
}

// The Source a client should hold: a Server script's is empty.
inline std::string expected_source(const engine_core::DataModel& object) {
    const auto* lua = dynamic_cast<const engine_core::LuaSource*>(&object);
    if (lua == nullptr) {
        return std::string();
    }
    const auto* script = dynamic_cast<const engine_core::Script*>(&object);
    if (script != nullptr && script->run_context() == engine_core::RunContext::Server) {
        return std::string();
    }
    return lua->source();
}

// The client's tree under client_id matches the server's under server_id: the
// same replicated children in order, each with the same class, GUID, name,
// saved properties, Source, and Connection Id.
inline void require_same_tree(const engine_network::Replicator& replicator, const engine_core::DataModel& server,
                              engine_core::InstanceId server_id, const engine_core::DataModel& client,
                              engine_core::InstanceId client_id) {
    std::vector<engine_core::InstanceId> want;
    for (engine_core::InstanceId c = server.first_child(server_id); c != 0; c = server.next_sibling(c)) {
        if (replicator.replicated(c)) {
            want.push_back(c);
        }
    }
    std::vector<engine_core::InstanceId> have;
    for (engine_core::InstanceId c = client.first_child(client_id); c != 0; c = client.next_sibling(c)) {
        if (c != client.core() && c != client.service("ServerStorage")) {
            have.push_back(c);
        }
    }
    INFO("under " << server.name(server_id));
    REQUIRE(have.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const engine_core::DataModel& s = *server.instance(want[i]);
        const engine_core::DataModel& c = *client.instance(have[i]);
        INFO(server.name(want[i]));
        REQUIRE(std::string(c.class_name()) == s.class_name());
        REQUIRE(client.guid(have[i]) == server.guid(want[i]));
        REQUIRE(client.name(have[i]) == server.name(want[i]));
        engine_core::PropertyBag sp;
        engine_core::PropertyBag cp;
        s.save_properties(sp);
        c.save_properties(cp);
        REQUIRE(cp == sp);
        if (const auto* lua = dynamic_cast<const engine_core::LuaSource*>(&c)) {
            REQUIRE(lua->source() == expected_source(s));
        }
        if (const auto* sc = dynamic_cast<const engine_core::ConnectionInstance*>(&s)) {
            REQUIRE(static_cast<const engine_core::ConnectionInstance&>(c).connection_id() == sc->connection_id());
        }
        require_same_tree(replicator, server, want[i], client, have[i]);
    }
}

inline void require_same_world(const engine_network::Replicator& replicator, const engine_core::DataModel& server,
                               const engine_core::DataModel& client) {
    require_same_tree(replicator, server, 0, client, 0);
}

}  // namespace replication
