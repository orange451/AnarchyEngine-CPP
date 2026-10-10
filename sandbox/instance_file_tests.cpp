// Instance files: a subtree as one JSON file, through the clipboard's copy and paste.

#include "support.hpp"

#include "Folder.hpp"
#include "Gui.hpp"
#include "InstanceFile.hpp"
#include "ModuleScript.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using engine_core::CopiedNode;
using engine_core::InstanceId;

// A plugin-shaped tree under Workspace: Scripts on and off, a module, GUI with CSS, and an empty Folder.
InstanceId sample_tree(engine_core::DataModel& game) {
    engine_core::Folder& root = game.create<engine_core::Folder>();
    game.set_name(root.id(), "MyPlugin");
    game.set_parent(root.id(), workspace_of(game));
    add_script(game, root.id(), "init", "print('hi')");
    engine_core::Script& off = add_script(game, root.id(), "Off", "print('off')");
    off.set_enabled(false);
    engine_core::ModuleScript& lib = game.create<engine_core::ModuleScript>();
    game.set_name(lib.id(), "Lib");
    lib.set_source("return 42");
    game.set_parent(lib.id(), root.id());
    engine_core::Pane& pane = game.create<engine_core::Pane>();
    game.set_name(pane.id(), "Panel");
    game.set_parent(pane.id(), root.id());
    engine_core::Label& label = game.create<engine_core::Label>();
    game.set_name(label.id(), "Title");
    game.set_parent(label.id(), pane.id());
    engine_core::Css& css = game.create<engine_core::Css>();
    game.set_name(css.id(), "Style");
    game.set_parent(css.id(), pane.id());
    engine_core::Folder& empty = game.create<engine_core::Folder>();
    game.set_name(empty.id(), "Empty");
    game.set_parent(empty.id(), root.id());
    return root.id();
}

// A tree's classes, names, and sources, for comparing two trees.
std::string shape(const CopiedNode& node) {
    std::string out = node.class_name + ":" + node.name;
    if (node.has_source) {
        out += "{" + node.source + "}";
    }
    out += "[";
    for (const CopiedNode& child : node.children) {
        out += shape(child) + ",";
    }
    return out + "]";
}

// read_instance_file's error for text, which must not read.
std::string read_error(const char* text) {
    engine_core::JsonValue json;
    std::string error;
    if (!engine_core::parse_json(text, json, error)) {
        return "parse: " + error;
    }
    std::vector<CopiedNode> roots;
    REQUIRE_FALSE(engine_core::read_instance_file(json, roots, error));
    REQUIRE(roots.empty());
    return error;
}

}  // namespace

TEST_CASE("IF1 a tree saves to JSON and reads back the same", "[IF1]") {
    ScriptRig rig;
    const InstanceId root = sample_tree(rig.game);
    const std::vector<CopiedNode> before{engine_core::copy_tree(rig.game, root)};

    const engine_core::JsonValue json = engine_core::write_instance_file(before);
    REQUIRE(json.find("format")->as_string() == "aeinst");
    REQUIRE(json.find("version")->as_number() == 1);

    std::vector<CopiedNode> after;
    std::string error;
    REQUIRE(engine_core::read_instance_file(json, after, error));
    REQUIRE(error.empty());
    REQUIRE(after.size() == 1);
    REQUIRE(shape(after[0]) == shape(before[0]));
    // Off stays disabled.
    REQUIRE(after[0].children[1].properties == before[0].children[1].properties);
}

TEST_CASE("IF2 a file holds several roots, and pasting builds each", "[IF2]") {
    ScriptRig rig;
    const InstanceId a = sample_tree(rig.game);
    const InstanceId b = sample_tree(rig.game);
    rig.game.set_name(b, "Other");
    TempDir dir;
    std::filesystem::create_directories(dir.path);
    std::string error;
    REQUIRE(engine_core::save_instance_file(
        dir / "two.aeinst", {engine_core::copy_tree(rig.game, a), engine_core::copy_tree(rig.game, b)}, error));
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "two.aeinst.tmp"));
    std::vector<CopiedNode> roots;
    REQUIRE(engine_core::load_instance_file(dir / "two.aeinst", roots, error));
    REQUIRE(roots.size() == 2);

    engine_core::Folder& target = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(target.id(), workspace_of(rig.game));
    std::vector<InstanceId> made;
    REQUIRE(engine_core::paste_copies(rig.game, roots, target.id(), &made));
    REQUIRE(made.size() == 2);
    REQUIRE(rig.game.name(made[1]) == "Other");
    REQUIRE(rig.game.find_first_child(made[0], "Lib") != 0);
    REQUIRE(rig.game.find_first_child(made[0], "Empty") != 0);
}

TEST_CASE("IF3 a bad file names where it went wrong and builds nothing", "[IF3]") {
    REQUIRE(read_error(R"({"format":"other","version":1,"roots":[]})").find("format") != std::string::npos);
    REQUIRE(read_error(R"({"format":"aeinst","version":2,"roots":[]})").find("version") != std::string::npos);
    REQUIRE(read_error(R"({"format":"aeinst","version":1,"roots":[{"name":"x"}]})").find("roots[0].class") !=
            std::string::npos);
    REQUIRE(read_error(R"({"format":"aeinst","version":1,"roots":[{"class":"NoSuchClass","name":"x"}]})")
                .find("roots[0].class") != std::string::npos);
    REQUIRE(read_error(R"({"format":"aeinst","version":1,"roots":[{"class":"Folder","name":"x","children":[{"class":"Folder"}]}]})")
                .find("roots[0].children[0].name") != std::string::npos);

    TempDir dir;
    std::filesystem::create_directories(dir.path);
    std::ofstream(dir.path / "half.aeinst") << R"({"format":"aeinst","vers)";
    std::vector<CopiedNode> roots;
    std::string error;
    REQUIRE_FALSE(engine_core::load_instance_file(dir / "half.aeinst", roots, error));
    REQUIRE_FALSE(error.empty());
    error.clear();
    REQUIRE_FALSE(engine_core::load_instance_file(dir / "missing.aeinst", roots, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("IF4 an unknown property key survives a round trip", "[IF4]") {
    ScriptRig rig;
    engine_core::Folder& root = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(root.id(), workspace_of(rig.game));
    rig.game.set_extra_property(root.id(), "FutureThing", engine_core::JsonValue::number(7));
    const engine_core::JsonValue json = engine_core::write_instance_file({engine_core::copy_tree(rig.game, root.id())});
    std::vector<CopiedNode> roots;
    std::string error;
    REQUIRE(engine_core::read_instance_file(json, roots, error));
    const engine_core::JsonValue* kept = engine_core::bag_find(roots[0].properties, "FutureThing");
    REQUIRE(kept != nullptr);
    REQUIRE(kept->as_number() == 7);
}

TEST_CASE("IF5 a key the class writes wins over a stale extra of the same name", "[IF5]") {
    ScriptRig rig;
    const InstanceId root = sample_tree(rig.game);
    engine_core::Script& off = add_script(rig.game, root, "Disabled", "print('off')");
    off.set_enabled(false);
    rig.game.set_extra_property(off.id(), "Enabled", engine_core::JsonValue::boolean(true));

    const CopiedNode copied = engine_core::copy_tree(rig.game, off.id());
    const engine_core::JsonValue* enabled = engine_core::bag_find(copied.properties, "Enabled");
    REQUIRE(enabled != nullptr);
    REQUIRE(enabled->is_bool());
    REQUIRE_FALSE(enabled->as_bool());
}
