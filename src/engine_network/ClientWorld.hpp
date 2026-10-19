#pragma once

// The world a client starts from. A client's place is the server's: what the
// client loaded from its own project is only there to be replaced, so before it
// connects, everything a server would replicate goes, and the snapshot builds
// the place into what is left.

#include "DataModel.hpp"

namespace engine_network {

// Destroys every instance a server would replicate (every non-service
// descendant of game, outside Core) and puts every service back at its
// defaults, so a client starts from what the snapshot binds.
void clear_for_client(engine_core::DataModel& game);

// clear_for_client, keeping what survives a swap: every instance that is
// Persistent or under a Persistent ancestor. A survivor whose parent does not
// survive moves to the service it was under, keeping its name and children.
// Every survivor, and everything under it, gets a fresh GUID: the client's
// own instances must never share one with what the server is about to create.
// Services reset to their defaults.
void swap_for_client(engine_core::DataModel& game);

}  // namespace engine_network
