#pragma once

#include <filesystem>

namespace engine_core {

// The engine's per-user folder: ANARCHY_USER_DATA when it is set and not
// empty, else Application Support/AnarchyEngine on a Mac, %APPDATA%\AnarchyEngine
// on Windows, and $XDG_CONFIG_HOME/anarchy-engine or ~/.config/anarchy-engine
// elsewhere, as the studio's config_directory. Empty when none can be found.
std::filesystem::path user_data_directory();

}  // namespace engine_core
