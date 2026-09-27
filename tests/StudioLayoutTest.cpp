#include "ide/IdeConsole.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"

#include "Engine.hpp"
#include "ScriptRuntime.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

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
        }
    }

    if (failures == 0) {
        std::printf("studio layout tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
}
