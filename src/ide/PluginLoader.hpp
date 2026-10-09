#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine_core {
class DataModel;
class ScriptRuntime;
}  // namespace engine_core

namespace ide {

// One built-in plugin: a Script's name and its source.
struct PluginFile {
    std::string name;
    std::string source;
};

// The studio's own plugins, under resources/.
inline constexpr const char* kBuiltinPlugins[] = {"plugins/SceneCamera.luau", "plugins/MoveTool.luau"};

// Reads a .luau file. The name is the file's stem. False, with why, when it cannot be read.
bool read_plugin_file(const std::filesystem::path& path, PluginFile& out, std::string& error);
// Reads each of kBuiltinPlugins. One that cannot be found or read is left out, and why goes to errors.
std::vector<PluginFile> read_builtin_plugins(std::vector<std::string>& errors);

// A user plugin file: a Folder saved as an instance file with this extension,
// in the plugins folder under the studio's config folder.
inline constexpr const char* kPluginExtension = ".aeplugin";

// One .aeplugin in the plugins folder: its stem, path, time, and size. Two
// stamps are equal when the name, time, and size are, so a rewrite shows.
struct PluginStamp {
    std::string name;
    std::filesystem::path path;
    std::filesystem::file_time_type time{};
    std::uintmax_t size = 0;
    bool operator==(const PluginStamp& other) const {
        return name == other.name && time == other.time && size == other.size;
    }
    bool operator!=(const PluginStamp& other) const { return !(*this == other); }
};

// The folder's .aeplugin files, sorted by name. Empty when the folder is missing.
std::vector<PluginStamp> scan_plugins(const std::filesystem::path& folder);
// The file name a Folder saves as: its Name, each character a file name cannot
// hold replaced by _, without trailing dots or spaces, or "Plugin" when nothing is left.
std::string plugin_file_name(const std::string& folder_name);

// Runs plugin files in the plugin VM, each as a Script in Core: not in the
// explorer, not saved, not an undo step, and kept through New, Open, Play, and
// Stop, so the studio loads them once.
class PluginLoader {
public:
    // SimulationThread. Destroys what the last load made, then makes one
    // Script per file in Core and starts them. Returns how many run as plugins.
    std::size_t load(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                     const std::vector<PluginFile>& files);
    const std::vector<engine_core::InstanceId>& loaded() const { return loaded_; }

    // SimulationThread. Brings Core's user plugins to what stamps lists: loads a new
    // file, unloads a gone one, and reloads a changed one. Unloading fires the
    // plugin's Unloading, stops it, and destroys its Folder. A file that fails
    // says why in the output as Plugin "X" failed to load, and is not tried again
    // until its stamp changes.
    void sync_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                   const std::vector<PluginStamp>& stamps);

    // A user plugin as sync_user last saw it: its file's stamp and its root, 0 when it failed.
    struct UserPlugin {
        PluginStamp stamp;
        engine_core::InstanceId root = 0;
    };
    // By name.
    const std::vector<UserPlugin>& user() const { return user_; }

private:
    void unload_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts, UserPlugin& plugin);
    void load_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts, UserPlugin& plugin);

    std::vector<engine_core::InstanceId> loaded_;
    std::vector<UserPlugin> user_;
};

}  // namespace ide
