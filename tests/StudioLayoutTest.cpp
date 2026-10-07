#include "ide/IdeAssets.hpp"
#include "ide/IdeConsole.hpp"
#include "ide/IdeDock.hpp"
#include "ide/IdeExplorer.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdePrefabEditor.hpp"
#include "ide/IdeResources.hpp"
#include "ide/IdePane.hpp"
#include "ide/IdeSearch.hpp"
#include "ide/IdeTerminal.hpp"
#include "ide/IdeTerrainEditor.hpp"
#include "runner/GameView.hpp"

#include "Engine.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "SelectionService.hpp"
#include "Project.hpp"
#include "SceneService.hpp"
#include "ScriptRuntime.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int RunFindReplaceTests(engine_core::Engine& engine);
int RunThemeTests(jadefx::Scene& scene);
int RunPreferencesTests();
int RunSaveConflictTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunTextureImportTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunModelImportTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunScratchResourcesTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunConflictsTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunProblemsStartupTests();
int RunProblemsPaneTests(engine_core::Engine& engine);
int RunProblemsWindowTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunScriptTabTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunLandingPageTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunTerminalPaneTests();
int RunGuiStyleTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunGuiImageTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunBillboardLayerTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunStatusBarTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunProfilerOverlayTests(ide::IdeLayout& layout, jadefx::Scene& scene);
int RunProfilerPlayerKeyTests();
int RunProfilerColorTests();
int RunUiFrameProfileTests();

// R10: the studio's default layout builds, and its docks hold the explorers,
// the console, and Properties. Runs headless: the threads are never started.
int main() {
    int failures = 0;
    auto expect = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };
    ide::IdeLayout layout(1280, 800);
    auto scene = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
    layout.mount(*scene);
    scene->layout(1280, 800, 0.1);
    scene->layout(1280, 800, 0.2);
    // Escape as the window's stage delivers it: when the scene does not consume
    // the key, leave_field_on_escape gets it.
    auto escape = [&scene] {
        if (!scene->noteKey(jadefx::Key::Escape, true, false, 0)) {
            ide::leave_field_on_escape(*scene, jadefx::Key::Escape, true);
        }
        scene->noteKey(jadefx::Key::Escape, false, false, 0);
    };

    // Preferences is in the File menu, and only there.
    // The menu bar is the first row of the root's top.
    jadefx::MenuBar* bar = nullptr;
    if (auto* root = dynamic_cast<jadefx::BorderPane*>(scene->getRoot())) {
        if (auto* top = dynamic_cast<jadefx::VBox*>(root->getTop()); top != nullptr && !top->getChildren().empty()) {
            bar = dynamic_cast<jadefx::MenuBar*>(top->getChildren()[0].get());
        }
    }
    expect(bar != nullptr, "the menu bar tops the studio");
    std::vector<std::string> holding;
    if (bar != nullptr) {
        for (const std::shared_ptr<jadefx::Menu>& menu : bar->getMenus().items()) {
            for (const std::shared_ptr<jadefx::MenuItem>& item : menu->getItems().items()) {
                if (item && item->getText() == "Preferences\u2026") {
                    holding.push_back(menu->getText());
                }
            }
        }
    }
    expect(holding == std::vector<std::string>{"File"}, "Preferences is in the File menu alone");

    // A new studio has the layout in resources/layouts/default-layout.json:
    // Search on the left, the scene view over Assets and the console, and the
    // game explorer over Properties on the right.
    auto shown_pane = [&scene](const std::string& name) -> ide::IdePane* {
        for (jadefx::Node* node : scene->getRoot()->getElementsByClassName("ide-pane")) {
            auto* pane = dynamic_cast<ide::IdePane*>(node);
            if (pane != nullptr && pane->name() == name) {
                return pane;
            }
        }
        return nullptr;
    };
    {
        ide::IdePane* search_pane = shown_pane("Search");
        ide::IdePane* view = shown_pane("Scene View");
        ide::IdePane* assets = shown_pane("Assets");
        ide::IdePane* game = shown_pane("Game Explorer");
        ide::IdePane* properties = shown_pane("Properties");
        expect(search_pane != nullptr && search_pane->getAbsoluteX() < 300, "Search is open on the left");
        expect(view != nullptr && view->getAbsoluteX() > 200 && view->getAbsoluteX() < 640,
               "the scene view is in the middle");
        expect(assets != nullptr && view != nullptr && std::abs(assets->getAbsoluteX() - view->getAbsoluteX()) < 1 &&
                   assets->getAbsoluteY() > view->getAbsoluteY() + view->getHeight() - 1,
               "Assets is under it, showing");
        expect(game != nullptr && game->getAbsoluteX() > 640, "the game explorer is on the right");
        expect(properties != nullptr && game != nullptr && properties->getWidth() > 100 &&
                   properties->getHeight() > 100 && std::abs(properties->getAbsoluteX() - game->getAbsoluteX()) < 1 &&
                   properties->getAbsoluteY() > game->getAbsoluteY() + game->getHeight() - 1,
               "Properties is under the game explorer, with room on screen");
        expect(shown_pane("Console") == nullptr, "the console is behind Assets");
        expect(shown_pane("Current Scene") == nullptr, "the scene explorer is closed");
    }
    // Picking a window that is behind another tab brings it forward.
    auto pick_window = [&bar](const std::string& text) {
        for (const std::shared_ptr<jadefx::Menu>& menu : bar != nullptr ? bar->getMenus().items()
                                                                         : std::vector<std::shared_ptr<jadefx::Menu>>{}) {
            for (const std::shared_ptr<jadefx::MenuItem>& item : menu->getItems().items()) {
                if (menu->getText() == "Window" && item && item->getText() == text) {
                    item->fire();
                }
            }
        }
    };
    pick_window("Console");
    scene->layout(1280, 800, 0.25);
    std::vector<ide::IdePane*> panes;
    for (jadefx::Node* node : scene->getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* pane = dynamic_cast<ide::IdePane*>(node)) {
            panes.push_back(pane);
        }
    }

    // Clear Output: from the log's right-click menu, and from Cmd+K (Ctrl+K elsewhere).
    ide::IdeConsole* console = nullptr;
    for (ide::IdePane* pane : panes) {
        if (auto* found = dynamic_cast<ide::IdeConsole*>(pane)) {
            console = found;
        }
    }
    expect(console != nullptr, "the console pane is an IdeConsole");
    if (console != nullptr) {
        engine_core::ScriptRuntime& scripts = layout.simulation().scripts();
        auto print = [&](const char* text) {
            scripts.append_output(engine_core::ScriptRuntime::OutputKind::Print, text);
            scene->layout(1280, 800, 0.3);
        };
        auto empty = [&] { return console->log().getText().empty(); };
        ide::ConsoleLog& log = console->log();
        const double x = log.getAbsoluteX() + log.getWidth() * 0.5;
        const double y = log.getAbsoluteY() + log.getHeight() * 0.5;

        print("one");
        expect(!empty(), "a print shows in the log");
        scene->noteButton(1, true, x, y);
        scene->noteButton(1, false, x, y);
        jadefx::Menu* menu = console->contextMenu();
        expect(menu != nullptr && menu->isShowing(), "a right-click on the log opens its menu");
        if (menu != nullptr && !menu->getItems().empty()) {
            jadefx::MenuItem& clear = *menu->getItems()[0];
            expect(menu->getItems().size() == 1 && clear.getText() == "Clear Output", "the menu offers Clear Output");
            expect(clear.getAcceleratorKey() == jadefx::Key::K && clear.getAcceleratorMods() == jadefx::Key::ModControl,
                   "Clear Output shows Cmd+K");
            clear.fire();
            menu->hide();
        }
        expect(empty(), "Clear Output empties the log");

        // Printed but not yet pulled into the log: the clear takes it too.
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Print, "waiting");
        console->clearOutput();
        scene->layout(1280, 800, 0.4);
        expect(empty(), "output printed before a clear does not come back");

        print("two");
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        expect(log.isFocused(), "a click focuses the log");
        escape();
        expect(log.isFocused(), "Escape leaves text fields, and the log is not one");
        scene->noteKey(jadefx::Key::K, true, false, jadefx::Key::ModControl);
        expect(empty(), "Cmd+K on the log clears it");

        print("three");
        scene->noteKey(jadefx::Key::K, true, false, 0);
        expect(!empty(), "K alone does not clear");
        scene->noteKey(jadefx::Key::K, true, false, jadefx::Key::ModControl | jadefx::Key::ModShift);
        expect(!empty(), "Cmd+Shift+K does not clear");
        const std::vector<jadefx::Node*> commands = console->getElementsByClassName("console-command");
        jadefx::Node* command = commands.empty() ? nullptr : commands.front();
        expect(command != nullptr, "the console has a command line");
        if (command != nullptr) {
            command->requestFocus();
            scene->noteKey(jadefx::Key::K, true, false, jadefx::Key::ModControl);
            expect(empty(), "Cmd+K on the command line clears the log");

            // Up and Down walk the submitted commands, and bring back the unsent line.
            auto& field = static_cast<jadefx::TextField&>(*command);
            auto submit = [&](const char* text) {
                field.setText(text);
                scene->noteKey(jadefx::Key::Enter, true, false, 0);
                scene->layout(1280, 800, 0.5);
            };
            auto key = [&](int code) { scene->noteKey(code, true, false, 0); };
            submit("local a = 1");
            submit("local b = 2");
            submit("local b = 2");
            expect(field.getText().empty(), "a submit empties the command line");
            field.setText("draft");
            key(jadefx::Key::Up);
            expect(field.getText() == "local b = 2", "Up shows the last command");
            expect(field.getCaretPosition() == field.getLength(), "the caret goes to the end of a recalled command");
            key(jadefx::Key::Up);
            expect(field.getText() == "local a = 1", "a repeated command is kept once");
            key(jadefx::Key::Up);
            expect(field.getText() == "local a = 1", "Up stops at the oldest command");
            key(jadefx::Key::Down);
            expect(field.getText() == "local b = 2", "Down steps to a newer command");
            key(jadefx::Key::Down);
            expect(field.getText() == "draft", "Down past the newest brings back the unsent line");
            key(jadefx::Key::Down);
            expect(field.getText() == "draft", "Down with nothing newer keeps the line");
            key(jadefx::Key::Up);
            submit("local a = 1");
            key(jadefx::Key::Up);
            expect(field.getText() == "local a = 1", "a rerun command is the newest entry");

            escape();
            expect(!field.isFocused(), "Escape leaves the command line");
            expect(field.getText() == "local a = 1", "Escape keeps the command line's text");

            // A command never runs inside layout. It runs at the start of the next
            // frame, once the frame showing the submitted line is on screen.
            command->requestFocus();
            field.setText("print(\"console ran it\")");
            scene->noteKey(jadefx::Key::Enter, true, false, 0);
            scene->layout(1280, 800, 0.45);
            scene->layout(1280, 800, 0.46);
            expect(console->log().getText().find("console ran it\n") == std::string::npos,
                   "layout does not run a submitted command");
            jadefx::drainRunLater();
            scene->layout(1280, 800, 0.47);
            expect(console->log().getText().find("console ran it\n") != std::string::npos,
                   "the next frame runs a submitted command");
        }
    }

    // Cmd+Shift+F (Ctrl+Shift+F elsewhere) docks Search beside the left explorer, with its field focused.
    scene->noteKey(jadefx::Key::F, true, false, jadefx::Key::ModControl | jadefx::Key::ModShift);
    scene->noteKey(jadefx::Key::F, false, false, 0);
    scene->layout(1280, 800, 0.6);
    scene->layout(1280, 800, 0.7);
    ide::IdeSearch* search = nullptr;
    for (jadefx::Node* node : scene->getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* found = dynamic_cast<ide::IdeSearch*>(node)) {
            search = found;
        }
    }
    expect(search != nullptr, "Cmd+Shift+F docks the Search pane");
    if (search != nullptr) {
        expect(search->getWidth() > 100 && search->getAbsoluteX() < 300, "Search sits on the left, with the explorer");
        expect(search->findInput().field().isFocused(), "the Search field takes the focus");
        expect(!search->replaceShown(), "Search opens with replace hidden");
        scene->noteKey(jadefx::Key::H, true, false, jadefx::Key::ModControl | jadefx::Key::ModShift);
        scene->noteKey(jadefx::Key::H, false, false, 0);
        expect(search->replaceShown(), "Cmd+Shift+H shows replace in the Search pane");

        search->findInput().field().requestFocus();
        search->setFindText("leave");
        escape();
        expect(!search->findInput().field().isFocused(), "Escape leaves the Search field");
        expect(search->findInput().text() == "leave", "Escape keeps the Search text");

        // Closing its tab and opening it again brings back the same search.
        search->setFindText("kept");
        jadefx::TabPane* tabs = nullptr;
        for (jadefx::Node* node = search->getParent(); node != nullptr && tabs == nullptr; node = node->getParent()) {
            tabs = dynamic_cast<jadefx::TabPane*>(node);
        }
        expect(tabs != nullptr, "Search is in a tab strip");
        if (tabs != nullptr) {
            const std::vector<std::shared_ptr<jadefx::Tab>> items = tabs->getTabs().items();
            for (const std::shared_ptr<jadefx::Tab>& tab : items) {
                if (tab && tab->getContent() == search) {
                    tabs->close(tab);
                }
            }
        }
        scene->layout(1280, 800, 0.8);
        scene->layout(1280, 800, 0.9);
        bool shown = false;
        for (jadefx::Node* node : scene->getRoot()->getElementsByClassName("ide-pane")) {
            shown = shown || node == search;
        }
        expect(!shown, "closing the tab takes Search out of the window");
        scene->noteKey(jadefx::Key::F, true, false, jadefx::Key::ModControl | jadefx::Key::ModShift);
        scene->noteKey(jadefx::Key::F, false, false, 0);
        scene->layout(1280, 800, 1.0);
        ide::IdeSearch* again = nullptr;
        for (jadefx::Node* node : scene->getRoot()->getElementsByClassName("ide-pane")) {
            if (auto* found = dynamic_cast<ide::IdeSearch*>(node)) {
                again = found;
            }
        }
        expect(again != nullptr && again->findInput().text() == "kept" && again->findInput().field().isFocused(),
               "Cmd+Shift+F after a close docks Search again with its search");
        expect(again != nullptr && !again->replaceShown(), "Search opens again with replace hidden");
    }

    // The Window menu lists the one-of-a-kind windows, then New Scene View under a separator.
    jadefx::Menu* windows = nullptr;
    if (bar != nullptr) {
        for (const std::shared_ptr<jadefx::Menu>& menu : bar->getMenus().items()) {
            if (menu && menu->getText() == "Window") {
                windows = menu.get();
            }
        }
    }
    expect(windows != nullptr, "the menu bar has a Window menu");
    if (windows != nullptr) {
        std::vector<std::string> labels;
        for (const std::shared_ptr<jadefx::MenuItem>& item : windows->getItems().items()) {
            labels.push_back(item ? item->getText() : std::string());
        }
        expect(labels == std::vector<std::string>{"Game Explorer", "Current Scene", "Properties", "Console", "Search", "Conflicts", "Problems", "Assets", "",
                                                  "New Scene View", "New Terminal", "Welcome Page", "", "Save Layout as Default", "Reset to Default Layout",
                                                  "Restore Built-in Default"},
               "Window lists the explorers, Properties, Console, Search, Conflicts, Problems, and Assets, then New Scene View, New Terminal, Welcome Page, and the default layout's items");
        double time = 1.1;
        auto frame = [&] {
            scene->layout(1280, 800, time);
            time += 0.01;
        };
        auto pick = [&](const std::string& text) {
            for (const std::shared_ptr<jadefx::MenuItem>& item : windows->getItems().items()) {
                if (item && item->getText() == text) {
                    item->fire();
                }
            }
            frame();
        };
        // Whether the row shows its check. Opening the menu lays its rows out, which works it out.
        auto checked = [&](const std::string& text) {
            windows->show(*scene, 10, 10);
            frame();
            bool on = false;
            for (const std::shared_ptr<jadefx::MenuItem>& item : windows->getItems().items()) {
                if (item && item->getText() == text && item->getGraphic()) {
                    for (jadefx::Node* check : item->getGraphic()->getElementsByClassName("ide-window-check")) {
                        on = on || check->isVisible();
                    }
                }
            }
            windows->hide();
            frame();
            return on;
        };
        // The page by that name the window shows now. A tab behind another is not shown.
        auto showing = [&](const std::string& name) -> ide::IdePane* {
            for (jadefx::Node* node : scene->getRoot()->getElementsByClassName("ide-pane")) {
                auto* pane = dynamic_cast<ide::IdePane*>(node);
                if (pane != nullptr && pane->name() == name) {
                    return pane;
                }
            }
            return nullptr;
        };

        // From the built-in layout, where the console is behind Assets.
        pick("Reset to Default Layout");
        expect(checked("Game Explorer") && checked("Properties") && checked("Console") && checked("Search") &&
                   checked("Assets"),
               "every window the built-in layout has open has a check");
        expect(!checked("Current Scene"), "the scene explorer starts closed, with no check");
        expect(!checked("New Scene View"), "New Scene View has no check");
        expect(showing("Console") == nullptr && showing("Assets") != nullptr, "the console is behind Assets");
        pick("Console");
        ide::IdePane* console_page = showing("Console");
        ide::IdeDock* console_home = nullptr;
        for (jadefx::Node* node = console_page; node != nullptr && console_home == nullptr; node = node->getParent()) {
            console_home = dynamic_cast<ide::IdeDock*>(node);
        }
        expect(console_page != nullptr && showing("Assets") == nullptr, "picking a window behind a tab brings it forward");
        expect(checked("Console"), "and it keeps its check");
        pick("Console");
        expect(showing("Console") == nullptr && !checked("Console"), "picking a showing window closes it");
        expect(showing("Assets") != nullptr, "and the tab beside it shows");
        pick("Console");
        ide::IdePane* reopened = showing("Console");
        expect(reopened != nullptr && checked("Console"), "picking a closed window opens it");
        expect(reopened == console_page, "it opens as the same page it was");
        ide::IdeDock* reopened_home = nullptr;
        for (jadefx::Node* node = reopened; node != nullptr && reopened_home == nullptr; node = node->getParent()) {
            reopened_home = dynamic_cast<ide::IdeDock*>(node);
        }
        expect(reopened_home != nullptr && reopened_home == console_home, "in the dock it closed from");
        pick("Assets");

        pick("Properties");
        expect(showing("Properties") == nullptr && !checked("Properties"), "Properties closes from the menu");
        pick("Properties");
        ide::IdePane* properties = showing("Properties");
        expect(properties != nullptr && checked("Properties"), "and opens again");
        expect(properties != nullptr && properties->getAbsoluteX() > 640, "on the right, where it was");

        expect(showing("Search") != nullptr, "Search shows on the left");
        pick("Search");
        expect(showing("Search") == nullptr && !checked("Search"), "Search closes from the menu");
        pick("Search");
        auto* search_again = dynamic_cast<ide::IdeSearch*>(showing("Search"));
        expect(search_again != nullptr && checked("Search") && search_again->findInput().text() == "kept",
               "Search opens again from the menu with its search");

        // Each pick makes another view, which closes like any tab.
        pick("New Scene View");
        ide::IdePane* view = showing("Scene View 2");
        expect(view != nullptr && view->closable(), "New Scene View docks a closable Scene View 2");
        pick("New Scene View");
        expect(showing("Scene View 3") != nullptr, "the next one is Scene View 3");
        jadefx::TabPane* strip = nullptr;
        for (jadefx::Node* node = showing("Scene View 3"); node != nullptr && strip == nullptr; node = node->getParent()) {
            strip = dynamic_cast<jadefx::TabPane*>(node);
        }
        expect(strip != nullptr, "the new views share a tab strip");
        if (strip != nullptr) {
            const std::vector<std::shared_ptr<jadefx::Tab>> tabs = strip->getTabs().items();
            for (const std::shared_ptr<jadefx::Tab>& tab : tabs) {
                if (tab && (tab->getText() == "Scene View 2" || tab->getText() == "Scene View 3")) {
                    strip->close(tab);
                }
            }
        }
        frame();
        expect(showing("Scene View 2") == nullptr && showing("Scene View 3") == nullptr,
               "a new view closes like any tab");

        // Each pick docks another terminal in with the console. Fired without a
        // frame, so no shell starts, and closed before the next one.
        {
            ide::IdePane* assets = showing("Assets");
            ide::IdeDock* console_dock = nullptr;
            for (jadefx::Node* node = assets; node != nullptr && console_dock == nullptr;
                 node = node->getParent()) {
                console_dock = dynamic_cast<ide::IdeDock*>(node);
            }
            auto fire = [&](const std::string& text) {
                for (const std::shared_ptr<jadefx::MenuItem>& item : windows->getItems().items()) {
                    if (item && item->getText() == text) {
                        item->fire();
                    }
                }
            };
            auto terminals = [&] {
                std::vector<std::shared_ptr<jadefx::Tab>> found;
                if (console_dock != nullptr) {
                    for (const std::shared_ptr<jadefx::Tab>& tab : console_dock->tabs()->getTabs().items()) {
                        if (tab && dynamic_cast<ide::IdeTerminal*>(tab->getContent()) != nullptr) {
                            found.push_back(tab);
                        }
                    }
                }
                return found;
            };
            expect(console_dock != nullptr && terminals().empty(), "the studio starts with no terminal");
            fire("New Terminal");
            fire("New Terminal");
            const std::vector<std::shared_ptr<jadefx::Tab>> made = terminals();
            expect(made.size() == 2 && made[0]->getContent() != made[1]->getContent(),
                   "New Terminal docks a new terminal each time, in with the console");
            expect(made.size() == 2 && made[1]->isSelected(), "the newest one is in front");
            expect(made.size() == 2 && made[0]->isClosable(), "a terminal closes like any tab");
            for (const std::shared_ptr<jadefx::Tab>& tab : made) {
                if (tab->getTabPane() != nullptr) {
                    tab->getTabPane()->close(tab);
                }
            }
            expect(terminals().empty(), "closing their tabs takes them away");
            if (console_dock != nullptr) {
                console_dock->select(assets);
            }
            frame();
        }

        // Reset to Default Layout puts the windows back as a new studio has
        // them. Close Properties, move the console in with the game
        // explorer, and open another view first.
        auto dock_of = [](jadefx::Node* node) -> ide::IdeDock* {
            for (; node != nullptr; node = node->getParent()) {
                if (auto* dock = dynamic_cast<ide::IdeDock*>(node)) {
                    return dock;
                }
            }
            return nullptr;
        };
        pick("Properties");
        ide::IdeDock* assets_dock = dock_of(showing("Assets"));
        ide::IdeDock* game_dock = dock_of(showing("Game Explorer"));
        std::shared_ptr<jadefx::Tab> console_tab;
        if (assets_dock != nullptr) {
            for (const std::shared_ptr<jadefx::Tab>& tab : assets_dock->tabs()->getTabs().items()) {
                if (tab && tab->getText() == "Console") {
                    console_tab = tab;
                }
            }
        }
        if (console_tab && game_dock != nullptr) {
            game_dock->take(console_tab);
        }
        pick("New Scene View");
        expect(showing("Properties") == nullptr && showing("Scene View 4") != nullptr &&
                   dock_of(showing("Console")) == game_dock,
               "the layout is rearranged before the reset");
        pick("Reset to Default Layout");
        frame();
        ide::IdePane* left = showing("Search");
        ide::IdePane* middle = showing("Scene View");
        ide::IdePane* below = showing("Assets");
        ide::IdePane* right = showing("Game Explorer");
        ide::IdePane* under = showing("Properties");
        expect(left != nullptr && left->getAbsoluteX() < 300, "the reset puts Search on the left");
        expect(middle != nullptr && middle->getAbsoluteX() > 200 && middle->getAbsoluteX() < 640,
               "the scene view in the middle, showing");
        expect(below != nullptr && middle != nullptr && std::abs(below->getAbsoluteX() - middle->getAbsoluteX()) < 1 &&
                   below->getAbsoluteY() > middle->getAbsoluteY() + middle->getHeight() - 1,
               "Assets under it, with the console behind");
        expect(right != nullptr && under != nullptr && right->getAbsoluteX() > 640 &&
                   std::abs(under->getAbsoluteX() - right->getAbsoluteX()) < 1 &&
                   under->getAbsoluteY() > right->getAbsoluteY() + right->getHeight() - 1,
               "and the game explorer over Properties on the right");
        expect(checked("Game Explorer") && checked("Properties") && checked("Console") && checked("Search") &&
                   checked("Assets"),
               "the built-in layout's windows are open");
        expect(!checked("Current Scene"), "the scene explorer is closed, as in a new studio");
        jadefx::TabPane* view_strip = nullptr;
        for (jadefx::Node* node = middle; node != nullptr && view_strip == nullptr; node = node->getParent()) {
            view_strip = dynamic_cast<jadefx::TabPane*>(node);
        }
        std::shared_ptr<jadefx::Tab> extra;
        if (view_strip != nullptr) {
            for (const std::shared_ptr<jadefx::Tab>& tab : view_strip->getTabs().items()) {
                if (tab && tab->getText() == "Scene View 4") {
                    extra = tab;
                }
            }
        }
        expect(extra != nullptr, "an extra scene view stays open, beside the first");
        if (extra != nullptr) {
            view_strip->close(extra);
        }
        frame();

        // A Prefab dragged from Assets onto the Scene View is added as a
        // GameObject; a Material is not.
        {
            engine_core::DataModel& world = layout.simulation().datamodel();
            engine_core::InstanceId crate = 0;
            engine_core::InstanceId stone = 0;
            layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                engine_core::DataModel* prefab = engine_core::lua_create_instance(game, "Prefab");
                game.set_name(prefab->id(), "Crate");
                game.set_parent(prefab->id(), game.service("Prefabs"));
                crate = prefab->id();
                engine_core::DataModel* material = engine_core::lua_create_instance(game, "Material");
                game.set_name(material->id(), "Stone");
                game.set_parent(material->id(), game.service("Materials"));
                stone = material->id();
            });
            const engine_core::InstanceId workspace = world.scene_service("Workspace");
            const std::vector<engine_core::InstanceId> before = world.get_children(workspace);
            auto* assets = dynamic_cast<ide::IdeAssets*>(showing("Assets"));
            ide::IdePane* view = showing("Scene View");
            expect(assets != nullptr && view != nullptr, "Assets and the Scene View both show");
            // The pointer over the Scene View just before the drop.
            jadefx::Cursor over = jadefx::Cursor::Default;
            auto drag = [&](engine_core::InstanceId id) {
                frame();
                jadefx::Node* from = assets != nullptr ? assets->itemNode(id) : nullptr;
                expect(from != nullptr && view != nullptr, "the dragged asset has a tile");
                if (from == nullptr || view == nullptr) {
                    return;
                }
                const double x = from->getAbsoluteX() + from->getWidth() * 0.5;
                const double y = from->getAbsoluteY() + from->getHeight() * 0.5;
                const double to_x = view->getAbsoluteX() + view->getWidth() * 0.5;
                const double to_y = view->getAbsoluteY() + view->getHeight() * 0.5;
                scene->noteButton(0, true, x, y);
                scene->noteMove(x + 10, y + 10);
                scene->noteMove(to_x, to_y);
                frame();
                scene->noteMove(to_x, to_y);
                over = scene->hoverCursor();
                scene->noteButton(0, false, to_x, to_y);
                frame();
            };
            if (assets != nullptr) {
                assets->openFolder(world.service("Materials"));
            }
            drag(stone);
            expect(over == jadefx::Cursor::NotAllowed, "a Material over the Scene View shows it cannot drop");
            expect(world.get_children(workspace) == before, "a Material dropped on the Scene View adds nothing");
            if (assets != nullptr) {
                assets->openFolder(world.service("Prefabs"));
            }
            drag(crate);
            expect(over == jadefx::Cursor::Copy, "a Prefab over the Scene View shows the plus");
            std::vector<engine_core::InstanceId> added;
            for (engine_core::InstanceId id : world.get_children(workspace)) {
                if (std::find(before.begin(), before.end(), id) == before.end()) {
                    added.push_back(id);
                }
            }
            const auto* object =
                added.size() == 1 ? dynamic_cast<const engine_core::GameObject*>(world.instance(added[0])) : nullptr;
            expect(object != nullptr && world.name(added[0]) == "Crate" && object->prefab().id == crate,
                   "a Prefab dropped on the Scene View adds a GameObject linked to it, named after it");
            expect(world.get_children(world.service("Prefabs")).size() == 1 && world.parent(crate) == world.service("Prefabs"),
                   "the Prefab stays in Prefabs");
            expect(added.size() == 1 && world.selection().get() == added, "the new GameObject is selected");
            layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                for (engine_core::InstanceId id : added) {
                    game.destroy_tree(id);
                }
                game.destroy_tree(crate);
                game.destroy_tree(stone);
            });
            frame();
        }

        // A click selects the Scene View. Escape goes to the game and leaves it
        // selected; only Shift+Esc deselects it, which frees a locked pointer.
        {
            auto* view = dynamic_cast<runner::GameView*>(showing("Scene View"));
            expect(view != nullptr, "the Scene View shows");
            if (view != nullptr) {
                frame();
                const double x = view->getAbsoluteX() + view->getWidth() * 0.5;
                const double y = view->getAbsoluteY() + view->getHeight() * 0.5;
                scene->noteButton(0, true, x, y);
                scene->noteButton(0, false, x, y);
                frame();
                expect(view->isFocused(), "a click selects the Scene View");
                scene->noteKey(jadefx::Key::Escape, true, false, 0);
                scene->noteKey(jadefx::Key::Escape, false, false, 0);
                frame();
                expect(view->isFocused(), "Escape leaves the Scene View selected");
                scene->noteKey(jadefx::Key::Escape, true, false, jadefx::Key::ModShift);
                scene->noteKey(jadefx::Key::Escape, false, false, jadefx::Key::ModShift);
                frame();
                expect(!view->isFocused(), "Shift+Esc deselects the Scene View");
                scene->noteButton(0, true, x, y);
                scene->noteButton(0, false, x, y);
                frame();
                expect(view->isFocused(), "a click selects the Scene View again");
            }
        }

        // The Scene View follows a Camera in Workspace, which its list at the top right offers.
        {
            auto* view = dynamic_cast<runner::GameView*>(showing("Scene View"));
            expect(view != nullptr, "the Scene View shows");
            if (view != nullptr) {
                engine_core::DataModel& world = layout.simulation().datamodel();
                jadefx::ComboBox& list = view->cameraList();
                expect(list.getItems().size() == 0 && list.getSelectionIndex() == -1,
                       "with no Camera the list is empty and shows its prompt");
                expect(list.getAbsoluteX() + list.getWidth() > view->getAbsoluteX() + view->getWidth() - 20 &&
                           list.getAbsoluteY() < view->getAbsoluteY() + 20,
                       "the list is in the view's top right corner");
                auto add_camera = [&](const char* name, engine_core::InstanceId parent) {
                    engine_core::InstanceId id = 0;
                    layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                        engine_core::DataModel* camera = engine_core::lua_create_instance(game, "Camera");
                        game.set_name(camera->id(), name);
                        game.set_parent(camera->id(), parent);
                        id = camera->id();
                    });
                    return id;
                };
                const engine_core::InstanceId workspace = world.scene_service("Workspace");
                const engine_core::InstanceId main = add_camera("Main", workspace);
                const std::string main_guid = world.guid(main);
                frame();
                expect(view->cameraGuid() == main_guid, "a view with no Camera takes the first one in Workspace");
                auto current_camera = [&] {
                    auto* service = dynamic_cast<engine_core::Workspace*>(world.instance(workspace));
                    return service != nullptr ? service->current_camera() : engine_core::InstanceId{0};
                };
                expect(current_camera() == main, "and, with none set, makes it the CurrentCamera without a click");
                expect(list.getItems().size() == 1 && list.getItems()[0] == "Main" && list.getSelectionIndex() == 0,
                       "the list offers it, chosen");

                engine_core::InstanceId folder = 0;
                layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                    engine_core::DataModel* made = engine_core::lua_create_instance(game, "Folder");
                    game.set_parent(made->id(), workspace);
                    folder = made->id();
                });
                const engine_core::InstanceId overhead = add_camera("Overhead", folder);
                const engine_core::InstanceId stored = add_camera("Stored", world.scene_service("Storage"));
                frame();
                expect(list.getItems().size() == 2 && list.getItems()[1] == "Overhead",
                       "Cameras deeper in Workspace are offered too, in tree order, and none from elsewhere");
                expect(view->cameraGuid() == main_guid && list.getSelectionIndex() == 0,
                       "another Camera does not take the link");
                view->linkCamera(world.guid(overhead));
                frame();
                expect(list.getSelectionIndex() == 1, "linking another Camera chooses it in the list");

                layout.simulation().on_simulation([&](engine_core::DataModel& game) { game.set_parent(overhead, engine_core::DataModel::kNoParent); });
                frame();
                expect(view->cameraGuid() == world.guid(overhead) && list.getItems().size() == 1 &&
                           list.getSelectionIndex() == -1,
                       "a Camera that leaves Workspace stays linked, and the list shows its prompt");
                layout.simulation().on_simulation([&](engine_core::DataModel& game) { game.set_parent(overhead, folder); });
                frame();
                expect(list.getSelectionIndex() == 1, "back in Workspace, the view follows it again");

                layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                    game.destroy_tree(folder);
                    game.destroy_tree(stored);
                });
                frame();
                expect(view->cameraGuid() != main_guid && list.getItems().size() == 1 && list.getSelectionIndex() == -1,
                       "a destroyed Camera stays linked, and no other takes its place");

                layout.simulation().on_simulation([&](engine_core::DataModel& game) { engine_core::Project::reset_place(game); });
                frame();
                std::vector<engine_core::InstanceId> fresh = world.get_children(workspace);
                expect(fresh.size() == 1 && view->cameraGuid() == world.guid(fresh[0]) &&
                           list.getSelectionIndex() == 0 && list.getItems()[0] == "Camera",
                       "a new place links the view to its Camera");
                expect(fresh.size() == 1 && current_camera() == fresh[0],
                       "and makes it the CurrentCamera, as loading a place does");
                layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                    for (engine_core::InstanceId id : game.get_children(workspace)) {
                        game.destroy_tree(id);
                    }
                });
                frame();
            }
        }

        // A double-click on a Prefab in Assets docks a Prefab editor with the
        // scene view, one per Prefab: editing one again brings its tab forward.
        {
            engine_core::DataModel& world = layout.simulation().datamodel();
            engine_core::InstanceId crate = 0;
            engine_core::InstanceId barrel = 0;
            layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                for (auto [name, id] : {std::pair{"Crate", &crate}, std::pair{"Barrel", &barrel}}) {
                    engine_core::DataModel* prefab = engine_core::lua_create_instance(game, "Prefab");
                    game.set_name(prefab->id(), name);
                    game.set_parent(prefab->id(), game.service("Prefabs"));
                    *id = prefab->id();
                }
            });
            auto* assets = dynamic_cast<ide::IdeAssets*>(showing("Assets"));
            ide::IdeDock* strip = dock_of(showing("Scene View"));
            expect(assets != nullptr && strip != nullptr, "Assets and the Scene View both show");
            if (assets != nullptr) {
                assets->openFolder(world.service("Prefabs"));
            }
            auto double_click = [&](engine_core::InstanceId id) {
                frame();
                jadefx::Node* tile = assets != nullptr ? assets->itemNode(id) : nullptr;
                expect(tile != nullptr, "the Prefab has a tile");
                if (tile == nullptr) {
                    return;
                }
                const double x = tile->getAbsoluteX() + tile->getWidth() * 0.5;
                const double y = tile->getAbsoluteY() + tile->getHeight() * 0.5;
                for (int click = 0; click < 2; ++click) {
                    scene->noteButton(0, true, x, y);
                    scene->noteButton(0, false, x, y);
                }
                frame();
            };
            auto editors = [&] {
                std::vector<std::shared_ptr<jadefx::Tab>> found;
                if (strip != nullptr) {
                    for (const std::shared_ptr<jadefx::Tab>& tab : strip->tabs()->getTabs().items()) {
                        if (tab && dynamic_cast<ide::IdePrefabEditor*>(tab->getContent()) != nullptr) {
                            found.push_back(tab);
                        }
                    }
                }
                return found;
            };
            auto editing = [](const std::shared_ptr<jadefx::Tab>& tab) {
                return static_cast<ide::IdePrefabEditor*>(tab->getContent())->prefab();
            };
            expect(editors().empty(), "no Prefab editor is open at first");
            double_click(crate);
            std::vector<std::shared_ptr<jadefx::Tab>> open = editors();
            expect(open.size() == 1 && editing(open[0]) == crate && open[0]->isSelected(),
                   "a double-click on a Prefab docks its editor in front");
            expect(open.size() == 1 && open[0]->isClosable(), "a Prefab editor closes like any tab");
            expect(open.size() == 1 && static_cast<ide::IdePane*>(open[0]->getContent())->title() == "Crate",
                   "its tab shows the Prefab's name");
            double_click(barrel);
            open = editors();
            expect(open.size() == 2 && editing(open[1]) == barrel && open[1]->isSelected(),
                   "another Prefab gets its own editor, in front");
            double_click(crate);
            open = editors();
            expect(open.size() == 2 && open[0]->isSelected() && !open[1]->isSelected(),
                   "editing a Prefab that has an editor brings that one forward");
            layout.simulation().on_simulation([&](engine_core::DataModel& game) { game.set_name(crate, "Box"); });
            frame();
            expect(open.size() == 2 && static_cast<ide::IdePane*>(open[0]->getContent())->title() == "Box",
                   "the tab follows a rename");
            for (const std::shared_ptr<jadefx::Tab>& tab : open) {
                if (tab->getTabPane() != nullptr) {
                    tab->getTabPane()->close(tab);
                }
            }
            expect(editors().empty(), "closing their tabs takes them away");
            // Barrel: presses on Crate's spot this soon would count on from the last double-click.
            double_click(barrel);
            expect(editors().size() == 1 && editing(editors()[0]) == barrel,
                   "a Prefab whose editor was closed opens a new one");
            for (const std::shared_ptr<jadefx::Tab>& tab : editors()) {
                tab->getTabPane()->close(tab);
            }
            layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                game.destroy_tree(crate);
                game.destroy_tree(barrel);
            });
            frame();
        }

        // A Terrain's Edit, its primary action, docks the Configure Terrain tab
        // with the scene view. Editing it again brings that tab forward.
        {
            engine_core::DataModel& world = layout.simulation().datamodel();
            engine_core::InstanceId island = 0;
            layout.simulation().on_simulation([&](engine_core::DataModel& game) {
                engine_core::DataModel* made = engine_core::lua_create_instance(game, "Terrain");
                game.set_name(made->id(), "Island");
                game.set_parent(made->id(), game.scene_service("Workspace"));
                island = made->id();
            });
            std::vector<engine_core::ContextAction> actions;
            world.instance(island)->context_actions(actions);
            expect(!actions.empty() && actions[0].action == engine_core::InstanceAction::Edit && actions[0].primary,
                   "a Terrain's primary action is Edit");
            ide::IdePane* scene_view = showing("Scene View");
            ide::IdeDock* home_strip = dock_of(scene_view);
            expect(home_strip != nullptr, "the Scene View shows");
            auto edit = [&] {
                world.selection().set({island});
                frame();
                frame();
                bool ran = false;
                for (jadefx::Node* node : scene->getElementsByClassName("explorer-pane")) {
                    if (auto* explorer = dynamic_cast<ide::IdeExplorer*>(node); explorer != nullptr && !ran) {
                        ran = explorer->run_on_selection(engine_core::InstanceAction::Edit);
                    }
                }
                expect(ran, "an explorer runs Edit on the selected Terrain");
                frame();
            };
            auto editors = [&] {
                std::vector<std::shared_ptr<jadefx::Tab>> found;
                if (home_strip != nullptr) {
                    for (const std::shared_ptr<jadefx::Tab>& tab : home_strip->tabs()->getTabs().items()) {
                        if (tab && dynamic_cast<ide::IdeTerrainEditor*>(tab->getContent()) != nullptr) {
                            found.push_back(tab);
                        }
                    }
                }
                return found;
            };
            expect(editors().empty(), "no Configure Terrain tab is open at first");
            edit();
            std::vector<std::shared_ptr<jadefx::Tab>> open = editors();
            expect(open.size() == 1 &&
                       static_cast<ide::IdeTerrainEditor*>(open[0]->getContent())->terrain() == island &&
                       open[0]->isSelected(),
                   "Edit on a Terrain docks its Configure Terrain tab in front");
            expect(open.size() == 1 && open[0]->isClosable(), "it closes like any tab");
            // Something else in front, then Edit again.
            if (home_strip != nullptr) {
                for (const std::shared_ptr<jadefx::Tab>& tab : home_strip->tabs()->getTabs().items()) {
                    if (tab && tab->getContent() == scene_view) {
                        home_strip->tabs()->select(tab);
                    }
                }
            }
            frame();
            edit();
            std::vector<std::shared_ptr<jadefx::Tab>> again = editors();
            expect(again.size() == 1 && open.size() == 1 && again[0] == open[0] && again[0]->isSelected(),
                   "editing it again brings the same tab forward, not a second one");
            for (const std::shared_ptr<jadefx::Tab>& tab : editors()) {
                tab->getTabPane()->close(tab);
            }
            expect(editors().empty(), "closing its tab takes it away");
            layout.simulation().on_simulation([&](engine_core::DataModel& game) { game.destroy_tree(island); });
            frame();
        }
    }

    failures += RunThemeTests(*scene);
    failures += RunFindReplaceTests(layout.simulation());
    failures += RunPreferencesTests();

    // Opening a project says so in a toast, and not in the console. Last, since it replaces the place.
    {
        namespace fs = std::filesystem;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path folder = fs::temp_directory_path() / ("anarchy-toast-test-" + std::to_string(stamp));
        const fs::path root = folder / "ToastPlace";
        layout.simulation().on_simulation([&](engine_core::DataModel&) { engine_core::Project::create(root); });
        engine_core::ScriptRuntime& scripts = layout.simulation().scripts();
        const std::uint64_t before = scripts.output_next();
        // AnarchyStudio <folder> opens the project this way, over a Welcome page shown at startup.
        layout.open_landing();
        scene->layout(1280, 800, 1.95);
        expect(shown_pane("Welcome") != nullptr, "the Welcome page is in front before the project opens");
        layout.open_project_at(root);
        scene->layout(1280, 800, 2.0);
        scene->layout(1280, 800, 2.05);
        expect(shown_pane("Welcome") == nullptr && shown_pane("Scene View") != nullptr,
               "opening a project closes the Welcome page and shows the scene view");
        jadefx::Label* toast = nullptr;
        for (jadefx::Node* node : scene->getElementsByClassName("toast")) {
            auto* label = dynamic_cast<jadefx::Label*>(node);
            if (label != nullptr && label->getText() == "Opened ToastPlace" && scene->isPopupShowing(label)) {
                toast = label;
            }
        }
        expect(toast != nullptr, "opening a project shows a toast naming it");
        expect(toast != nullptr && toast->getWidth() > 0 &&
                   std::abs(toast->getAbsoluteX() + toast->getWidth() - (1280 - 16)) < 1,
               "the toast sits at the right of the window");
        bool logged = false;
        for (const auto& line : scripts.output_since(before, 100).lines) {
            logged = logged || line.text.find("Opened") != std::string::npos;
        }
        expect(!logged, "and the console does not say it");

        // During a test, a project that stops reading says so once, instead of
        // failing quietly on every check.
        auto key = [&](int code, int mods) {
            scene->noteKey(code, true, false, mods);
            scene->noteKey(code, false, false, 0);
        };
        key(jadefx::Key::F5, 0);
        std::string saved_json;
        {
            std::ifstream in(root / "project.json", std::ios::binary);
            saved_json.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        {
            std::ofstream out(root / "project.json", std::ios::binary | std::ios::trunc);
            out << "<<<<<<< HEAD\n{";
        }
        layout.check_disk();
        layout.check_disk();
        scene->layout(1280, 800, 2.1);
        int told = 0;
        for (jadefx::Node* node : scene->getElementsByClassName("toast")) {
            auto* label = dynamic_cast<jadefx::Label*>(node);
            if (label != nullptr && label->getText().find("Can't read the project on disk") == 0) {
                ++told;
            }
        }
        expect(told == 1, "a project that stops reading during a test is reported once");
        {
            std::ofstream out(root / "project.json", std::ios::binary | std::ios::trunc);
            out << saved_json;
        }
        key(jadefx::Key::F5, jadefx::Key::ModShift);
        std::error_code error;
        fs::remove_all(folder, error);
    }
    failures += RunSaveConflictTests(layout, *scene);
    failures += RunTextureImportTests(layout, *scene);
    failures += RunModelImportTests(layout, *scene);
    failures += RunScratchResourcesTests(layout, *scene);
    failures += RunConflictsTests(layout, *scene);
    failures += RunProblemsStartupTests();
    failures += RunProblemsPaneTests(layout.simulation());
    failures += RunProblemsWindowTests(layout, *scene);
    // Conflicts opened beside the game explorer, the built-in layout's only
    // one, and is in front of it. Edit needs an explorer showing.
    pick_window("Game Explorer");
    scene->layout(1280, 800, 5.9);
    failures += RunScriptTabTests(layout, *scene);
    failures += RunLandingPageTests(layout, *scene);
    failures += RunTerminalPaneTests();
    failures += RunGuiStyleTests(layout, *scene);
    failures += RunGuiImageTests(layout, *scene);
    failures += RunBillboardLayerTests(layout, *scene);
    failures += RunStatusBarTests(layout, *scene);
    failures += RunProfilerOverlayTests(layout, *scene);
    failures += RunProfilerPlayerKeyTests();
    failures += RunProfilerColorTests();
    failures += RunUiFrameProfileTests();

    // The layout is kept in layout.json in the config folder, and the next
    // studio docks the windows that way again.
    {
        namespace fs = std::filesystem;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path config = fs::temp_directory_path() / ("anarchy-layout-test-" + std::to_string(stamp));
        double time = 3.0;
        auto frame = [&time](jadefx::Scene& at) {
            at.layout(1280, 800, time);
            at.layout(1280, 800, time + 0.01);
            time += 0.02;
        };
        auto showing = [](jadefx::Scene& at, const std::string& name) -> ide::IdePane* {
            for (jadefx::Node* node : at.getRoot()->getElementsByClassName("ide-pane")) {
                auto* pane = dynamic_cast<ide::IdePane*>(node);
                if (pane != nullptr && pane->name() == name) {
                    return pane;
                }
            }
            return nullptr;
        };
        auto window_item = [](jadefx::Scene& at, const std::string& text) -> jadefx::MenuItem* {
            auto* root = dynamic_cast<jadefx::BorderPane*>(at.getRoot());
            auto* top = root != nullptr ? dynamic_cast<jadefx::VBox*>(root->getTop()) : nullptr;
            auto* menus = top != nullptr ? dynamic_cast<jadefx::MenuBar*>(top->getChildren()[0].get()) : nullptr;
            for (const std::shared_ptr<jadefx::Menu>& menu : menus != nullptr ? menus->getMenus().items()
                                                                               : std::vector<std::shared_ptr<jadefx::Menu>>{}) {
                for (const std::shared_ptr<jadefx::MenuItem>& item : menu->getItems().items()) {
                    if (menu->getText() == "Window" && item && item->getText() == text) {
                        return item.get();
                    }
                }
            }
            return nullptr;
        };
        auto dock_of = [](jadefx::Node* node) -> ide::IdeDock* {
            for (; node != nullptr; node = node->getParent()) {
                if (auto* dock = dynamic_cast<ide::IdeDock*>(node)) {
                    return dock;
                }
            }
            return nullptr;
        };
        double left_width = 0;
        {
            ide::IdeLayout first(1280, 800, config);
            auto at = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
            first.mount(*at);
            frame(*at);
            // Close Properties, open Conflicts beside the game explorer, move
            // the console in with them, and widen the left column.
            if (jadefx::MenuItem* item = window_item(*at, "Properties")) {
                item->fire();
            }
            if (jadefx::MenuItem* item = window_item(*at, "Conflicts")) {
                item->fire();
            }
            frame(*at);
            ide::IdeDock* assets_dock = dock_of(showing(*at, "Assets"));
            ide::IdeDock* east = dock_of(showing(*at, "Conflicts"));
            std::shared_ptr<jadefx::Tab> console_tab;
            if (assets_dock != nullptr) {
                for (const std::shared_ptr<jadefx::Tab>& tab : assets_dock->tabs()->getTabs().items()) {
                    if (tab && tab->getText() == "Console") {
                        console_tab = tab;
                    }
                }
            }
            expect(console_tab != nullptr && east != nullptr && east != assets_dock,
                   "the console is behind Assets, and Conflicts is docked elsewhere");
            if (console_tab != nullptr && east != nullptr) {
                east->take(console_tab);
            }
            jadefx::SplitPane* columns = nullptr;
            for (jadefx::Node* node = dock_of(showing(*at, "Search")); node != nullptr && columns == nullptr;
                 node = node->getParent()) {
                columns = dynamic_cast<jadefx::SplitPane*>(node);
            }
            if (columns != nullptr) {
                columns->setDividerPosition(0, 0.3);
            }
            frame(*at);
            if (ide::IdePane* shown = showing(*at, "Search")) {
                left_width = shown->getWidth();
            }
            expect(left_width > 300, "the left column is wider");
            first.save_layout();
        }
        expect(fs::exists(config / "layout.json"), "save_layout writes layout.json in the config folder");
        {
            ide::IdeLayout second(1280, 800, config);
            auto at = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
            second.mount(*at);
            frame(*at);
            expect(showing(*at, "Properties") == nullptr, "a closed window stays closed");
            ide::IdePane* search_pane = showing(*at, "Search");
            expect(search_pane != nullptr && search_pane->getAbsoluteX() < 300, "Search opens where it was, showing");
            expect(search_pane != nullptr && std::abs(search_pane->getWidth() - left_width) < 2,
                   "the left column keeps its width");
            ide::IdePane* console_pane = showing(*at, "Console");
            expect(console_pane != nullptr && console_pane->getAbsoluteX() > 640,
                   "the console opens with the game explorer, in front");
            bool conflicts_kept = false;
            if (ide::IdeDock* right = dock_of(console_pane)) {
                for (const std::shared_ptr<jadefx::Tab>& tab : right->tabs()->getTabs().items()) {
                    auto* pane = tab ? dynamic_cast<ide::IdePane*>(tab->getContent()) : nullptr;
                    conflicts_kept = conflicts_kept || (pane != nullptr && pane->name() == "Conflicts");
                }
            }
            expect(conflicts_kept, "Conflicts opens where it was, beside the console");
            expect(showing(*at, "Game Explorer") == nullptr, "the game explorer is behind them, as it was");
            ide::IdePane* view = showing(*at, "Scene View");
            ide::IdePane* assets = showing(*at, "Assets");
            expect(view != nullptr && assets != nullptr && assets->getAbsoluteY() > view->getAbsoluteY(),
                   "Assets stays under the scene view");
            // Opened again from the menu, Properties goes where the default layout has it.
            if (jadefx::MenuItem* item = window_item(*at, "Properties")) {
                item->fire();
            }
            frame(*at);
            ide::IdePane* properties = showing(*at, "Properties");
            expect(properties != nullptr && properties->getAbsoluteX() > 640, "Properties opens on the right");
        }
        {
            std::string error;
            ide::write_file(config / "layout.json", "{ not json", error);
            ide::IdeLayout third(1280, 800, config);
            auto at = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
            third.mount(*at);
            frame(*at);
            expect(showing(*at, "Properties") != nullptr && showing(*at, "Assets") != nullptr &&
                       showing(*at, "Game Explorer") != nullptr && showing(*at, "Search") != nullptr,
                   "a layout.json that cannot be read starts the default layout");
            bool said = false;
            for (const auto& line : third.simulation().scripts().output_since(0, 1000).lines) {
                said = said || line.text.rfind("Layout: ", 0) == 0;
            }
            expect(said, "and the console says why");
        }

        // Save Layout as Default keeps the layout in default-layout.json, and
        // Reset to Default Layout puts it back, in this studio and the next.
        // Restore Built-in Default forgets it.
        const fs::path defaults = config / "defaults";
        // A frame as the main window runs one: lay out, then flushFrame, which
        // takes away docks a move left empty.
        auto step = [&frame](ide::IdeLayout& studio, jadefx::Scene& at) {
            frame(at);
            studio.flushFrame();
            frame(at);
        };
        auto pick_in = [&window_item](jadefx::Scene& at, const std::string& text) {
            if (jadefx::MenuItem* item = window_item(at, text)) {
                item->fire();
            }
        };
        // As resources/layouts/default-layout.json has it.
        auto builtin = [&](jadefx::Scene& at) {
            ide::IdePane* left = showing(at, "Search");
            ide::IdePane* view = showing(at, "Scene View");
            ide::IdePane* assets = showing(at, "Assets");
            ide::IdePane* right = showing(at, "Game Explorer");
            ide::IdePane* properties = showing(at, "Properties");
            return left != nullptr && view != nullptr && assets != nullptr && right != nullptr &&
                   properties != nullptr && left->getAbsoluteX() < 300 &&
                   std::abs(assets->getAbsoluteX() - view->getAbsoluteX()) < 1 && right->getAbsoluteX() > 640 &&
                   std::abs(properties->getAbsoluteX() - right->getAbsoluteX()) < 1 &&
                   showing(at, "Console") == nullptr && showing(at, "Current Scene") == nullptr;
        };
        // The saved default: Properties and Search closed, and the console in
        // front of the game explorer.
        auto saved_shape = [&](jadefx::Scene& at) {
            ide::IdePane* console = showing(at, "Console");
            return showing(at, "Properties") == nullptr && showing(at, "Search") == nullptr && console != nullptr &&
                   console->getAbsoluteX() > 640 && showing(at, "Game Explorer") == nullptr &&
                   showing(at, "Assets") != nullptr;
        };
        auto restore_disabled = [&window_item](jadefx::Scene& at) {
            jadefx::MenuItem* item = window_item(at, "Restore Built-in Default");
            return item != nullptr && item->isDisable();
        };
        {
            ide::IdeLayout first(1280, 800, defaults);
            auto at = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
            first.mount(*at);
            step(first, *at);
            expect(restore_disabled(*at), "Restore Built-in Default is greyed out with no saved default");
            pick_in(*at, "Properties");
            pick_in(*at, "Search");
            step(first, *at);
            ide::IdeDock* assets_dock = dock_of(showing(*at, "Assets"));
            ide::IdeDock* east = dock_of(showing(*at, "Game Explorer"));
            if (assets_dock != nullptr && east != nullptr) {
                // A copy: the move takes the tab out of the list this came from.
                const std::vector<std::shared_ptr<jadefx::Tab>> tabs = assets_dock->tabs()->getTabs().items();
                for (const std::shared_ptr<jadefx::Tab>& tab : tabs) {
                    if (tab && tab->getText() == "Console") {
                        east->take(tab);
                    }
                }
            }
            step(first, *at);
            expect(saved_shape(*at), "the layout is rearranged before it is saved");
            pick_in(*at, "Save Layout as Default");
            expect(fs::exists(defaults / "default-layout.json"), "Save Layout as Default writes default-layout.json");
            expect(!restore_disabled(*at), "and Restore Built-in Default is no longer greyed out");
            std::string text;
            std::string error;
            engine_core::JsonValue saved;
            expect(ide::read_file(defaults / "default-layout.json", text, error) &&
                       engine_core::parse_json(text, saved, error) && saved.find("main") != nullptr &&
                       saved.find("window") == nullptr,
                   "the default keeps the docks but not the main window's place");
            // Rearranged again, then reset.
            pick_in(*at, "Properties");
            pick_in(*at, "Search");
            step(first, *at);
            expect(showing(*at, "Properties") != nullptr && showing(*at, "Search") != nullptr,
                   "Properties and Search are open before the reset");
            pick_in(*at, "Reset to Default Layout");
            step(first, *at);
            expect(saved_shape(*at), "Reset to Default Layout puts back the saved default");
            expect(showing(*at, "Current Scene") == nullptr, "with the scene explorer still closed");
        }
        {
            ide::IdeLayout second(1280, 800, defaults);
            auto at = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
            second.mount(*at);
            step(second, *at);
            expect(builtin(*at), "a studio with no layout.json starts with the built-in layout");
            expect(!restore_disabled(*at), "a saved default outlives the studio that saved it");
            pick_in(*at, "Reset to Default Layout");
            step(second, *at);
            expect(saved_shape(*at), "and the next studio's reset puts it back");
            pick_in(*at, "Restore Built-in Default");
            step(second, *at);
            expect(!fs::exists(defaults / "default-layout.json"), "Restore Built-in Default removes default-layout.json");
            expect(builtin(*at), "and puts back the built-in layout");
            expect(restore_disabled(*at), "and is greyed out again");
            pick_in(*at, "Properties");
            step(second, *at);
            pick_in(*at, "Reset to Default Layout");
            step(second, *at);
            expect(builtin(*at), "with no saved default, the reset is the built-in layout");
        }
        {
            std::string error;
            ide::write_file(defaults / "default-layout.json", "{ not json", error);
            ide::IdeLayout third(1280, 800, defaults);
            auto at = jadefx::make<jadefx::Scene>(nullptr, 1280, 800);
            third.mount(*at);
            step(third, *at);
            pick_in(*at, "Properties");
            step(third, *at);
            pick_in(*at, "Reset to Default Layout");
            step(third, *at);
            expect(builtin(*at), "a default-layout.json that cannot be read resets to the built-in layout");
            bool said = false;
            for (const auto& line : third.simulation().scripts().output_since(0, 1000).lines) {
                said = said || line.text.rfind("Default layout: ", 0) == 0;
            }
            expect(said, "and the console says why");
            expect(!restore_disabled(*at), "Restore Built-in Default can still clear it");
        }
        std::error_code error;
        fs::remove_all(config, error);
    }

    // Grid, at the ribbon's right end, shows the Scene Views' floor grid. A
    // test hides it, and Stop brings it back.
    {
        jadefx::Node* grid = scene->getElementById("grid-toggle");
        auto lit = [&grid] {
            const auto& names = grid->getClassList().items();
            return std::find(names.begin(), names.end(), "on") != names.end();
        };
        expect(grid != nullptr, "the ribbon has Grid");
        if (grid != nullptr) {
            expect(grid->getAbsoluteX() + grid->getWidth() > 1280 - 20, "at its right end");
            expect(layout.scene_grid() && lit(), "the grid shows from the start, and Grid is lit");
            auto press_grid = [&] {
                const double x = grid->getAbsoluteX() + grid->getWidth() * 0.5;
                const double y = grid->getAbsoluteY() + grid->getHeight() * 0.5;
                scene->noteButton(0, true, x, y);
                scene->noteButton(0, false, x, y);
            };
            auto key = [&](int code, int mods) {
                scene->noteKey(code, true, false, mods);
                scene->noteKey(code, false, false, 0);
            };
            key(jadefx::Key::F5, 0);
            expect(!layout.scene_grid() && lit(), "a test hides the grid, and Grid stays lit");
            key(jadefx::Key::F5, jadefx::Key::ModShift);
            expect(layout.scene_grid(), "Stop shows it again");
            press_grid();
            expect(!layout.scene_grid() && !lit(), "pressing Grid hides it");
            key(jadefx::Key::F5, 0);
            key(jadefx::Key::F5, jadefx::Key::ModShift);
            expect(!layout.scene_grid(), "and a test does not bring it back");
            press_grid();
            expect(layout.scene_grid() && lit(), "pressing Grid again shows it");
        }
    }

    if (failures == 0) {
        std::printf("studio layout tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
}
