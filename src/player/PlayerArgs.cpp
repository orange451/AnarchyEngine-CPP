#include "PlayerArgs.hpp"

namespace player {

std::optional<PlayerArgs> parse_player_args(int argc, char** argv, std::string& error) {
    PlayerArgs args;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const std::string arg = argv[i];
        // Finder passes -psn_ to a program it starts on a Mac, and Xcode its own
        // -NS settings, each with a value ("-NSDocumentRevisionsDebugMode YES");
        // neither is a folder or ours. Any other single dash is a mistake.
        if (arg.rfind("-psn_", 0) == 0) {
            continue;
        }
        if (arg.rfind("-NS", 0) == 0) {
            if (i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '-') {
                ++i;
            }
            continue;
        }
        if (arg == "--connect") {
            if (i + 1 >= argc || argv[i + 1] == nullptr || argv[i + 1][0] == '\0') {
                error = "--connect needs a server address, such as --connect localhost:7777";
                return std::nullopt;
            }
            args.connect = argv[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            error = "Unknown option " + arg + ". Usage: AnarchyPlayer [project folder] [--connect host:port]";
            return std::nullopt;
        }
        if (!args.folder.empty()) {
            error = "Only one project folder can be played: " + arg;
            return std::nullopt;
        }
        args.folder = std::filesystem::u8path(arg);
    }
    return args;
}

}  // namespace player
