#pragma once

#include "types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine_core {

struct RemoteValues;

// What a session offers RemoteEvent (docs/superpowers/specs/2026-10-20-remote-events-design.md).
// A Server or Client world's session installs one with DataModel::set_remote_sink;
// a standalone world has none and loops remotes back. engine_core calls it on
// the simulation thread, inside the firing script's call.
class IRemoteSink {
public:
    virtual ~IRemoteSink() = default;
    // Client: to the server. False with error when it cannot be sent (too large).
    virtual bool fire_server(InstanceId remote, std::shared_ptr<const RemoteValues> values, bool unreliable,
                             std::string& error) = 0;
    // Server: to the listed Connections; all=true is every connected client.
    // Each listed id is a live Connection under Connections.
    virtual bool fire_clients(InstanceId remote, const std::vector<InstanceId>& connections, bool all,
                              std::shared_ptr<const RemoteValues> values, bool unreliable, std::string& error) = 0;
};

}  // namespace engine_core
