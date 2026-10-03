#pragma once

#include "types.hpp"

#include <cstddef>
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
inline constexpr const char* kBuiltinPlugins[] = {"plugins/SceneCamera.luau"};

// Reads a .luau file. The name is the file's stem. False, with why, when it cannot be read.
bool read_plugin_file(const std::filesystem::path& path, PluginFile& out, std::string& error);
// Reads each of kBuiltinPlugins. One that cannot be found or read is left out, and why goes to errors.
std::vector<PluginFile> read_builtin_plugins(std::vector<std::string>& errors);

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

private:
    std::vector<engine_core::InstanceId> loaded_;
};

}  // namespace ide
