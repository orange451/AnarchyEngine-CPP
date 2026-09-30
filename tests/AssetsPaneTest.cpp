#include "ide/CutSet.hpp"
#include "ide/IdeAssets.hpp"
#include "SelectionService.hpp"

#include "DataModel.hpp"
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

    void clickNode(jadefx::Node* node, int clicks = 1, int button = 0) {
        Expect(node != nullptr, "the node to click is on screen");
        if (node == nullptr) {
            return;
        }
        const double x = node->getAbsoluteX() + node->getWidth() * 0.5;
        const double y = node->getAbsoluteY() + node->getHeight() * 0.5;
        for (int click = 0; click < clicks; ++click) {
            scene->noteButton(button, true, x, y, mods);
            scene->noteButton(button, false, x, y, mods);
        }
    }

    // Lays out at `at`, then clicks the middle of id's widget `clicks` times.
    void clickItem(InstanceId id, double at, int clicks = 1) {
        frame(at);
        clickNode(pane->itemNode(id), clicks);
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

}  // namespace

int main() {
    starts_in_saved_view();
    navigates();
    selection_is_shared();
    three_views_same_folder();
    list_and_columns();
    if (gFailures == 0) {
        std::printf("assets tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d assets tests failed\n", gFailures);
    return 1;
}
