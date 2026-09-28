#include "ide/IdeConsole.hpp"
#include "ide/IdeDock.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdeResources.hpp"
#include "ide/IdePane.hpp"
#include "ide/IdeSearch.hpp"

#include "Engine.hpp"
#include "Project.hpp"
#include "ScriptRuntime.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

int RunFindReplaceTests(engine_core::Engine& engine);
int RunThemeTests(jadefx::Scene& scene);
int RunPreferencesTests();
int RunSaveConflictTests(ide::IdeLayout& layout, jadefx::Scene& scene);

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

    std::vector<std::string> names;
    std::vector<ide::IdePane*> panes;
    for (jadefx::Node* node : scene->getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* pane = dynamic_cast<ide::IdePane*>(node)) {
            names.push_back(pane->name());
            panes.push_back(pane);
        }
    }
    auto has = [&names](const char* name) { return std::find(names.begin(), names.end(), name) != names.end(); };
    expect(has("Game Explorer"), "the left explorer is docked");
    expect(has("Current Scene"), "the right explorer is docked");
    expect(has("Console"), "the console is docked");
    expect(has("Properties"), "Properties is docked");
    for (ide::IdePane* pane : panes) {
        if (pane->name() == "Properties") {
            expect(pane->getWidth() > 100 && pane->getHeight() > 100, "Properties has room on screen");
            expect(pane->getAbsoluteX() > 640, "Properties is on the right, under the scene explorer");
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
        expect(labels == std::vector<std::string>{"Game Explorer", "Current Scene", "Properties", "Console", "Search", "",
                                                  "New Scene View", "", "Reset to Default Layout"},
               "Window lists the explorers, Properties, Console, and Search, then New Scene View and the reset");
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

        // Search docked beside the left explorer above, so the explorer is behind it.
        expect(checked("Game Explorer") && checked("Current Scene") && checked("Properties") && checked("Console") &&
                   checked("Search"),
               "every window starts open, and has a check");
        expect(!checked("New Scene View"), "New Scene View has no check");
        expect(showing("Game Explorer") == nullptr && showing("Search") != nullptr,
               "the left explorer is behind Search");
        pick("Game Explorer");
        ide::IdePane* explorer = showing("Game Explorer");
        expect(explorer != nullptr && showing("Search") == nullptr, "picking a window behind a tab brings it forward");
        expect(checked("Game Explorer"), "and it keeps its check");
        pick("Game Explorer");
        expect(showing("Game Explorer") == nullptr && !checked("Game Explorer"), "picking a showing window closes it");
        expect(showing("Search") != nullptr, "and the tab beside it shows");
        pick("Game Explorer");
        ide::IdePane* reopened = showing("Game Explorer");
        expect(reopened != nullptr && checked("Game Explorer"), "picking a closed window opens it");
        expect(reopened == explorer, "it opens as the same page it was");
        expect(reopened != nullptr && reopened->getAbsoluteX() < 300, "in the dock it closed from");

        pick("Properties");
        expect(showing("Properties") == nullptr && !checked("Properties"), "Properties closes from the menu");
        pick("Properties");
        ide::IdePane* properties = showing("Properties");
        expect(properties != nullptr && checked("Properties"), "and opens again");
        expect(properties != nullptr && properties->getAbsoluteX() > 640, "on the right, where it was");

        pick("Search");
        expect(showing("Search") != nullptr, "Search comes forward from behind the explorer");
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

        // Reset to Default Layout puts the windows back as a new studio has
        // them. Close Properties, move the console in with the scene
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
        ide::IdeDock* console_dock = dock_of(showing("Console"));
        ide::IdeDock* scene_dock = dock_of(showing("Current Scene"));
        if (console_dock != nullptr && scene_dock != nullptr) {
            const std::shared_ptr<jadefx::Tab> moving = console_dock->tabs()->getTabs().items().front();
            scene_dock->take(moving);
        }
        pick("New Scene View");
        expect(showing("Properties") == nullptr && showing("Scene View 4") != nullptr &&
                   dock_of(showing("Console")) == scene_dock,
               "the layout is rearranged before the reset");
        pick("Reset to Default Layout");
        frame();
        ide::IdePane* left = showing("Game Explorer");
        ide::IdePane* middle = showing("Scene View");
        ide::IdePane* below = showing("Console");
        ide::IdePane* right = showing("Current Scene");
        ide::IdePane* under = showing("Properties");
        expect(left != nullptr && left->getAbsoluteX() < 300, "the reset puts the game explorer on the left");
        expect(middle != nullptr && middle->getAbsoluteX() > 200 && middle->getAbsoluteX() < 640,
               "the scene view in the middle, showing");
        expect(below != nullptr && middle != nullptr && std::abs(below->getAbsoluteX() - middle->getAbsoluteX()) < 1 &&
                   below->getAbsoluteY() > middle->getAbsoluteY() + middle->getHeight() - 1,
               "the console under it");
        expect(right != nullptr && under != nullptr && right->getAbsoluteX() > 640 &&
                   std::abs(under->getAbsoluteX() - right->getAbsoluteX()) < 1 &&
                   under->getAbsoluteY() > right->getAbsoluteY() + right->getHeight() - 1,
               "and the scene explorer over Properties on the right");
        expect(checked("Game Explorer") && checked("Current Scene") && checked("Properties") && checked("Console"),
               "the four windows are open");
        expect(!checked("Search"), "Search is closed, as in a new studio");
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
        layout.open_project_at(root);
        scene->layout(1280, 800, 2.0);
        scene->layout(1280, 800, 2.05);
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
        std::error_code error;
        fs::remove_all(folder, error);
    }
    failures += RunSaveConflictTests(layout, *scene);

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
            // Close Properties, open Search beside the game explorer, move the
            // console in with the scene explorer, and widen the left column.
            if (jadefx::MenuItem* item = window_item(*at, "Properties")) {
                item->fire();
            }
            if (jadefx::MenuItem* item = window_item(*at, "Search")) {
                item->fire();
            }
            frame(*at);
            ide::IdeDock* console_dock = dock_of(showing(*at, "Console"));
            ide::IdeDock* east = dock_of(showing(*at, "Current Scene"));
            expect(console_dock != nullptr && east != nullptr, "the console and the scene explorer are docked");
            if (console_dock != nullptr && east != nullptr) {
                // A copy: the move takes the tab out of the list this came from.
                const std::shared_ptr<jadefx::Tab> moving = console_dock->tabs()->getTabs().items().front();
                east->take(moving);
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
            if (ide::IdePane* search = showing(*at, "Search")) {
                left_width = search->getWidth();
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
            ide::IdePane* search = showing(*at, "Search");
            expect(search != nullptr && search->getAbsoluteX() < 300, "Search opens where it was, showing");
            expect(search != nullptr && std::abs(search->getWidth() - left_width) < 2,
                   "the left column keeps its width");
            expect(showing(*at, "Game Explorer") == nullptr, "the game explorer is behind Search again");
            ide::IdePane* console = showing(*at, "Console");
            expect(console != nullptr && console->getAbsoluteX() > 640, "the console opens with the scene explorer");
            expect(showing(*at, "Current Scene") == nullptr, "which is behind it, as it was");
            ide::IdePane* view = showing(*at, "Scene View");
            expect(view != nullptr && view->getHeight() > 600, "the scene view fills the column the console left");
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
            expect(showing(*at, "Properties") != nullptr && showing(*at, "Console") != nullptr &&
                       showing(*at, "Game Explorer") != nullptr,
                   "a layout.json that cannot be read starts the default layout");
            bool said = false;
            for (const auto& line : third.simulation().scripts().output_since(0, 1000).lines) {
                said = said || line.text.rfind("Layout: ", 0) == 0;
            }
            expect(said, "and the console says why");
        }
        std::error_code error;
        fs::remove_all(config, error);
    }

    if (failures == 0) {
        std::printf("studio layout tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
}
