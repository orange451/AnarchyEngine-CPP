#include "ide/IdeConsole.hpp"
#include "ide/IdeLayout.hpp"
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

    if (failures == 0) {
        std::printf("studio layout tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
}
