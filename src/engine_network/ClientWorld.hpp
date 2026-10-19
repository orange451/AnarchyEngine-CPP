#pragma once

// The world a client starts from. A client's place is the server's: what the
// client loaded from its own project is only there to be replaced, so before it
// connects, everything a server would replicate goes, and the snapshot builds
// the place into what is left.

#include "DataModel.hpp"

namespace engine_network {

// Destroys every instance a server would replicate (every non-service
// descendant of game, outside Core), leaving the services and their
// properties, so a client starts from what the snapshot binds.
void clear_for_client(engine_core::DataModel& game);

}  // namespace engine_network
