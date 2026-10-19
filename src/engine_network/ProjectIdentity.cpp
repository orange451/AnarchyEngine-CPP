#include "ProjectIdentity.hpp"

#include "FileBytes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace engine_network {

bool project_identity(const engine_core::Project& project, network::GameIdentity& out, std::string& error) {
    network::GameIdentity identity;
    std::filesystem::path resources;
    if (!project_identity_unhashed(project, identity, resources, error) || !hash_identity(identity, resources, error)) {
        return false;
    }
    out = identity;
    error.clear();
    return true;
}

bool project_identity_unhashed(const engine_core::Project& project, network::GameIdentity& out,
                               std::filesystem::path& resources, std::string& error) {
    network::GameIdentity identity;
    const std::string& game_id = project.game_id();
    if (game_id.empty()) {
        error = "The project has no gameId in project.json";
        return false;
    }
    if (!network::parse_game_id(game_id, identity.game_id)) {
        error = "The project's gameId in project.json is not 32 hex characters";
        return false;
    }
    // All zeros is what an unset id looks like; every game would share it.
    if (std::all_of(identity.game_id.begin(), identity.game_id.end(), [](std::uint8_t byte) { return byte == 0; })) {
        error = "The project's gameId in project.json is all zeros, so it names no game";
        return false;
    }
    identity.version = project.version();
    out = identity;
    resources = project.resources_root();
    error.clear();
    return true;
}

bool hash_identity(network::GameIdentity& out, const std::filesystem::path& resources, std::string& error) {
    // A server and its clients must ship the same files; 0 means the folder could not be read at all.
    const std::uint64_t hash = network::hash_resources(resources);
    if (hash == 0) {
        error = "Could not read the project's resources folder (" + engine_core::utf8_path(resources) + ")";
        return false;
    }
    out.resources_hash = hash;
    return true;
}

}  // namespace engine_network
