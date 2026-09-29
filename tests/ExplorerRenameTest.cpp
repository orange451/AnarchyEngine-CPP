#include "ide/CutSet.hpp"
#include "ide/IdeExplorer.hpp"
#include "DataModelLock.hpp"
#include "SelectionService.hpp"

#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <initializer_list>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
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
// straight to the game, standing in for the simulation thread.
struct Rig {
    engine_core::Game game;
    std::vector<engine_core::InstanceId> ids;
    std::vector<std::pair<engine_core::InstanceId, std::string>> renames;
    std::vector<std::string> runs;
    int moves = 0;
    std::vector<std::pair<std::string, std::vector<engine_core::InstanceId>>> batches;
    // Each insert's class and parent. Each makes a Folder there, whatever the class.
    std::vector<std::pair<std::string, engine_core::InstanceId>> inserts;
    // Messages the explorer asked the studio to show.
    std::vector<std::string> notices;
    // Set, an insert makes nothing and says this, as a full place does.
    std::string refuse_inserts;
    // The modifier keys the next click sees.
    int mods = 0;
    std::shared_ptr<ide::IdeExplorer> explorer;
    std::shared_ptr<jadefx::Scene> scene;

    Rig() {
        for (const char* name : {"Alpha", "Beta", "Gamma"}) {
            engine_core::Folder& folder = game.create<engine_core::Folder>();
            game.set_name(folder.id(), name);
            game.set_parent(folder.id(), game.id());
            ids.push_back(folder.id());
        }
        ide::ExplorerHost host;
        host.run = [this](engine_core::InstanceAction action, engine_core::InstanceId) {
            runs.emplace_back(engine_core::action_label(action));
        };
        host.run_many = [this](engine_core::InstanceAction action, const std::vector<engine_core::InstanceId>& ids) {
            batches.emplace_back(engine_core::action_label(action), ids);
        };
        host.enabled = [](engine_core::InstanceAction) { return true; };
        host.notice = [this](std::string text) { notices.push_back(std::move(text)); };
        host.rename = [this](engine_core::InstanceId id, std::string name) {
            game.set_name(id, name);
            renames.emplace_back(id, std::move(name));
        };
        host.move = [this](const std::vector<engine_core::InstanceId>& moved, engine_core::InstanceId parent) {
            ++moves;
            ide::move_set(game, moved, parent);
        };
        host.insert = [this](std::string class_name, engine_core::InstanceId parent,
                             std::shared_ptr<ide::InsertResult> result) {
            inserts.emplace_back(std::move(class_name), parent);
            if (!refuse_inserts.empty()) {
                result->error = refuse_inserts;
                result->done = true;
                return;
            }
            engine_core::Folder& made = game.create<engine_core::Folder>();
            game.set_name(made.id(), "Made");
            game.set_parent(made.id(), parent);
            result->id = made.id();
            result->done = true;
        };
        explorer = jadefx::make<ide::IdeExplorer>(game, "Explorer", host);
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
        scene->noteButton(button, true, x, y, mods);
        scene->noteButton(button, false, x, y, mods);
    }

    void key(int code) { scene->noteKey(code, true, false, 0); }

    // Presses the middle of from's row, moves to along (0 top, 1 bottom) of
    // to's row, and releases there. The move lays out in between.
    void drag(const std::string& from, const std::string& to, double along, double at) {
        frame(at);
        jadefx::Node* source = cell(from);
        jadefx::Node* target = cell(to);
        Expect(source != nullptr && target != nullptr, "both drag rows are on screen");
        if (source == nullptr || target == nullptr) {
            return;
        }
        const double x = source->getAbsoluteX() + source->getWidth() * 0.4;
        scene->noteButton(0, true, x, source->getAbsoluteY() + source->getHeight() * 0.5, mods);
        const double y = target->getAbsoluteY() + target->getHeight() * along;
        scene->noteMove(x, y);
        frame(at + 0.05);
        scene->noteMove(x, y);
        scene->noteButton(0, false, x, y, mods);
        frame(at + 0.1);
    }

    std::vector<engine_core::InstanceId> children(engine_core::InstanceId parent) const {
        return game.get_children(parent);
    }

    // The row draws the selection bar.
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

    // The row is drawn inside the tree's bounds.
    bool in_view(const std::string& name) {
        jadefx::Node* row = cell(name);
        jadefx::TreeView* view = tree();
        return row != nullptr && view != nullptr && row->isVisible() && row->getHeight() > 0 &&
               row->getAbsoluteY() >= view->getAbsoluteY() &&
               row->getAbsoluteY() + row->getHeight() <= view->getAbsoluteY() + view->getHeight();
    }

    jadefx::TreeView* tree() {
        const std::vector<jadefx::Node*> cells = explorer->getElementsByClassName("tree-cell");
        for (jadefx::Node* up = cells.empty() ? nullptr : cells.front(); up != nullptr; up = up->getParent()) {
            if (auto* found = dynamic_cast<jadefx::TreeView*>(up)) {
                return found;
            }
        }
        return nullptr;
    }

    jadefx::TextField* filter() {
        const std::vector<jadefx::Node*> found = explorer->getElementsByClassName("explorer-filter");
        return found.empty() ? nullptr : dynamic_cast<jadefx::TextField*>(found.front());
    }

    void type_filter(const std::string& text, double at) {
        if (jadefx::TextField* box = filter()) {
            box->setText(text);
        }
        frame(at);
        frame(at + 0.01);
    }

    std::vector<engine_core::InstanceId> selection() const { return game.selection().get(); }

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
    Expect(rig.game.name(rig.ids[1]) == "Beta", "Escape keeps the old name");
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
    Expect(rig.game.name(rig.ids[1]) == "Beta", "a click outside keeps the old name");
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
    // Rows show Alpha, Beta, Gamma. The range runs from the anchor, Gamma, up to Alpha.
    Expect(rig.selection() == rig.pick({2, 1, 0}), "Shift and a click selects every row between");
    Expect(rig.painted("Alpha") && rig.painted("Beta") && rig.painted("Gamma"), "the whole range is drawn");

    rig.clickRow("Beta", 1.6);
    rig.frame(1.7);
    Expect(rig.selection() == rig.pick({2, 1}), "the range starts from the last plain click");
}

void TestSetShowsInTheTree() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.game.selection().set(rig.pick({1, 2}));
    rig.frame(0.2);
    Expect(rig.painted("Beta") && rig.painted("Gamma") && !rig.painted("Alpha"), "a Set shows in the explorer");
    jadefx::Node* gamma = rig.cell("Gamma");
    Expect(gamma != nullptr && gamma->isSelected(), "the tree marks a selected row");

    rig.game.selection().set({});
    rig.frame(0.3);
    Expect(!rig.painted("Alpha") && !rig.painted("Beta") && !rig.painted("Gamma"), "an empty Set clears the rows");
}

void TestSetOpensTheBranch() {
    Rig rig;
    engine_core::Folder& inner = rig.game.create<engine_core::Folder>();
    rig.game.set_name(inner.id(), "Inner");
    rig.game.set_parent(inner.id(), rig.ids[0]);
    rig.frame(0.1);
    Expect(rig.cell("Inner") == nullptr, "a child of a closed folder has no row");
    rig.game.selection().set({inner.id()});
    rig.frame(0.2);
    rig.frame(0.3);
    Expect(rig.painted("Inner"), "a Set opens the folder above the selected row");
}

void TestSelectionIsShared() {
    Rig rig;
    ide::ExplorerHost host;
    auto other = jadefx::make<ide::IdeExplorer>(rig.game, "Other", host);
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
    rig.clickRow("Alpha", 0.1);
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

void TestRebuildKeepsTheSelection() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.mods = jadefx::Key::ModControl;
    rig.clickRow("Gamma", 0.8);
    rig.mods = 0;
    // Enough new rows at once that the explorer rebuilds the tree detached.
    for (int i = 0; i < 12; ++i) {
        engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
        rig.game.set_parent(folder.id(), rig.game.id());
    }
    rig.frame(0.9);
    rig.frame(1.0);
    Expect(rig.selection() == rig.pick({0, 2}), "a rebuild does not change the selection");
    Expect(rig.painted("Alpha") && rig.painted("Gamma") && !rig.painted("Beta"), "a rebuild keeps the rows drawn");
}

void TestCutRunsOnTheSelection() {
    Rig rig;
    rig.game.selection().set(rig.pick({0, 2}));
    rig.frame(0.1);
    rig.clickRow("Alpha", 0.2, 1);
    rig.clickMenu("Cut");
    Expect(rig.batches.size() == 1 && rig.batches[0].first == "Cut" && rig.batches[0].second == rig.pick({0, 2}),
           "Cut from the menu runs once on every selected instance");
    Expect(rig.runs.empty(), "a multiple Cut does not also run on the clicked row");
    Expect(rig.explorer->run_on_selection(engine_core::InstanceAction::Cut), "Cut runs on the selection by name too");
    Expect(rig.batches.size() == 2 && rig.batches[1].first == "Cut", "that Cut is one batch too");
}

void TestCutSet() {
    Rig rig;
    // Alpha, Beta, Gamma at the top; Inner under Alpha.
    engine_core::Folder& inner = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(inner.id(), rig.ids[0]);
    const engine_core::InstanceId alpha = rig.ids[0];
    const engine_core::InstanceId beta = rig.ids[1];
    const engine_core::InstanceId gamma = rig.ids[2];
    Expect(ide::cut_set(rig.game, {gamma, alpha}) == std::vector<engine_core::InstanceId>{alpha, gamma},
           "a cut takes instances in the order the tree shows them");
    Expect(ide::cut_set(rig.game, {inner.id(), alpha}) == std::vector<engine_core::InstanceId>{alpha},
           "a child goes with its selected parent");
    Expect(ide::cut_set(rig.game, {beta, inner.id()}) == std::vector<engine_core::InstanceId>{inner.id(), beta},
           "a child whose parent is not selected is taken on its own");
    rig.game.destroy(beta);
    Expect(ide::cut_set(rig.game, {beta, 0}).empty(), "a cut skips the root and gone instances");
}

void TestDeleteRunsOnTheSelection() {
    Rig rig;
    rig.clickRow("Beta", 0.1);
    Expect(rig.explorer->run_on_selection(engine_core::InstanceAction::Delete), "Delete runs on one selected row");
    Expect(rig.runs.size() == 1 && rig.runs[0] == "Delete" && rig.batches.empty(), "one row uses run");
    rig.game.selection().set(rig.pick({0, 1, 2}));
    rig.frame(0.2);
    Expect(rig.explorer->run_on_selection(engine_core::InstanceAction::Delete), "Delete runs on a multiple selection");
    Expect(rig.batches.size() == 1 && rig.batches[0].second == rig.pick({0, 1, 2}), "many rows use run_many");
    Expect(!rig.explorer->run_on_selection(engine_core::InstanceAction::Edit), "a folder does not offer Edit");
}

void TestDragIntoAnotherRow() {
    Rig rig;
    rig.drag("Gamma", "Alpha", 0.5, 0.1);
    Expect(rig.moves == 1, "a drop moves once");
    Expect(rig.game.parent(rig.ids[2]) == rig.ids[0], "dropping on the middle of a row parents inside it");
    Expect(rig.selection() == rig.pick({2}), "the dragged row is selected");
    Expect(!rig.editing(), "the release after a drag does not start a rename");
    rig.frame(0.3);
    Expect(rig.cell("Gamma") != nullptr, "the new parent opens to show the moved row");
    rig.frame(1.5);
    Expect(!rig.editing(), "a drag never becomes a slow click");
}

void TestDragBesideAnotherRow() {
    Rig rig;
    // Folders are parented in order, so the rows read Alpha, Beta, Gamma.
    Expect(rig.children(0) == rig.pick({0, 1, 2}), "the rows start in the order they arrived");
    rig.drag("Alpha", "Gamma", 0.95, 0.1);
    Expect(rig.children(0) == rig.pick({0, 1, 2}), "beside a row under the same parent changes nothing");
    rig.drag("Gamma", "Beta", 0.5, 0.5);
    Expect(rig.children(rig.ids[1]) == rig.pick({2}), "the middle of a row puts it inside");
    rig.drag("Alpha", "Gamma", 0.05, 1.0);
    Expect(rig.game.parent(rig.ids[0]) == rig.ids[1], "beside a row puts it under that row's parent");
    Expect(rig.children(rig.ids[1]) == rig.pick({2, 0}), "and last there, not where the line was");
}

void TestDragCarriesTheSelection() {
    Rig rig;
    rig.game.selection().set(rig.pick({2, 0}));
    rig.frame(0.05);
    rig.drag("Alpha", "Beta", 0.5, 0.1);
    Expect(rig.children(rig.ids[1]) == rig.pick({0, 2}), "every selected row moves, in row order");
    Expect(rig.moves == 1, "the selection moves as one step");
}

void TestDragRefusesItsOwnChild() {
    Rig rig;
    rig.drag("Gamma", "Alpha", 0.5, 0.1);
    rig.frame(0.3);
    rig.drag("Alpha", "Gamma", 0.5, 0.5);
    Expect(rig.moves == 1, "a row cannot drop inside its own child");
    Expect(rig.game.parent(rig.ids[0]) == rig.game.id(), "the refused drop leaves the row where it was");
}

void TestMoveSet() {
    engine_core::Game game;
    std::vector<engine_core::InstanceId> ids;
    for (const char* name : {"A", "B", "C", "D"}) {
        engine_core::Folder& folder = game.create<engine_core::Folder>();
        game.set_name(folder.id(), name);
        game.set_parent(folder.id(), 0);
        ids.push_back(folder.id());
    }
    Expect(game.get_children(0) == ids, "children are in the order they arrived");
    Expect(!ide::move_set(game, {ids[3]}, 0), "an instance already under the parent stays put");
    Expect(game.get_children(0) == ids, "and keeps its place");
    Expect(ide::move_set(game, {ids[3], ids[2]}, ids[0]), "move_set moves");
    Expect(game.get_children(ids[0]) == std::vector<engine_core::InstanceId>{ids[3], ids[2]},
           "the moved instances go last, in the order given");
    Expect(ide::move_set(game, {ids[3]}, 0), "back to the root");
    Expect(game.get_children(0) == std::vector<engine_core::InstanceId>{ids[0], ids[1], ids[3]}, "last again");
    Expect(!ide::move_set(game, {ids[0]}, ids[2]), "an instance never goes inside its own child");
    Expect(!ide::move_set(game, {0}, ids[1]), "the root never moves");
}

// Alpha holds Inner, which holds Deep.
engine_core::InstanceId NestDeep(Rig& rig) {
    engine_core::Folder& inner = rig.game.create<engine_core::Folder>();
    rig.game.set_name(inner.id(), "Inner");
    rig.game.set_parent(inner.id(), rig.ids[0]);
    engine_core::Folder& deep = rig.game.create<engine_core::Folder>();
    rig.game.set_name(deep.id(), "Deep");
    rig.game.set_parent(deep.id(), inner.id());
    rig.frame(0.1);
    return deep.id();
}

void TestFilterHidesOtherRows() {
    Rig rig;
    Expect(rig.filter() != nullptr, "the explorer has a filter field");
    rig.type_filter("bet", 0.1);
    Expect(rig.cell("Beta") != nullptr, "a row whose name contains the filter shows");
    Expect(rig.cell("Alpha") == nullptr && rig.cell("Gamma") == nullptr, "rows that do not match are hidden");
    rig.type_filter("BETA", 0.2);
    Expect(rig.cell("Beta") != nullptr, "the filter ignores case");
    rig.type_filter("", 0.3);
    Expect(rig.cell("Alpha") != nullptr && rig.cell("Beta") != nullptr && rig.cell("Gamma") != nullptr,
           "an empty filter shows every row");
}

void TestFilterShowsTheWayToAMatch() {
    Rig rig;
    NestDeep(rig);
    Expect(rig.cell("Deep") == nullptr, "Deep starts in a closed folder");
    rig.type_filter("dee", 0.2);
    Expect(rig.cell("Alpha") != nullptr && rig.cell("Inner") != nullptr && rig.cell("Deep") != nullptr,
           "a match shows with the open branches above it");
    Expect(rig.cell("Beta") == nullptr, "a row with no match under it is hidden");
    rig.type_filter("", 0.3);
    Expect(rig.cell("Alpha") != nullptr && rig.cell("Inner") == nullptr, "clearing the filter closes what it opened");
}

void TestFilterEscapeLeavesTheField() {
    Rig rig;
    rig.clickRow("Gamma", 0.1);
    rig.type_filter("gam", 0.2);
    rig.filter()->requestFocus();
    rig.key(jadefx::Key::Escape);
    rig.frame(0.3);
    Expect(!rig.filter()->isFocused(), "Escape takes the focus from the filter");
    Expect(rig.filter()->getText() == "gam", "Escape keeps the filter text");
    Expect(rig.selection() == rig.pick({2}), "Escape in the filter keeps the selection");
    rig.key(jadefx::Key::Escape);
    rig.frame(0.4);
    Expect(rig.selection().empty(), "a second Escape clears the selection");
    Expect(!rig.painted("Gamma"), "the cleared row no longer shows selected");
}

void TestEscapeClearsTheSelection() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.mods = jadefx::Key::ModControl;
    rig.clickRow("Beta", 0.2);
    rig.mods = 0;
    Expect(rig.selection().size() == 2, "two rows are selected");
    rig.key(jadefx::Key::Escape);
    rig.frame(0.3);
    Expect(rig.selection().empty(), "Escape on the tree clears the selection");
}

void TestFilterClearButton() {
    Rig rig;
    auto clear_button = [&]() -> jadefx::Node* {
        const std::vector<jadefx::Node*> found = rig.explorer->getElementsByClassName("explorer-filter-clear");
        return found.empty() ? nullptr : found.front();
    };
    Expect(clear_button() != nullptr && clear_button()->isVisible() && clear_button()->isDisabled(),
           "the clear button shows disabled while the filter is empty");
    rig.filter()->requestFocus();
    rig.type_filter("bet", 0.1);
    jadefx::Node* button = clear_button();
    Expect(button != nullptr && button->isVisible() && !button->isDisabled(),
           "the clear button is enabled once there is filter text");
    if (button == nullptr) {
        return;
    }
    jadefx::TextField* field = rig.filter();
    Expect(button->getAbsoluteX() + button->getWidth() <= field->getAbsoluteX() + field->getWidth() &&
               button->getAbsoluteX() > field->getAbsoluteX() + field->getWidth() * 0.5,
           "the clear button sits at the field's right end");
    const double x = button->getAbsoluteX() + button->getWidth() * 0.5;
    const double y = button->getAbsoluteY() + button->getHeight() * 0.5;
    rig.scene->noteButton(0, true, x, y);
    rig.scene->noteButton(0, false, x, y);
    rig.frame(0.2);
    rig.frame(0.21);
    Expect(field->getText().empty(), "the clear button empties the filter");
    Expect(!field->isFocused(), "the clear button takes the focus from the filter");
    Expect(rig.cell("Alpha") != nullptr, "every row shows again");
    Expect(button->isVisible() && button->isDisabled(), "the clear button is disabled again");
}

void TestBusyPlaceSaysSo() {
    Rig rig;
    rig.game.selection().set(rig.pick({0, 1, 2}));
    rig.frame(0.2);
    // Another thread holds the place for longer than an action waits.
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread busy([&] {
        engine_core::DataModelLock lock(rig.game, engine_core::DataModelLock::Write);
        held = true;
        while (!release) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (!held) {
        std::this_thread::yield();
    }
    Expect(rig.explorer->run_on_selection(engine_core::InstanceAction::Delete), "a busy place still takes Delete");
    release = true;
    busy.join();
    Expect(rig.runs.empty() && rig.batches.empty(), "Delete runs on none of the selection, not part of it");
    Expect(rig.notices.size() == 1 && rig.notices[0].find("busy") != std::string::npos, "a busy place says so");
    Expect(rig.explorer->run_on_selection(engine_core::InstanceAction::Delete) && rig.batches.size() == 1 &&
               rig.batches[0].second == rig.pick({0, 1, 2}),
           "once the place is free, Delete runs on the whole selection");
}

void TestFilterKeepsHiddenSelection() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.type_filter("gam", 0.2);
    Expect(rig.selection() == rig.pick({0}), "filtering keeps a hidden selected instance selected");
    rig.batches.clear();
    rig.runs.clear();
    Expect(!rig.explorer->run_on_selection(engine_core::InstanceAction::Delete), "Delete skips a selected row the filter hides");
}

void TestRevealOpensTheBranch() {
    Rig rig;
    const engine_core::InstanceId deep = NestDeep(rig);
    rig.game.selection().set({deep});
    rig.frame(0.2);
    // A Set opens the branch too. Close it again to see reveal open it.
    jadefx::TreeView* tree = rig.tree();
    Expect(tree != nullptr, "the explorer's tree is found");
    if (tree != nullptr) {
        tree->getRoot()->getChildren()[0]->setExpanded(false);
    }
    rig.frame(0.25);
    Expect(rig.cell("Deep") == nullptr, "closing Alpha hides Deep");
    rig.explorer->reveal_selection();
    rig.frame(0.3);
    rig.frame(0.31);
    Expect(rig.painted("Deep"), "reveal shows the selected row");
}

void TestRevealScrolls() {
    Rig rig;
    engine_core::InstanceId last = 0;
    for (int i = 0; i < 60; ++i) {
        engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
        rig.game.set_name(folder.id(), "Row" + std::to_string(i));
        rig.game.set_parent(folder.id(), rig.game.id());
        last = folder.id();
    }
    rig.frame(0.1);
    rig.game.selection().set({last});
    rig.frame(0.2);
    jadefx::TreeView* tree = rig.tree();
    if (tree != nullptr) {
        tree->scrollTo(0);
    }
    rig.frame(0.25);
    Expect(!rig.in_view("Row59"), "the last row starts below the view");
    Expect(rig.explorer->reveal_selection(), "reveal runs with a selection");
    rig.frame(0.3);
    rig.frame(0.31);
    Expect(rig.in_view("Row59"), "reveal scrolls the selected row into view");
    rig.game.selection().set({});
    rig.frame(0.4);
    Expect(!rig.explorer->reveal_selection(), "reveal does nothing with no selection");
}

void TestRevealClearsAHidingFilter() {
    Rig rig;
    rig.clickRow("Alpha", 0.1);
    rig.type_filter("gam", 0.2);
    rig.explorer->reveal_selection();
    rig.frame(0.3);
    rig.frame(0.31);
    Expect(rig.filter()->getText().empty(), "reveal clears a filter that hides the selection");
    Expect(rig.painted("Alpha"), "the revealed row shows selected");
}

engine_core::DataModel& CreateFolder(engine_core::DataModel& world) {
    return world.create<engine_core::Folder>();
}

// An insert the studio could not make, as in a full place, says why.
void TestRefusedInsertSaysWhy() {
    engine_core::register_lua_creatable("Folder", CreateFolder);
    Rig rig;
    rig.refuse_inserts = "The place is full.";
    const std::vector<jadefx::Node*> found = rig.explorer->getElementsByClassName("explorer-add");
    if (found.empty()) {
        Expect(false, "the header has a + button");
        return;
    }
    jadefx::Node* add = found.front();
    const double x = add->getAbsoluteX() + add->getWidth() * 0.5;
    const double y = add->getAbsoluteY() + add->getHeight() * 0.5;
    rig.scene->noteButton(0, true, x, y, 0);
    rig.scene->noteButton(0, false, x, y, 0);
    rig.frame(0.1);
    rig.key(jadefx::Key::Enter);
    rig.frame(0.2);
    rig.frame(0.3);
    Expect(rig.inserts.size() == 1, "the refused insert was asked for");
    Expect(rig.notices.size() == 1 && rig.notices[0] == "The place is full.", "a refused insert says why");
    Expect(!rig.painted("Made"), "a refused insert shows nothing new");
}

void TestHeaderInsertsUnderTheRoot() {
    // The engine's registrars are not linked in here, so the class list needs one.
    engine_core::register_lua_creatable("Folder", CreateFolder);
    Rig rig;
    const std::vector<jadefx::Node*> found = rig.explorer->getElementsByClassName("explorer-add");
    jadefx::TextField* filter = rig.filter();
    Expect(found.size() == 1 && filter != nullptr, "the header has a filter and a + button");
    if (found.empty() || filter == nullptr) {
        return;
    }
    jadefx::Node* add = found.front();
    Expect(add->getAbsoluteY() >= filter->getAbsoluteY() + filter->getHeight(),
           "the + sits under the filter");
    Expect(add->getAbsoluteX() + add->getWidth() > filter->getAbsoluteX() + filter->getWidth() - 1,
           "the + sits at the header's right edge");
    jadefx::TreeView* view = rig.tree();
    Expect(view != nullptr && add->getAbsoluteY() + add->getHeight() <= view->getAbsoluteY(),
           "the + sits above the rows");
    const double x = add->getAbsoluteX() + add->getWidth() * 0.5;
    const double y = add->getAbsoluteY() + add->getHeight() * 0.5;
    rig.scene->noteButton(0, true, x, y, 0);
    rig.scene->noteButton(0, false, x, y, 0);
    rig.frame(0.1);
    // The class list takes the focus with its first class highlighted.
    rig.key(jadefx::Key::Enter);
    rig.frame(0.2);
    Expect(rig.inserts.size() == 1, "Enter in the list inserts once");
    Expect(!rig.inserts.empty() && rig.inserts.front().second == rig.game.id(), "the header's + inserts under game");
    rig.frame(0.3);
    Expect(rig.painted("Made"), "the new instance shows selected");
    jadefx::Node* focused = rig.scene->focusedNode();
    Expect(focused != nullptr && focused == rig.tree(), "the tree takes the focus after an insert");
    // Escape reaches the tree without another click.
    rig.key(jadefx::Key::Escape);
    rig.frame(0.4);
    Expect(rig.selection().empty(), "Escape clears the new instance's selection");
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
    TestRebuildKeepsTheSelection();
    TestDeleteRunsOnTheSelection();
    TestCutRunsOnTheSelection();
    TestCutSet();
    TestDragIntoAnotherRow();
    TestDragBesideAnotherRow();
    TestDragCarriesTheSelection();
    TestDragRefusesItsOwnChild();
    TestMoveSet();
    TestFilterHidesOtherRows();
    TestFilterShowsTheWayToAMatch();
    TestFilterEscapeLeavesTheField();
    TestEscapeClearsTheSelection();
    TestFilterClearButton();
    TestBusyPlaceSaysSo();
    TestFilterKeepsHiddenSelection();
    TestRevealOpensTheBranch();
    TestRevealScrolls();
    TestRevealClearsAHidingFilter();
    TestHeaderInsertsUnderTheRoot();
    TestRefusedInsertSaysWhy();
    if (gFailures == 0) {
        std::printf("explorer tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d explorer tests failed\n", gFailures);
    return 1;
}
