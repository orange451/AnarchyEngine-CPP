#include "ide/AssetBrowser.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"

#include <cstdio>
#include <string>
#include <vector>

// The Assets pane's model: navigation, each view's rows, sort, and search.
namespace {

using engine_core::InstanceId;

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

InstanceId Make(engine_core::DataModel& game, const char* klass, const char* name, InstanceId parent) {
    engine_core::DataModel* object = engine_core::lua_create_instance(game, klass);
    game.set_name(object->id(), name);
    game.set_parent(object->id(), parent);
    return object->id();
}

std::vector<std::string> Names(const std::vector<ide::AssetRow>& rows) {
    std::vector<std::string> out;
    for (const ide::AssetRow& row : rows) {
        out.push_back(row.name);
    }
    return out;
}

void TestNavigates() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    engine_core::Game game;
    ide::AssetBrowser browser(game);
    Expect(browser.folder() == game.service("Materials"), "starts in the first category");
    Expect(Names(browser.categories()) ==
               std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"},
           "the five categories, in order");
    const InstanceId walls = Make(game, "Folder", "Walls", game.service("Textures"));
    const InstanceId brick = Make(game, "Texture", "Brick", walls);
    Expect(browser.open(game.service("Textures")), "opens a category");
    Expect(browser.open(walls), "opens a Folder");
    const auto crumbs = browser.crumbs();
    Expect(crumbs.size() == 3 && crumbs[0].second == "Assets" && crumbs[2].second == "Walls", "crumbs from Assets");
    Expect(Names(browser.children()) == std::vector<std::string>{"Brick"}, "a folder's children");
    Expect(browser.back() && browser.folder() == game.service("Textures"), "back");
    Expect(browser.forward() && browser.folder() == walls, "forward");
    Expect(!browser.can_forward(), "nothing after the newest");
    Expect(!browser.open(game.service("Workspace")), "Workspace is not under Assets");
    Expect(!browser.open(brick), "a Texture does not open");
    // The folder shown is destroyed: refresh falls back to its nearest live ancestor.
    game.destroy_tree(walls);
    browser.refresh();
    Expect(browser.folder() == game.service("Textures"), "falls back to the category");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

void TestListsSortsAndSearches() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    engine_core::Game game;
    const InstanceId textures = game.service("Textures");
    const InstanceId b = Make(game, "Folder", "b", textures);
    Make(game, "Texture", "Deep", b);
    const InstanceId c = Make(game, "Texture", "c", textures);
    dynamic_cast<engine_core::Texture*>(game.instance(c))->set_path("textures/c.png");
    Make(game, "Texture", "a", textures);

    ide::AssetBrowser browser(game);
    browser.open(textures);
    Expect(Names(browser.children()) == std::vector<std::string>{"b", "c", "a"}, "icons keep sibling order");
    Expect(Names(browser.list_rows()) == std::vector<std::string>{"a", "b", "c"}, "list sorts by name");
    browser.set_expanded(b, true);
    const std::vector<ide::AssetRow> open = browser.list_rows();
    Expect(Names(open) == std::vector<std::string>{"a", "b", "Deep", "c"}, "an open folder lists its children");
    Expect(open.size() == 4 && open[2].depth == 1 && open[1].opens && open[1].expanded, "under it, one deeper");
    browser.set_sort(ide::AssetSort::Name, true);
    Expect(Names(browser.list_rows()) == std::vector<std::string>{"c", "b", "Deep", "a"}, "descending");
    browser.set_sort(ide::AssetSort::Path, false);
    Expect(browser.list_rows().back().name == "c", "by path, an empty path first");

    browser.set_search("DEE");
    const std::vector<ide::AssetRow> found = browser.search_rows();
    Expect(found.size() == 1 && found[0].name == "Deep" && found[0].where == "b/Deep", "search ignores case");
    browser.set_search("");

    browser.open(b);
    const auto columns = browser.columns();
    Expect(columns.size() == 3, "a column per level: categories, Textures, b");
    Expect(columns.size() == 3 && Names(columns[2]) == std::vector<std::string>{"Deep"}, "the last is the folder");
    Expect(browser.new_kind() == "Texture", "New Texture in a Folder under Textures");
    const InstanceId crate = Make(game, "Prefab", "Crate", game.service("Prefabs"));
    browser.open(crate);
    Expect(browser.new_kind() == "Model", "New Model in a Prefab");
    browser.open(game.service("Audio"));
    Expect(browser.new_kind() == "Sound", "New Sound in Audio");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

}  // namespace

int RunAssetBrowserTests() {
    gFailures = 0;
    TestNavigates();
    TestListsSortsAndSearches();
    return gFailures;
}
