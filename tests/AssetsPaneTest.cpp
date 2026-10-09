#include "ide/CutSet.hpp"
#include "ide/IdeAssets.hpp"
#include "ide/MaterialPreviews.hpp"
#include "SelectionService.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The Assets pane in a headless scene: its views, navigation, selection, and actions.
namespace {

using engine_core::InstanceId;

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

constexpr double kWidth = 640;
constexpr double kHeight = 400;

// Marks this thread as the simulation thread while it lives, as asset setters need.
struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

// One pane over a place whose Textures hold a Folder Walls (with a Texture in
// it) and the Textures Brick and Rock. The host applies actions straight to the
// game, standing in for the simulation thread, and records them.
struct Rig {
    SimRole role;
    // Its object file registers the classes Instance.new creates, which insert_instance uses.
    engine_core::ScriptRuntime runtime;
    engine_core::Game game;
    InstanceId textures = 0;
    InstanceId walls = 0;
    InstanceId brick = 0;
    InstanceId rock = 0;
    std::vector<std::string> notices;
    std::vector<std::pair<std::string, InstanceId>> inserts;
    // Each Import <kind>'s kind and folder.
    std::vector<std::pair<std::string, InstanceId>> imports;
    std::vector<std::pair<InstanceId, std::string>> renames;
    std::vector<std::pair<std::string, InstanceId>> runs;
    std::vector<std::pair<std::string, std::vector<InstanceId>>> batches;
    std::vector<InstanceId> added_as_game_object;
    int moves = 0;
    std::string saved;
    std::vector<std::string> saves;
    // The modifier keys the next click sees.
    int mods = 0;
    std::shared_ptr<ide::IdeAssets> pane;
    std::shared_ptr<jadefx::Scene> scene;

    explicit Rig(std::string saved_view = "icons") : saved(std::move(saved_view)) {
        textures = game.service("Textures");
        walls = make("Folder", "Walls", textures);
        make("Texture", "Mortar", walls);
        brick = make("Texture", "Brick", textures);
        rock = make("Texture", "Rock", textures);

        ide::AssetsHost host;
        host.actions.run = [this](engine_core::InstanceAction action, InstanceId id) {
            runs.emplace_back(engine_core::action_label(action), id);
        };
        host.actions.run_many = [this](engine_core::InstanceAction action, const std::vector<InstanceId>& ids) {
            batches.emplace_back(engine_core::action_label(action), ids);
        };
        host.actions.enabled = [](engine_core::InstanceAction) { return true; };
        host.actions.notice = [this](std::string text) { notices.push_back(std::move(text)); };
        host.actions.insert = [this](std::string class_name, InstanceId parent,
                                     std::shared_ptr<ide::InsertResult> result) {
            inserts.emplace_back(class_name, parent);
            std::string error;
            result->id = ide::insert_instance(game, class_name, parent, error);
            result->error = error;
            result->done = true;
        };
        host.actions.rename = [this](InstanceId id, std::string name) {
            game.set_name(id, name);
            renames.emplace_back(id, std::move(name));
        };
        host.actions.move = [this](const std::vector<InstanceId>& moved, InstanceId parent) {
            ++moves;
            ide::move_set(game, moved, parent);
        };
        host.add_as_game_object = [this](InstanceId prefab) { added_as_game_object.push_back(prefab); };
        host.import_assets = [this](InstanceId folder, const std::string& kind) { imports.emplace_back(kind, folder); };
        host.saved_view = [this] { return saved; };
        host.save_view = [this](const std::string& view) { saves.push_back(view); };
        pane = jadefx::make<ide::IdeAssets>(game, std::move(host));
        pane->setPrefWidthRatio(1);
        pane->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(pane, kWidth, kHeight);
    }

    InstanceId make(const char* klass, const char* name, InstanceId parent) {
        engine_core::DataModel* object = engine_core::lua_create_instance(game, klass);
        Expect(object != nullptr, "the rig can create asset classes");
        if (object == nullptr) {
            return 0;
        }
        game.set_name(object->id(), name);
        game.set_parent(object->id(), parent);
        return object->id();
    }

    void frame(double at) { scene->layout(kWidth, kHeight, at); }

    // along places the click across the node, 0 at its left and 1 at its right. JadeFX counts
    // presses on one spot within 0.4 s of real time as a double-click, whatever the scene time.
    void clickNode(jadefx::Node* node, int clicks = 1, int button = 0, double along = 0.5) {
        Expect(node != nullptr, "the node to click is on screen");
        if (node == nullptr) {
            return;
        }
        const double x = node->getAbsoluteX() + node->getWidth() * along;
        const double y = node->getAbsoluteY() + node->getHeight() * 0.5;
        for (int click = 0; click < clicks; ++click) {
            scene->noteButton(button, true, x, y, mods);
            scene->noteButton(button, false, x, y, mods);
        }
    }

    // Lays out at `at`, then clicks the middle of id's widget `clicks` times.
    void clickItem(InstanceId id, double at, int clicks = 1, double along = 0.5) {
        frame(at);
        clickNode(pane->itemNode(id), clicks, 0, along);
    }

    // The node of that class whose own text, or a label inside it, reads text.
    jadefx::Node* labeled(const char* style_class, const std::string& text) {
        for (jadefx::Node* node : pane->getElementsByClassName(style_class)) {
            if (auto* own = dynamic_cast<jadefx::Labeled*>(node); own != nullptr && own->getText() == text) {
                return node;
            }
            for (jadefx::Node* child : node->getElementsByClassName("assets-sidebar-label")) {
                if (auto* label = dynamic_cast<jadefx::Labeled*>(child); label != nullptr && label->getText() == text) {
                    return node;
                }
            }
        }
        return nullptr;
    }

    std::vector<std::string> crumbs() {
        std::vector<std::string> out;
        for (jadefx::Node* node : pane->getElementsByClassName("assets-crumb")) {
            if (auto* button = dynamic_cast<jadefx::Labeled*>(node)) {
                out.push_back(button->getText());
            }
        }
        return out;
    }

    jadefx::Node* first(const char* style_class) {
        const std::vector<jadefx::Node*> found = pane->getElementsByClassName(style_class);
        return found.empty() ? nullptr : found.front();
    }

    void key(int code, int with = 0) { scene->noteKey(code, true, false, with); }

    // Lays out at `at`, then right-clicks the middle of node.
    void rightClick(jadefx::Node* node, double at) {
        frame(at);
        clickNode(node, 1, 1);
    }

    void rightClickItem(InstanceId id, double at) { rightClick(pane->itemNode(id), at); }

    // Lays out at `at`, then right-clicks the center's bottom right corner, below every item.
    void rightClickEmpty(double at) {
        frame(at);
        jadefx::Node* center = first("assets-center");
        Expect(center != nullptr, "the center is on screen");
        if (center == nullptr) {
            return;
        }
        const double x = center->getAbsoluteX() + center->getWidth() - 20;
        const double y = center->getAbsoluteY() + center->getHeight() - 20;
        scene->noteButton(1, true, x, y);
        scene->noteButton(1, false, x, y);
    }

    // The open menu's row with that label, or null.
    jadefx::Node* menuItem(const std::string& label) {
        jadefx::Node* text = scene->getElementById("menu-label:" + label);
        return text != nullptr ? text->getParent() : nullptr;
    }

    // Whether the open menu's row with that label shows an icon.
    bool menuItemHasIcon(const std::string& label) {
        // jadefx keeps a node's children protected; a pointer to the member reads them.
        struct Peek : jadefx::Node {
            using Kids = jadefx::ObservableList<std::shared_ptr<jadefx::Node>>;
            static const Kids& of(const jadefx::Node& node) {
                const Kids& (jadefx::Node::*getter)() const = &Peek::children;
                return (node.*getter)();
            }
        };
        const jadefx::Node* row = menuItem(label);
        if (row == nullptr) {
            return false;
        }
        const auto& kids = Peek::of(*row);
        return std::any_of(kids.begin(), kids.end(), [](const std::shared_ptr<jadefx::Node>& kid) {
            return kid && std::string(kid->getElementType()) == "image-view";
        });
    }

    void clickMenu(const std::string& label) {
        jadefx::Node* item = menuItem(label);
        Expect(item != nullptr, ("the menu lists " + label).c_str());
        if (item == nullptr) {
            return;
        }
        const double x = item->getAbsoluteX() + item->getWidth() * 0.5;
        const double y = item->getAbsoluteY() + item->getHeight() * 0.5;
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
    }

    jadefx::TextField* renameField() { return dynamic_cast<jadefx::TextField*>(first("assets-rename")); }

    bool editing() {
        jadefx::TextField* field = renameField();
        return field != nullptr && field->isVisible();
    }

    // Presses the middle of from, moves to the middle of to, and releases there.
    void drag(jadefx::Node* from, jadefx::Node* to, double at) {
        frame(at);
        Expect(from != nullptr && to != nullptr, "both ends of the drag are on screen");
        if (from == nullptr || to == nullptr) {
            return;
        }
        const double x = from->getAbsoluteX() + from->getWidth() * 0.5;
        const double y = from->getAbsoluteY() + from->getHeight() * 0.5;
        const double to_x = to->getAbsoluteX() + to->getWidth() * 0.5;
        const double to_y = to->getAbsoluteY() + to->getHeight() * 0.5;
        scene->noteButton(0, true, x, y);
        scene->noteMove(x + 10, y + 10);
        scene->noteMove(to_x, to_y);
        frame(at + 0.05);
        scene->noteMove(to_x, to_y);
        scene->noteButton(0, false, to_x, to_y);
        frame(at + 0.1);
    }
};

bool HasClass(const jadefx::Node* node, const char* name) {
    if (node == nullptr) {
        return false;
    }
    const auto& classes = node->getClassList();
    return std::find(classes.begin(), classes.end(), std::string(name)) != classes.end();
}

void starts_in_saved_view() {
    Rig rig("list");
    rig.frame(0);
    Expect(rig.pane->view() == ide::AssetView::List, "the pane starts in the saved view");
    Expect(rig.saves.empty(), "starting does not save the view");
}

void navigates() {
    Rig rig;
    rig.frame(0);
    rig.clickNode(rig.labeled("assets-sidebar-row", "Textures"));
    Expect(rig.pane->browser().folder() == rig.textures, "the sidebar opens Textures");
    rig.clickItem(rig.walls, 0.5, 2);
    Expect(rig.pane->browser().folder() == rig.walls, "a double-click opens a Folder");
    rig.frame(1.0);
    Expect(rig.crumbs() == std::vector<std::string>{"Assets", "Textures", "Walls"}, "crumbs from Assets down");
    Expect(HasClass(rig.labeled("assets-sidebar-row", "Textures"), "selected"), "the sidebar marks the category");
    rig.clickNode(rig.labeled("assets-crumb", "Textures"));
    Expect(rig.pane->browser().folder() == rig.textures, "a crumb goes up");
    rig.frame(1.5);
    Expect(rig.crumbs() == std::vector<std::string>{"Assets", "Textures"}, "the crumbs follow");
    rig.clickNode(rig.first("assets-back"));
    Expect(rig.pane->browser().folder() == rig.walls, "back goes back to Walls");
    rig.frame(2.0);
    Expect(rig.pane->itemNode(rig.walls) == nullptr, "Walls' own contents show");
    Expect(rig.crumbs().size() == 3, "back shows Walls' crumbs");
}

void selection_is_shared() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    rig.clickItem(rig.brick, 0.1);
    Expect(rig.game.selection().get() == std::vector<InstanceId>{rig.brick}, "a click selects Brick");
    rig.mods = jadefx::Key::ModControl;
    rig.clickItem(rig.rock, 1.0);
    Expect((rig.game.selection().get() == std::vector<InstanceId>{rig.brick, rig.rock}), "Ctrl+click adds Rock");
    rig.frame(1.1);
    Expect(HasClass(rig.pane->itemNode(rig.brick), "selected"), "Brick shows selected");
    rig.mods = 0;
    rig.game.selection().set({rig.rock});
    rig.frame(2.0);
    Expect(HasClass(rig.pane->itemNode(rig.rock), "selected"), "a selection set elsewhere shows on Rock");
    Expect(!HasClass(rig.pane->itemNode(rig.brick), "selected"), "and not on Brick");
    Expect(rig.pane->browser().folder() == rig.textures, "the folder stays");
}

void three_views_same_folder() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    double at = 0.5;
    for (const char* view : {"icons", "list", "columns"}) {
        rig.clickNode(rig.first((std::string("assets-view-") + view).c_str()));
        rig.frame(at);
        rig.frame(at + 0.05);
        const std::string where = std::string(" in the ") + view + " view";
        Expect(rig.pane->itemNode(rig.brick) != nullptr, ("Brick shows" + where).c_str());
        Expect(rig.pane->itemNode(rig.rock) != nullptr, ("Rock shows" + where).c_str());
        Expect(ide::asset_view_name(rig.pane->view()) == std::string(view), ("the toggle shows" + where).c_str());
        at += 1.0;
    }
    Expect((rig.saves == std::vector<std::string>{"icons", "list", "columns"}), "each view is saved in turn");
    Expect(rig.pane->browser().folder() == rig.textures, "the folder stays across views");
}

// The List view sorts by its headers and opens folders in place; Columns marks
// the path and previews a single selected asset.
void list_and_columns() {
    Rig rig("list");
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    Expect(rig.pane->itemNode(rig.walls) != nullptr, "Walls has a row");
    jadefx::Node* mortar_before = nullptr;
    for (InstanceId child : rig.game.get_children(rig.walls)) {
        mortar_before = rig.pane->itemNode(child);
    }
    Expect(mortar_before == nullptr, "Walls starts closed");
    jadefx::Node* walls = rig.pane->itemNode(rig.walls);
    const std::vector<jadefx::Node*> disclosures =
        walls != nullptr ? walls->getElementsByClassName("assets-disclosure") : std::vector<jadefx::Node*>{};
    rig.clickNode(disclosures.empty() ? nullptr : disclosures.front());
    rig.frame(0.5);
    const InstanceId mortar = rig.game.get_children(rig.walls).front();
    Expect(rig.pane->itemNode(mortar) != nullptr, "the disclosure opens Walls in place");
    Expect(rig.game.selection().get().empty(), "the disclosure does not select");
    Expect(rig.pane->browser().folder() == rig.textures, "the disclosure does not open the folder");

    rig.clickNode(rig.first("assets-sort-Name"));
    Expect(rig.pane->browser().sort() == ide::AssetSort::Name && rig.pane->browser().descending(),
           "clicking the active header reverses it");
    rig.frame(1.0);
    rig.clickNode(rig.first("assets-sort-Kind"));
    Expect(rig.pane->browser().sort() == ide::AssetSort::Kind && !rig.pane->browser().descending(),
           "another header sorts ascending");
    rig.frame(1.5);
    auto* kind = dynamic_cast<jadefx::Labeled*>(rig.first("assets-sort-Kind"));
    Expect(HasClass(kind, "ascending") && kind != nullptr && kind->getGraphic() != nullptr,
           "the active header shows its direction");

    rig.pane->setView(ide::AssetView::Columns);
    rig.frame(2.0);
    Expect(rig.first("assets-sidebar") == nullptr, "Columns has no sidebar");
    jadefx::Node* on_path = rig.first("assets-on-path");
    const std::vector<jadefx::Node*> on_path_label =
        on_path != nullptr ? on_path->getElementsByClassName("assets-cell") : std::vector<jadefx::Node*>{};
    Expect(on_path_label.size() == 1 && dynamic_cast<jadefx::Labeled*>(on_path_label.front())->getText() == "Textures",
           "the category on the path is marked");
    rig.clickItem(rig.walls, 2.5);
    Expect(rig.pane->browser().folder() == rig.walls, "selecting a Folder in Columns opens its column");
    rig.clickItem(mortar, 3.5);
    rig.frame(3.6);
    bool previewed = false;
    for (jadefx::Node* node : rig.pane->getElementsByClassName("assets-preview-name")) {
        auto* label = dynamic_cast<jadefx::Labeled*>(node);
        previewed = previewed || (label != nullptr && label->getText() == "Mortar");
    }
    Expect(previewed, "a selected asset has a preview");
    rig.clickItem(rig.brick, 4.5);
    Expect(rig.pane->browser().folder() == rig.textures, "selecting an asset in an earlier column closes the later ones");
}

void new_folder_and_kind() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    rig.rightClickEmpty(0.1);
    Expect(rig.menuItem("New Folder") != nullptr, "empty space offers New Folder");
    Expect(rig.menuItem("New Texture") != nullptr, "empty space in Textures offers New Texture");
    Expect(rig.menuItem("Paste") != nullptr, "empty space offers Paste");
    {
        const jadefx::Node* kind = rig.menuItem("New Texture");
        const jadefx::Node* folder = rig.menuItem("New Folder");
        const jadefx::Node* import = rig.menuItem("Import Texture…");
        Expect(import != nullptr && kind != nullptr && import->getAbsoluteY() < kind->getAbsoluteY(),
               "Import Texture… comes first, above New Texture");
        Expect(kind != nullptr && folder != nullptr && kind->getAbsoluteY() < folder->getAbsoluteY(),
               "and New Texture above New Folder");
    }
    rig.clickMenu("Import Texture…");
    Expect((rig.imports == std::vector<std::pair<std::string, InstanceId>>{{"Texture", rig.textures}}),
           "Import Texture… imports Textures into the folder");
    rig.rightClickEmpty(0.15);
    rig.clickMenu("New Texture");
    Expect((rig.inserts == std::vector<std::pair<std::string, InstanceId>>{{"Texture", rig.textures}}),
           "New Texture inserts a Texture in the folder");
    rig.frame(0.2);
    const InstanceId made = rig.game.get_children(rig.textures).back();
    Expect(made != rig.rock && rig.game.instance(made) != nullptr && std::string(rig.game.instance(made)->class_name()) == "Texture", "the insert made a Texture");
    Expect(rig.game.selection().get() == std::vector<InstanceId>{made}, "the new item is selected");
    Expect(rig.editing(), "the new item's name is open for renaming");
    rig.frame(0.3);
    Expect(rig.editing(), "the rename stays open on the next frame");
}

void rename_delete_cut_paste() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    rig.pane->beginRename(rig.brick);
    rig.frame(0.1);
    Expect(rig.editing(), "beginRename opens the field");
    if (jadefx::TextField* field = rig.renameField()) {
        Expect(field->getText() == "Brick", "the field starts with the name");
        field->setText("Stone");
    }
    rig.key(jadefx::Key::Enter);
    rig.frame(0.2);
    Expect((rig.renames == std::vector<std::pair<InstanceId, std::string>>{{rig.brick, "Stone"}}),
           "Enter renames Brick to Stone");
    Expect(!rig.editing(), "Enter closes the field");

    rig.clickItem(rig.brick, 1.0);
    // Delete is the studio's, on the selection wherever the keyboard is; the pane passes it on.
    const std::size_t before = rig.batches.size();
    Expect(!rig.scene->noteKey(jadefx::Key::Delete, true, false, 0) && rig.batches.size() == before,
           "the pane leaves Delete to the studio");

    rig.rightClickItem(rig.brick, 2.0);
    Expect(rig.menuItem("Rename") != nullptr && rig.menuItem("Delete") != nullptr, "an item's menu");
    rig.clickMenu("Cut");
    Expect(!rig.batches.empty() && rig.batches.back().first == "Cut" &&
               rig.batches.back().second == std::vector<InstanceId>{rig.brick},
           "Cut runs on the selection");
    rig.rightClickItem(rig.walls, 3.0);
    rig.clickMenu("Paste");
    Expect(!rig.runs.empty() && rig.runs.back() == std::make_pair(std::string("Paste"), rig.walls),
           "Paste on a Folder's menu goes into it");
    rig.rightClickItem(rig.rock, 3.5);
    rig.clickMenu("Paste");
    Expect(!rig.runs.empty() && rig.runs.back() == std::make_pair(std::string("Paste"), rig.textures),
           "Paste on an asset's menu goes into the folder");
    rig.clickItem(rig.brick, 4.0);
    rig.key(jadefx::Key::V, jadefx::Key::ModControl);
    Expect(!rig.runs.empty() && rig.runs.back() == std::make_pair(std::string("Paste"), rig.textures),
           "Paste from the keyboard goes into the folder");

    // A slow second click renames; Escape drops it.
    rig.clickItem(rig.rock, 5.0);
    rig.clickItem(rig.rock, 5.8, 1, 0.3);
    rig.frame(6.4);
    Expect(rig.editing(), "a slow second click renames");
    rig.key(jadefx::Key::Escape);
    rig.frame(6.5);
    Expect(!rig.editing() && rig.renames.size() == 1, "Escape drops the rename");
    // Enter on a single selected item renames it; a click elsewhere drops it.
    rig.key(jadefx::Key::Enter);
    rig.frame(6.6);
    Expect(rig.editing(), "Enter renames the selected item");
    rig.clickItem(rig.brick, 7.5);
    rig.frame(7.6);
    Expect(!rig.editing() && rig.renames.size() == 1, "a click elsewhere drops the rename");
    // Escape that clears the selection is the studio's too.
    Expect(!rig.scene->noteKey(jadefx::Key::Escape, true, false, 0) && !rig.game.selection().get().empty(),
           "the pane leaves Escape to the studio");
}

void refused_drop_says_why() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    rig.drag(rig.pane->itemNode(rig.walls), rig.labeled("assets-sidebar-row", "Meshes"), 0.5);
    Expect(std::find(rig.notices.begin(), rig.notices.end(), "Meshes holds Meshes and Folders") != rig.notices.end(),
           "a refused drop says why");
    Expect(rig.game.parent(rig.walls) == rig.textures && rig.moves == 0, "Walls did not move");
    rig.drag(rig.pane->itemNode(rig.brick), rig.pane->itemNode(rig.walls), 1.5);
    Expect(rig.moves == 1 && rig.game.parent(rig.brick) == rig.walls, "a drop on a Folder moves into it");
}

// A dragged asset's icon follows the pointer until the release.
void drag_shows_icon() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    jadefx::Node* from = rig.pane->itemNode(rig.brick);
    Expect(from != nullptr, "Brick has a tile");
    if (from == nullptr) {
        return;
    }
    const double x = from->getAbsoluteX() + from->getWidth() * 0.5;
    const double y = from->getAbsoluteY() + from->getHeight() * 0.5;
    rig.scene->noteButton(0, true, x, y);
    rig.scene->noteMove(x + 40, y + 30);
    rig.frame(0.05);
    rig.scene->noteMove(x + 40, y + 30);
    jadefx::Node* icon = rig.scene->getElementById("instance-drag-icon");
    Expect(icon != nullptr && rig.scene->isPopupShowing(icon), "a drag shows the asset's icon");
    Expect(icon != nullptr && std::abs(icon->getAbsoluteX() + icon->getWidth() * 0.5 - (x + 40)) < 1 &&
               std::abs(icon->getAbsoluteY() + icon->getHeight() * 0.5 - (y + 30)) < 1,
           "the icon is centered on the pointer");
    rig.scene->noteButton(0, false, x + 40, y + 30);
    rig.frame(0.1);
    Expect(rig.scene->getElementById("instance-drag-icon") == nullptr, "the release takes the icon away");
}

// In Columns, an asset in a Folder's column drags back out into an earlier column.
void columns_drop_into_earlier_column() {
    Rig rig("columns");
    rig.pane->openFolder(rig.walls);
    rig.frame(0);
    const InstanceId mortar = rig.game.get_children(rig.walls).front();
    const std::vector<jadefx::Node*> columns = rig.pane->getElementsByClassName("assets-column");
    Expect(columns.size() == 3, "Columns shows the categories, Textures, and Walls");
    if (columns.size() != 3) {
        return;
    }
    // Onto the empty space below the Textures column's rows.
    jadefx::Node* textures_column = columns[1];
    rig.frame(0.5);
    const double x = textures_column->getAbsoluteX() + textures_column->getWidth() * 0.5;
    const double y = textures_column->getAbsoluteY() + textures_column->getHeight() - 20;
    jadefx::Node* from = rig.pane->itemNode(mortar);
    Expect(from != nullptr, "Mortar has a row");
    if (from == nullptr) {
        return;
    }
    const double from_x = from->getAbsoluteX() + from->getWidth() * 0.5;
    const double from_y = from->getAbsoluteY() + from->getHeight() * 0.5;
    rig.scene->noteButton(0, true, from_x, from_y);
    rig.scene->noteMove(from_x + 10, from_y + 10);
    rig.scene->noteMove(x, y);
    rig.frame(0.55);
    rig.scene->noteMove(x, y);
    rig.scene->noteButton(0, false, x, y);
    rig.frame(0.6);
    Expect(rig.moves == 1 && rig.game.parent(mortar) == rig.textures,
           "a drop on a column's empty space moves into its folder");

    // Onto an asset's row in an earlier column: into that column's folder.
    rig.pane->openFolder(rig.walls);
    rig.frame(1.0);
    rig.drag(rig.pane->itemNode(rig.brick), rig.pane->itemNode(rig.walls), 1.5);
    Expect(rig.game.parent(rig.brick) == rig.walls, "Brick moves into Walls");
    rig.drag(rig.pane->itemNode(rig.brick), rig.pane->itemNode(rig.rock), 2.5);
    Expect(rig.moves == 3 && rig.game.parent(rig.brick) == rig.textures,
           "a drop on an asset's row moves into its column's folder");
}

void search_filters() {
    Rig rig;
    const InstanceId trim = rig.make("Texture", "BrickTrim", rig.walls);
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    rig.pane->searchField().setText("bri");
    rig.frame(0.1);
    Expect(rig.pane->itemNode(rig.brick) != nullptr, "Brick matches");
    Expect(rig.pane->itemNode(trim) != nullptr, "a match in a Folder under the folder shows");
    Expect(rig.pane->itemNode(rig.rock) == nullptr && rig.pane->itemNode(rig.walls) == nullptr,
           "the rest is hidden");
    rig.clickNode(rig.first("assets-search-clear"));
    rig.frame(0.2);
    Expect(rig.pane->searchField().getText().empty(), "× empties the field");
    Expect(rig.pane->itemNode(rig.rock) != nullptr && rig.pane->itemNode(trim) == nullptr, "× restores the view");
}

void pane_follows_tree() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.pane->openFolder(rig.walls);
    rig.frame(0);
    const InstanceId fresh = rig.make("Texture", "Fresh", rig.walls);
    rig.frame(0.1);
    Expect(rig.pane->itemNode(fresh) != nullptr, "an asset made in the folder shows");
    rig.game.destroy_tree(rig.walls);
    rig.frame(0.2);
    Expect(rig.pane->browser().folder() == rig.textures, "the destroyed folder falls back to its category");
    Expect(rig.crumbs() == std::vector<std::string>{"Assets", "Textures"}, "the crumbs follow");
    Expect(rig.pane->itemNode(rig.brick) != nullptr, "the category's items show");
}

void insert_through_pane() {
    Rig rig;
    const InstanceId prefabs = rig.game.service("Prefabs");
    rig.pane->openFolder(prefabs);
    rig.frame(0);
    rig.rightClickEmpty(0.1);
    Expect(rig.menuItem("New Prefab") != nullptr, "Prefabs offers New Prefab");
    rig.clickMenu("New Folder");
    Expect((rig.inserts == std::vector<std::pair<std::string, InstanceId>>{{"Folder", prefabs}}),
           "New Folder inserts a Folder in the folder shown");
}

// A Prefab is one item: it does not open, and its Models show nowhere.
void prefab_hides_its_models() {
    Rig rig("list");
    const InstanceId prefabs = rig.game.service("Prefabs");
    const InstanceId crate = rig.make("Prefab", "Crate", prefabs);
    const InstanceId lid = rig.make("Model", "Lid", crate);
    rig.pane->openFolder(prefabs);
    rig.frame(0);
    Expect(!rig.pane->openFolder(crate), "a Prefab does not open");
    Expect(rig.pane->browser().folder() == prefabs, "the folder shown stays Prefabs");
    jadefx::Node* row = rig.pane->itemNode(crate);
    Expect(row != nullptr, "the Prefab has a row");
    const std::vector<jadefx::Node*> disclosures =
        row != nullptr ? row->getElementsByClassName("assets-disclosure") : std::vector<jadefx::Node*>{};
    Expect(disclosures.size() == 1 && dynamic_cast<jadefx::Labeled*>(disclosures.front())->getGraphic() == nullptr,
           "the Prefab's row has no disclosure");
    rig.clickItem(crate, 0.1, 2);
    rig.frame(0.2);
    Expect(rig.pane->browser().folder() == prefabs, "a double-click does not open it");
    Expect(rig.runs == std::vector<std::pair<std::string, InstanceId>>{{"Edit", crate}},
           "a double-click runs Edit on the Prefab");
    Expect(rig.pane->itemNode(lid) == nullptr, "its Model has no row");

    rig.pane->searchField().setText("lid");
    rig.frame(0.3);
    Expect(rig.pane->itemNode(lid) == nullptr, "search does not find its Model");
    rig.pane->searchField().setText("crate");
    rig.frame(0.4);
    Expect(rig.pane->itemNode(crate) != nullptr, "search finds the Prefab");

    rig.pane->setView(ide::AssetView::Columns);
    rig.frame(0.5);
    rig.clickItem(crate, 0.6);
    rig.frame(0.7);
    Expect(rig.pane->browser().folder() == prefabs, "in Columns, selecting it opens no column");
    Expect(rig.pane->itemNode(lid) == nullptr, "its Model has no row in Columns");
}

void prefab_menu_offers_add_as_game_object() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    rig.rightClickItem(rig.brick, 0.1);
    Expect(rig.menuItem("Add as GameObject") == nullptr, "a Texture's menu has no such item");
    Expect(rig.menuItem("Edit") == nullptr, "a Texture's menu has no Edit");

    const InstanceId statue = rig.make("Prefab", "Statue", rig.game.service("Prefabs"));
    rig.pane->openFolder(rig.game.service("Prefabs"));
    rig.frame(1.0);
    rig.rightClickItem(statue, 1.1);
    Expect(rig.menuItem("Add as GameObject") != nullptr, "a Prefab's menu offers Add as GameObject");
    Expect(rig.menuItemHasIcon("Rename"), "Rename shows its icon");
    Expect(rig.menuItemHasIcon("Add as GameObject"), "Add as GameObject shows an icon like its neighbors");
    rig.clickMenu("Add as GameObject");
    Expect(rig.added_as_game_object == std::vector<InstanceId>{statue},
           "choosing it calls the host with the right-clicked Prefab");

    rig.frame(1.2);
    rig.rightClickItem(statue, 1.3);
    Expect(rig.menuItem("Edit") != nullptr, "a Prefab's menu offers Edit");
    Expect(rig.menuItemHasIcon("Edit"), "Edit shows an icon like its neighbors");
    rig.clickMenu("Edit");
    Expect(!rig.runs.empty() && rig.runs.back() == std::pair<std::string, InstanceId>{"Edit", statue},
           "choosing Edit runs Edit on the right-clicked Prefab");
}

void up_goes_up_one_level() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.pane->openFolder(rig.walls);
    rig.frame(0);
    rig.clickItem(rig.game.get_children(rig.walls).front(), 0.1);
    rig.key(jadefx::Key::Up, jadefx::Key::ModControl);
    Expect(rig.pane->browser().folder() == rig.textures, "Ctrl+Up goes from Textures › Walls to Textures");
    rig.frame(0.5);
    rig.key(jadefx::Key::Up, jadefx::Key::ModControl);
    Expect(rig.pane->browser().folder() == rig.textures, "Ctrl+Up stops at the category");
}

void status_counts_only_shown() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    auto* status = dynamic_cast<jadefx::Labeled*>(rig.first("assets-status"));
    engine_core::Folder& part = rig.game.create<engine_core::Folder>();
    rig.game.set_name(part.id(), "Part");
    rig.game.set_parent(part.id(), rig.game.scene_service("Workspace"));
    rig.game.selection().set({part.id()});
    rig.frame(0.1);
    Expect(status != nullptr && status->getText() == "3 items", "a Workspace selection is not in the status");
    rig.game.selection().set({part.id(), rig.brick});
    rig.frame(0.2);
    Expect(status != nullptr && status->getText() == "3 items · Brick selected",
           "the status names only the shown selection");
}

}  // namespace

// A property set in the game, with nothing else changed, shows at the next
// frame: List's Path column, and the Columns preview.
void property_edits_refresh() {
    Rig rig("list");
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    auto* brick = dynamic_cast<engine_core::Texture*>(rig.game.instance(rig.brick));
    Expect(brick != nullptr, "Brick is a Texture");
    if (brick == nullptr) {
        return;
    }
    auto path_cell = [&rig]() -> std::string {
        jadefx::Node* row = rig.pane->itemNode(rig.brick);
        const std::vector<jadefx::Node*> cells =
            row != nullptr ? row->getElementsByClassName("assets-cell") : std::vector<jadefx::Node*>{};
        auto* label = cells.empty() ? nullptr : dynamic_cast<jadefx::Labeled*>(cells.back());
        return label != nullptr ? label->getText() : std::string("<no cell>");
    };
    Expect(path_cell().empty(), "Brick's Path starts empty");
    Expect(!brick->set_path("textures/brick.png"), "the Path is taken");
    rig.frame(0.1);
    Expect(path_cell() == "textures/brick.png", "List's Path column follows a Path set in the game");

    rig.pane->setView(ide::AssetView::Columns);
    rig.frame(1.0);
    rig.clickItem(rig.brick, 1.5);
    rig.frame(1.6);
    auto previewed = [&rig](const std::string& text) {
        for (jadefx::Node* node : rig.pane->getElementsByClassName("assets-preview-value")) {
            auto* label = dynamic_cast<jadefx::Labeled*>(node);
            if (label != nullptr && label->getText() == text) {
                return true;
            }
        }
        return false;
    };
    Expect(previewed("textures/brick.png"), "the preview shows Brick's Path");
    Expect(!brick->set_path("textures/other.png"), "a second Path is taken");
    rig.frame(2.0);
    Expect(previewed("textures/other.png"), "the preview follows a Path set in the game");
}

// The Columns preview and the Icons tile of a Texture draw the file its Path
// names, fit in the icon's box; with no file there, the Texture icon.
void texture_preview_shows_file() {
    Rig rig("columns");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anarchy-assets-preview-test";
    std::filesystem::create_directories(root);
    // A 2x1 24-bit BMP: one red pixel, one blue, the row padded to 4 bytes.
    const unsigned char bmp[] = {'B', 'M', 62, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
                                 40, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 8, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 255, 255, 0, 0, 0, 0};
    {
        std::ofstream out(root / "wide.bmp", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bmp), sizeof(bmp));
    }
    rig.game.set_resources_root(root);
    auto* brick = dynamic_cast<engine_core::Texture*>(rig.game.instance(rig.brick));
    Expect(brick != nullptr && !brick->set_path("wide.bmp"), "Brick's Path names the file");
    // The texture image under node, or null when it shows the class's icon.
    auto texture_image = [](jadefx::Node* node) -> jadefx::ImageView* {
        const std::vector<jadefx::Node*> found =
            node != nullptr ? node->getElementsByClassName("assets-texture-image") : std::vector<jadefx::Node*>{};
        return found.empty() ? nullptr : dynamic_cast<jadefx::ImageView*>(found.front());
    };
    auto preview = [&rig]() -> jadefx::Node* {
        const std::vector<jadefx::Node*> found = rig.pane->getElementsByClassName("assets-preview");
        return found.empty() ? nullptr : found.front();
    };
    auto draws_file = [](jadefx::ImageView* view, double width, double height) {
        return view != nullptr && view->getImage() && view->getImage()->getWidth() == 2 &&
               view->getImage()->getHeight() == 1 && view->getPrefWidth() == width && view->getPrefHeight() == height;
    };

    // Waits out the loader's thread, then draws the frame that shows what it loaded.
    auto stream_in = [&rig](double at) {
        for (int tries = 0; tries < 1000 && !rig.pane->thumbnailsIdle(); ++tries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        rig.frame(at);
    };

    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    rig.clickItem(rig.brick, 0.5);
    rig.frame(0.6);
    stream_in(0.7);
    Expect(draws_file(texture_image(preview()), 64, 32), "the preview draws the file once loaded, keeping its shape");

    rig.pane->setView(ide::AssetView::Icons);
    rig.frame(1.0);
    stream_in(1.1);
    Expect(draws_file(texture_image(rig.pane->itemNode(rig.brick)), 40, 20), "the Icons tile draws the file too");
    Expect(rig.pane->itemNode(rig.rock) != nullptr && texture_image(rig.pane->itemNode(rig.rock)) == nullptr,
           "a Texture with no Path keeps the Texture icon");

    // Into Walls and back: Brick's thumbnail is kept while Brick names its file, so it draws at once.
    rig.pane->openFolder(rig.walls);
    rig.frame(1.2);
    rig.pane->openFolder(rig.textures);
    rig.frame(1.3);
    Expect(draws_file(texture_image(rig.pane->itemNode(rig.brick)), 40, 20),
           "a folder shown again draws its thumbnails without loading them");

    Expect(brick != nullptr && !brick->set_path("missing.png"), "a Path with no file is taken");
    rig.frame(1.5);
    stream_in(1.6);
    Expect(rig.pane->itemNode(rig.brick) != nullptr && texture_image(rig.pane->itemNode(rig.brick)) == nullptr,
           "with no file, the tile draws the Texture icon");
    rig.pane->setView(ide::AssetView::Columns);
    rig.frame(2.0);
    rig.clickItem(rig.brick, 2.5);
    rig.frame(2.6);
    stream_in(2.7);
    Expect(preview() != nullptr && texture_image(preview()) == nullptr,
           "with no file, the preview draws the Texture icon");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// A thumbnail averages the pixels each of its own covers, weighting color by
// alpha, and comes out top row first.
void thumbnails_shrink() {
    // 4x2, bottom row first: the bottom row opaque red, the top row half clear blue and fully clear.
    std::vector<std::uint8_t> rgba = {
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
        0,   0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    int width = 4;
    int height = 2;
    const std::vector<std::uint8_t> small = ide::shrink_pixels(rgba, width, height, 2);
    Expect(width == 2 && height == 1 && small.size() == 8, "4x2 fits 2 as 2x1");
    // Left: two red, two blue, all opaque. Right: two red opaque, two clear.
    Expect(small.size() == 8 && small[0] == 128 && small[2] == 128 && small[3] == 255, "the left pixel mixes red and blue");
    Expect(small.size() == 8 && small[4] == 255 && small[6] == 0 && small[7] == 128,
           "clear pixels thin the alpha, not the color");

    width = 2;
    height = 2;
    const std::vector<std::uint8_t> flipped = ide::shrink_pixels({1, 1, 1, 255, 1, 1, 1, 255, 9, 9, 9, 255, 9, 9, 9, 255},
                                                                 width, height, 128);
    Expect(width == 2 && height == 2 && flipped.size() == 16 && flipped[0] == 9 && flipped[8] == 1,
           "a small image keeps its size, turned top row first");
}

// A loaded thumbnail stays while retain lists its file, however long it goes
// unasked for, and goes once retain leaves it out.
void thumbnails_kept_while_in_use() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anarchy-thumbnail-keep-test";
    std::filesystem::create_directories(root);
    const std::filesystem::path file = root / "pixel.bmp";
    // A 1x1 24-bit BMP, its row padded to 4 bytes.
    const unsigned char bmp[] = {'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
                                 40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 4, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 255, 0, 0};
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bmp), sizeof(bmp));
    }
    {
        ide::ThumbnailLoader loader(128, {});
        auto settle = [&loader] {
            for (int tries = 0; tries < 1000 && !loader.idle(); ++tries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        Expect(loader.get(file) == nullptr, "a file's first ask queues it");
        settle();
        Expect(loader.get(file) != nullptr, "it streams in");
        loader.retain({file});
        loader.retain({file});
        Expect(loader.get(file) != nullptr, "it stays while in use, though not asked for");
        loader.retain({});
        Expect(loader.get(file) == nullptr, "it goes once nothing uses it");
        settle();
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// A ball cut from its render and shrunk: clear outside the circle, opaque in
// it, partly clear at its edge, and the edge's color only the ball's.
void ball_cut_from_render() {
    // 8x8, red inside a circle of radius 3 about the middle, blue outside it.
    runner::ViewPixels render;
    render.width = 8;
    render.height = 8;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool inside = std::hypot(x + 0.5 - 4, y + 0.5 - 4) < 3;
            render.rgba.insert(render.rgba.end(), {static_cast<std::uint8_t>(inside ? 255 : 0), 0,
                                                   static_cast<std::uint8_t>(inside ? 0 : 255), 255});
        }
    }
    const runner::ViewPixels ball = ide::cut_ball(render, 0.75, 4);
    Expect(ball.width == 4 && ball.height == 4 && ball.rgba.size() == 64, "the ball is shrunk to the size asked");
    if (ball.rgba.size() != 64) {
        return;
    }
    auto at = [&ball](int x, int y) { return ball.rgba.data() + (y * 4 + x) * 4; };
    Expect(at(0, 0)[3] == 0, "a corner is clear");
    Expect(at(1, 1)[3] == 255 && at(1, 1)[0] == 255 && at(1, 1)[2] == 0, "the middle is the ball, opaque");
    Expect(at(1, 0)[3] > 0 && at(1, 0)[3] < 255, "the edge is partly clear");
    Expect(at(1, 0)[0] == 255 && at(1, 0)[2] == 0, "the edge takes no color from around the ball");
}

// What a Material looks like, read from the place: its numbers, colors, and
// each Texture's Path; nothing for an instance that is not a Material.
void material_look_reads_material() {
    Rig rig;
    const InstanceId gold = rig.make("Material", "Gold", rig.game.service("Materials"));
    auto* material = dynamic_cast<engine_core::Material*>(rig.game.instance(gold));
    auto* brick = dynamic_cast<engine_core::Texture*>(rig.game.instance(rig.brick));
    Expect(material != nullptr && brick != nullptr, "Gold is a Material and Brick a Texture");
    if (material == nullptr || brick == nullptr) {
        return;
    }
    Expect(!brick->set_path("textures/brick.png"), "Brick names a file");
    Expect(!material->set_color({1.f, 0.8f, 0.2f, 1.f}) && !material->set_metalness(0.9) &&
               !material->set_roughness(1.5),
           "Gold's properties are set");
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = rig.brick;
    Expect(!material->set_reference(engine_core::Material::kDiffuseTextureReference, slot), "Gold uses Brick");

    const std::optional<ide::MaterialLook> look = ide::material_look(rig.game, gold);
    Expect(look.has_value(), "a Material has a look");
    if (!look) {
        return;
    }
    Expect(look->color.r == 1.f && look->color.g == 0.8f && look->color.b == 0.2f, "the look has the Color");
    Expect(look->metalness == 0.9f && look->roughness == 1.f, "and the numbers, held between 0 and 1");
    Expect(look->diffuse_texture == "textures/brick.png" && look->normal_texture.empty(),
           "and the Path of each Texture it uses");
    Expect(!ide::material_look(rig.game, rig.brick).has_value(), "a Texture has no look");

    ide::MaterialLook other = *look;
    other.roughness = 0.5f;
    Expect(other != *look, "looks differ by a number");
    other = *look;
    other.diffuse_texture = "textures/rock.png";
    Expect(other != *look, "and by a Texture's Path");
}

// Previews drawn a few a pass, kept until the look changes or the Material
// goes, and the last one shown while a new one waits.
void material_previews_draw_and_keep() {
    std::vector<float> drawn;
    bool fail = false;
    ide::MaterialPreviews previews(
        [&drawn, &fail](const ide::MaterialLook& look, std::shared_ptr<jadefx::Image>& image) {
            drawn.push_back(look.roughness);
            image = fail ? nullptr : jadefx::Image::fromRgba(1, 1, std::vector<std::uint8_t>{0, 0, 0, 255});
            return true;
        },
        2);
    ide::MaterialLook look;
    Expect(previews.idle(), "nothing waits at first");
    Expect(previews.get(1, look) == nullptr, "a preview not drawn yet is null");
    Expect(previews.get(1, look) == nullptr && !previews.idle(), "and waits, once however often it is asked for");
    Expect(previews.draw_pending() && drawn.size() == 1, "a pass draws it");
    const std::shared_ptr<jadefx::Image> first = previews.get(1, look);
    Expect(first != nullptr && previews.idle(), "then it is kept");
    Expect(!previews.draw_pending() && drawn.size() == 1, "and not drawn again for the same look");

    ide::MaterialLook rough = look;
    rough.roughness = 0.9f;
    Expect(previews.get(1, rough) == first, "a changed look shows the last preview while the new one waits");
    previews.draw_pending();
    Expect(drawn.size() == 2 && drawn.back() == 0.9f && previews.get(1, rough) != first,
           "then shows the one drawn for it");

    for (InstanceId id = 2; id <= 6; ++id) {
        previews.get(id, look);
    }
    previews.draw_pending();
    Expect(drawn.size() == 4 && !previews.idle(), "a pass draws only as many as it may");

    previews.retain({1, 2, 3});
    previews.draw_pending();
    Expect(drawn.size() == 4 && previews.idle(), "a Material no longer kept is not drawn");
    Expect(previews.get(2, look) != nullptr && previews.get(4, look) == nullptr,
           "and its preview is gone, while a kept one stays");

    fail = true;
    previews.draw_pending();
    Expect(drawn.size() == 5 && previews.get(4, look) == nullptr, "a draw that fails shows no preview");
    previews.draw_pending();
    Expect(drawn.size() == 5 && previews.idle(), "and is not tried again until the look changes");
}

// A draw that is not ready yet, as while GL prepares the renderer, stays
// queued and ends the pass; a later pass draws it. One never ready gives up.
void material_previews_wait_for_renderer() {
    int ready_after = 2;
    int calls = 0;
    ide::MaterialPreviews previews(
        [&ready_after, &calls](const ide::MaterialLook&, std::shared_ptr<jadefx::Image>& image) {
            ++calls;
            if (ready_after > 0) {
                --ready_after;
                return false;
            }
            image = jadefx::Image::fromRgba(1, 1, std::vector<std::uint8_t>{0, 0, 0, 255});
            return true;
        },
        2);
    ide::MaterialLook look;
    previews.get(1, look);
    previews.get(2, look);
    Expect(!previews.draw_pending() && calls == 1, "a draw not ready yet ends the pass, drawing nothing");
    Expect(previews.get(1, look) == nullptr && !previews.idle(), "and its Material still waits");
    previews.draw_pending();
    Expect(previews.draw_pending() && calls == 4, "once ready, a pass draws as many as it may");
    Expect(previews.get(1, look) != nullptr && previews.get(2, look) != nullptr && previews.idle(),
           "and each waiting Material has its preview");

    ready_after = 1000;
    calls = 0;
    ide::MaterialLook rough = look;
    rough.roughness = 0.9f;
    const std::shared_ptr<jadefx::Image> shown = previews.get(1, rough);
    for (int pass = 0; pass < ide::MaterialPreviews::kMaxWaits; ++pass) {
        previews.draw_pending();
    }
    Expect(calls == ide::MaterialPreviews::kMaxWaits && previews.idle(),
           "a renderer never ready is given up on after kMaxWaits passes");
    Expect(shown != nullptr && previews.get(1, rough) == nullptr, "which counts as a draw that failed");
}

int main() {
    ball_cut_from_render();
    material_look_reads_material();
    material_previews_draw_and_keep();
    material_previews_wait_for_renderer();
    thumbnails_shrink();
    thumbnails_kept_while_in_use();
    starts_in_saved_view();
    navigates();
    selection_is_shared();
    three_views_same_folder();
    list_and_columns();
    new_folder_and_kind();
    rename_delete_cut_paste();
    refused_drop_says_why();
    columns_drop_into_earlier_column();
    drag_shows_icon();
    search_filters();
    pane_follows_tree();
    insert_through_pane();
    prefab_hides_its_models();
    prefab_menu_offers_add_as_game_object();
    up_goes_up_one_level();
    status_counts_only_shown();
    property_edits_refresh();
    texture_preview_shows_file();
    if (gFailures == 0) {
        std::printf("assets tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d assets tests failed\n", gFailures);
    return 1;
}
