#pragma once

#include <filesystem>
#include <string>

namespace ide {

// A file under the studio's resources/ tree, such as "icons/Plus.png" or
// "themes/light.css". Looks in the Mac bundle's Contents/Resources, then
// resources/ under the working directory, then beside the executable. Empty,
// after a note on stderr of where it looked, when no copy exists.
std::filesystem::path find_resource(const std::string& relative);

// A path as UTF-8, the encoding JadeFX and the engine take.
std::string utf8_path(const std::filesystem::path& path);
// A UTF-8 string as a path.
std::filesystem::path path_from_utf8(const std::string& text);

// Where the studio keeps this user's settings: Library/Application Support/
// AnarchyEngine on a Mac, %APPDATA%\AnarchyEngine on Windows, and
// $XDG_CONFIG_HOME/anarchy-engine (else ~/.config/anarchy-engine) elsewhere.
// Not created here. Empty when there is no home folder to put it in.
std::filesystem::path config_directory();

// The whole file. False, with error set, when it cannot be read.
bool read_file(const std::filesystem::path& path, std::string& out, std::string& error);
// Writes beside the target, then renames over it, so a crash leaves the old
// file whole. Creates the folder. False, with error set, when it fails.
bool write_file(const std::filesystem::path& path, const std::string& bytes, std::string& error);

// Opens a folder in the system's file browser: Finder, Explorer, or whatever
// xdg-open picks. Returns at once. False when none could be started.
bool reveal_folder(const std::filesystem::path& folder);

}  // namespace ide
