#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"

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
    if (failures == 0) {
        std::printf("studio layout tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
}
