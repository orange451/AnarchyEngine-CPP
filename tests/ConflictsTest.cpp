#include "ide/IdeDock.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <memory>
#include <string>

namespace {

ide::IdeDock* DockOf(jadefx::Node* node) {
    for (; node != nullptr; node = node->getParent()) {
        if (auto* dock = dynamic_cast<ide::IdeDock*>(node)) {
            return dock;
        }
    }
    return nullptr;
}

std::shared_ptr<jadefx::Tab> TabOf(ide::IdeDock& dock, const jadefx::Node* pane) {
    for (const std::shared_ptr<jadefx::Tab>& tab : dock.tabs()->getTabs().items()) {
        if (tab && tab->getContent() == pane) {
            return tab;
        }
    }
    return nullptr;
}

}  // namespace

// reveal_window shows any dockable pane: it selects the pane's tab, and docks
// a closed pane through the open it is given.
int RunConflictsTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    int failures = 0;
    auto expect = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };

    scene.noteKey(jadefx::Key::F, true, false, jadefx::Key::ModControl | jadefx::Key::ModShift);
    scene.noteKey(jadefx::Key::F, false, false, 0);
    auto* search = dynamic_cast<ide::IdePane*>(scene.getElementsByClassName("search-pane").empty()
                                                   ? nullptr
                                                   : scene.getElementsByClassName("search-pane").front());
    ide::IdeDock* dock = DockOf(search);
    expect(search != nullptr && dock != nullptr, "Find in Scripts docks the Search pane");
    if (search != nullptr && dock != nullptr) {
        const std::shared_ptr<jadefx::Tab> tab = TabOf(*dock, search);
        for (const std::shared_ptr<jadefx::Tab>& other : dock->tabs()->getTabs().items()) {
            if (other != tab) {
                dock->tabs()->select(other);
                break;
            }
        }
        expect(tab && !tab->isSelected(), "another tab is in front of Search");
        bool opened = false;
        layout.reveal_window(search, [&opened] { opened = true; });
        expect(tab && tab->isSelected(), "reveal_window selects a docked pane's tab");
        expect(!opened, "and does not open it again");

        dock->tabs()->close(tab);
        scene.layout(1280, 800, 4.0);
        layout.reveal_window(search, [&opened] { opened = true; });
        expect(opened, "reveal_window opens a pane no dock holds");
    }
    return failures;
}
