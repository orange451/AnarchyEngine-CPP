#include "ide/IdeLayout.hpp"
#include "ide/PluginLoader.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Script.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

// Plugins in the studio: Save as Plugin writes the file, and polling the
// plugins folder loads and unloads what it holds.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

std::filesystem::path TempConfig() {
    std::random_device device;
    return std::filesystem::temp_directory_path() /
           ("ae-plugins-" + std::to_string(device()) + std::to_string(device()));
}

// Whether Core holds an instance named name: a loaded user plugin's Folder.
bool InCore(engine_core::Engine& engine, const std::string& name) {
    bool found = false;
    engine.on_simulation([&](engine_core::DataModel& game) {
        for (engine_core::InstanceId id : game.get_children(game.core())) {
            found = found || game.name(id) == name;
        }
    });
    return found;
}

// A Folder in Workspace with one Script, as a user builds a plugin.
engine_core::InstanceId MakeToolFolder(engine_core::Engine& engine, const char* name, const char* source) {
    engine_core::InstanceId folder = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::Folder& made = game.create<engine_core::Folder>();
        game.set_name(made.id(), name);
        game.set_parent(made.id(), game.scene_service("Workspace"));
        engine_core::Script& script = game.create<engine_core::Script>();
        game.set_name(script.id(), "init");
        script.set_source(source);
        game.set_parent(script.id(), made.id());
        folder = made.id();
    });
    return folder;
}

}  // namespace

int RunPluginsTests() {
    const std::filesystem::path config = TempConfig();
    std::filesystem::create_directories(config);
    {
        ide::IdeLayout layout(1280, 800, config);
        auto scene = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
        layout.mount(*scene);
        double time = 0.1;
        auto frames = [&](int count) {
            for (int i = 0; i < count; ++i) {
                scene->layout(1280, 800, time);
                layout.flushFrame();
                time += 0.02;
            }
        };
        frames(2);
        engine_core::Engine& engine = layout.simulation();

        const engine_core::InstanceId folder = MakeToolFolder(engine, "Hello Tool", "_G.hello = true");
        layout.save_as_plugin(folder);
        frames(2);
        Expect(std::filesystem::exists(config / "plugins" / "Hello Tool.aeplugin"), "Save as Plugin writes the file");
        Expect(InCore(engine, "Hello Tool"), "the saved plugin loads into Core without waiting for the poll");

        std::filesystem::remove(config / "plugins" / "Hello Tool.aeplugin");
        layout.poll_plugins(true);
        frames(2);
        Expect(!InCore(engine, "Hello Tool"), "deleting the file unloads the plugin");
    }
    std::error_code error;
    std::filesystem::remove_all(config, error);
    return gFailures;
}
