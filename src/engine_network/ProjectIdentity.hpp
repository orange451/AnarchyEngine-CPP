#pragma once

// What a server or a client of a project presents to the network handshake,
// read from the project as loaded.

#include "GameIdentity.hpp"
#include "Project.hpp"

#include <string>

namespace engine_network {

// The identity a server or client of this project presents: game_id parsed
// from project.gameId, version, and hash_resources(project.resources_root()).
// False, with error worded for a person, when gameId is missing, malformed, or
// all zeros, or the hash is 0.
bool project_identity(const engine_core::Project& project, network::GameIdentity& out, std::string& error);

}  // namespace engine_network
