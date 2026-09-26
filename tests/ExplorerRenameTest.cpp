#include "ide/IdeExplorer.hpp"

#include "DataModel.hpp"
#include "Folder.hpp"
#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

constexpr double kWidth = 320;
constexpr double kHeight = 400;

// One explorer over a place with three folders. The host applies a rename
// straight to the model, standing in for the simulation thread.
struct Rig {
    engine_core::DataModel model;
    std::vector<engine_core::InstanceId> ids;
    std::vector<std::pair<engine_core::InstanceId, std::string>> renames;
    std::vector<std::string> runs;
    std::shared_ptr<ide::IdeExplorer> explorer;
    std::shared_ptr<jadefx::Scene> scene;

    Rig() {
        for (const char* name : {"Alpha", "Beta", "Gamma"}) {
            engine_core::Folder& folder = model.create<engine_core::Folder>();
            model.set_name(folder.id(), name);
            model.set_parent(folder.id(), model.id());
            ids.push_back(folder.id());
        }
        ide::ExplorerHost host;
        host.run = [this](std::string_view action, engine_core::InstanceId) { runs.emplace_back(action); };
        host.enabled = [](std::string_view) { return true; };
        host.rename = [this](engine_core::InstanceId id, std::string name) {
            model.set_name(id, name);
            renames.emplace_back(id, std::move(name));
        };
        explorer = jadefx::make<ide::IdeExplorer>(model, "Explorer", host);
        explorer->setPrefWidthRatio(1);
        explorer->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(explorer, kWidth, kHeight);
        frame(0);
    }

    void frame(double at) { scene->layout(kWidth, kHeight, at); }

    jadefx::Node* cell(const std::string& name) {
        for (jadefx::Node* candidate : explorer->getElementsByClassName("tree-cell")) {
            for (jadefx::Node* label : candidate->getElementsByClassName("tree-cell-label")) {
                auto* text = dynamic_cast<jadefx::Label*>(label);
                if (text != nullptr && text->getText() == name) {
                    return candidate;
                }
            }
        }
        return nullptr;
    }

    jadefx::TextField* field() {
        const std::vector<jadefx::Node*> found = explorer->getElementsByClassName("explorer-rename");
        return found.empty() ? nullptr : dynamic_cast<jadefx::TextField*>(found.front());
    }

    bool editing() {
        jadefx::TextField* box = field();
        return box != nullptr && box->isVisible();
    }

    // Lays out at `at`, then clicks the middle of the row showing `name`,
    // left of the + slot.
    void clickRow(const std::string& name, double at, int button = 0) {
        frame(at);
        jadefx::Node* row = cell(name);
        Expect(row != nullptr, "the row to click is on screen");
        if (row == nullptr) {
            return;
        }
        const double x = row->getAbsoluteX() + row->getWidth() * 0.4;
        const double y = row->getAbsoluteY() + row->getHeight() * 0.5;
        scene->noteButton(button, true, x, y);
        scene->noteButton(button, false, x, y);
    }

    void key(int code) { scene->noteKey(code, true, false, 0); }

    // Two clicks on one row 0.7 s apart, then long enough for the rename to open.
    void slowClick(const std::string& name, double start) {
        clickRow(name, start);
        clickRow(name, start + 0.7);
        frame(start + 1.2);
        Expect(editing(), "a slow click opens the name field");
    }
};

void TestSlowClickRenames() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.clickRow("Alpha", 0.8);
    rig.frame(1.0);
    Expect(!rig.editing(), "a slow click waits out the double-click window");
    rig.frame(1.3);
    Expect(rig.editing(), "a second click half a second later opens the name field");
    jadefx::TextField* box = rig.field();
    if (box == nullptr) {
        return;
    }
    Expect(rig.scene->focusedNode() == box, "the field takes focus");
    Expect(box->getText() == "Alpha", "the field starts with the name");
    Expect(box->getSelectedText() == "Alpha", "the whole name starts selected");
    jadefx::Node* row = rig.cell("Alpha");
    Expect(row != nullptr, "the row stays under the field");
    if (row != nullptr) {
        Expect(box->getAbsoluteY() >= row->getAbsoluteY() &&
                   box->getAbsoluteY() + box->getHeight() <= row->getAbsoluteY() + row->getHeight(),
               "the field sits inside its row");
        Expect(box->getAbsoluteX() > row->getAbsoluteX() + 10, "the field leaves the icon uncovered");
        Expect(box->getWidth() > 100, "the field runs toward the row's right edge");
    }

    rig.scene->noteText("Delta");
    rig.key(jadefx::Key::Enter);
    Expect(rig.renames.size() == 1 && rig.renames[0].first == rig.ids[0] && rig.renames[0].second == "Delta",
           "Enter applies the typed name");
    Expect(!rig.editing(), "Enter closes the field");
    jadefx::Node* focus = rig.scene->focusedNode();
    Expect(focus != nullptr && std::string_view(focus->getElementType()) == "treeview",
           "Enter gives the keys back to the tree");
    rig.frame(1.4);
    Expect(rig.cell("Delta") != nullptr, "the row shows the new name");
}

void TestEscapeCancels() {
    Rig rig;
    rig.slowClick("Beta", 0.1);
    rig.scene->noteText("Zeta");
    rig.key(jadefx::Key::Escape);
    Expect(!rig.editing(), "Escape closes the field");
    Expect(rig.renames.empty(), "Escape does not rename");
    Expect(rig.model.name(rig.ids[1]) == "Beta", "Escape keeps the old name");
}

void TestEmptyNameCancels() {
    Rig rig;
    rig.slowClick("Beta", 0.1);
    rig.key(jadefx::Key::Backspace);
    Expect(rig.field() != nullptr && rig.field()->getText().empty(), "Backspace clears the selected name");
    rig.key(jadefx::Key::Enter);
    Expect(!rig.editing(), "Enter on an empty name closes the field");
    Expect(rig.renames.empty(), "an empty name is not applied");
}

void TestUnchangedNameIsNotApplied() {
    Rig rig;
    rig.slowClick("Beta", 0.1);
    rig.key(jadefx::Key::Enter);
    Expect(!rig.editing(), "Enter closes an unchanged field");
    Expect(rig.renames.empty(), "an unchanged name adds no rename");
}

void TestClickOutsideCancels() {
    Rig rig;
    rig.slowClick("Beta", 0.1);
    rig.scene->noteText("Zeta");
    rig.clickRow("Gamma", 1.5);
    rig.frame(1.6);
    Expect(!rig.editing(), "a click on another row closes the field");
    Expect(rig.renames.empty(), "a click outside does not rename");
    Expect(rig.model.name(rig.ids[1]) == "Beta", "a click outside keeps the old name");
}

void TestRightClickCancels() {
    Rig rig;
    rig.slowClick("Beta", 0.1);
    rig.clickRow("Gamma", 1.5, 1);
    Expect(!rig.editing(), "a right-click closes the field");
    Expect(rig.renames.empty(), "a right-click does not rename");
}

void TestFastDoubleClickDoesNotRename() {
    Rig rig;
    rig.clickRow("Gamma", 0.1);
    rig.clickRow("Gamma", 0.3);
    rig.frame(1.5);
    Expect(!rig.editing(), "a fast double-click does not rename");
}

void TestDoubleClickAfterSlowClickWins() {
    Rig rig;
    rig.clickRow("Gamma", 0.1);
    rig.clickRow("Gamma", 0.8);
    rig.clickRow("Gamma", 0.9);
    rig.frame(1.5);
    Expect(!rig.editing(), "a double-click that starts on the slow click does not rename");
}

void TestClicksOnDifferentRowsDoNotPair() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.clickRow("Beta", 0.8);
    rig.frame(1.5);
    Expect(!rig.editing(), "clicks on two rows do not rename");
}

void TestFocusLeavingBreaksThePair() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.scene->requestFocus(nullptr);
    rig.frame(0.3);
    rig.clickRow("Alpha", 0.8);
    rig.frame(1.5);
    Expect(!rig.editing(), "focus leaving the tree between clicks does not rename");
}

void TestMenuRename() {
    Rig rig;
    rig.clickRow("Gamma", 0.1, 1);
    jadefx::Node* label = rig.scene->getElementById("menu-label:Rename");
    Expect(label != nullptr && label->getParent() != nullptr, "the row menu lists Rename");
    if (label == nullptr || label->getParent() == nullptr) {
        return;
    }
    jadefx::Node* item = label->getParent();
    const double x = item->getAbsoluteX() + item->getWidth() * 0.5;
    const double y = item->getAbsoluteY() + item->getHeight() * 0.5;
    rig.scene->noteButton(0, true, x, y);
    rig.scene->noteButton(0, false, x, y);
    rig.frame(0.2);
    Expect(rig.editing(), "Rename from the menu opens the name field");
    Expect(rig.runs.empty(), "the explorer handles Rename without the host's run");
    jadefx::TextField* box = rig.field();
    if (box == nullptr) {
        return;
    }
    Expect(rig.scene->focusedNode() == box, "the menu's field takes focus");
    Expect(box->getSelectedText() == "Gamma", "the menu's field starts with the name selected");
    rig.scene->noteText("Omega");
    rig.key(jadefx::Key::KpEnter);
    Expect(rig.renames.size() == 1 && rig.renames[0].second == "Omega", "the menu's rename applies on Enter");
}

}  // namespace

int main() {
    TestSlowClickRenames();
    TestEscapeCancels();
    TestEmptyNameCancels();
    TestUnchangedNameIsNotApplied();
    TestClickOutsideCancels();
    TestRightClickCancels();
    TestFastDoubleClickDoesNotRename();
    TestDoubleClickAfterSlowClickWins();
    TestClicksOnDifferentRowsDoNotPair();
    TestFocusLeavingBreaksThePair();
    TestMenuRename();
    if (gFailures == 0) {
        std::printf("explorer rename tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d explorer rename tests failed\n", gFailures);
    return 1;
}
