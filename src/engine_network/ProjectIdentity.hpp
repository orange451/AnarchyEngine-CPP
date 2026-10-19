#pragma once

// What a server or a client of a project presents to the network handshake,
// read from the project as loaded.

#include "GameIdentity.hpp"
#include "Project.hpp"

#include <filesystem>
#include <string>

namespace engine_network {

// The identity a server or client of this project presents: game_id parsed
// from project.gameId, version, and hash_resources(project.resources_root()).
// False, with error worded for a person, when gameId is missing, malformed, or
// all zeros, or the hash is 0.
bool project_identity(const engine_core::Project& project, network::GameIdentity& out, std::string& error);

// project_identity without the hash, which reads every resource file: out has
// no resources_hash yet, and resources is the folder to hash (hash_identity)
// when the identity is first needed. False, with why, as above for the gameId.
bool project_identity_unhashed(const engine_core::Project& project, network::GameIdentity& out,
                               std::filesystem::path& resources, std::string& error);

// Fills out.resources_hash from resources. Any thread: it reads only files.
// False, with why, when the folder cannot be read.
bool hash_identity(network::GameIdentity& out, const std::filesystem::path& resources, std::string& error);

}  // namespace engine_network
