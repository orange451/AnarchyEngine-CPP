#pragma once

#include <filesystem>
#include <string>

namespace ide {

// What File > Export Game packs: a saved project, as its folders are on disk.
struct GameExportRequest {
    std::string name;
    std::filesystem::path project_root;
    std::filesystem::path tree_root;
    std::filesystem::path resources_root;
    // The game to write, as typed in the save dialog. game_path gives it the
    // system's extension when it has none.
    std::filesystem::path output;
};

// The name the save dialog suggests: the project's name with the system's
// extension, Name.exe on Windows, Name.app on a Mac, and Name on Linux.
std::string game_file_name(const std::string& project_name);

// path, with .exe added on Windows and .app on a Mac when it does not end so,
// so the game runs when opened.
std::filesystem::path game_path(const std::filesystem::path& path);

// The player program export copies: AnarchyPlayer beside the studio, or on a
// Mac AnarchyPlayer.app in the studio bundle's Resources. Empty when it is not there.
std::filesystem::path find_player();

// Writes the game to game_path(request.output): a copy of the player with the
// project and the engine's shaders packed into it (runner/GamePack.hpp). On
// Windows and Linux that is one file to send. On a Mac it is an .app, the
// player bundle with the pack in its Resources, signed again. A file already
// there is replaced; on a Mac, only an earlier export is. Blocks
// while it copies: call it off the UI thread. False, with why, when it fails;
// written is the game's path when it succeeds.
bool export_game(const GameExportRequest& request, std::filesystem::path& written, std::string& error);

}  // namespace ide
