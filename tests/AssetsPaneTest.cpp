#include "ide/CutSet.hpp"
#include "ide/IdeAssets.hpp"
#include "SelectionService.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
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
    rig.key(jadefx::Key::Delete);
    Expect(!rig.batches.empty() && rig.batches.back().first == "Delete" &&
               rig.batches.back().second == std::vector<InstanceId>{rig.brick},
           "Delete deletes the selection");

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
    rig.key(jadefx::Key::Escape);
    Expect(rig.game.selection().get().empty(), "Escape clears the selection");
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
    const InstanceId crate = rig.make("Prefab", "Crate", rig.game.service("Prefabs"));
    rig.pane->openFolder(rig.game.service("Prefabs"));
    rig.pane->openFolder(crate);
    rig.frame(0);
    rig.rightClickEmpty(0.1);
    Expect(rig.menuItem("New Model") != nullptr, "a Prefab offers New Model");
    rig.clickMenu("New Folder");
    Expect((rig.inserts == std::vector<std::pair<std::string, InstanceId>>{{"Folder", crate}}),
           "New Folder inserts a Folder in the Prefab");
    rig.frame(0.2);
    Expect((rig.notices == std::vector<std::string>{"A Prefab holds only Models"}), "the refusal is a notice");
    Expect(rig.game.get_children(crate).empty(), "nothing is left in the Prefab");
    Expect(!rig.editing(), "nothing is renamed");
}

void prefab_menu_offers_add_as_game_object() {
    Rig rig;
    rig.pane->openFolder(rig.textures);
    rig.frame(0);
    rig.rightClickItem(rig.brick, 0.1);
    Expect(rig.menuItem("Add as GameObject") == nullptr, "a Texture's menu has no such item");

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

int main() {
    starts_in_saved_view();
    navigates();
    selection_is_shared();
    three_views_same_folder();
    list_and_columns();
    new_folder_and_kind();
    rename_delete_cut_paste();
    refused_drop_says_why();
    search_filters();
    pane_follows_tree();
    insert_through_pane();
    prefab_menu_offers_add_as_game_object();
    up_goes_up_one_level();
    status_counts_only_shown();
    property_edits_refresh();
    if (gFailures == 0) {
        std::printf("assets tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d assets tests failed\n", gFailures);
    return 1;
}
