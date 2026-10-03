#include "PluginLoader.hpp"

#include "DataModel.hpp"
#include "IdeResources.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"

#include <fstream>
#include <sstream>

namespace ide {

bool read_plugin_file(const std::filesystem::path& path, PluginFile& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not read " + path.u8string();
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    out.name = path.stem().u8string();
    out.source = text.str();
    return true;
}

std::vector<PluginFile> read_builtin_plugins(std::vector<std::string>& errors) {
    std::vector<PluginFile> files;
    for (const char* relative : kBuiltinPlugins) {
        const std::filesystem::path path = find_resource(relative);
        PluginFile file;
        std::string error;
        if (path.empty()) {
            errors.push_back(std::string("no ") + relative + " in resources");
        } else if (!read_plugin_file(path, file, error)) {
            errors.push_back(error);
        } else {
            files.push_back(std::move(file));
        }
    }
    return files;
}

std::size_t PluginLoader::load(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                               const std::vector<PluginFile>& files) {
    // In Core, so none of this is the user's edit.
    for (engine_core::InstanceId id : loaded_) {
        if (game.alive(id)) {
            game.destroy(id);
        }
    }
    loaded_.clear();
    const engine_core::InstanceId core = game.core();
    for (const PluginFile& file : files) {
        engine_core::Script& script = game.create<engine_core::Script>();
        game.set_name(script.id(), file.name);
        script.set_source(file.source);
        game.set_parent(script.id(), core);
        loaded_.push_back(script.id());
    }
    scripts.start_core_scripts();
    std::size_t registered = 0;
    for (engine_core::InstanceId id : loaded_) {
        if (scripts.is_plugin(id)) {
            ++registered;
        }
    }
    return registered;
}

}  // namespace ide
