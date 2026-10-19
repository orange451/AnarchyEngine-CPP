// AnarchyPlayer's command line: a project folder and --connect, in either order
// (src/player/PlayerArgs.hpp).

#include "player/PlayerArgs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

// argv as main gets it, from literal arguments.
std::optional<player::PlayerArgs> parse(std::vector<std::string> args, std::string& error) {
    std::vector<char*> argv;
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    return player::parse_player_args(static_cast<int>(args.size()), argv.data(), error);
}

}  // namespace

TEST_CASE("PL1 parse_player_args reads a folder and --connect in either order", "[player]") {
    std::string error;

    std::optional<player::PlayerArgs> args = parse({"AnarchyPlayer"}, error);
    REQUIRE(args);
    REQUIRE(args->connect.empty());
    REQUIRE(args->folder.empty());

    args = parse({"AnarchyPlayer", "game"}, error);
    REQUIRE(args);
    REQUIRE(args->connect.empty());
    REQUIRE(args->folder == "game");

    args = parse({"AnarchyPlayer", "--connect", "host:7777"}, error);
    REQUIRE(args);
    REQUIRE(args->connect == "host:7777");
    REQUIRE(args->folder.empty());

    args = parse({"AnarchyPlayer", "--connect", "h:1", "game"}, error);
    REQUIRE(args);
    REQUIRE(args->connect == "h:1");
    REQUIRE(args->folder == "game");

    args = parse({"AnarchyPlayer", "game", "--connect", "h:1"}, error);
    REQUIRE(args);
    REQUIRE(args->connect == "h:1");
    REQUIRE(args->folder == "game");

    // Finder passes a -psn_ argument to a program it starts.
    args = parse({"AnarchyPlayer", "-psn_0_123"}, error);
    REQUIRE(args);
    REQUIRE(args->connect.empty());
    REQUIRE(args->folder.empty());

    error.clear();
    REQUIRE_FALSE(parse({"AnarchyPlayer", "--connect"}, error));
    REQUIRE_FALSE(error.empty());

    error.clear();
    REQUIRE_FALSE(parse({"AnarchyPlayer", "--bogus"}, error));
    REQUIRE_FALSE(error.empty());
}
