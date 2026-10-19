#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace player {

// What AnarchyPlayer was started with: a project folder, a server to join, both, or neither.
struct PlayerArgs {
    std::string connect;              // "host:port", empty: play standalone
    std::filesystem::path folder;     // empty: the packed game
};

// argv as main gets it. Ignores macOS -psn_ arguments. nullopt with error on
// --connect without a value or an unknown -- flag.
std::optional<PlayerArgs> parse_player_args(int argc, char** argv, std::string& error);

}  // namespace player
