#include "ide/IdeConflicts.hpp"
#include "ide/IdeDock.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"
#include "SelectionService.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

// A translation's 16 numbers as a conflict row shows them.
std::string Moved(const char* xyz) { return std::string("1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, ") + xyz + ", 1"; }

engine_core::SaveConflict Row(const char* guid, const char* name, const char* key, const std::string& studio,
                              const std::string& disk,
                              engine_core::SaveConflict::Kind kind = engine_core::SaveConflict::Kind::EditedOutside) {
    engine_core::SaveConflict row;
    row.guid = guid;
    row.path = std::string("src/") + name + "." + guid + ".json";
    row.kind = kind;
    row.key = key;
    row.studio = studio;
    row.disk = disk;
    row.name = name;
    row.where = "game";
    return row;
}

// The window on its own: its groups, picks, and buttons.
int RunConflictsPaneTests() {
    int failures = 0;
    auto expect = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };
    std::vector<engine_core::DiskChoice> applied;
    std::string selected;
    int refreshed = 0;
    ide::ConflictsHost host;
    host.apply = [&applied](const std::vector<engine_core::DiskChoice>& choices) { applied = choices; };
    host.refresh = [&refreshed] { ++refreshed; };
    host.select = [&selected](const std::string& guid) { selected = guid; };
    host.class_of = [](const std::string&) { return std::string("GameObject"); };
    auto pane = jadefx::make<ide::IdeConflicts>(host);

    const std::vector<engine_core::SaveConflict> rows = {
        Row("p", "Part", "Transform", Moved("1, 0, 0"), Moved("0, 0, 1")),
        Row("p", "Part", "VisualOnly", "(default)", "true"),
        Row("b", "Bounce", "Source", "2: print(2)", "2: print(\"hi\")"),
        Row("c", "Crate", "", "changed in the studio", "deleted", engine_core::SaveConflict::Kind::DeletedOutside),
    };
    pane->setConflicts(rows);
    expect(pane->summary() == "4 conflicts in 3 instances", "the header counts rows and instances");
    const auto& groups = pane->tree().getRoot()->getChildren();
    expect(groups.size() == 3, "each instance is a group");
    expect(groups.size() == 3 && groups.items()[0]->getChildren().size() == 2, "with a row per property under it");
    expect(!pane->pick(0).has_value(), "a row starts with no pick");

    pane->choose(0, true);
    pane->chooseInstance("b", false);
    std::vector<engine_core::DiskChoice> choices = pane->choices();
    expect(choices.size() == 2, "only picked rows are choices");
    expect(choices.size() == 2 && choices[0].disk && choices[0].conflict.key == "Transform", "Disk for Part's Transform");
    expect(choices.size() == 2 && !choices[1].disk && choices[1].conflict.guid == "b", "IDE for Bounce");
    expect(pane->chosenText() == "2 of 4 chosen", "the footer counts the picks");

    // A row's own Disk button picks it.
    const std::vector<jadefx::Node*> disk_buttons = pane->getElementsByClassName("conflict-disk");
    expect(disk_buttons.size() == 7, "a Disk button on every group and every row");
    if (auto* crate = dynamic_cast<jadefx::ButtonBase*>(disk_buttons.back())) {
        crate->fire();
    }
    expect(pane->pick(3) == std::optional<bool>(true), "the Crate row's Disk button picks Disk");
    if (auto* crate = dynamic_cast<jadefx::ButtonBase*>(disk_buttons.back())) {
        crate->fire();
    }
    expect(!pane->pick(3).has_value(), "clicking it again clears the pick");

    // The same rows keep their picks; a row whose value changed starts over.
    std::vector<engine_core::SaveConflict> again = rows;
    again[1].disk = "false";
    pane->choose(1, true);
    pane->setConflicts(again);
    expect(pane->pick(0) == std::optional<bool>(true), "an unchanged row keeps its pick");
    expect(!pane->pick(1).has_value(), "a changed row loses its pick");
    expect(pane->pick(2) == std::optional<bool>(false), "and the rest keep theirs");

    pane->chooseAll(false);
    pane->apply();
    expect(applied.size() == 4, "Apply sends every picked row");
    expect(applied.size() == 4 && !applied[3].disk, "All IDE picked IDE for each");

    const std::vector<jadefx::Node*> refresh = pane->getElementsByClassName("conflicts-refresh");
    if (!refresh.empty()) {
        if (auto* button = dynamic_cast<jadefx::ButtonBase*>(refresh.front())) {
            button->fire();
        }
    }
    expect(refreshed == 1, "Refresh checks again");

    pane->showInstance(groups.items()[1].get());
    expect(selected == "b", "a row shows its instance in the explorers");

    pane->setConflicts({});
    expect(pane->summary() == "No conflicts. Changes made outside the studio load when you switch back to it.",
           "an empty list says so");
    return failures;
}

}  // namespace

// reveal_window shows any dockable pane: it selects the pane's tab, and docks
// a closed pane through the open it is given.
int RunConflictsTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    int failures = RunConflictsPaneTests();
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
    // The built-in layout gives Search a dock of its own. In with an explorer,
    // it has a tab to go behind.
    if (search != nullptr && dock != nullptr && dock->tabs()->getTabs().size() == 1) {
        for (jadefx::Node* node : scene.getElementsByClassName("explorer-pane")) {
            ide::IdeDock* beside = DockOf(node);
            if (beside != nullptr && beside != dock) {
                beside->take(TabOf(*dock, search));
                dock = beside;
                break;
            }
        }
    }
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

    // Checks: what only the disk changed loads; a conflict goes to the window and the ribbon.
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("anarchy-sync-test-" + std::to_string(stamp));
    const fs::path root = folder / "SyncPlace";
    std::string part_file;
    layout.simulation().on_simulation([&](engine_core::DataModel&) {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.datamodel();
        engine_core::GameObject& part = game.create_game_object();
        game.set_name(part.id(), "Part");
        game.set_parent(part.id(), game.scene_service("Workspace"));
        project.save();
        part_file = "src/Workspace.workspace/Part." + game.guid(part.id()) + ".json";
    });
    layout.open_project_at(root);
    // Moves Part on disk: its file's Transform becomes a translation to (x, y, z).
    auto set_disk = [&](float x, float y, float z) {
        std::ifstream in(root / part_file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        engine_core::JsonValue doc;
        std::string error;
        engine_core::parse_json(bytes, doc, error);
        const engine_core::Matrix4 moved = engine_core::matrix4_translation(x, y, z);
        doc.set("Transform", engine_core::json_floats(moved.m, 16));
        std::ofstream out(root / part_file, std::ios::binary | std::ios::trunc);
        out << engine_core::write_json(doc);
    };
    auto part_position = [&layout] {
        engine_core::Vec3 position;
        layout.simulation().on_simulation([&position](engine_core::DataModel& game) {
            position = game.game_object(game.find_first_child(game.scene_service("Workspace"), "Part"))->position();
        });
        return position;
    };
    auto part_has = [&layout](const char* key) {
        bool found = false;
        layout.simulation().on_simulation([&found, key](engine_core::DataModel& game) {
            engine_core::PropertyBag bag;
            game.instance(game.find_first_child(game.scene_service("Workspace"), "Part"))->save_properties(bag);
            found = engine_core::bag_find(bag, key) != nullptr;
        });
        return found;
    };
    // Moves Part in the studio.
    auto move_part = [&layout](float x, float y, float z) {
        layout.simulation().on_simulation([x, y, z](engine_core::DataModel& game) {
            // As an edit made in the studio: one undo step, so there is something to save.
            const std::optional<std::string> step = game.history().try_begin_recording("Move");
            game.game_object(game.find_first_child(game.scene_service("Workspace"), "Part"))
                ->set_position(engine_core::Vec3{x, y, z});
            if (step) {
                game.history().finish_recording(*step, engine_core::FinishRecordingOperation::Commit);
            }
        });
    };
    auto count_shown = [&scene] {
        const jadefx::Node* badge = scene.getElementById("conflicts-count");
        return badge != nullptr && badge->isVisible();
    };

    set_disk(2, 2, 2);
    layout.check_disk();
    expect(part_has("Transform"), "a change only the disk made loads when the studio checks");
    expect(!layout.has_unsaved_changes(), "and leaves nothing to save");
    expect(!count_shown(), "with no conflict, the status bar shows no count");

    set_disk(0, 1, 0);
    scene.noteWindowFocus(false);
    layout.flushFrame();
    scene.noteWindowFocus(true);
    layout.flushFrame();
    expect(part_position().y == 1.f && part_position().x == 0.f, "coming back to the window checks the disk");

    // A Properties value being typed in holds that check back, as a rename does.
    layout.simulation().on_simulation(
        [](engine_core::DataModel& game) { game.selection().set({game.find_first_child(game.scene_service("Workspace"), "Part")}); });
    scene.layout(1280, 800, 4.5);
    scene.layout(1280, 800, 4.55);
    jadefx::TextField* typing = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("properties-field")) {
        if (typing == nullptr) {
            typing = dynamic_cast<jadefx::TextField*>(node);
        }
    }
    expect(typing != nullptr, "Properties shows the part's fields");
    if (typing != nullptr) {
        scene.requestFocus(typing);
        set_disk(1, 1, 0);
        scene.noteWindowFocus(false);
        layout.flushFrame();
        scene.noteWindowFocus(true);
        layout.flushFrame();
        expect(scene.focusedNode() == typing, "the field keeps the focus with the window");
        expect(part_position().x == 0.f, "a check waits while a Properties field is being typed in");
        scene.releaseFocus(typing);
        layout.flushFrame();
        expect(part_position().x == 1.f && part_position().y == 1.f, "and runs once the field lets go");
    }

    move_part(0.25f, 0.5f, 0.75f);
    set_disk(1, 0, 0);
    layout.check_disk();
    expect(count_shown(), "a conflict shows a count on the status bar");
    const auto* count = dynamic_cast<const jadefx::Label*>(scene.getElementById("conflicts-count-text"));
    expect(count != nullptr && count->getText() == "1", "counting the rows");

    layout.show_conflicts();
    ide::IdeConflicts* pane = scene.getElementsByClassName("conflicts-pane").empty()
                                  ? nullptr
                                  : dynamic_cast<ide::IdeConflicts*>(scene.getElementsByClassName("conflicts-pane").front());
    ide::IdeDock* home = DockOf(pane);
    const std::shared_ptr<jadefx::Tab> pane_tab = home != nullptr ? TabOf(*home, pane) : nullptr;
    expect(pane_tab && pane_tab->isSelected(), "the count's window docks and comes to the front");
    expect(pane != nullptr && pane->conflicts().size() == 1, "listing the conflict");
    expect(pane_tab && pane_tab->getText() == "Conflicts (1)", "its tab counts the conflict");
    if (pane != nullptr && pane->conflicts().size() == 1) {
        pane->choose(0, true);
        pane->apply();
        expect(part_position().x == 1.f && part_position().z == 0.f, "Apply takes the disk's side");
        expect(pane->conflicts().empty() && !count_shown(), "and the row and the count go");
        expect(pane_tab && pane_tab->getText() == "Conflicts", "and the tab drops its count");
    }

    move_part(0.25f, 0.5f, 0.75f);
    set_disk(0, 0, 1);
    if (pane_tab) {
        for (const std::shared_ptr<jadefx::Tab>& other : home->tabs()->getTabs().items()) {
            if (other != pane_tab) {
                home->tabs()->select(other);
                break;
            }
        }
    }
    scene.noteKey(jadefx::Key::S, true, false, jadefx::Key::ModControl);
    scene.noteKey(jadefx::Key::S, false, false, 0);
    auto* show = dynamic_cast<jadefx::ButtonBase*>(scene.getElementById("save-conflict-show"));
    expect(show != nullptr, "Save with a conflict offers the Conflicts window");
    if (show != nullptr) {
        show->fire();
    }
    expect(pane_tab && pane_tab->isSelected(), "Show Conflicts brings it to the front");
    const auto* cancel = scene.getElementById("save-conflict-cancel");
    expect(cancel == nullptr, "and closes the question");

    // In the side dock where it opens, each row's toggle and the header's buttons fit the pane.
    scene.layout(1280, 800, 5.0);
    scene.layout(1280, 800, 5.05);
    if (pane != nullptr) {
        const double right = pane->getAbsoluteX() + pane->getWidth() + 0.5;
        bool fits = !pane->getElementsByClassName("conflict-disk").empty();
        for (jadefx::Node* button : pane->getElementsByClassName("conflict-disk")) {
            fits = fits && button->getAbsoluteX() + button->getWidth() <= right;
        }
        expect(fits, "every row's Disk toggle is inside the pane");
        const std::vector<jadefx::Node*> all_disk = pane->getElementsByClassName("conflicts-all-disk");
        expect(!all_disk.empty() && all_disk.front()->getAbsoluteX() + all_disk.front()->getWidth() <= right,
               "and so is All Disk");
        // The toggles and the buttons sit at the right, the toggles lined up down the list.
        const double middle = pane->getAbsoluteX() + pane->getWidth() * 0.5;
        auto right_edge = [](const jadefx::Node* node) { return node->getAbsoluteX() + node->getWidth(); };
        const std::vector<jadefx::Node*> disks = pane->getElementsByClassName("conflict-disk");
        bool lined_up = !disks.empty() && disks.front()->getAbsoluteX() > middle;
        for (const jadefx::Node* disk : disks) {
            lined_up = lined_up && std::abs(right_edge(disk) - right_edge(disks.front())) < 1.0;
        }
        expect(lined_up, "the IDE/Disk toggles line up at the right");
        const std::vector<jadefx::Node*> apply = pane->getElementsByClassName("conflicts-apply");
        expect(!all_disk.empty() && all_disk.front()->getAbsoluteX() > middle, "All IDE and All Disk sit at the right");
        expect(!apply.empty() && apply.front()->getAbsoluteX() > middle && right_edge(apply.front()) <= right,
               "and so do Refresh and Apply");
    }

    // During a test, Apply waits for Stop, and Save does not point to the window.
    auto key = [&scene](int code, int mods) {
        scene.noteKey(code, true, false, mods);
        scene.noteKey(code, false, false, 0);
    };
    key(jadefx::Key::F5, 0);
    if (pane != nullptr) {
        const std::vector<jadefx::Node*> apply = pane->getElementsByClassName("conflicts-apply");
        pane->chooseAll(true);
        expect(!apply.empty() && apply.front()->isDisabled(), "Apply is off during a test");
        expect(pane->chosenText() == "Stop the test to apply", "and says why");
    }
    key(jadefx::Key::S, jadefx::Key::ModControl);
    expect(scene.getElementById("save-conflict-cancel") != nullptr, "Save during a test still asks");
    expect(scene.getElementById("save-conflict-show") == nullptr, "without pointing to the Conflicts window");
    expect(scene.getElementById("save-conflict-overwrite") == nullptr, "or offering to overwrite");
    if (auto* close = dynamic_cast<jadefx::ButtonBase*>(scene.getElementById("save-conflict-cancel"))) {
        close->fire();
    }
    key(jadefx::Key::F5, jadefx::Key::ModShift);
    if (pane != nullptr) {
        const std::vector<jadefx::Node*> apply = pane->getElementsByClassName("conflicts-apply");
        expect(!apply.empty() && !apply.front()->isDisabled(), "after Stop, Apply is back");
    }

    // A project.json that does not parse, as in a merge, is no crash.
    std::string project_json;
    {
        std::ifstream in(root / "project.json", std::ios::binary);
        project_json.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    {
        std::ofstream out(root / "project.json", std::ios::binary | std::ios::trunc);
        out << "<<<<<<< HEAD\n{";
    }
    layout.check_disk();
    move_part(0.5f, 0.5f, 0.5f);
    layout.flushFrame();
    expect(layout.has_unsaved_changes(), "the place still says it has changes");
    {
        std::ofstream out(root / "project.json", std::ios::binary | std::ios::trunc);
        out << project_json;
    }
    layout.check_disk();

    // File > New starts with no conflicts.
    expect(count_shown(), "a conflict is still listed");
    key(jadefx::Key::N, jadefx::Key::ModControl);
    if (auto* discard = dynamic_cast<jadefx::ButtonBase*>(scene.getElementById("unsaved-discard"))) {
        discard->fire();
    }
    expect(!count_shown(), "a new place shows no conflict count");
    expect(pane == nullptr || pane->conflicts().empty(), "and lists no conflicts");

    std::error_code error;
    fs::remove_all(folder, error);
    return failures;
}
