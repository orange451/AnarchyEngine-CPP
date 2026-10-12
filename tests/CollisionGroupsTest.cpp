#include "ide/CollisionGroupsModel.hpp"
#include "ide/IdeCollisionGroups.hpp"
#include "ide/NodeClasses.hpp"

#include "ChangeHistoryService.hpp"
#include "CollisionGroupEdits.hpp"
#include "CollisionGroups.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "PhysicsObject.hpp"
#include "ScriptRuntime.hpp"
#include "SelectionService.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The Collision Groups pane in a headless scene, and the data behind it: what
// read_collision_groups shows, and what each run_* write does to the place's
// groups and bodies (docs/superpowers/specs/2026-10-12-collision-groups-editor-design.md).
namespace {

using engine_core::CollisionGroups;
using engine_core::InstanceId;
using FilterType = CollisionGroups::FilterType;

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

void Expect(bool condition, const std::string& message) { Expect(condition, message.c_str()); }

struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

// A place with Players and Ghosts registered, two bodies in Players, one in
// Ghosts, two in Default, and one naming Crates, which no group has. One
// Players body is in a Folder.
struct Rig {
    SimRole role;
    engine_core::ScriptRuntime runtime;
    engine_core::Game game;
    InstanceId folder = 0;
    InstanceId p1 = 0;
    InstanceId p2 = 0;
    InstanceId ghost = 0;
    InstanceId d1 = 0;
    InstanceId d2 = 0;
    InstanceId crate = 0;

    Rig() {
        CollisionGroups& groups = game.collision_groups();
        groups.register_group("Players", FilterType::Exclude);
        groups.register_group("Ghosts", FilterType::Include);
        groups.set_collidable("Players", "Players", false);
        groups.set_collidable("Ghosts", "Players", true);
        engine_core::Folder& made = game.create<engine_core::Folder>();
        folder = made.id();
        game.set_name(folder, "Team");
        game.set_parent(folder, game.scene_service("Workspace"));
        p1 = body("P1", "Players", folder);
        p2 = body("P2", "Players", game.scene_service("Workspace"));
        ghost = body("Ghost", "Ghosts", game.scene_service("Workspace"));
        d1 = body("D1", "Default", game.scene_service("Workspace"));
        d2 = body("D2", "Default", game.scene_service("Storage"));
        crate = body("Crate", "Crates", game.scene_service("Workspace"));
    }

    InstanceId body(const char* name, const char* group, InstanceId parent) {
        engine_core::PhysicsObject& object = game.create<engine_core::PhysicsObject>();
        game.set_name(object.id(), name);
        object.set_collision_group(group);
        game.set_parent(object.id(), parent);
        return object.id();
    }

    std::string group_of(InstanceId id) {
        auto* object = dynamic_cast<engine_core::PhysicsBase*>(game.instance(id));
        return object != nullptr ? object->collision_group() : std::string("?");
    }

    CollisionGroups& groups() { return game.collision_groups(); }
};

void M1_view() {
    Rig rig;
    const ide::CollisionGroupsView view = ide::read_collision_groups(rig.game);
    Expect(view.groups.size() == 3, "M1 three groups");
    Expect(view.groups[0].name == "Default" && view.groups[1].name == "Players" && view.groups[2].name == "Ghosts",
           "M1 in registration order, Default first");
    Expect(view.groups[2].filter_type == FilterType::Include, "M1 Ghosts is Include");
    Expect(view.groups[0].bodies == 2 && view.groups[1].bodies == 2 && view.groups[2].bodies == 1,
           "M1 bodies are counted by exact name, under Folders and in Storage too");
    Expect(view.collide.size() == 3 && view.collide[0].size() == 1 && view.collide[2].size() == 3,
           "M1 the matrix is the lower triangle");
    Expect(view.collide[0][0], "M1 Default collides with itself");
    Expect(!view.collide[1][1], "M1 Players does not collide with itself");
    Expect(view.collide[2][1], "M1 Ghosts collides with Players");
    Expect(!view.collide[2][0] && !view.collide[2][2], "M1 Ghosts, Include, collides with nothing else");
    Expect(view.unregistered.size() == 1 && view.unregistered[0].name == "Crates" &&
               view.unregistered[0].bodies == 1,
           "M1 Crates is named but not registered");
    Expect(!view.playing, "M1 not playing");
    Expect(view.index_of("Ghosts") == 2 && view.index_of("Nope") == -1, "M1 index_of");
    const std::vector<InstanceId> players = ide::collision_group_bodies(rig.game, "Players");
    Expect(players.size() == 2 && std::count(players.begin(), players.end(), rig.p1) == 1 &&
               std::count(players.begin(), players.end(), rig.p2) == 1,
           "M1 collision_group_bodies lists the group's bodies");
}

void M2_new_group() {
    Rig rig;
    std::string made;
    Expect(!ide::run_new_group(rig.game, made), "M2 a new group");
    Expect(made == "New Group" && rig.groups().registered("New Group"), "M2 named New Group");
    Expect(!ide::run_new_group(rig.game, made), "M2 another");
    Expect(made == "New Group 2", "M2 then New Group 2");
    rig.game.history().undo();
    Expect(!rig.groups().registered("New Group 2") && rig.groups().registered("New Group"),
           "M2 each is one undo step");
}

void M3_rename() {
    Rig rig;
    Expect(!ide::run_rename_group(rig.game, "Players", "  Heroes  "), "M3 rename, trimmed");
    Expect(rig.groups().registered("Heroes") && !rig.groups().registered("Players"), "M3 renamed");
    Expect(rig.group_of(rig.p1) == "Heroes" && rig.group_of(rig.p2) == "Heroes", "M3 its bodies moved with it");
    Expect(!rig.groups().collidable("Heroes", "Heroes"), "M3 its settings stayed");
    const std::optional<std::string> empty = ide::run_rename_group(rig.game, "Heroes", "   ");
    Expect(empty && *empty == "A collision group needs a name", "M3 an empty name is refused");
    const std::optional<std::string> taken = ide::run_rename_group(rig.game, "Heroes", "Ghosts");
    Expect(taken && *taken == "A collision group named \"Ghosts\" already exists", "M3 a taken name is refused");
    rig.game.history().undo();
    Expect(rig.groups().registered("Players") && rig.group_of(rig.p1) == "Players",
           "M3 one undo step puts the name and the bodies back");
}

void M4_delete() {
    Rig rig;
    Expect(!ide::run_delete_group(rig.game, "Players"), "M4 delete");
    Expect(!rig.groups().registered("Players"), "M4 unregistered");
    Expect(rig.group_of(rig.p1) == "Default" && rig.group_of(rig.p2) == "Default", "M4 its bodies are Default now");
    rig.game.history().undo();
    Expect(rig.groups().registered("Players") && rig.group_of(rig.p1) == "Players" &&
               !rig.groups().collidable("Players", "Players"),
           "M4 one undo step puts the group, its settings, and its bodies back");
    const std::optional<std::string> base = ide::run_delete_group(rig.game, "Default");
    Expect(base && *base == "Default cannot be unregistered", "M4 Default stays");
}

void M5_collidable_and_filter() {
    Rig rig;
    Expect(!ide::run_set_collidable(rig.game, "Ghosts", "Default", true), "M5 set collidable");
    Expect(rig.groups().collidable("Ghosts", "Default"), "M5 now they collide");
    Expect(!ide::run_set_filter_type(rig.game, "Players", FilterType::Include), "M5 set filter type");
    Expect(!rig.groups().collidable("Players", "Default"), "M5 Players is Include now");
    rig.game.history().undo();
    Expect(rig.groups().collidable("Players", "Default"), "M5 undo the filter type");
    rig.game.history().undo();
    Expect(!rig.groups().collidable("Ghosts", "Default"), "M5 undo the pair");
}

void M6_assign() {
    Rig rig;
    std::size_t moved = 0;
    Expect(!ide::run_assign_group(rig.game, {rig.folder, rig.d1, rig.ghost}, "Ghosts", moved), "M6 assign");
    Expect(moved == 2, "M6 two bodies changed: P1 under the Folder, and D1");
    Expect(rig.group_of(rig.p1) == "Ghosts" && rig.group_of(rig.d1) == "Ghosts", "M6 they are Ghosts");
    Expect(rig.group_of(rig.p2) == "Players", "M6 others stay");
    rig.game.history().undo();
    Expect(rig.group_of(rig.p1) == "Players" && rig.group_of(rig.d1) == "Default", "M6 one undo step");
    const std::optional<std::string> unknown = ide::run_assign_group(rig.game, {rig.d1}, "Nope", moved);
    Expect(unknown.has_value(), "M6 an unregistered group is refused");
}

void M7_register() {
    Rig rig;
    Expect(!ide::run_register_groups(rig.game, {"Crates", "Barrels"}), "M7 register");
    Expect(rig.groups().registered("Crates") && rig.groups().registered("Barrels"), "M7 both are groups now");
    Expect(ide::read_collision_groups(rig.game).unregistered.empty(), "M7 no unregistered names are left");
    rig.game.history().undo();
    Expect(!rig.groups().registered("Crates") && !rig.groups().registered("Barrels"), "M7 one undo step");
}

void M9_core_and_empty_names() {
    Rig rig;
    const InstanceId plugin = rig.body("PluginBody", "Players", rig.game.core());
    const InstanceId blank = rig.body("Blank", "", rig.game.scene_service("Workspace"));
    const ide::CollisionGroupsView view = ide::read_collision_groups(rig.game);
    Expect(view.groups[1].bodies == 2, "M9 a body in Core is not the place's, so it is not counted");
    Expect(view.unregistered.size() == 1 && view.unregistered[0].name == "Crates",
           "M9 an empty name is Default's, not an unregistered group");
    Expect(!ide::run_rename_group(rig.game, "Players", "Heroes"), "M9 rename");
    Expect(rig.group_of(plugin) == "Players", "M9 a body in Core keeps its group: Stop never puts Core back");
    Expect(rig.group_of(blank).empty(), "M9 and the blank one stays blank");
}

void M8_fresh_name() {
    CollisionGroups groups;
    Expect(ide::fresh_group_name(groups) == "New Group", "M8 New Group first");
    groups.register_group("New Group", FilterType::Exclude);
    groups.register_group("New Group 2", FilterType::Exclude);
    Expect(ide::fresh_group_name(groups) == "New Group 3", "M8 then the next number free");
}


constexpr double kWidth = 1100;
constexpr double kHeight = 640;

// The Rig with the pane open. The host writes straight to the game, standing
// in for the simulation thread, and notes what it was asked.
struct PaneRig : Rig {
    std::vector<std::string> notices;
    std::vector<std::string> calls;
    std::shared_ptr<ide::IdeCollisionGroups> pane;
    std::shared_ptr<jadefx::Scene> scene;
    double time = 0;

    PaneRig() {
        ide::CollisionGroupsHost host;
        auto note = [this](std::optional<std::string> refused) {
            if (refused) {
                notices.push_back(*refused);
            }
        };
        host.add = [this, note](std::shared_ptr<ide::NewGroupResult> result) {
            calls.push_back("add");
            std::string made;
            if (std::optional<std::string> refused = ide::run_new_group(game, made)) {
                result->error = *refused;
            }
            result->name = made;
            result->done = true;
        };
        host.rename = [this, note](std::string from, std::string to) {
            calls.push_back("rename " + from + " -> " + to);
            note(ide::run_rename_group(game, from, to));
        };
        host.remove = [this, note](std::string name) {
            calls.push_back("remove " + name);
            note(ide::run_delete_group(game, name));
        };
        host.set_filter_type = [this, note](std::string name, FilterType type) {
            calls.push_back("filter " + name + (type == FilterType::Include ? " Include" : " Exclude"));
            note(ide::run_set_filter_type(game, name, type));
        };
        host.set_collidable = [this, note](std::string a, std::string b, bool collidable) {
            calls.push_back("collidable " + a + " " + b + (collidable ? " true" : " false"));
            note(ide::run_set_collidable(game, a, b, collidable));
        };
        host.assign_selection = [this, note](std::string name) {
            calls.push_back("assign " + name);
            std::size_t moved = 0;
            note(ide::run_assign_group(game, game.selection().get(), name, moved));
        };
        host.select_bodies = [this](std::string name) {
            calls.push_back("select " + name);
            game.selection().set(ide::collision_group_bodies(game, name));
        };
        host.register_names = [this, note](std::vector<std::string> names) {
            calls.push_back("register " + std::to_string(names.size()));
            note(ide::run_register_groups(game, names));
        };
        host.notice = [this](std::string text) { notices.push_back(std::move(text)); };
        pane = jadefx::make<ide::IdeCollisionGroups>(game, std::move(host));
        pane->setPrefWidthRatio(1);
        pane->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(pane, kWidth, kHeight);
        frames(2);
    }

    void frames(int count = 1) {
        for (int i = 0; i < count; ++i) {
            scene->layout(kWidth, kHeight, time);
            time += 0.05;
        }
    }

    void move_to(jadefx::Node* node) {
        if (node != nullptr) {
            scene->noteMove(node->getAbsoluteX() + node->getWidth() * 0.5, node->getAbsoluteY() + node->getHeight() * 0.5);
            frames();
        }
    }

    // Presses and releases the middle of node. A move away first, so two
    // clicks on one spot are not one double-click.
    void click(jadefx::Node* node, const char* what) {
        Expect(node != nullptr, std::string("the ") + what + " is on screen");
        if (node == nullptr) {
            return;
        }
        scene->noteMove(1, 1);
        const double x = node->getAbsoluteX() + node->getWidth() * 0.5;
        const double y = node->getAbsoluteY() + node->getHeight() * 0.5;
        scene->noteMove(x, y);
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        frames(2);
    }

    void key(int code) {
        scene->noteKey(code, true, false, 0);
        frames();
    }

    bool on(jadefx::Node* node) const { return node != nullptr && ide::has_class(*node, "on"); }

    bool called(const std::string& call) const {
        return std::find(calls.begin(), calls.end(), call) != calls.end();
    }

    std::string subtitle() const { return pane->subtitle() != nullptr ? pane->subtitle()->getText() : std::string(); }
};

void U1_rows_and_matrix() {
    PaneRig rig;
    Expect(rig.pane->rowNode("Default") && rig.pane->rowNode("Players") && rig.pane->rowNode("Ghosts"),
           "U1 a row for each group");
    Expect(rig.subtitle() == "3 of 62 groups \xC2\xB7 6 bodies", "U1 the subtitle counts groups and bodies (" +
                                                                   rig.subtitle() + ")");
    Expect(rig.on(rig.pane->cellNode("Players", "Ghosts")) && rig.on(rig.pane->cellNode("Ghosts", "Players")),
           "U1 Players and Ghosts collide, either order");
    Expect(!rig.on(rig.pane->cellNode("Players", "Players")), "U1 Players does not collide with itself");
    Expect(rig.on(rig.pane->cellNode("Default", "Default")), "U1 Default collides with itself");
    Expect(rig.pane->cellNode("Default", "Ghosts") != nullptr && !rig.on(rig.pane->cellNode("Default", "Ghosts")),
           "U1 Ghosts, Include, does not collide with Default");
    Expect(rig.pane->columnLabel("Default") && rig.pane->columnLabel("Players") && rig.pane->columnLabel("Ghosts"),
           "U1 every column is named");
    // Each column's name sits just above its first cell.
    jadefx::Node* label = rig.pane->columnLabel("Players");
    jadefx::Node* top = rig.pane->cellNode("Players", "Players");
    Expect(label != nullptr && top != nullptr && label->getAbsoluteY() < top->getAbsoluteY() &&
               label->getAbsoluteY() + label->getHeight() > top->getAbsoluteY() - 40 &&
               label->getAbsoluteX() <= top->getAbsoluteX() + top->getWidth() * 0.5 &&
               label->getAbsoluteX() + label->getWidth() > top->getAbsoluteX() + top->getWidth() * 0.5,
           "U1 Players' name is over Players' column");
    Expect(rig.pane->unregisteredRow() != nullptr, "U1 Crates is named but not registered");
    Expect(rig.pane->emptyHint() == nullptr, "U1 no empty hint with groups");
    Expect(rig.pane->playBanner() == nullptr, "U1 no play banner while stopped");
    Expect(rig.pane->deleteButton("Default") == nullptr && rig.pane->deleteButton("Players") != nullptr,
           "U1 Default has no delete");
    Expect(rig.pane->nameLabel("Ghosts") != nullptr && rig.pane->nameLabel("Ghosts")->getText() == "Ghosts",
           "U1 rows show names");
}

void U2_cell_click_switches() {
    PaneRig rig;
    rig.click(rig.pane->cellNode("Ghosts", "Default"), "Ghosts/Default cell");
    Expect(rig.called("collidable Ghosts Default true"), "U2 a hollow cell asks to collide");
    Expect(rig.groups().collidable("Ghosts", "Default"), "U2 and they do");
    Expect(rig.on(rig.pane->cellNode("Ghosts", "Default")), "U2 the cell fills");
    rig.click(rig.pane->cellNode("Players", "Ghosts"), "Players/Ghosts cell");
    Expect(rig.called("collidable Ghosts Players false"), "U2 a filled cell asks not to");
    Expect(!rig.on(rig.pane->cellNode("Players", "Ghosts")), "U2 the cell empties");
}

void U3_filter_segments() {
    PaneRig rig;
    Expect(rig.on(rig.pane->filterSegment("Players", FilterType::Exclude)) &&
               !rig.on(rig.pane->filterSegment("Players", FilterType::Include)),
           "U3 Players shows Exclude");
    rig.click(rig.pane->filterSegment("Players", FilterType::Include), "Include segment");
    Expect(rig.called("filter Players Include"), "U3 a click on Include asks for it");
    Expect(rig.on(rig.pane->filterSegment("Players", FilterType::Include)), "U3 and shows it");
    Expect(!rig.on(rig.pane->cellNode("Players", "Default")), "U3 the matrix follows: Players no longer meets Default");
    const std::size_t before = rig.calls.size();
    rig.click(rig.pane->filterSegment("Players", FilterType::Include), "Include segment again");
    Expect(rig.calls.size() == before, "U3 the segment already on asks nothing");
}

void U4_new_group_renames_in_place() {
    PaneRig rig;
    rig.click(rig.pane->newButton(), "New Group button");
    rig.frames(2);
    Expect(rig.called("add"), "U4 New Group asks for one");
    Expect(rig.pane->rowNode("New Group") != nullptr, "U4 its row shows");
    jadefx::TextField* field = rig.pane->renameField();
    Expect(field != nullptr && field->isVisible() && field->isFocused() && field->getText() == "New Group",
           "U4 and its name is being edited");
    Expect(rig.pane->selectedRow() == "New Group", "U4 the new row is selected");
    if (field != nullptr) {
        field->setText("Debris");
        field->fire();
    }
    rig.frames(2);
    Expect(rig.called("rename New Group -> Debris"), "U4 Enter renames");
    Expect(rig.pane->rowNode("Debris") != nullptr && rig.pane->rowNode("New Group") == nullptr, "U4 the row follows");
    Expect(rig.pane->selectedRow() == "Debris", "U4 and stays selected under its new name");
    Expect(rig.subtitle().rfind("4 of 62 groups", 0) == 0, "U4 the count follows");
}

void U5_rename_escape_and_default() {
    PaneRig rig;
    rig.pane->beginRename("Default");
    rig.frames();
    Expect(rig.pane->renameField() == nullptr || !rig.pane->renameField()->isVisible(), "U5 Default is not renamed");
    rig.click(rig.pane->rowNode("Ghosts"), "Ghosts row");
    rig.key(jadefx::Key::F2);
    jadefx::TextField* field = rig.pane->renameField();
    Expect(field != nullptr && field->isVisible() && field->getText() == "Ghosts", "U5 F2 renames the selected row");
    if (field != nullptr) {
        field->setText("Spirits");
    }
    rig.key(jadefx::Key::Escape);
    Expect(rig.pane->rowNode("Ghosts") != nullptr && std::none_of(rig.calls.begin(), rig.calls.end(),
                                                                   [](const std::string& call) {
                                                                       return call.rfind("rename", 0) == 0;
                                                                   }),
           "U5 Escape keeps the name");
    rig.pane->beginRename("Ghosts");
    rig.frames();
    if (rig.pane->renameField() != nullptr) {
        rig.pane->renameField()->setText("Players");
        rig.pane->renameField()->fire();
    }
    rig.frames();
    Expect(!rig.notices.empty() && rig.notices.back() == "A collision group named \"Players\" already exists",
           "U5 a taken name says why");
}

void U6_delete() {
    PaneRig rig;
    rig.groups().register_group("Empty", FilterType::Exclude);
    rig.frames(2);
    rig.click(rig.pane->deleteButton("Empty"), "Empty's delete");
    Expect(rig.called("remove Empty") && rig.pane->removeAlert() == nullptr, "U6 an empty group goes at once");
    rig.frames(2);
    Expect(rig.pane->rowNode("Empty") == nullptr, "U6 its row goes");
    rig.pane->selectRow("Players");
    rig.key(jadefx::Key::Delete);
    Expect(rig.pane->removeAlert() != nullptr, "U6 a group with bodies asks first");
    auto* cancel = dynamic_cast<jadefx::Button*>(rig.scene->getElementById("collision-groups-remove-cancel"));
    Expect(cancel != nullptr, "U6 the question has Cancel");
    if (cancel != nullptr) {
        cancel->fire();
    }
    rig.frames();
    Expect(!rig.called("remove Players") && rig.groups().registered("Players"), "U6 Cancel keeps it");
    rig.pane->requestRemove("Players");
    auto* move = dynamic_cast<jadefx::Button*>(rig.scene->getElementById("collision-groups-remove-move"));
    Expect(move != nullptr, "U6 the question offers to move the bodies");
    if (move != nullptr) {
        move->fire();
    }
    rig.frames(2);
    Expect(rig.called("remove Players") && !rig.groups().registered("Players"), "U6 Move removes it");
    Expect(rig.group_of(rig.p1) == "Default", "U6 and its bodies are Default");
    rig.pane->selectRow("Default");
    const std::size_t before = rig.calls.size();
    rig.key(jadefx::Key::Delete);
    Expect(rig.calls.size() == before && rig.pane->removeAlert() == nullptr, "U6 Delete on Default does nothing");
}

void U7_select_and_assign() {
    PaneRig rig;
    rig.click(rig.pane->rowNode("Players"), "Players row");
    Expect(rig.pane->selectedRow() == "Players", "U7 a click selects the row");
    rig.click(rig.pane->selectButton("Players"), "Players' Select");
    std::vector<InstanceId> selected = rig.game.selection().get();
    Expect(selected.size() == 2 && std::count(selected.begin(), selected.end(), rig.p1) == 1,
           "U7 Select selects the group's bodies");
    rig.game.selection().set({rig.d1, rig.folder});
    rig.frames(2);
    jadefx::Node* assign = rig.pane->assignButton("Ghosts");
    Expect(assign != nullptr && !assign->isDisabled(), "U7 Assign is on with a selection");
    rig.click(assign, "Ghosts' Assign");
    Expect(rig.called("assign Ghosts"), "U7 Assign asks");
    Expect(rig.group_of(rig.d1) == "Ghosts" && rig.group_of(rig.p1) == "Ghosts", "U7 the selection is in Ghosts");
    rig.game.selection().set({});
    rig.frames(2);
    Expect(rig.pane->assignButton("Ghosts") != nullptr && rig.pane->assignButton("Ghosts")->isDisabled(),
           "U7 Assign is off with nothing selected");
}

void U8_register_unregistered() {
    PaneRig rig;
    rig.click(rig.pane->registerButton(), "Register");
    Expect(rig.called("register 1") && rig.groups().registered("Crates"), "U8 Register registers Crates");
    rig.frames(2);
    Expect(rig.pane->unregisteredRow() == nullptr, "U8 the row goes");
    Expect(rig.pane->rowNode("Crates") != nullptr, "U8 and Crates has a row");
}

void U9_outside_changes_show() {
    PaneRig rig;
    engine_core::edit_collision_groups(rig.game, [](CollisionGroups& groups) {
        return groups.register_group("Late", FilterType::Include);
    });
    rig.frames(2);
    Expect(rig.pane->rowNode("Late") != nullptr, "U9 a group a script registers shows");
    auto* body = dynamic_cast<engine_core::PhysicsBase*>(rig.game.instance(rig.d1));
    body->set_collision_group("Late");
    rig.frames(ide::IdeCollisionGroups::kCountFrames + 1);
    const int late = rig.pane->view().index_of("Late");
    Expect(late >= 0 && rig.pane->view().groups[static_cast<std::size_t>(late)].bodies == 1,
           "U9 a body moving group is counted");
    rig.game.history().undo();
    rig.frames(2);
}

void U10_hover_lights_the_way() {
    PaneRig rig;
    rig.move_to(rig.pane->cellNode("Ghosts", "Players"));
    Expect(ide::IdeCollisionGroups::lit(rig.pane->cellNode("Ghosts", "Default")), "U10 the cell left of it lights");
    Expect(ide::IdeCollisionGroups::lit(rig.pane->cellNode("Players", "Players")), "U10 the cell above it lights");
    Expect(ide::IdeCollisionGroups::lit(rig.pane->nameLabel("Ghosts")), "U10 its row's name lights");
    Expect(ide::IdeCollisionGroups::lit(rig.pane->columnLabel("Players")), "U10 its column's name lights");
    Expect(!ide::IdeCollisionGroups::lit(rig.pane->cellNode("Default", "Default")), "U10 others stay dark");
    Expect(!ide::IdeCollisionGroups::lit(rig.pane->columnLabel("Ghosts")), "U10 other names stay dark");
    rig.scene->noteMove(2, kHeight - 2);
    rig.frames();
    Expect(!ide::IdeCollisionGroups::lit(rig.pane->nameLabel("Ghosts")), "U10 leaving puts them out");
}

void U12_rename_survives_new_groups() {
    PaneRig rig;
    rig.pane->beginRename("Ghosts");
    rig.frames();
    if (rig.pane->renameField() != nullptr) {
        rig.pane->renameField()->setText("Spirits");
    }
    // A script registers a group while the name is being typed.
    engine_core::edit_collision_groups(rig.game, [](CollisionGroups& groups) {
        return groups.register_group("Late", FilterType::Exclude);
    });
    rig.frames(3);
    Expect(rig.pane->rowNode("Late") != nullptr, "U12 the new group shows");
    jadefx::TextField* field = rig.pane->renameField();
    Expect(field != nullptr && field->isVisible() && field->isFocused() && field->getText() == "Spirits",
           "U12 the rename goes on with what was typed");
    Expect(std::none_of(rig.calls.begin(), rig.calls.end(),
                        [](const std::string& call) { return call.rfind("rename", 0) == 0; }),
           "U12 and nothing was renamed yet");
    if (field != nullptr) {
        field->fire();
    }
    rig.frames(2);
    Expect(rig.called("rename Ghosts -> Spirits"), "U12 Enter renames as typed");
}

void U13_counts_are_read_sparingly() {
    PaneRig rig;
    rig.frames(ide::IdeCollisionGroups::kCountFrames + 2);
    const std::size_t before = rig.pane->readsForTests();
    rig.game.selection().set({rig.d1});
    rig.frames(2);
    Expect(rig.pane->readsForTests() == before, "U13 a selection change reads no bodies");
    auto* body = dynamic_cast<engine_core::PhysicsBase*>(rig.game.instance(rig.d1));
    for (int i = 0; i < ide::IdeCollisionGroups::kCountFrames; ++i) {
        body->set_collision_group(i % 2 == 0 ? "Players" : "Ghosts");
        rig.frames();
    }
    Expect(rig.pane->readsForTests() <= before + 1, "U13 edits every frame read the bodies at most once a period");
    body->set_collision_group("Players");
    rig.frames(ide::IdeCollisionGroups::kCountFrames + 1);
    const int players = rig.pane->view().index_of("Players");
    Expect(players >= 0 && rig.pane->view().groups[static_cast<std::size_t>(players)].bodies == 3,
           "U13 the counts catch up");
    const std::size_t settled = rig.pane->readsForTests();
    engine_core::edit_collision_groups(rig.game, [](CollisionGroups& groups) {
        return groups.register_group("Now", FilterType::Exclude);
    });
    rig.frames();
    Expect(rig.pane->readsForTests() == settled + 1 && rig.pane->rowNode("Now") != nullptr,
           "U13 a change to the groups shows at once");
}

void U14_double_click_switches_twice() {
    PaneRig rig;
    jadefx::Node* cell = rig.pane->cellNode("Ghosts", "Default");
    if (cell == nullptr) {
        Expect(false, "U14 the cell is on screen");
        return;
    }
    const double x = cell->getAbsoluteX() + cell->getWidth() * 0.5;
    const double y = cell->getAbsoluteY() + cell->getHeight() * 0.5;
    rig.scene->noteMove(x, y);
    for (int i = 0; i < 2; ++i) {
        rig.scene->noteButton(0, true, x, y);
        rig.scene->noteButton(0, false, x, y);
    }
    rig.frames(2);
    Expect(!rig.groups().collidable("Ghosts", "Default") && !rig.on(rig.pane->cellNode("Ghosts", "Default")),
           "U14 two quick clicks switch it there and back");
}

void U15_backspace_does_not_delete() {
    PaneRig rig;
    rig.groups().register_group("Empty", FilterType::Exclude);
    rig.frames(2);
    rig.click(rig.pane->rowNode("Empty"), "Empty row");
    rig.key(jadefx::Key::Backspace);
    Expect(!rig.called("remove Empty") && rig.groups().registered("Empty"), "U15 Backspace deletes nothing");
    rig.key(jadefx::Key::Delete);
    Expect(rig.called("remove Empty"), "U15 Delete does");
}

void U11_play_banner_and_empty_hint() {
    PaneRig rig;
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(2);
    Expect(rig.pane->playBanner() != nullptr, "U11 playing shows the banner");
    rig.game.stop_simulation();
    rig.frames(2);
    Expect(rig.pane->playBanner() == nullptr, "U11 Stop hides it");

    SimRole role;
    engine_core::ScriptRuntime runtime;
    engine_core::Game bare;
    auto pane = jadefx::make<ide::IdeCollisionGroups>(bare);
    pane->setPrefWidthRatio(1);
    pane->setPrefHeightRatio(1);
    auto scene = jadefx::make<jadefx::Scene>(pane, kWidth, kHeight);
    scene->layout(kWidth, kHeight, 0);
    scene->layout(kWidth, kHeight, 0.05);
    Expect(pane->emptyHint() != nullptr, "U11 Default alone shows how to start");
    Expect(pane->subtitle() != nullptr && pane->subtitle()->getText() == "1 of 62 groups \xC2\xB7 0 bodies",
           "U11 and counts one group");
}

}  // namespace

int main() {
    M1_view();
    M2_new_group();
    M3_rename();
    M4_delete();
    M5_collidable_and_filter();
    M6_assign();
    M7_register();
    M8_fresh_name();
    M9_core_and_empty_names();
    U1_rows_and_matrix();
    U2_cell_click_switches();
    U3_filter_segments();
    U4_new_group_renames_in_place();
    U5_rename_escape_and_default();
    U6_delete();
    U7_select_and_assign();
    U8_register_unregistered();
    U9_outside_changes_show();
    U10_hover_lights_the_way();
    U11_play_banner_and_empty_hint();
    U12_rename_survives_new_groups();
    U13_counts_are_read_sparingly();
    U14_double_click_switches_twice();
    U15_backspace_does_not_delete();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return 1;
    }
    std::printf("collision groups tests passed\n");
    return 0;
}
