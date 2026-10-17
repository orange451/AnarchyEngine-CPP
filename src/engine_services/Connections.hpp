#pragma once

#include "SceneService.hpp"

namespace engine_core {

// Who is connected to a networked game: one Connection per client, made and
// destroyed by the engine. A child of game outside the place, like Core: never
// saved, untouched by Play and Stop. The explorer shows it only while it holds
// a Connection. LocalConnection is a client's own Connection, nil on a server
// and in single-player.
class Connections : public Service {
public:
    using Service::Service;
    const char* class_name() const override;
    bool hidden_in_explorer() const override;

    InstanceId local_connection() const;
    // The engine's: replication sets it when a client's world arrives.
    void set_local_connection(InstanceId id);

private:
    InstanceId local_connection_ = 0;
};

}  // namespace engine_core
