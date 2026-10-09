#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"
#include "ide/PluginLoader.hpp"
#include "ide/PluginRibbon.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "SelectionService.hpp"
#include "FileBytes.hpp"
#include "InstanceFile.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

        // Each side joins the dock already there, as a tab: by where docks sit, not by window names.
        struct SideCase {
            const char* side;
            const char* beside;
        };
        const SideCase sides[] = {{"TopRight", "Game Explorer"}, {"BottomRight", "Properties"}, {"Bottom", "Console"}};
        for (const SideCase& side : sides) {
            const std::string folderName = std::string("Side ") + side.side;
            const std::string source =
                std::string("plugin:CreateDockWidget('W', {InitialDock = '") + side.side + "', Enabled = true})";
            layout.save_as_plugin(MakeToolFolder(engine, folderName.c_str(), source.c_str()));
            frames(4);
            ide::IdePane* placed = layout.page_named_for_tests("plugin:" + folderName + "/W");
            const void* expected = layout.dock_of_for_tests(layout.page_named_for_tests(side.beside));
            Expect(placed != nullptr && expected != nullptr && layout.dock_of_for_tests(placed) == expected,
                   (std::string(side.side) + " joins the dock that is there, as a tab").c_str());
        }

        // Closing and opening a widget again and again splits off no docks.
        const std::size_t docksBefore = layout.dock_count_for_tests();
        const engine_core::InstanceId cycling = MakeToolFolder(
            engine, "Cycling Tool", "_G.cycle = plugin:CreateDockWidget('W', {Enabled = true})");
        layout.save_as_plugin(cycling);
        frames(4);
        for (int i = 0; i < 4; ++i) {
            layout.close_page_for_tests(layout.page_named_for_tests("plugin:Cycling Tool/W"));
            frames(3);
            engine.on_simulation([&](engine_core::DataModel& game) {
                for (engine_core::InstanceId id : game.get_children(game.core())) {
                    if (game.name(id) == "Cycling Tool") {
                        if (auto* widget = dynamic_cast<engine_core::DockWidget*>(
                                game.instance(game.find_first_child(id, "W")))) {
                            widget->set_enabled(true);
                        }
                    }
                }
            });
            frames(3);
        }
        Expect(layout.page_open_for_tests(layout.page_named_for_tests("plugin:Cycling Tool/W")),
               "the cycled widget ends open");
        Expect(layout.dock_count_for_tests() == docksBefore, "opening and closing a widget makes no new docks");

        // Dropping an instance file or a plugin file puts its tree in Workspace, selected.
        {
            engine_core::CopiedNode tree;
            tree.class_name = "Folder";
            tree.name = "Dropped Tree";
            engine_core::CopiedNode inside;
            inside.class_name = "Script";
            inside.name = "Inside";
            inside.has_source = true;
            inside.source = "print('not run: it is in Workspace while stopped')";
            tree.children.push_back(inside);
            std::string error;
            const std::filesystem::path file = config / "Dropped Tree.aeinst";
            Expect(engine_core::save_instance_file(file, {tree}, error), "the instance file to drop is written");
            const std::filesystem::path bad = config / "Broken.aeinst";
            { std::ofstream(bad) << "{"; }
            Expect(layout.import_instance_files({file.u8string(), bad.u8string(), (config / "notes.txt").u8string()}),
                   "dropped instance files are taken");
            frames(2);
            bool inWorkspace = false;
            bool selected = false;
            engine.on_simulation([&](engine_core::DataModel& game) {
                const engine_core::InstanceId found = game.find_first_child(game.scene_service("Workspace"), "Dropped Tree");
                inWorkspace = found != 0 && game.find_first_child(found, "Inside") != 0;
                const std::vector<engine_core::InstanceId> now = game.selection().get();
                selected = now.size() == 1 && now[0] == found;
            });
            Expect(inWorkspace, "a dropped .aeinst lands in Workspace with its children");
            Expect(selected, "and is selected");
            Expect(!layout.import_instance_files({(config / "notes.txt").u8string()}), "other files are not taken");
        }

        // Deleting the plugin takes the page away.
        std::filesystem::remove(config / "plugins" / "Widget Tool.aeplugin");
        layout.poll_plugins(true);
        frames(3);
        Expect(layout.page_named_for_tests(paneName) == nullptr, "unloading the plugin removes its page");

        // Where the user puts a page is where it opens again; InitialDock is only its first place.
        auto setSpotEnabled = [&](bool enabled) {
            engine.on_simulation([&](engine_core::DataModel& game) {
                for (engine_core::InstanceId id : game.get_children(game.core())) {
                    if (game.name(id) == "Spot Tool") {
                        if (auto* widget = dynamic_cast<engine_core::DockWidget*>(
                                game.instance(game.find_first_child(id, "W")))) {
                            widget->set_enabled(enabled);
                        }
                    }
                }
            });
        };
        const char* const spotName = "plugin:Spot Tool/W";
        layout.save_as_plugin(MakeToolFolder(engine, "Spot Tool",
                                             "plugin:CreateDockWidget('W', {InitialDock = 'TopRight', Enabled = true})"));
        frames(4);
        ide::IdePane* console = layout.page_named_for_tests("Console");
        // As a tab with Console.
        layout.move_page_for_tests(layout.page_named_for_tests(spotName), console, 0);
        frames(3);
        layout.close_page_for_tests(layout.page_named_for_tests(spotName));
        frames(3);
        setSpotEnabled(true);
        frames(3);
        Expect(layout.dock_of_for_tests(layout.page_named_for_tests(spotName)) == layout.dock_of_for_tests(console),
               "a page moved in with Console opens there again, not at its InitialDock");
        // In a dock of its own, left of Console's.
        layout.move_page_for_tests(layout.page_named_for_tests(spotName), console, 1);
        frames(3);
        const void* ownDock = layout.dock_of_for_tests(layout.page_named_for_tests(spotName));
        Expect(ownDock != nullptr && ownDock != layout.dock_of_for_tests(console), "the page has a dock of its own");
        const std::size_t docksMoved = layout.dock_count_for_tests();
        layout.close_page_for_tests(layout.page_named_for_tests(spotName));
        frames(3);
        setSpotEnabled(true);
        frames(3);
        ide::IdePane* spot = layout.page_named_for_tests(spotName);
        Expect(layout.page_open_for_tests(spot) && layout.dock_of_for_tests(spot) != layout.dock_of_for_tests(console) &&
                   layout.dock_of_for_tests(spot) != layout.dock_of_for_tests(layout.page_named_for_tests("Game Explorer")),
               "a page alone in its own dock opens in a dock of its own again");
        Expect(layout.dock_count_for_tests() == docksMoved, "beside Console, as it was, with no extra dock");
        layout.move_page_for_tests(spot, layout.page_named_for_tests("Properties"), 0);
        frames(3);
    }
    // A new studio with the same config folder opens the page where it was left.
    {
        ide::IdeLayout layout(1280, 800, config);
        auto scene = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
        layout.mount(*scene);
        double time = 0.1;
        for (int i = 0; i < 2; ++i) {
            scene->layout(1280, 800, time);
            layout.flushFrame();
            time += 0.02;
        }
        layout.poll_plugins(true);
        for (int i = 0; i < 4; ++i) {
            scene->layout(1280, 800, time);
            layout.flushFrame();
            time += 0.02;
        }
        ide::IdePane* spot = layout.page_named_for_tests("plugin:Spot Tool/W");
        Expect(spot != nullptr && layout.dock_of_for_tests(spot) != nullptr &&
                   layout.dock_of_for_tests(spot) == layout.dock_of_for_tests(layout.page_named_for_tests("Properties")),
               "after a restart the page opens where the user left it");
    }
    std::error_code error;
    std::filesystem::remove_all(config, error);
    return gFailures;
}
