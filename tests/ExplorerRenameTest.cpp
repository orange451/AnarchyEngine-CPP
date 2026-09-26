#include "ide/IdeExplorer.hpp"

#include "DataModel.hpp"
#include "Folder.hpp"
#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <initializer_list>
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
    std::vector<std::pair<std::string, std::vector<engine_core::InstanceId>>> batches;
    // The modifier keys the next click sees.
    int mods = 0;
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
        host.run_many = [this](std::string_view action, const std::vector<engine_core::InstanceId>& ids) {
            batches.emplace_back(std::string(action), ids);
        };
        host.enabled = [](std::string_view) { return true; };
        host.modifiers = [this] { return mods; };
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

    // The row draws the selection bar: the tree's own row, or one the explorer painted.
    bool painted(const std::string& name) {
        jadefx::Node* row = cell(name);
        if (row == nullptr) {
            return false;
        }
        for (jadefx::Node* bar : row->getElementsByClassName("selection-bar")) {
            if (bar->isVisible() && bar->getWidth() > 0) {
                return true;
            }
        }
        return false;
    }

    std::vector<engine_core::InstanceId> selection() const { return model.selection().get(); }

    std::vector<engine_core::InstanceId> pick(std::initializer_list<int> indexes) const {
        std::vector<engine_core::InstanceId> out;
        for (int index : indexes) {
            out.push_back(ids[static_cast<std::size_t>(index)]);
        }
        return out;
    }

    void clickMenu(const std::string& label) {
        jadefx::Node* text = scene->getElementById("menu-label:" + label);
        Expect(text != nullptr && text->getParent() != nullptr, "the row menu lists the action");
        if (text == nullptr || text->getParent() == nullptr) {
            return;
        }
        jadefx::Node* item = text->getParent();
        const double x = item->getAbsoluteX() + item->getWidth() * 0.5;
        const double y = item->getAbsoluteY() + item->getHeight() * 0.5;
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
    }

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

void TestClickSelectsOne() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    Expect(rig.selection() == rig.pick({0}), "a click selects that instance");
    rig.clickRow("Gamma", 0.8);
    rig.frame(0.9);
    Expect(rig.selection() == rig.pick({2}), "a plain click replaces the selection");
    Expect(rig.painted("Gamma") && !rig.painted("Alpha"), "only the clicked row is drawn selected");
}

void TestControlClickAddsAndRemoves() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.mods = jadefx::Key::ModControl;
    rig.clickRow("Gamma", 0.8);
    rig.frame(0.9);
    Expect(rig.selection() == rig.pick({0, 2}), "Ctrl and a click adds a row");
    Expect(rig.painted("Alpha") && rig.painted("Gamma") && !rig.painted("Beta"), "both selected rows are drawn");

    rig.clickRow("Alpha", 1.6);
    rig.frame(1.7);
    Expect(rig.selection() == rig.pick({2}), "Ctrl and a click on a selected row removes it");
    Expect(!rig.painted("Alpha") && rig.painted("Gamma"), "the removed row is no longer drawn selected");

    rig.mods = jadefx::Key::ModSuper;
    rig.clickRow("Beta", 2.4);
    rig.frame(2.5);
    Expect(rig.selection() == rig.pick({2, 1}), "Cmd and a click adds a row too");

    rig.mods = 0;
    rig.clickRow("Alpha", 3.2);
    rig.frame(3.3);
    Expect(rig.selection() == rig.pick({0}), "a plain click after that selects one row again");
    Expect(!rig.painted("Beta") && !rig.painted("Gamma"), "the other rows clear");
}

void TestModifiedClicksDoNotRename() {
    Rig rig;
    rig.mods = jadefx::Key::ModControl;
    rig.clickRow("Alpha", 0.1);
    rig.clickRow("Alpha", 0.8);
    rig.frame(1.5);
    Expect(!rig.editing(), "two Ctrl clicks do not rename");
}

void TestShiftClickSelectsRange() {
    Rig rig;
    rig.clickRow("Gamma", 0.1);
    rig.mods = jadefx::Key::ModShift;
    rig.clickRow("Alpha", 0.8);
    rig.frame(0.9);
    // Rows show newest first: Gamma, Beta, Alpha. The range keeps that order.
    Expect(rig.selection() == rig.pick({2, 1, 0}), "Shift and a click selects every row between");
    Expect(rig.painted("Alpha") && rig.painted("Beta") && rig.painted("Gamma"), "the whole range is drawn");

    rig.clickRow("Beta", 1.6);
    rig.frame(1.7);
    Expect(rig.selection() == rig.pick({2, 1}), "the range starts from the last plain click");
}

void TestSetShowsInTheTree() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.model.selection().set(rig.pick({1, 2}));
    rig.frame(0.2);
    Expect(rig.painted("Beta") && rig.painted("Gamma") && !rig.painted("Alpha"), "a Set shows in the explorer");
    jadefx::Node* gamma = rig.cell("Gamma");
    Expect(gamma != nullptr && gamma->isSelected(), "the tree's own row moves to the last selected");

    rig.model.selection().set({});
    rig.frame(0.3);
    Expect(!rig.painted("Alpha") && !rig.painted("Beta") && !rig.painted("Gamma"), "an empty Set clears the rows");
}

void TestSetOpensTheBranch() {
    Rig rig;
    engine_core::Folder& inner = rig.model.create<engine_core::Folder>();
    rig.model.set_name(inner.id(), "Inner");
    rig.model.set_parent(inner.id(), rig.ids[0]);
    rig.frame(0.1);
    Expect(rig.cell("Inner") == nullptr, "a child of a closed folder has no row");
    rig.model.selection().set({inner.id()});
    rig.frame(0.2);
    rig.frame(0.3);
    Expect(rig.painted("Inner"), "a Set opens the folder above the selected row");
}

void TestSelectionIsShared() {
    Rig rig;
    ide::ExplorerHost host;
    auto other = jadefx::make<ide::IdeExplorer>(rig.model, "Other", host);
    other->setPrefWidthRatio(1);
    other->setPrefHeightRatio(1);
    auto scene = jadefx::make<jadefx::Scene>(other, kWidth, kHeight);
    scene->layout(kWidth, kHeight, 0);
    rig.clickRow("Beta", 0.1);
    scene->layout(kWidth, kHeight, 0.2);
    bool shown = false;
    for (jadefx::Node* row : other->getElementsByClassName("tree-cell")) {
        if (row->isSelected()) {
            for (jadefx::Node* label : row->getElementsByClassName("tree-cell-label")) {
                auto* text = dynamic_cast<jadefx::Label*>(label);
                shown = text != nullptr && text->getText() == "Beta";
            }
        }
    }
    Expect(shown, "a click in one explorer selects the row in the other");
}

void TestKeysMoveTheSelection() {
    Rig rig;
    rig.clickRow("Gamma", 0.1);
    rig.key(jadefx::Key::Down);
    rig.frame(0.2);
    Expect(rig.selection() == rig.pick({1}), "the arrow keys move the selection");
}

void TestRightClickKeepsTheSelection() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.mods = jadefx::Key::ModControl;
    rig.clickRow("Gamma", 0.8);
    rig.mods = 0;
    rig.clickRow("Gamma", 1.5, 1);
    Expect(rig.selection() == rig.pick({0, 2}), "a right-click on a selected row keeps the others");
    rig.clickMenu("Delete");
    Expect(rig.batches.size() == 1 && rig.batches[0].first == "Delete" && rig.batches[0].second == rig.pick({0, 2}),
           "Delete from the menu runs once on every selected instance");

    rig.clickRow("Beta", 2.2, 1);
    Expect(rig.selection() == rig.pick({1}), "a right-click on another row selects only it");
}

void TestDeleteRunsOnTheSelection() {
    Rig rig;
    rig.clickRow("Beta", 0.1);
    Expect(rig.explorer->run_on_selection("Delete"), "Delete runs on one selected row");
    Expect(rig.runs.size() == 1 && rig.runs[0] == "Delete" && rig.batches.empty(), "one row uses run");
    rig.model.selection().set(rig.pick({0, 1, 2}));
    rig.frame(0.2);
    Expect(rig.explorer->run_on_selection("Delete"), "Delete runs on a multiple selection");
    Expect(rig.batches.size() == 1 && rig.batches[0].second == rig.pick({0, 1, 2}), "many rows use run_many");
    Expect(!rig.explorer->run_on_selection("Edit"), "a folder does not offer Edit");
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
    TestClickSelectsOne();
    TestControlClickAddsAndRemoves();
    TestModifiedClicksDoNotRename();
    TestShiftClickSelectsRange();
    TestSetShowsInTheTree();
    TestSetOpensTheBranch();
    TestSelectionIsShared();
    TestKeysMoveTheSelection();
    TestRightClickKeepsTheSelection();
    TestDeleteRunsOnTheSelection();
    if (gFailures == 0) {
        std::printf("explorer tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d explorer tests failed\n", gFailures);
    return 1;
}
