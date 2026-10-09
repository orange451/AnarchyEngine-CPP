#include "PluginLoader.hpp"

#include "DataModel.hpp"
#include "IdeResources.hpp"
#include "InstanceFile.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

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

std::vector<PluginStamp> scan_plugins(const std::filesystem::path& folder) {
    std::vector<PluginStamp> out;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const std::filesystem::path& path = it->path();
        std::error_code entry_error;
        if (path.extension() != kPluginExtension || !it->is_regular_file(entry_error)) {
            continue;
        }
        PluginStamp stamp;
        stamp.name = path.stem().u8string();
        stamp.path = path;
        stamp.time = std::filesystem::last_write_time(path, entry_error);
        if (!entry_error) {
            stamp.size = std::filesystem::file_size(path, entry_error);
        }
        // One being replaced as the folder is listed is picked up by the next scan.
        if (!entry_error) {
            out.push_back(std::move(stamp));
        }
    }
    std::sort(out.begin(), out.end(), [](const PluginStamp& a, const PluginStamp& b) { return a.name < b.name; });
    return out;
}

std::string plugin_file_name(const std::string& folder_name) {
    std::string out;
    out.reserve(folder_name.size());
    for (char c : folder_name) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|' || static_cast<unsigned char>(c) < 32;
        out += bad ? '_' : c;
    }
    // Windows drops trailing dots and spaces from file names.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    return out.empty() ? "Plugin" : out;
}

void PluginLoader::unload_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts, UserPlugin& plugin) {
    if (plugin.root != 0 && game.alive(plugin.root)) {
        scripts.fire_plugin_unloading(plugin.root);
        scripts.unregister_plugin(plugin.root);
        // Unloading's handlers may have destroyed it.
        if (game.alive(plugin.root)) {
            game.destroy(plugin.root);
        }
    }
    plugin.root = 0;
}

void PluginLoader::load_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts, UserPlugin& plugin) {
    std::vector<engine_core::CopiedNode> roots;
    std::string error;
    // A plugin's name names its dock widgets' pages, so two plugins may not share one.
    const bool builtin_name = std::any_of(loaded_.begin(), loaded_.end(), [&](engine_core::InstanceId id) {
        const std::string* name = scripts.plugin_name(id);
        return name != nullptr && *name == plugin.stamp.name;
    });
    if (builtin_name) {
        error = "a built-in plugin has that name";
    } else if (engine_core::load_instance_file(plugin.stamp.path, roots, error) &&
        (roots.size() != 1 || roots[0].class_name != "Folder")) {
        error = "its root must be one Folder";
    }
    std::vector<engine_core::InstanceId> made;
    if (error.empty() && !engine_core::paste_copies(game, roots, game.core(), &made, &error) && error.empty()) {
        error = "nothing could be built";
    }
    if (!error.empty()) {
        for (engine_core::InstanceId id : made) {
            game.destroy(id);
        }
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error,
                              "Plugin \"" + plugin.stamp.name + "\" failed to load: " + error);
        return;
    }
    plugin.root = made[0];
    scripts.register_plugin(plugin.root, plugin.stamp.name);
}

void PluginLoader::sync_user(engine_core::DataModel& game, engine_core::ScriptRuntime& scripts,
                             const std::vector<PluginStamp>& stamps) {
    std::vector<UserPlugin> next;
    for (UserPlugin& have : user_) {
        const auto found = std::find_if(stamps.begin(), stamps.end(),
                                        [&](const PluginStamp& stamp) { return stamp.name == have.stamp.name; });
        if (found == stamps.end() || *found != have.stamp) {
            unload_user(game, scripts, have);
        } else {
            next.push_back(have);
        }
    }
    for (const PluginStamp& stamp : stamps) {
        const bool kept = std::any_of(next.begin(), next.end(),
                                      [&](const UserPlugin& plugin) { return plugin.stamp.name == stamp.name; });
        if (kept) {
            continue;
        }
        UserPlugin plugin;
        plugin.stamp = stamp;
        load_user(game, scripts, plugin);
        // A failed load keeps its stamp too, so the same broken file is not tried at every scan.
        next.push_back(std::move(plugin));
    }
    std::sort(next.begin(), next.end(),
              [](const UserPlugin& a, const UserPlugin& b) { return a.stamp.name < b.stamp.name; });
    user_ = std::move(next);
    // The Scripts that entered Core with each Folder run with it, not as plugins of their own.
    scripts.start_core_scripts();
}

}  // namespace ide
