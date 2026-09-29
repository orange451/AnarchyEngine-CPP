#pragma once

#include "engine_core/FileBytes.hpp"

#include <filesystem>
#include <string>

namespace ide {

// A file under the studio's resources/ tree, such as "icons/Plus.png" or
// "themes/light.css". Looks in the Mac bundle's Contents/Resources, then
// resources/ under the working directory, then beside the executable. Empty,
// after a note on stderr of where it looked, when no copy exists.
std::filesystem::path find_resource(const std::string& relative);

using engine_core::utf8_path;
// A UTF-8 string as a path.
std::filesystem::path path_from_utf8(const std::string& text);

// Where the studio keeps this user's settings: Library/Application Support/
// AnarchyEngine on a Mac, %APPDATA%\AnarchyEngine on Windows, and
// $XDG_CONFIG_HOME/anarchy-engine (else ~/.config/anarchy-engine) elsewhere.
// Not created here. Empty when there is no home folder to put it in.
std::filesystem::path config_directory();

using engine_core::read_file;
using engine_core::write_file;

// Opens a folder in the system's file browser: Finder, Explorer, or whatever
// xdg-open picks. Returns at once. False when none could be started.
bool reveal_folder(const std::filesystem::path& folder);

}  // namespace ide
