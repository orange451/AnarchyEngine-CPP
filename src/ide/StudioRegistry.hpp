#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ide {

// One open studio's MCP server, as it tells other programs where it is. Each
// studio writes one file, <pid>-<port>.json, in the registry folder, and
// removes it when it closes. The bridge reads them to pick a studio.
struct StudioEntry {
    long long pid = 0;
    int port = 0;
    // The project's name, or "Untitled" before the first Save As.
    std::string project;
    // The project folder, as UTF-8. Empty when the place has none.
    std::string root;
    // The bearer token the studio's MCP server wants. The registry folder is
    // the user's own, which is what keeps other users from reading it.
    std::string token;
};

// studios/ in the config folder. Empty when there is no config folder.
std::filesystem::path studio_registry_dir();

// This process's id.
long long current_pid();
// False when no process has that id.
bool process_alive(long long pid);

// Writes or replaces the entry's file, creating the folder, which only this
// user may open where the system has such permissions. False, with error set,
// when it fails.
bool write_studio(const std::filesystem::path& dir, const StudioEntry& entry, std::string& error);
// Removes the entry's file, if it is there.
void remove_studio(const std::filesystem::path& dir, const StudioEntry& entry);
// Every studio whose process is still running, by pid, then port. The file of
// one whose process ended, as after a crash, is deleted. Files that do not
// read as an entry are skipped.
std::vector<StudioEntry> list_studios(const std::filesystem::path& dir);

}  // namespace ide
