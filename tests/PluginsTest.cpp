#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"
#include "ide/PluginLoader.hpp"
#include "ide/PluginRibbon.hpp"
#include "ide/PluginWidgetPane.hpp"
#include "ide/AssetPicker.hpp"
#include "runner/GameView.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "Matrix4.hpp"
#include "UserInputService.hpp"
#include "SelectionService.hpp"
#include "FileBytes.hpp"
#include "InstanceFile.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cmath>
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
        {
            // The tab bar: Test, Pause, Resume, and Stop at its left, Home and Plugins in its middle.
            jadefx::Node* bar = ribbon->tabBar();
            jadefx::Node* testButton = bar->getElementById("test-button");
            Expect(testButton != nullptr, "Test is on the tab bar");
            Expect(testButton != nullptr && testButton->getAbsoluteX() < bar->getAbsoluteX() + 40,
                   "at its left");
            std::vector<jadefx::Node*> tabs = bar->getElementsByClassName("ide-ribbon-tab");
            Expect(tabs.size() == 2, "Home and Plugins are on the tab bar");
            if (tabs.size() == 2) {
                const double middle = (tabs[0]->getAbsoluteX() + tabs[1]->getAbsoluteX() + tabs[1]->getWidth()) / 2;
                const double barMiddle = bar->getAbsoluteX() + bar->getWidth() / 2;
                Expect(std::abs(middle - barMiddle) < 4, "and the tabs sit in the middle of the bar");
            }
            jadefx::Node* grid = bar->getElementById("grid-toggle");
            Expect(grid != nullptr, "the floor grid toggle is on the tab bar");
            Expect(grid != nullptr && grid->getAbsoluteX() + grid->getWidth() > bar->getAbsoluteX() + bar->getWidth() - 40,
                   "at its right");
            Expect(ribbon->homeRow()->getElementsByClassName("ide-ribbon-button").empty(),
                   "the Home row has no buttons until a built-in plugin adds a toolbar");
        }
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
        // A card: its icon above its name, in a row three times the tab bar's height.
        Expect(HasClass(go, "ide-ribbon-card"), "a plugin's button is a card");
        Expect(go != nullptr && !go->getElementsByClassName("ide-ribbon-card-icon").empty() &&
                   !go->getElementsByClassName("ide-ribbon-card-name").empty(),
               "with an icon and a nameplate");
        if (go != nullptr && !go->getElementsByClassName("ide-ribbon-card-icon").empty() &&
            !go->getElementsByClassName("ide-ribbon-card-name").empty()) {
            const jadefx::Node* icon = go->getElementsByClassName("ide-ribbon-card-icon")[0];
            const jadefx::Node* name = go->getElementsByClassName("ide-ribbon-card-name")[0];
            Expect(icon->getAbsoluteY() + icon->getHeight() <= name->getAbsoluteY() + 1, "the name is under the icon");
            Expect(std::abs((icon->getAbsoluteX() + icon->getWidth() / 2) - (go->getAbsoluteX() + go->getWidth() / 2)) < 2,
                   "the icon sits in the middle of the card");
        }
        if (go != nullptr && !go->getElementsByClassName("ide-ribbon-card-icon").empty()) {
            Expect(std::abs(go->getElementsByClassName("ide-ribbon-card-icon")[0]->getHeight() - 24) < 1,
                   "a card's icon is 24 points");
        }
        Expect(std::abs(ribbon->pluginsRow()->getHeight() - 64) < 2, "the plugins row is 64 points tall");
        // The toolbar's name under its buttons, in the middle of the group.
        std::vector<jadefx::Node*> captions = ribbon->pluginsRow()->getElementsByClassName("ide-ribbon-group-name");
        Expect(captions.size() == 1, "a group shows its toolbar's name");
        if (go != nullptr && captions.size() == 1) {
            auto* caption = dynamic_cast<jadefx::Label*>(captions[0]);
            Expect(caption != nullptr && caption->getText() == "Bar", "the name is the toolbar's");
            jadefx::Node* group = go->getParent() != nullptr ? go->getParent()->getParent() : nullptr;
            Expect(group != nullptr && HasClass(group, "ide-ribbon-group"), "the card is in a group");
            Expect(captions[0]->getAbsoluteY() >= go->getAbsoluteY() + go->getHeight() - 1, "the name is under the cards");
            if (group != nullptr) {
                Expect(std::abs((captions[0]->getAbsoluteX() + captions[0]->getWidth() / 2) -
                                (group->getAbsoluteX() + group->getWidth() / 2)) < 2,
                       "in the middle of the group");
            }
            Expect(captions[0]->getAbsoluteY() + captions[0]->getHeight() <=
                       ribbon->pluginsRow()->getAbsoluteY() + ribbon->pluginsRow()->getHeight() + 1,
                   "inside the row");
        }
        if (go != nullptr) {
            const double above = go->getAbsoluteY() - ribbon->pluginsRow()->getAbsoluteY();
            const double below = ribbon->pluginsRow()->getAbsoluteY() + ribbon->pluginsRow()->getHeight() -
                                 (go->getAbsoluteY() + go->getHeight());
            Expect(above <= 8 && below <= 22, "with little room above a card, and its group's name below");
        }
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

        // A built-in plugin's toolbar goes on Home, beside Test and Stop; a user plugin's stays on Plugins.
        engine_core::InstanceId builtin = 0;
        engine.on_simulation([&](engine_core::DataModel& game) {
            engine_core::Script& script = game.create<engine_core::Script>();
            game.set_name(script.id(), "BuiltinTool");
            script.set_source("plugin:CreateToolbar('Built In'):CreateButton('Bi', '', 'icons/Grid.png', 'Bi')");
            game.set_parent(script.id(), game.core());
            engine.scripts().start_core_scripts();
            builtin = script.id();
        });
        frames(3);
        std::uint32_t builtinButton = 0;
        engine.on_simulation([&](engine_core::DataModel&) {
            for (const auto& bar : engine.scripts().plugin_ui().toolbars()) {
                if (bar.name == "Built In" && !bar.buttons.empty()) {
                    builtinButton = bar.buttons[0].id;
                }
            }
        });
        auto inside = [](const jadefx::Node* node, const jadefx::Node* holder) {
            for (; node != nullptr; node = node->getParent()) {
                if (node == holder) {
                    return true;
                }
            }
            return false;
        };
        jadefx::Node* bi = ribbon->buttonNode(builtinButton);
        Expect(bi != nullptr && inside(bi, ribbon->homeRow()), "a built-in plugin's button is on the Home tab");
        Expect(!inside(bi, ribbon->pluginsRow()), "and not on the Plugins tab");
        Expect(ribbon->pluginsRow()->getElementsByClassName("ide-ribbon-empty").size() == 1,
               "the Plugins tab still says no plugins are installed");
        engine.on_simulation([&](engine_core::DataModel& game) {
            game.destroy(builtin);
            engine.scripts().start_core_scripts();
        });
        frames(3);
        Expect(ribbon->buttonNode(builtinButton) == nullptr, "a built-in plugin that goes takes its button off Home");

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
        layout.replace_plugin_for_tests();
        frames(4);
        ide::IdePane* reloaded = layout.page_named_for_tests(paneName);
        Expect(layout.page_open_for_tests(reloaded), "a reload opens the page again");
        Expect(dockBefore != nullptr && layout.dock_of_for_tests(reloaded) == dockBefore, "in the dock it was in");

        // A click on an AssetPicker opens the studio's asset picker, and a pick writes its Value.
        {
            engine_core::InstanceId stone = 0;
            engine.on_simulation([&](engine_core::DataModel& game) {
                engine_core::Material& made = game.create<engine_core::Material>();
                game.set_name(made.id(), "Stone");
                game.set_parent(made.id(), game.service("Materials"));
                stone = made.id();
            });
            const engine_core::InstanceId pickerTool = MakeToolFolder(
                engine, "Picker Tool",
                "local w = plugin:CreateDockWidget('P', {Enabled = true})\n"
                "local p = Instance.new('AssetPicker') p.Name = 'Pick' p.AssetType = Enum.AssetType.Material p.Parent = w");
            layout.save_as_plugin(pickerTool);
            frames(4);
            auto* pane = dynamic_cast<ide::PluginWidgetPane*>(layout.page_named_for_tests("plugin:Picker Tool/P"));
            Expect(pane != nullptr, "the picker's widget is a page");
            auto* pick = pane != nullptr ? dynamic_cast<jadefx::Button*>(pane->getElementById("Pick")) : nullptr;
            Expect(pick != nullptr && pick->getText() == "None", "an AssetPicker with no Value shows None");
            auto click = [&](jadefx::Node* node) {
                const double x = node->getAbsoluteX() + node->getWidth() / 2;
                const double y = node->getAbsoluteY() + node->getHeight() / 2;
                scene->noteButton(0, true, x, y);
                scene->noteButton(0, false, x, y);
                frames(2);
            };
            if (pick != nullptr) {
                click(pick);
                Expect(pane->picker() != nullptr && pane->picker()->showing(), "a click opens the asset picker");
                jadefx::Node* row = pane->picker() != nullptr ? pane->picker()->row(stone) : nullptr;
                Expect(row != nullptr, "listing the Materials");
                if (row != nullptr) {
                    click(row);
                    DrainEvents(engine);
                    frames(2);
                }
                engine_core::InstanceId picked = 0;
                engine.on_simulation([&](engine_core::DataModel& game) {
                    for (engine_core::InstanceId id : game.get_children(game.core())) {
                        if (game.name(id) != "Picker Tool") {
                            continue;
                        }
                        const engine_core::InstanceId widget = game.find_first_child(id, "P");
                        if (const auto* gui = dynamic_cast<const engine_core::AssetPicker*>(
                                game.instance(game.find_first_child(widget, "Pick")))) {
                            picked = gui->asset_id();
                        }
                    }
                });
                Expect(picked == stone, "the pick writes Value");
                pick = dynamic_cast<jadefx::Button*>(pane->getElementById("Pick"));
                Expect(pick != nullptr && pick->getText() == "Stone", "and the picker shows the asset's name");
            }
        }

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

        // A side column that is one dock is split: BottomLeft opens below Search, not as its tab.
        {
            layout.save_as_plugin(MakeToolFolder(
                engine, "Side BottomLeft",
                "plugin:CreateDockWidget('W', {InitialDock = 'BottomLeft', Enabled = true, Height = 400})"));
            frames(4);
            ide::IdePane* placed = layout.page_named_for_tests("plugin:Side BottomLeft/W");
            ide::IdePane* search = layout.page_named_for_tests("Search");
            Expect(placed != nullptr && search != nullptr &&
                       layout.dock_of_for_tests(placed) != layout.dock_of_for_tests(search) &&
                       std::abs(placed->getAbsoluteX() - search->getAbsoluteX()) < 2 &&
                       placed->getAbsoluteY() > search->getAbsoluteY() + search->getHeight() - 2,
                   "BottomLeft splits the left column, below Search");
            // Closed and opened again, it comes back there, not beside the bottom row's dock.
            layout.close_page_for_tests(placed);
            frames(3);
            engine.on_simulation([&](engine_core::DataModel& game) {
                for (engine_core::InstanceId id : game.get_children(game.core())) {
                    if (game.name(id) == "Side BottomLeft") {
                        if (auto* widget = dynamic_cast<engine_core::DockWidget*>(
                                game.instance(game.find_first_child(id, "W")))) {
                            widget->set_enabled(true);
                        }
                    }
                }
            });
            frames(4);
            placed = layout.page_named_for_tests("plugin:Side BottomLeft/W");
            Expect(placed != nullptr && layout.page_open_for_tests(placed) &&
                       std::abs(placed->getAbsoluteX() - search->getAbsoluteX()) < 2 &&
                       placed->getAbsoluteY() > search->getAbsoluteY() + search->getHeight() - 2,
                   "and opened again it is back below Search");
            layout.close_page_for_tests(placed);
            frames(3);
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

        // An active plugin hears the Scene View's mouse and has its left button to itself.
        {
            auto* view = dynamic_cast<runner::GameView*>(layout.page_named_for_tests("Scene View"));
            Expect(view != nullptr, "the studio has a Scene View");
            // The Scene View in front, with a camera, as a painted frame would leave it.
            layout.close_landing();
            // The Center test above left its page in front of the Scene View.
            layout.close_page_for_tests(layout.page_named_for_tests("plugin:Center Tool/Mid"));
            if (view != nullptr) {
                layout.reveal_window(view);
                view->setViewForTests(engine_core::matrix4_translation(0.f, 5.f, 10.f), 70.f);
                view->requestFocus();
            }
            frames(2);
            const engine_core::InstanceId tool = MakeToolFolder(
                engine, "Mouse Tool",
                "_G.mousePlugin = plugin\n"
                "plugin.Deactivation:Connect(function() Instance.new('Folder', workspace).Name = 'MouseOff' end)\n"
                "local mouse = plugin:GetMouse()\n"
                "mouse.Button1Down:Connect(function()\n"
                "    local f = Instance.new('Folder')\n"
                "    f.Name = 'MouseDown'\n"
                "    f.Parent = workspace\n"
                "end)\n"
                "plugin:Activate()");
            layout.save_as_plugin(tool);
            frames(3);
            // Where the press lands, and whether the game's input saw the left button go down.
            auto click = [&](bool& gameSaw) {
                const double x = view->getAbsoluteX() + view->getWidth() / 2;
                const double y = view->getAbsoluteY() + view->getHeight() / 2;
                scene->noteMove(x, y);
                // The pointer is over the view, as a layout finds, before it presses.
                frames(1);
                scene->noteButton(0, true, x, y);
                const engine_core::ThreadRole role = engine_core::thread_role();
                engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
                engine.on_simulation([&](engine_core::DataModel& game) {
                    game.input().dispatch(game.events());
                    gameSaw = game.input().button_down(0);
                    game.events().drain();
                });
                engine_core::set_thread_role(role);
                scene->noteButton(0, false, x, y);
                DrainEvents(engine);
                frames(2);
            };
            auto inWorkspace = [&](const char* name) {
                bool found = false;
                engine.on_simulation([&](engine_core::DataModel& game) {
                    found = game.find_first_child(game.scene_service("Workspace"), name) != 0;
                });
                return found;
            };
            bool gameSaw = true;
            if (view != nullptr) {
                click(gameSaw);
            }
            Expect(inWorkspace("MouseDown"), "the active plugin's Button1Down fires on a click in the Scene View");
            Expect(!gameSaw, "and the game's input, where selection listens, never sees that left button");

            // Deactivating the plugin, as another tool turning on does, gives the left button back.
            if (view != nullptr) {
                engine.on_simulation([&engine](engine_core::DataModel&) {
                    engine.scripts().plugin_ui().deactivate_all();
                });
                frames(2);
                DrainEvents(engine);
                frames(1);
                click(gameSaw);
            }
            Expect(inWorkspace("MouseOff"), "deactivating the plugin fires its Deactivation");
            Expect(gameSaw, "and after it the game's input sees the left button again");
        }

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

        // Two tools whose panes the user keeps in the same place: switching from one to
        // the other, as turning one tool on turns the other off, shows the new one in the
        // old one's dock at its size, and that is the size it remembers.
        {
            auto setSwap = [&](const char* tool, bool enabled) {
                engine.on_simulation([&, tool, enabled](engine_core::DataModel& game) {
                    for (engine_core::InstanceId id : game.get_children(game.core())) {
                        if (game.name(id) == tool) {
                            if (auto* widget = dynamic_cast<engine_core::DockWidget*>(
                                    game.instance(game.find_first_child(id, "W")))) {
                                widget->set_enabled(enabled);
                            }
                        }
                    }
                });
            };
            layout.save_as_plugin(MakeToolFolder(engine, "Swap A",
                                                 "plugin:CreateDockWidget('W', {InitialDock = 'BottomLeft', Enabled = false})"));
            layout.save_as_plugin(MakeToolFolder(engine, "Swap B",
                                                 "plugin:CreateDockWidget('W', {InitialDock = 'BottomLeft', Enabled = false})"));
            frames(4);
            ide::IdePane* swapConsole = layout.page_named_for_tests("Console");
            // Each left in a dock of its own below Console.
            for (const char* tool : {"Swap A", "Swap B"}) {
                setSwap(tool, true);
                frames(4);
                layout.move_page_for_tests(layout.page_named_for_tests(std::string("plugin:") + tool + "/W"),
                                           swapConsole, 4);
                frames(4);
                setSwap(tool, false);
                frames(4);
            }
            auto heightOf = [&](const char* name) {
                ide::IdePane* page = layout.page_named_for_tests(name);
                return page != nullptr && layout.page_open_for_tests(page) ? page->getHeight() : -1.0;
            };
            setSwap("Swap A", true);
            frames(4);
            const double aHeight = heightOf("plugin:Swap A/W");
            const std::size_t docks = layout.dock_count_for_tests();
            // In one step.
            setSwap("Swap A", false);
            setSwap("Swap B", true);
            frames(4);
            Expect(std::abs(heightOf("plugin:Swap B/W") - aHeight) < 1 && layout.dock_count_for_tests() == docks,
                   "switching shows the new pane in the old one's place, at its height");
            // And back, the old pane closing a frame after the new one opens.
            setSwap("Swap A", true);
            frames(1);
            setSwap("Swap B", false);
            frames(4);
            Expect(std::abs(heightOf("plugin:Swap A/W") - aHeight) < 1 && layout.dock_count_for_tests() == docks,
                   "so does switching when the old pane closes a frame later");
            // Many switches later, still the same.
            for (int round = 0; round < 3; ++round) {
                setSwap("Swap A", false);
                setSwap("Swap B", true);
                frames(3);
                setSwap("Swap B", false);
                setSwap("Swap A", true);
                frames(3);
            }
            Expect(std::abs(heightOf("plugin:Swap A/W") - aHeight) < 1 && layout.dock_count_for_tests() == docks,
                   "and its height holds over many switches");
            // The switches leave each pane's own place, not "a tab with the other one":
            // opened alone, B comes back there.
            setSwap("Swap A", false);
            frames(3);
            setSwap("Swap B", true);
            frames(4);
            // Closed and opened alone again and again, it keeps that height.
            for (int i = 0; i < 5; ++i) {
                setSwap("Swap B", false);
                frames(3);
                setSwap("Swap B", true);
                frames(4);
            }
            Expect(std::abs(heightOf("plugin:Swap B/W") - aHeight) < 1 && layout.dock_count_for_tests() == docks,
                   "opened alone after the switches, and again and again, a pane is back in its place at that height");
            setSwap("Swap B", false);
            frames(3);
        }
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
