#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"
#include "ide/PluginLoader.hpp"
#include "ide/PluginRibbon.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
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

bool HasClass(const jadefx::Node* node, const char* name) {
    if (node == nullptr) {
        return false;
    }
    const auto& classes = node->getClassList();
    return std::find(classes.begin(), classes.end(), std::string(name)) != classes.end();
}

// Runs queued event handlers, as the simulation does each step. The engine is
// not started here, so this thread stands in for it.
void DrainEvents(engine_core::Engine& engine) {
    const engine_core::ThreadRole role = engine_core::thread_role();
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    engine.on_simulation([](engine_core::DataModel& game) { game.events().drain(); });
    engine_core::set_thread_role(role);
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

        // A plugin's toolbar shows on the Plugins tab, and a click reaches its script.
        ide::PluginRibbon* ribbon = layout.plugin_ribbon_for_tests();
        Expect(ribbon != nullptr, "the studio has a tabbed ribbon");
        if (ribbon == nullptr) {
            return gFailures;
        }
        Expect(ribbon->tab() == 0, "the ribbon opens on Home");
        Expect(ribbon->pluginsRow()->getElementsByClassName("ide-ribbon-empty").size() == 1,
               "with no plugin toolbars the Plugins tab says how to add one");
        const engine_core::InstanceId tool = MakeToolFolder(
            engine, "Ribbon Tool",
            "local b = plugin:CreateToolbar('Bar'):CreateButton('Go', 'Go now', 'icons/Play.png', 'Go')\n"
            "b.Click:Connect(function() b:SetActive(true) end)");
        layout.save_as_plugin(tool);
        ribbon->showTab(1);
        frames(3);
        std::uint32_t buttonId = 0;
        engine.on_simulation([&](engine_core::DataModel&) {
            const auto bars = engine.scripts().plugin_ui().toolbars();
            if (!bars.empty() && !bars[0].buttons.empty()) {
                buttonId = bars[0].buttons[0].id;
            }
        });
        jadefx::Node* go = ribbon->buttonNode(buttonId);
        Expect(go != nullptr, "the plugin's button is on the Plugins tab");
        Expect(ribbon->pluginsRow()->getElementsByClassName("ide-ribbon-empty").empty(), "the hint goes");
        if (go != nullptr) {
            const double x = go->getAbsoluteX() + go->getWidth() / 2;
            const double y = go->getAbsoluteY() + go->getHeight() / 2;
            scene->noteButton(0, true, x, y);
            scene->noteButton(0, false, x, y);
            DrainEvents(engine);
            frames(2);
            Expect(ribbon->buttonNode(buttonId) == go, "lighting a button keeps its node");
            Expect(HasClass(ribbon->buttonNode(buttonId), "on"), "SetActive from the Click handler lights the button");
        }
        std::filesystem::remove(config / "plugins" / "Ribbon Tool.aeplugin");
        layout.poll_plugins(true);
        frames(2);
        Expect(ribbon->buttonNode(buttonId) == nullptr, "unloading the plugin takes its toolbar away");

        // A dock widget is a studio page drawing the plugin's GUI, and closing it clears Enabled.
        const engine_core::InstanceId widgetTool =
            MakeToolFolder(engine, "Widget Tool",
                           "local w = plugin:CreateDockWidget('Panel', {Title = 'Widget Tool', Enabled = true})\n"
                           "local label = Instance.new('Label') label.Name = 'Hello' label.Text = 'Hello' label.Parent = w\n"
                           "_G.widget = w");
        layout.save_as_plugin(widgetTool);
        frames(4);
        const char* const paneName = "plugin:Widget Tool/Panel";
        ide::IdePane* page = layout.page_named_for_tests(paneName);
        Expect(page != nullptr, "the widget is a studio page named plugin:<plugin>/<id>");
        Expect(page != nullptr && page->title() == "Widget Tool", "the page shows the widget's Title");
        Expect(page != nullptr && page->getElementById("Hello") != nullptr, "the plugin's GUI is drawn in the page");
        Expect(layout.page_open_for_tests(page), "Enabled = true opens it");

        layout.close_page_for_tests(page);
        frames(3);
        auto widgetEnabled = [&] {
            bool enabled = true;
            engine.on_simulation([&](engine_core::DataModel& game) {
                for (engine_core::InstanceId id : game.get_children(game.core())) {
                    if (game.name(id) == "Widget Tool") {
                        const auto* widget = dynamic_cast<const engine_core::DockWidget*>(
                            game.instance(game.find_first_child(id, "Panel")));
                        enabled = widget != nullptr && widget->enabled();
                    }
                }
            });
            return enabled;
        };
        Expect(!widgetEnabled(), "closing the page sets Enabled to false");
        Expect(!layout.page_open_for_tests(page), "and it stays closed");

        // Setting Enabled opens it again, with the GUI as the plugin changed it.
        engine.on_simulation([&](engine_core::DataModel& game) {
            for (engine_core::InstanceId id : game.get_children(game.core())) {
                if (game.name(id) == "Widget Tool") {
                    auto* widget =
                        dynamic_cast<engine_core::DockWidget*>(game.instance(game.find_first_child(id, "Panel")));
                    if (widget != nullptr) {
                        widget->set_enabled(true);
                        game.destroy(game.find_first_child(widget->id(), "Hello"));
                    }
                }
            }
        });
        frames(3);
        page = layout.page_named_for_tests(paneName);
        Expect(layout.page_open_for_tests(page), "Enabled = true opens the page again");
        Expect(page != nullptr && page->getElementById("Hello") == nullptr, "GUI the plugin destroyed leaves the page");

        // Saving again reloads the plugin; its page comes back in the same dock.
        const void* dockBefore = layout.dock_of_for_tests(page);
        layout.save_as_plugin(widgetTool);
        frames(4);
        ide::IdePane* reloaded = layout.page_named_for_tests(paneName);
        Expect(layout.page_open_for_tests(reloaded), "a reload opens the page again");
        Expect(dockBefore != nullptr && layout.dock_of_for_tests(reloaded) == dockBefore, "in the dock it was in");

        // InitialDock Center docks beside the Scene View.
        const engine_core::InstanceId centerTool = MakeToolFolder(
            engine, "Center Tool", "plugin:CreateDockWidget('Mid', {InitialDock = 'Center', Enabled = true})");
        layout.save_as_plugin(centerTool);
        frames(4);
        ide::IdePane* mid = layout.page_named_for_tests("plugin:Center Tool/Mid");
        Expect(mid != nullptr && layout.dock_of_for_tests(mid) != nullptr &&
                   layout.dock_of_for_tests(mid) == layout.dock_of_for_tests(layout.page_named_for_tests("Scene View")),
               "InitialDock Center opens the page in the Scene View's dock");

        // Deleting the plugin takes the page away.
        std::filesystem::remove(config / "plugins" / "Widget Tool.aeplugin");
        layout.poll_plugins(true);
        frames(3);
        Expect(layout.page_named_for_tests(paneName) == nullptr, "unloading the plugin removes its page");
    }
    std::error_code error;
    std::filesystem::remove_all(config, error);
    return gFailures;
}
