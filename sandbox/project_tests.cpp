#include "DataModel.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using engine_core::DataModel;
using engine_core::Game;
using engine_core::InstanceId;
using engine_core::Project;
using engine_core::ProjectError;

namespace {

struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

// A fresh directory under the system temp dir, removed at the end of the test.
struct TempDir {
    fs::path path;

    TempDir() {
        std::random_device device;
        path = fs::temp_directory_path() / ("ae-project-" + std::to_string(device()) + std::to_string(device()));
        fs::remove_all(path);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path operator/(const char* child) const { return path / child; }
};

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

// Every file under src/, path relative to the project root -> bytes.
std::map<std::string, std::string> tree_files(const fs::path& root) {
    std::map<std::string, std::string> out;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root / "src")) {
        if (entry.is_regular_file()) {
            out[fs::relative(entry.path(), root).generic_u8string()] = read_file(entry.path());
        }
    }
    return out;
}

// Paths whose bytes differ, plus paths only on one side.
std::set<std::string> changed(const std::map<std::string, std::string>& before,
                              const std::map<std::string, std::string>& after) {
    std::set<std::string> out;
    for (const auto& [path, bytes] : before) {
        const auto found = after.find(path);
        if (found == after.end() || found->second != bytes) {
            out.insert(path);
        }
    }
    for (const auto& [path, bytes] : after) {
        if (before.count(path) == 0) {
            out.insert(path);
        }
    }
    return out;
}

engine_core::GameObject& add_part(DataModel& game, InstanceId parent, const char* name) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_name(part.id(), name);
    game.set_parent(part.id(), parent);
    return part;
}

engine_core::Script& add_script(DataModel& game, InstanceId parent, const char* name, const char* source) {
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), name);
    script.set_source(source);
    game.set_parent(script.id(), parent);
    return script;
}

engine_core::ColorRgb rgb(float r, float g, float b) {
    engine_core::ColorRgb color;
    color.r = r;
    color.g = g;
    color.b = b;
    return color;
}

std::string leaf(const DataModel& game, InstanceId id, const char* ext = ".json") {
    return "src/" + engine_core::sanitize_file_name(game.name(id)) + "." + game.guid(id) + ext;
}

InstanceId by_guid(const DataModel& game, const std::string& guid) {
    const std::optional<InstanceId> id = game.find_guid(guid);
    REQUIRE(id.has_value());
    return *id;
}

std::vector<std::string> child_guids(const DataModel& game, InstanceId parent) {
    std::vector<std::string> out;
    for (InstanceId child : game.get_children(parent)) {
        out.push_back(game.guid(child));
    }
    return out;
}

std::string meta(const char* klass, const char* guid, const char* name, const char* extra = "") {
    return std::string("{\n  \"class\": \"") + klass + "\",\n  \"id\": \"" + guid + "\",\n  \"Name\": \"" + name +
           "\"" + extra + "\n}\n";
}

// A hand-made project: project.json and a root with the given children array.
void write_bare_project(const fs::path& root, const char* root_extra = "") {
    write_file(root / "project.json",
               "{\"format\": 1, \"name\": \"Hand\", \"engine\": \"engine_core\", \"tree\": {\"src\": \"src\"}, "
               "\"resources\": {\"root\": \"resources\"}}\n");
    write_file(root / "src" / "init.json", meta("DataModel", "root0", "Hand", root_extra));
}

// Sets one key of an instance file on disk, as another editor would.
void edit_key(const fs::path& file, const char* key, const engine_core::JsonValue& value) {
    engine_core::JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(read_file(file), doc, error));
    doc.set(key, value);
    write_file(file, engine_core::write_json(doc));
}

engine_core::JsonValue triple(float x, float y, float z) {
    const float values[3] = {x, y, z};
    return engine_core::json_floats(values, 3);
}

}  // namespace

TEST_CASE("P1 save then load keeps names, GUIDs, and source bytes", "[P1][project]") {
    SimRole role;
    TempDir dir;
    const std::string source = "local x = 1\r\nprint(\"tab\\there\", x)\n-- no newline at end";
    std::string part_guid;
    std::string script_guid;
    std::string root_guid;
    {
        Project project = Project::create(dir.path);
        DataModel& game = project.datamodel();
        engine_core::GameObject& part = add_part(game, 0, "Part");
        part.set_color(rgb(0.25f, 0.5f, 0.1f));
        part.set_transform(engine_core::transform_translation(1.5f, -2.f, 0.1f));
        engine_core::Script& script = add_script(game, 0, "Main", source.c_str());
        part_guid = game.guid(part.id());
        script_guid = game.guid(script.id());
        root_guid = game.guid(0);
        project.save();
        REQUIRE(fs::exists(dir.path / leaf(game, part.id())));
        REQUIRE(fs::exists(dir.path / leaf(game, script.id(), ".luau")));
        REQUIRE(fs::exists(dir.path / leaf(game, script.id(), ".meta.json")));
        REQUIRE(read_file(dir.path / leaf(game, script.id(), ".luau")) == source);
        // Source is only in the .luau file.
        REQUIRE(read_file(dir.path / leaf(game, script.id(), ".meta.json")).find("print") == std::string::npos);
    }
    Project loaded = Project::load(dir.path);
    DataModel& game = loaded.datamodel();
    REQUIRE(game.guid(0) == root_guid);
    const InstanceId part = by_guid(game, part_guid);
    const InstanceId script = by_guid(game, script_guid);
    REQUIRE(game.name(part) == "Part");
    REQUIRE(game.name(script) == "Main");
    REQUIRE(game.parent(part) == 0);
    REQUIRE(std::string(game.instance(script)->class_name()) == "Script");
    REQUIRE(dynamic_cast<engine_core::Script*>(game.instance(script))->source() == source);
    const engine_core::GameObject* body = game.game_object(part);
    REQUIRE(body != nullptr);
    REQUIRE(body->color().r == 0.25f);
    REQUIRE(body->color().g == 0.5f);
    REQUIRE(body->color().b == 0.1f);
    REQUIRE(body->transform().m[12] == 1.5f);
    REQUIRE(body->transform().m[14] == 0.1f);
    REQUIRE(loaded.instance_for(part_guid) == part);
    // Nothing loaded is undoable, and Stop Play returns to this tree.
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("P2 a second save with no edits writes nothing", "[P2][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& folderish = add_part(game, 0, "Holder");
    add_part(game, folderish.id(), "Inner").set_color(rgb(0.f, 1.f, 0.f));
    add_script(game, 0, "Main", "print(1)\n");
    project.save();
    const auto before = tree_files(dir.path);
    project.save();
    REQUIRE(project.last_save().written.empty());
    REQUIRE(project.last_save().moved.empty());
    REQUIRE(project.last_save().removed.empty());
    REQUIRE(tree_files(dir.path) == before);

    // A fresh load, then save: still nothing, even though nothing is cached as dirty.
    Project again = Project::load(dir.path);
    again.save();
    REQUIRE(again.last_save().written.empty());
    REQUIRE(tree_files(dir.path) == before);
}

TEST_CASE("P3 one color edit rewrites only that part", "[P3][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    add_part(game, 0, "B");
    add_script(game, 0, "Main", "print(1)\n");
    project.save();
    const auto before = tree_files(dir.path);

    a.set_color(rgb(0.2f, 0.3f, 0.4f));
    project.save();
    const std::set<std::string> diff = changed(before, tree_files(dir.path));
    REQUIRE(diff == std::set<std::string>{leaf(game, a.id())});
    REQUIRE(project.last_save().written == std::vector<std::string>{leaf(game, a.id())});
    REQUIRE(read_file(dir.path / leaf(game, a.id())).find("\"Color\": [0.2, 0.3, 0.4]") != std::string::npos);
}

TEST_CASE("P4 a source edit rewrites only the .luau file", "[P4][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    add_part(game, 0, "A");
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\n");
    project.save();
    const auto before = tree_files(dir.path);

    main.set_source("print(2)\n");
    project.save();
    REQUIRE(changed(before, tree_files(dir.path)) == std::set<std::string>{leaf(game, main.id(), ".luau")});
    REQUIRE(read_file(dir.path / leaf(game, main.id(), ".luau")) == "print(2)\n");
}

namespace {

struct ScriptRig {
    SimRole role;
    Game game;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;

    ScriptRig() {
        scheduler.reserve(16);
        game.attach_scheduler(&scheduler);
        runtime.attach(game, scheduler);
    }

    void frames(int count, double dt = 1.0 / 60.0) {
        for (int i = 0; i < count; ++i) {
            scheduler.run_phase(engine_core::Phase::Heartbeat, dt);
            game.events().drain();
            runtime.heartbeat(dt);
            game.events().drain();
        }
    }
};

}  // namespace

TEST_CASE("P5 a save during play writes the place, never play-only instances", "[P5][project]") {
    TempDir dir;
    ScriptRig rig;
    {
        Project project = Project::create(dir.path, rig.game);
        add_part(rig.game, 0, "Door");
        add_script(rig.game, 0, "Maker", R"(
            local made = Instance.new("GameObject")
            made.Name = "Session"
            made.Parent = script.Parent
            script.Parent:FindFirstChild("Door").Name = "Moved"
        )");
        project.save();
    }
    Project project = Project::load(dir.path, rig.game);
    const auto saved = tree_files(dir.path);
    const InstanceId door = rig.game.find_first_child(0, "Door");
    REQUIRE(door != 0);

    rig.game.start_simulation();
    rig.frames(1, 0.05);
    const InstanceId session = rig.game.find_first_child(0, "Session");
    REQUIRE(session != 0);
    REQUIRE(rig.game.name(door) == "Moved");

    project.save();
    REQUIRE(project.last_save().written.empty());
    REQUIRE(project.last_save().removed.empty());
    const auto during = tree_files(dir.path);
    REQUIRE(during == saved);
    // The Maker source mentions "Session"; no instance file may be named for it.
    for (const auto& [path, bytes] : during) {
        REQUIRE(bytes.find("\"Name\": \"Session\"") == std::string::npos);
        REQUIRE(bytes.find("\"Name\": \"Moved\"") == std::string::npos);
    }

    rig.game.stop_simulation();
    REQUIRE_FALSE(rig.game.alive(session));
    REQUIRE(rig.game.name(door) == "Door");
    project.save();
    REQUIRE(project.last_save().written.empty());
    REQUIRE(tree_files(dir.path) == saved);
}

TEST_CASE("P6 destroying an authored part deletes its file", "[P6][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& keep = add_part(game, 0, "Keep");
    engine_core::GameObject& gone = add_part(game, 0, "Gone");
    engine_core::GameObject& child = add_part(game, gone.id(), "Child");
    project.save();
    const std::string gone_dir = "src/Gone." + game.guid(gone.id());
    const std::string gone_init = gone_dir + "/init.json";
    const std::string child_file = gone_dir + "/Child." + game.guid(child.id()) + ".json";
    REQUIRE(fs::exists(dir.path / gone_init));
    REQUIRE(fs::exists(dir.path / child_file));

    game.destroy(child.id());
    game.destroy(gone.id());
    project.save();
    REQUIRE_FALSE(fs::exists(dir.path / gone_init));
    REQUIRE_FALSE(fs::exists(dir.path / child_file));
    // The emptied folder goes too.
    REQUIRE_FALSE(fs::exists(dir.path / gone_dir));
    REQUIRE(fs::exists(dir.path / leaf(game, keep.id())));
    REQUIRE(project.last_save().removed.size() == 2);
}

TEST_CASE("P7 a children array orders siblings; without one they sort by GUID", "[P7][project]") {
    SimRole role;
    SECTION("hand-written order") {
        TempDir dir;
        write_bare_project(dir.path, ",\n  \"children\": [\"ccc\", \"aaa\"]");
        write_file(dir.path / "src" / "Z.aaa.json", meta("Folder", "aaa", "Z"));
        write_file(dir.path / "src" / "Y.bbb.json", meta("Folder", "bbb", "Y"));
        write_file(dir.path / "src" / "X.ccc.json", meta("Folder", "ccc", "X"));
        Project project = Project::load(dir.path);
        REQUIRE(child_guids(project.datamodel(), 0) == std::vector<std::string>{"ccc", "aaa", "bbb"});
    }
    SECTION("omitted sorts by GUID, not by Name") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "A.ccc.json", meta("Folder", "ccc", "A"));
        write_file(dir.path / "src" / "B.aaa.json", meta("Folder", "aaa", "B"));
        write_file(dir.path / "src" / "C.bbb.json", meta("Folder", "bbb", "C"));
        Project project = Project::load(dir.path);
        REQUIRE(child_guids(project.datamodel(), 0) == std::vector<std::string>{"aaa", "bbb", "ccc"});
        REQUIRE(project.datamodel().find_first_child(0, "B") == *project.datamodel().find_guid("aaa"));
    }
    SECTION("live order round-trips") {
        TempDir dir;
        std::vector<std::string> order;
        {
            Project project = Project::create(dir.path);
            DataModel& game = project.datamodel();
            for (int i = 0; i < 6; ++i) {
                add_part(game, 0, "Part");
            }
            order = child_guids(game, 0);
            project.save();
            const bool sorted = std::is_sorted(order.begin(), order.end());
            REQUIRE((read_file(dir.path / "src" / "init.json").find("\"children\"") == std::string::npos) == sorted);
        }
        Project loaded = Project::load(dir.path);
        REQUIRE(child_guids(loaded.datamodel(), 0) == order);
    }
}

TEST_CASE("P8 create writes the project skeleton", "[P8][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path / "MyPlace");
    const fs::path root = dir.path / "MyPlace";
    REQUIRE(project.name() == "MyPlace");
    engine_core::JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(read_file(root / "project.json"), doc, error));
    REQUIRE(doc.find("format")->as_number() == 1);
    REQUIRE(doc.find("name")->as_string() == "MyPlace");
    REQUIRE(doc.find("engine")->as_string() == "engine_core");
    REQUIRE(doc.find("tree")->find("src")->as_string() == "src");
    REQUIRE(doc.find("resources")->find("root")->as_string() == "resources");
    for (const char* kind : {"textures", "meshes", "audio"}) {
        REQUIRE(fs::is_regular_file(root / "resources" / kind / ".gitkeep"));
    }
    const std::string readme = read_file(root / "RESOURCES.md");
    REQUIRE(readme.find("project.json") != std::string::npos);
    REQUIRE(readme.find("must not rewrite") != std::string::npos);
    REQUIRE(read_file(root / ".gitignore").find(".studio/") != std::string::npos);
    REQUIRE(read_file(root / ".gitattributes").find("*.luau text eol=lf") != std::string::npos);
    REQUIRE(fs::is_regular_file(root / "src" / "init.json"));
    // The root is game, class Game.
    REQUIRE(engine_core::parse_json(read_file(root / "src" / "init.json"), doc, error));
    REQUIRE(doc.find("class")->as_string() == "Game");
    REQUIRE_THROWS_AS(Project::create(root), ProjectError);
}

// Enabled is Script's. A ModuleScript saved when it had one still loads, and
// the key stays in its file as one the class does not know.
TEST_CASE("P16 a ModuleScript saved with Enabled still loads", "[P16][project]") {
    SimRole role;
    TempDir dir;
    write_bare_project(dir.path);
    write_file(dir.path / "src" / "Mod.aaa.luau", "return 1\n");
    write_file(dir.path / "src" / "Mod.aaa.meta.json", meta("ModuleScript", "aaa", "Mod", ",\n  \"Enabled\": false"));
    write_file(dir.path / "src" / "Main.bbb.luau", "print(1)\n");
    write_file(dir.path / "src" / "Main.bbb.meta.json", meta("Script", "bbb", "Main", ",\n  \"Enabled\": false"));

    Project project = Project::load(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId mod = by_guid(game, "aaa");
    REQUIRE(dynamic_cast<engine_core::ModuleScript*>(game.instance(mod)) != nullptr);
    const engine_core::JsonValue* kept = engine_core::bag_find(game.extra_properties(mod), "Enabled");
    REQUIRE(kept != nullptr);
    REQUIRE(kept->is_bool());
    REQUIRE_FALSE(kept->as_bool());
    auto* main = dynamic_cast<engine_core::Script*>(game.instance(by_guid(game, "bbb")));
    REQUIRE(main != nullptr);
    REQUIRE_FALSE(main->enabled());
    REQUIRE(engine_core::bag_find(game.extra_properties(main->id()), "Enabled") == nullptr);
}

TEST_CASE("P9 an unknown hand-edited key round-trips through the property bag", "[P9][project]") {
    SimRole role;
    TempDir dir;
    std::string guid;
    std::string path;
    {
        Project project = Project::create(dir.path);
        engine_core::GameObject& part = add_part(project.datamodel(), 0, "Part");
        guid = project.datamodel().guid(part.id());
        project.save();
        path = leaf(project.datamodel(), part.id());
    }
    const std::string original = read_file(dir.path / path);
    std::string edited = original;
    edited.insert(edited.rfind('\n', edited.size() - 2), ",\n  \"path\": \"textures/brick.png\"");
    write_file(dir.path / path, edited);

    Project project = Project::load(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId part = by_guid(game, guid);
    const engine_core::JsonValue* value = engine_core::bag_find(game.extra_properties(part), "path");
    REQUIRE(value != nullptr);
    REQUIRE(value->as_string() == "textures/brick.png");

    // Untouched, it is not rewritten. Edited, the key survives the rewrite.
    project.save();
    REQUIRE(read_file(dir.path / path) == edited);
    game.game_object(part)->set_color(rgb(1.f, 0.f, 0.f));
    project.save();
    const std::string rewritten = read_file(dir.path / path);
    REQUIRE(rewritten.find("\"path\": \"textures/brick.png\"") != std::string::npos);
    Project again = Project::load(dir.path);
    REQUIRE(engine_core::bag_find(again.datamodel().extra_properties(by_guid(again.datamodel(), guid)), "path")
                ->as_string() == "textures/brick.png");
}

TEST_CASE("P11 two siblings named Part are two files", "[P11][project]") {
    SimRole role;
    TempDir dir;
    std::string first;
    std::string second;
    {
        Project project = Project::create(dir.path);
        DataModel& game = project.datamodel();
        first = game.guid(add_part(game, 0, "Part").id());
        second = game.guid(add_part(game, 0, "Part").id());
        REQUIRE(first != second);
        project.save();
    }
    REQUIRE(fs::exists(dir.path / "src" / ("Part." + first + ".json")));
    REQUIRE(fs::exists(dir.path / "src" / ("Part." + second + ".json")));
    Project project = Project::load(dir.path);
    DataModel& game = project.datamodel();
    REQUIRE(game.get_children(0).size() == 2);
    REQUIRE(game.name(by_guid(game, first)) == "Part");
    REQUIRE(game.name(by_guid(game, second)) == "Part");
}

TEST_CASE("P12 adding a third Part adds one file and renames none", "[P12][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "Part").id();
    const InstanceId b = add_part(game, 0, "Part").id();
    project.save();
    const auto before = tree_files(dir.path);
    const std::string a_file = leaf(game, a);
    const std::string b_file = leaf(game, b);

    const InstanceId c = add_part(game, 0, "Part").id();
    project.save();
    const auto after = tree_files(dir.path);
    REQUIRE(after.at(a_file) == before.at(a_file));
    REQUIRE(after.at(b_file) == before.at(b_file));
    REQUIRE(project.last_save().moved.empty());
    // One new instance file. The root's init.json may change only to record child order.
    std::set<std::string> diff = changed(before, after);
    diff.erase("src/init.json");
    REQUIRE(diff == std::set<std::string>{leaf(game, c)});
    REQUIRE(after.size() == before.size() + 1);
}

TEST_CASE("P13 two Scripts named Main keep their own source", "[P13][project]") {
    SimRole role;
    TempDir dir;
    std::string one;
    std::string two;
    {
        Project project = Project::create(dir.path);
        DataModel& game = project.datamodel();
        one = game.guid(add_script(game, 0, "Main", "return 'one'\n").id());
        two = game.guid(add_script(game, 0, "Main", "return 'two'\n").id());
        project.save();
    }
    Project project = Project::load(dir.path);
    DataModel& game = project.datamodel();
    auto source_of = [&game](const std::string& guid) {
        return dynamic_cast<engine_core::Script*>(game.instance(by_guid(game, guid)))->source();
    };
    REQUIRE(source_of(one) == "return 'one'\n");
    REQUIRE(source_of(two) == "return 'two'\n");
}

TEST_CASE("P14 a rename moves only that file and keeps the GUID", "[P14][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId floor = add_part(game, 0, "Part").id();
    const InstanceId other = add_part(game, 0, "Part").id();
    project.save();
    const std::string guid = game.guid(floor);
    const std::string old_file = leaf(game, floor);
    const std::string other_file = leaf(game, other);
    const std::string other_bytes = read_file(dir.path / other_file);

    game.set_name(floor, "Floor");
    project.save();
    const std::string new_file = "src/Floor." + guid + ".json";
    REQUIRE(game.guid(floor) == guid);
    REQUIRE_FALSE(fs::exists(dir.path / old_file));
    REQUIRE(fs::exists(dir.path / new_file));
    REQUIRE(project.last_save().moved == std::vector<std::string>{old_file + " -> " + new_file});
    REQUIRE(read_file(dir.path / new_file).find("\"id\": \"" + guid + "\"") != std::string::npos);
    REQUIRE(read_file(dir.path / other_file) == other_bytes);
}

TEST_CASE("P15 illegal Name characters save as _ with the real Name inside", "[P15][project]") {
    SimRole role;
    TempDir dir;
    std::string slash;
    std::string mixed;
    {
        Project project = Project::create(dir.path);
        DataModel& game = project.datamodel();
        slash = game.guid(add_part(game, 0, "/").id());
        mixed = game.guid(add_part(game, 0, "a:b*c?").id());
        project.save();
    }
    const fs::path slash_file = dir.path / "src" / ("_." + slash + ".json");
    REQUIRE(fs::exists(slash_file));
    REQUIRE(read_file(slash_file).find("\"Name\": \"/\"") != std::string::npos);
    REQUIRE(fs::exists(dir.path / "src" / ("a_b_c_." + mixed + ".json")));
    Project project = Project::load(dir.path);
    REQUIRE(project.datamodel().name(by_guid(project.datamodel(), slash)) == "/");
    REQUIRE(project.datamodel().name(by_guid(project.datamodel(), mixed)) == "a:b*c?");
}

TEST_CASE("sanitize_file_name covers empty, dots, and device names", "[project]") {
    REQUIRE(engine_core::sanitize_file_name("") == "_");
    REQUIRE(engine_core::sanitize_file_name(".") == "_");
    REQUIRE(engine_core::sanitize_file_name("..") == "_");
    REQUIRE(engine_core::sanitize_file_name(".hidden") == "_hidden");
    REQUIRE(engine_core::sanitize_file_name("con") == "_con");
    REQUIRE(engine_core::sanitize_file_name("COM1.txt") == "_COM1.txt");
    REQUIRE(engine_core::sanitize_file_name("Console") == "Console");
    REQUIRE(engine_core::sanitize_file_name("tab\there") == "tab_here");
    REQUIRE(engine_core::sanitize_file_name(std::string(300, 'x')).size() == 120);
}

TEST_CASE("load errors", "[project]") {
    SimRole role;
    SECTION("missing project.json") {
        TempDir dir;
        fs::create_directories(dir.path / "src");
        REQUIRE_THROWS_AS(Project::load(dir.path), ProjectError);
    }
    SECTION("filename GUID differs from id") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "Part.aaa.json", meta("GameObject", "bbb", "Part"));
        REQUIRE_THROWS_AS(Project::load(dir.path), ProjectError);
    }
    SECTION("two files claim one GUID") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "Part.aaa.json", meta("GameObject", "aaa", "Part"));
        write_file(dir.path / "src" / "F.fff" / "init.json", meta("Folder", "fff", "F"));
        write_file(dir.path / "src" / "F.fff" / "Part.aaa.json", meta("GameObject", "aaa", "Part"));
        REQUIRE_THROWS_AS(Project::load(dir.path), ProjectError);
    }
    SECTION("unknown class") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "T.aaa.json", meta("Texture", "aaa", "T"));
        REQUIRE_THROWS_AS(Project::load(dir.path), ProjectError);
    }
    SECTION("only the root is a Game") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "G.aaa.json", meta("Game", "aaa", "G"));
        REQUIRE_THROWS_AS(Project::load(dir.path), ProjectError);
    }
    SECTION("the root is a Game or, from before Game, a DataModel") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "init.json", meta("Folder", "root0", "Hand"));
        REQUIRE_THROWS_AS(Project::load(dir.path), ProjectError);
    }
    SECTION("a .luau without .meta.json is an error") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "Main.aaa.luau", "print(1)\n");
        REQUIRE_THROWS_AS(Project::load(dir.path), ProjectError);
    }
    SECTION("Source inside json is refused") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "Main.aaa.luau", "print(1)\n");
        write_file(dir.path / "src" / "Main.aaa.meta.json", meta("Script", "aaa", "Main", ",\n  \"Source\": \"x\""));
        REQUIRE_THROWS_AS(Project::load(dir.path), ProjectError);
    }
    SECTION("a bad value leaves the bound DataModel untouched") {
        TempDir dir;
        write_bare_project(dir.path);
        write_file(dir.path / "src" / "P.aaa.json", meta("GameObject", "aaa", "P", ",\n  \"Color\": [1]"));
        Game game;
        const InstanceId keep = add_part(game, 0, "Keep").id();
        REQUIRE_THROWS_AS(Project::load(dir.path, game), ProjectError);
        REQUIRE(game.alive(keep));
        REQUIRE(game.find_first_child(0, "Keep") == keep);
    }
}

TEST_CASE("folders: first child, folder rename, script with children, undo of a delete", "[project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Folder& box = game.create<engine_core::Folder>();
    game.set_name(box.id(), "Box");
    game.set_parent(box.id(), 0);
    project.save();
    const std::string box_guid = game.guid(box.id());
    REQUIRE(fs::exists(dir.path / "src" / ("Box." + box_guid + ".json")));

    // The first child turns the leaf into a folder.
    const InstanceId inner = add_part(game, box.id(), "Inner").id();
    project.save();
    const std::string box_dir = "src/Box." + box_guid;
    REQUIRE_FALSE(fs::exists(dir.path / "src" / ("Box." + box_guid + ".json")));
    REQUIRE(fs::exists(dir.path / box_dir / "init.json"));
    REQUIRE(fs::exists(dir.path / box_dir / ("Inner." + game.guid(inner) + ".json")));

    // Renaming the folder moves its children with it.
    const std::string inner_bytes = read_file(dir.path / box_dir / ("Inner." + game.guid(inner) + ".json"));
    game.set_name(box.id(), "Crate");
    project.save();
    const std::string crate_dir = "src/Crate." + box_guid;
    REQUIRE_FALSE(fs::exists(dir.path / box_dir));
    REQUIRE(read_file(dir.path / crate_dir / ("Inner." + game.guid(inner) + ".json")) == inner_bytes);

    // A script with a child is a folder with init.luau and init.meta.json.
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\n");
    add_part(game, main.id(), "Handle");
    project.save();
    const std::string main_dir = "src/Main." + game.guid(main.id());
    REQUIRE(read_file(dir.path / main_dir / "init.luau") == "print(1)\n");
    REQUIRE(fs::exists(dir.path / main_dir / "init.meta.json"));

    // Undo of a delete brings back the same GUID, so the same file.
    game.history().end_gesture();
    const std::string inner_guid = game.guid(inner);
    game.destroy(inner);
    game.history().end_gesture();
    project.save();
    REQUIRE_FALSE(fs::exists(dir.path / crate_dir / ("Inner." + inner_guid + ".json")));
    game.history().undo();
    REQUIRE(game.guid(inner) == inner_guid);
    project.save();
    REQUIRE(read_file(dir.path / crate_dir / ("Inner." + inner_guid + ".json")) == inner_bytes);

    Project loaded = Project::load(dir.path);
    DataModel& again = loaded.datamodel();
    const InstanceId script = by_guid(again, game.guid(main.id()));
    REQUIRE(dynamic_cast<engine_core::Script*>(again.instance(script))->source() == "print(1)\n");
    REQUIRE(again.name(again.get_children(script).at(0)) == "Handle");
}

TEST_CASE("save_as writes the whole tree under a new root", "[project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path / "One");
    add_part(project.datamodel(), 0, "Part");
    project.save();
    write_file(dir.path / "One" / "resources" / "textures" / "brick.png", std::string("\x89PNG", 4));
    project.save_as(dir.path / "Two");
    REQUIRE(project.root() == dir.path / "Two");
    REQUIRE(tree_files(dir.path / "One") == tree_files(dir.path / "Two"));
    REQUIRE(read_file(dir.path / "Two" / "resources" / "textures" / "brick.png") == std::string("\x89PNG", 4));
}

TEST_CASE("json numbers use one formatter", "[project]") {
    using engine_core::JsonValue;
    REQUIRE(engine_core::format_json_number(1.0) == "1");
    REQUIRE(engine_core::format_json_number(-0.0) == "0");
    REQUIRE(engine_core::format_json_number(0.5) == "0.5");
    REQUIRE(engine_core::write_json(JsonValue::number_from_float(0.1f)) == "0.1\n");
    REQUIRE(engine_core::write_json(JsonValue::number_from_float(1.0f / 3.0f)) == "0.33333334\n");
    JsonValue doc = JsonValue::object();
    doc.set("b", JsonValue::number(2));
    doc.set("Name", JsonValue::string("x"));
    doc.set("id", JsonValue::string("g"));
    doc.set("class", JsonValue::string("Folder"));
    REQUIRE(engine_core::write_json(doc) ==
            "{\n  \"class\": \"Folder\",\n  \"id\": \"g\",\n  \"Name\": \"x\",\n  \"b\": 2\n}\n");
}

TEST_CASE("adopt writes an unsaved place without clearing it", "[project]") {
    SimRole role;
    TempDir dir;
    Game game;
    const InstanceId part = add_part(game, 0, "Part").id();
    game.history().end_gesture();
    REQUIRE(game.history().can_undo().first);
    const std::string guid = game.guid(part);

    Project project = Project::adopt(dir.path / "Adopted", game);
    REQUIRE(&project.datamodel() == &game);
    REQUIRE(project.name() == "Adopted");
    REQUIRE(game.alive(part));
    REQUIRE(game.history().can_undo().first);
    REQUIRE(fs::exists(dir.path / "Adopted" / "project.json"));
    REQUIRE(fs::exists(dir.path / "Adopted" / "src" / ("Part." + guid + ".json")));
    REQUIRE_THROWS_AS(Project::adopt(dir.path / "Adopted", game), ProjectError);

    Project loaded = Project::load(dir.path / "Adopted");
    REQUIRE(loaded.datamodel().name(by_guid(loaded.datamodel(), guid)) == "Part");
}

TEST_CASE("the fingerprint changes with the saved bytes, not with edits that cancel out", "[project]") {
    TempDir dir;
    ScriptRig rig;
    Project project = Project::create(dir.path, rig.game);
    DataModel& game = rig.game;
    engine_core::GameObject& part = add_part(game, 0, "Part");
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\n");
    project.save();
    const std::uint64_t saved = Project::place_fingerprint(game);
    REQUIRE(Project::place_fingerprint(game) == saved);

    part.set_color(rgb(1.f, 0.f, 0.f));
    REQUIRE(Project::place_fingerprint(game) != saved);
    part.set_color(rgb(1.f, 1.f, 1.f));
    REQUIRE(Project::place_fingerprint(game) == saved);

    game.set_name(part.id(), "Floor");
    REQUIRE(Project::place_fingerprint(game) != saved);
    game.set_name(part.id(), "Part");
    main.set_source("print(2)\n");
    REQUIRE(Project::place_fingerprint(game) != saved);
    main.set_source("print(1)\n");

    const InstanceId extra = add_part(game, 0, "Extra").id();
    REQUIRE(Project::place_fingerprint(game) != saved);
    game.destroy(extra);
    REQUIRE(Project::place_fingerprint(game) == saved);

    // Play-only instances are not part of what a save writes.
    game.capture_place();
    game.start_simulation();
    add_part(game, 0, "Session");
    REQUIRE(Project::place_fingerprint(game) == saved);
    game.stop_simulation();
    REQUIRE(Project::place_fingerprint(game) == saved);
}

TEST_CASE("reset_place empties the place and drops undo", "[project]") {
    SimRole role;
    Game game;
    add_part(game, 0, "Part");
    add_script(game, 0, "Main", "print(1)\n");
    game.history().end_gesture();
    REQUIRE(game.history().can_undo().first);
    const std::string old_root = game.guid(0);
    game.start_simulation();

    Project::reset_place(game);
    REQUIRE_FALSE(game.simulation_running());
    REQUIRE(game.get_children(0).empty());
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE(game.guid(0) != old_root);
    // Stop Play returns to the empty place, not the old one.
    game.start_simulation();
    add_part(game, 0, "Session");
    game.stop_simulation();
    REQUIRE(game.get_children(0).empty());
}

TEST_CASE("a project opened in a running engine keeps colors and transforms", "[project]") {
    TempDir dir;
    std::string guid;
    {
        SimRole role;
        Project project = Project::create(dir.path);
        engine_core::GameObject& part = add_part(project.datamodel(), 0, "Part");
        part.set_color(rgb(0.25f, 0.5f, 0.75f));
        part.set_transform(engine_core::transform_translation(4.f, 5.f, 6.f));
        guid = project.datamodel().guid(part.id());
        project.save();
    }
    // The IDE opens a project as a paused edit on the UI thread.
    engine_core::Engine engine;
    engine.start();
    std::unique_ptr<Project> project;
    engine.on_simulation([&](DataModel& game) { project = std::make_unique<Project>(Project::load(dir.path, game)); });
    const DataModel& game = engine.datamodel();
    const engine_core::GameObject* part = game.game_object(*game.find_guid(guid));
    REQUIRE(part != nullptr);
    REQUIRE(part->color().g == 0.5f);
    REQUIRE(part->transform().m[14] == 6.f);
    // Nothing waits for the next step: a save right away writes the same bytes.
    const auto before = tree_files(dir.path);
    engine.on_simulation([&](DataModel&) { project->save(); });
    REQUIRE(project->last_save().written.empty());
    REQUIRE(tree_files(dir.path) == before);
    engine.stop();
}

TEST_CASE("destroy_tree destroys descendants and one undo brings them back", "[project][history]") {
    SimRole role;
    Game game;
    engine_core::Folder& box = game.create<engine_core::Folder>();
    game.set_parent(box.id(), 0);
    const InstanceId inner = add_part(game, box.id(), "Inner").id();
    const InstanceId deep = add_part(game, inner, "Deep").id();
    game.history().end_gesture();
    game.history().reset_waypoints();

    game.history().set_pending_gesture("Delete");
    game.destroy_tree(box.id());
    game.history().end_gesture();
    REQUIRE_FALSE(game.alive(box.id()));
    REQUIRE_FALSE(game.alive(inner));
    REQUIRE_FALSE(game.alive(deep));
    REQUIRE(game.history().can_undo().second == "Delete");

    game.history().undo();
    REQUIRE(game.alive(box.id()));
    REQUIRE(game.parent(box.id()) == 0);
    REQUIRE(game.parent(inner) == box.id());
    REQUIRE(game.parent(deep) == inner);
    REQUIRE(game.name(deep) == "Deep");

    game.destroy_tree(0);
    REQUIRE(game.get_children(0).size() == 1);

    std::vector<engine_core::ContextAction> root_actions;
    game.context_actions(root_actions);
    std::vector<engine_core::ContextAction> part_actions;
    game.instance(inner)->context_actions(part_actions);
    auto has_delete = [](const std::vector<engine_core::ContextAction>& actions) {
        return std::any_of(actions.begin(), actions.end(),
                           [](const engine_core::ContextAction& a) { return std::string(a.name) == "Delete"; });
    };
    REQUIRE_FALSE(has_delete(root_actions));
    REQUIRE(has_delete(part_actions));
}

namespace {

// The conflicts a guarded save stopped on. Empty when it saved.
std::vector<engine_core::SaveConflict> save_conflicts(Project& project) {
    try {
        project.save();
    } catch (const engine_core::ProjectConflict& conflict) {
        return conflict.conflicts();
    }
    return {};
}

}  // namespace

TEST_CASE("G1 an outside edit to an instance the studio left alone survives a save", "[G1][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    const std::string outside = read_file(dir.path / leaf(game, a.id())) + "\n";
    write_file(dir.path / leaf(game, a.id()), outside);

    b.set_color(rgb(0.f, 0.f, 1.f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().written == std::vector<std::string>{leaf(game, b.id())});
    REQUIRE(read_file(dir.path / leaf(game, a.id())) == outside);
}

TEST_CASE("G2 an outside edit under a studio edit stops the save and writes nothing", "[G2][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    const std::string path = leaf(game, a.id());
    edit_key(dir.path / path, "Size", triple(2, 2, 2));

    a.set_color(rgb(1.f, 0.f, 0.f));
    b.set_color(rgb(0.f, 1.f, 0.f));
    const auto before = tree_files(dir.path);
    std::string message;
    try {
        project.save();
    } catch (const engine_core::ProjectConflict& conflict) {
        message = conflict.what();
        REQUIRE(conflict.conflicts().size() == 1);
        REQUIRE(conflict.conflicts()[0].guid == game.guid(a.id()));
        REQUIRE(conflict.conflicts()[0].path == path);
        REQUIRE(conflict.conflicts()[0].kind == engine_core::SaveConflict::Kind::EditedOutside);
        REQUIRE(conflict.conflicts()[0].key == "Size");
        REQUIRE(conflict.conflicts()[0].studio == "(default)");
        REQUIRE(conflict.conflicts()[0].disk == "2, 2, 2");
        REQUIRE(conflict.conflicts()[0].name == "A");
        REQUIRE(conflict.conflicts()[0].where == "game");
    }
    REQUIRE(message.find(path) != std::string::npos);
    // Nothing was written, B included, and the next save sees the same thing.
    REQUIRE(tree_files(dir.path) == before);
    REQUIRE(save_conflicts(project).size() == 1);
    REQUIRE(tree_files(dir.path) == before);
}

TEST_CASE("G3 a studio delete of a file edited outside stops the save", "[G3][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    const std::string path = leaf(game, a);
    edit_key(dir.path / path, "Size", triple(2, 2, 2));
    const std::string outside = read_file(dir.path / path);

    game.destroy(a);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].path == path);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::EditedOutside);
    REQUIRE(conflicts[0].key.empty());
    REQUIRE(read_file(dir.path / path) == outside);
}

TEST_CASE("G4 a folder rename carries a child edited outside along", "[G4][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& holder = add_part(game, 0, "Holder");
    engine_core::GameObject& inner = add_part(game, holder.id(), "Inner");
    project.save();
    const std::string inner_name = "/Inner." + game.guid(inner.id()) + ".json";
    const fs::path old_path = dir.path / ("src/Holder." + game.guid(holder.id()) + inner_name);
    const std::string outside = read_file(old_path) + "\n";
    write_file(old_path, outside);

    // Inner's bytes do not change, so the save only moves its file.
    game.set_name(holder.id(), "Box");
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / ("src/Box." + game.guid(holder.id()) + inner_name)) == outside);
}

TEST_CASE("G5 a script's two files are checked one by one", "[G5][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\n");
    project.save();
    const std::string luau = leaf(game, main.id(), ".luau");
    write_file(dir.path / luau, "print(\"outside\")\n");

    SECTION("a studio change to the other file saves, and keeps the outside source") {
        main.set_enabled(false);
        REQUIRE(save_conflicts(project).empty());
        REQUIRE(project.last_save().written == std::vector<std::string>{leaf(game, main.id(), ".meta.json")});
        REQUIRE(read_file(dir.path / luau) == "print(\"outside\")\n");
    }
    SECTION("a studio change to the same file stops the save") {
        main.set_source("print(2)\n");
        const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
        REQUIRE(conflicts.size() == 1);
        REQUIRE(conflicts[0].path == luau);
        REQUIRE(conflicts[0].key == "Source");
        REQUIRE(read_file(dir.path / luau) == "print(\"outside\")\n");
    }
}

TEST_CASE("G6 a file rewritten with the same bytes is no conflict", "[G6][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    write_file(dir.path / path, read_file(dir.path / path));

    a.set_color(rgb(1.f, 0.f, 0.f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().written == std::vector<std::string>{path});
}

TEST_CASE("G7 a save during play checks the place captured at Test", "[G7][guard][project]") {
    TempDir dir;
    ScriptRig rig;
    {
        Project project = Project::create(dir.path, rig.game);
        add_part(rig.game, 0, "Door");
        project.save();
    }
    Project project = Project::load(dir.path, rig.game);
    const InstanceId door = rig.game.find_first_child(0, "Door");
    REQUIRE(door != 0);
    const std::string path = leaf(rig.game, door);
    edit_key(dir.path / path, "Size", triple(2, 2, 2));
    // An edit before Test, recaptured into the place as the studio does after each edit.
    rig.game.game_object(door)->set_color(rgb(1.f, 0.f, 0.f));
    rig.game.capture_place();

    rig.game.start_simulation();
    rig.frames(1, 0.05);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].path == path);
    rig.game.stop_simulation();
}

namespace {

// src/ files whose own name carries guid: a leaf's .json, .meta.json, or .luau.
std::vector<std::string> leaf_files_of(const fs::path& root, const std::string& guid) {
    std::vector<std::string> out;
    for (const auto& [path, bytes] : tree_files(root)) {
        if (path.substr(path.rfind('/') + 1).find("." + guid + ".") != std::string::npos) {
            out.push_back(path);
        }
    }
    return out;
}

// A folder instance Box holding Keep, and a leaf beside it.
struct BoxAndLeaf {
    InstanceId box = 0;
    InstanceId keep = 0;
    InstanceId leaf = 0;

    BoxAndLeaf(DataModel& game, const char* leaf_name) {
        box = add_part(game, 0, "Box").id();
        keep = add_part(game, box, "Keep").id();
        leaf = add_part(game, 0, leaf_name).id();
    }

    // Where a file for the leaf lands when it is moved into Box.
    fs::path moved(const fs::path& root, const DataModel& game) const {
        return root / ("src/Box." + game.guid(box)) /
               (engine_core::sanitize_file_name(game.name(leaf)) + "." + game.guid(leaf) + ".json");
    }
};

}  // namespace

TEST_CASE("G8 a file deleted outside stays deleted when the studio left it alone", "[G8][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    fs::remove(dir.path / leaf(game, a.id()));

    b.set_color(rgb(0.f, 0.f, 1.f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().written == std::vector<std::string>{leaf(game, b.id())});
    REQUIRE_FALSE(fs::exists(dir.path / leaf(game, a.id())));
    project.save();
    REQUIRE(project.last_save().written.empty());
    REQUIRE_FALSE(fs::exists(dir.path / leaf(game, a.id())));
}

TEST_CASE("G9 a leaf moved outside keeps one file when the studio left it alone", "[G9][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const BoxAndLeaf ids(game, "Loose");
    project.save();
    const std::string guid = game.guid(ids.leaf);
    fs::rename(dir.path / leaf(game, ids.leaf), ids.moved(dir.path, game));

    game.game_object(ids.keep)->set_color(rgb(0.f, 1.f, 0.f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(leaf_files_of(dir.path, guid).size() == 1);
    Project loaded = Project::load(dir.path);
    DataModel& again = loaded.datamodel();
    REQUIRE(again.guid(again.parent(by_guid(again, guid))) == game.guid(ids.box));
}

TEST_CASE("G10 a file deleted outside under a studio edit stops the save", "[G10][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    fs::remove(dir.path / path);

    a.set_color(rgb(1.f, 0.f, 0.f));
    const auto before = tree_files(dir.path);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].path == path);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::DeletedOutside);
    REQUIRE(tree_files(dir.path) == before);
}

TEST_CASE("G11 a file moved outside under a studio edit stops the save", "[G11][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    // A dot in the Name: the GUID is still the last part of the file name.
    const BoxAndLeaf ids(game, "Loose.v2");
    project.save();
    const std::string path = leaf(game, ids.leaf);
    fs::rename(dir.path / path, ids.moved(dir.path, game));

    game.game_object(ids.leaf)->set_color(rgb(1.f, 0.f, 0.f));
    const auto before = tree_files(dir.path);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].guid == game.guid(ids.leaf));
    REQUIRE(conflicts[0].path == path);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::MovedOutside);
    REQUIRE(tree_files(dir.path) == before);
}

TEST_CASE("G12 a file deleted on both sides saves quietly", "[G12][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    fs::remove(dir.path / leaf(game, a));

    game.destroy(a);
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().removed.empty());
}

TEST_CASE("G13 a studio delete of a file moved outside stops the save", "[G13][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const BoxAndLeaf ids(game, "Loose");
    project.save();
    const std::string path = leaf(game, ids.leaf);
    const fs::path moved = ids.moved(dir.path, game);
    fs::rename(dir.path / path, moved);

    game.destroy(ids.leaf);
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].path == path);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::MovedOutside);
    REQUIRE(fs::exists(moved));
}

TEST_CASE("G14 a new child under a folder deleted outside stops the save", "[G14][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const BoxAndLeaf ids(game, "Loose");
    project.save();
    const std::string box_dir = "src/Box." + game.guid(ids.box);
    fs::remove_all(dir.path / box_dir);

    add_part(game, ids.box, "New");
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    bool box_listed = false;
    for (const engine_core::SaveConflict& conflict : conflicts) {
        box_listed = box_listed || (conflict.guid == game.guid(ids.box) && conflict.path == box_dir + "/init.json" &&
                                    conflict.kind == engine_core::SaveConflict::Kind::DeletedOutside);
    }
    REQUIRE(box_listed);
    REQUIRE_FALSE(fs::exists(dir.path / box_dir));
}

TEST_CASE("G15 junk that mentions a GUID does not make a deleted file look moved", "[G15][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    const std::string guid = game.guid(a.id());
    const std::string bytes = read_file(dir.path / path);
    write_file(dir.path / "src" / ".DS_Store", "junk");
    write_file(dir.path / "src" / ".backup" / ("A." + guid + ".json"), bytes);
    write_file(dir.path / "src" / ("A." + guid + " copy.json"), bytes);
    fs::remove(dir.path / path);

    a.set_color(rgb(1.f, 0.f, 0.f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::DeletedOutside);
}

TEST_CASE("G16 Overwrite writes the studio's version over an outside edit", "[G16][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    edit_key(dir.path / path, "Size", triple(2, 2, 2));

    a.set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    project.save(conflicts);
    REQUIRE(project.last_save().written == std::vector<std::string>{path});
    REQUIRE(read_file(dir.path / path).find("\"Color\": [0.25, 0.5, 0.75]") != std::string::npos);
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(project.last_save().written.empty());
}

TEST_CASE("G17 Overwrite of a file moved outside leaves one file for its GUID", "[G17][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const BoxAndLeaf ids(game, "Loose");
    project.save();
    const std::string guid = game.guid(ids.leaf);
    const fs::path moved = ids.moved(dir.path, game);
    fs::rename(dir.path / leaf(game, ids.leaf), moved);

    game.game_object(ids.leaf)->set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    project.save(conflicts);
    REQUIRE(leaf_files_of(dir.path, guid) == std::vector<std::string>{leaf(game, ids.leaf)});
    REQUIRE_FALSE(fs::exists(moved));
    Project loaded = Project::load(dir.path);
    DataModel& again = loaded.datamodel();
    const InstanceId loose = by_guid(again, guid);
    REQUIRE(again.parent(loose) == 0);
    REQUIRE(again.game_object(loose)->color().b == 0.75f);
}

TEST_CASE("G18 Overwrite puts back a file deleted outside", "[G18][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    const std::string guid = game.guid(a.id());
    fs::remove(dir.path / path);

    a.set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    project.save(conflicts);
    REQUIRE(fs::exists(dir.path / path));
    Project loaded = Project::load(dir.path);
    REQUIRE(loaded.datamodel().game_object(by_guid(loaded.datamodel(), guid))->color().b == 0.75f);
}

TEST_CASE("G19 Overwrite after a folder deleted outside leaves a project that loads", "[G19][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    add_part(game, box, "Other");
    project.save();
    const std::string box_dir = "src/Box." + game.guid(box);
    fs::remove_all(dir.path / box_dir);

    game.game_object(keep)->set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    bool box_listed = false;
    for (const engine_core::SaveConflict& conflict : conflicts) {
        box_listed = box_listed || conflict.path == box_dir + "/init.json";
    }
    REQUIRE(box_listed);
    project.save(conflicts);
    // Load throws when a folder has no init file.
    Project loaded = Project::load(dir.path);
    DataModel& again = loaded.datamodel();
    const InstanceId kept = by_guid(again, game.guid(keep));
    REQUIRE(again.guid(again.parent(kept)) == game.guid(box));
    REQUIRE(again.game_object(kept)->color().b == 0.75f);
}

TEST_CASE("G20 Overwrite of a new child two folders below one deleted outside leaves a project that loads",
          "[G20][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    const InstanceId b = add_part(game, a, "B").id();
    add_part(game, b, "Keep");
    project.save();
    fs::remove_all(dir.path / ("src/A." + game.guid(a)));

    const InstanceId added = add_part(game, b, "New").id();
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE_FALSE(conflicts.empty());
    project.save(conflicts);
    Project loaded = Project::load(dir.path);
    by_guid(loaded.datamodel(), game.guid(added));
}

TEST_CASE("G21 Overwrite of a folder moved outside brings its children back with it", "[G21][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId q = add_part(game, 0, "Q").id();
    add_part(game, q, "QKeep");
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    project.save();
    fs::rename(dir.path / ("src/Box." + game.guid(box)),
               dir.path / ("src/Q." + game.guid(q)) / ("Box." + game.guid(box)));

    game.game_object(box)->set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE_FALSE(conflicts.empty());
    project.save(conflicts);
    Project loaded = Project::load(dir.path);
    DataModel& again = loaded.datamodel();
    REQUIRE(again.parent(by_guid(again, game.guid(box))) == 0);
    REQUIRE(again.guid(again.parent(by_guid(again, game.guid(keep)))) == game.guid(box));
    REQUIRE(leaf_files_of(dir.path, game.guid(keep)).size() == 1);
}

TEST_CASE("G22 Overwrite keeps the file it wrote when the name on disk differs only in case",
          "[G22][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& door = add_part(game, 0, "Door");
    project.save();
    const std::string guid = game.guid(door.id());
    std::string bytes = read_file(dir.path / leaf(game, door.id()));
    bytes.replace(bytes.find("\"Name\": \"Door\""), 14, "\"Name\": \"door\"");
    const fs::path renamed = dir.path / ("src/door." + guid + ".json");
    fs::rename(dir.path / leaf(game, door.id()), renamed);
    write_file(renamed, bytes);

    door.set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].key == "Name");
    project.save(conflicts);
    REQUIRE(leaf_files_of(dir.path, guid).size() == 1);
    Project loaded = Project::load(dir.path);
    REQUIRE(loaded.datamodel().game_object(by_guid(loaded.datamodel(), guid))->color().b == 0.75f);
}

TEST_CASE("G23 Overwrite writes over only the conflicts it lists", "[G23][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    const std::string a_path = leaf(game, a.id());
    const std::string b_path = leaf(game, b.id());
    edit_key(dir.path / a_path, "Size", triple(2, 2, 2));
    const std::string a_outside = read_file(dir.path / a_path);
    a.set_color(rgb(1.f, 0.f, 0.f));
    b.set_color(rgb(0.f, 1.f, 0.f));
    const std::vector<engine_core::SaveConflict> listed = save_conflicts(project);
    REQUIRE(listed.size() == 1);

    // B changes on disk after the list was made.
    edit_key(dir.path / b_path, "Size", triple(2, 2, 2));
    const std::string b_outside = read_file(dir.path / b_path);
    std::vector<engine_core::SaveConflict> again;
    try {
        project.save(listed);
    } catch (const engine_core::ProjectConflict& conflict) {
        again = conflict.conflicts();
    }
    REQUIRE(again.size() == 2);
    REQUIRE(read_file(dir.path / a_path) == a_outside);
    REQUIRE(read_file(dir.path / b_path) == b_outside);
}

TEST_CASE("G24 a file added outside inside a folder the save moves or deletes stops the save",
          "[G24][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    project.save();
    const std::string added = "src/Box." + game.guid(box) + "/Added.added-0001.json";
    write_file(dir.path / added, meta("Folder", "added-0001", "Added"));
    auto added_listed = [&added](const std::vector<engine_core::SaveConflict>& conflicts) {
        for (const engine_core::SaveConflict& conflict : conflicts) {
            if (conflict.path == added && conflict.kind == engine_core::SaveConflict::Kind::AddedOutside) {
                return true;
            }
        }
        return false;
    };

    SECTION("a rename of the folder") {
        game.set_name(box, "Crate");
        const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
        REQUIRE(added_listed(conflicts));
        REQUIRE(fs::exists(dir.path / added));
        project.save(conflicts);
        REQUIRE_FALSE(fs::exists(dir.path / added));
        REQUIRE_NOTHROW(Project::load(dir.path));
    }
    SECTION("a delete of the folder") {
        game.destroy(keep);
        game.destroy(box);
        REQUIRE(added_listed(save_conflicts(project)));
        REQUIRE(fs::exists(dir.path / added));
    }
    SECTION("a delete of its last child, which makes it a leaf") {
        game.destroy(keep);
        REQUIRE(added_listed(save_conflicts(project)));
        REQUIRE(fs::exists(dir.path / added));
    }
}

TEST_CASE("G25 a copy of a folder beside it is not taken for a move", "[G25][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    project.save();
    const std::string box_dir = "src/Box." + game.guid(box);
    const std::string keep_name = "Keep." + game.guid(keep) + ".json";
    const fs::path copy = dir.path / (box_dir + " copy");
    fs::copy(dir.path / box_dir, copy, fs::copy_options::recursive);
    fs::remove(dir.path / box_dir / keep_name);

    game.game_object(keep)->set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].kind == engine_core::SaveConflict::Kind::DeletedOutside);
    project.save(conflicts);
    REQUIRE(fs::exists(copy / keep_name));
}

TEST_CASE("G26 part of an instance deleted outside is written back when the studio left it alone",
          "[G26][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\n");
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "Keep");
    engine_core::GameObject& other = add_part(game, 0, "Other");
    project.save();
    const std::string luau = leaf(game, main.id(), ".luau");
    const std::string init = "src/Box." + game.guid(box) + "/init.json";
    fs::remove(dir.path / luau);
    fs::remove(dir.path / init);

    other.set_color(rgb(0.25f, 0.5f, 0.75f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / luau) == "print(1)\n");
    REQUIRE(fs::exists(dir.path / init));
    REQUIRE_NOTHROW(Project::load(dir.path));
}

TEST_CASE("G27 a file only reformatted outside is no conflict", "[G27][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    std::string wide = read_file(dir.path / path);
    for (std::size_t at = wide.find("\n  "); at != std::string::npos; at = wide.find("\n  ", at + 5)) {
        wide.replace(at, 3, "\n    ");
    }
    write_file(dir.path / path, wide);

    a.set_color(rgb(0.25f, 0.5f, 0.75f));
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / path).find("\"Color\": [0.25, 0.5, 0.75]") != std::string::npos);
}

TEST_CASE("G28 a property file that does not parse is one row for its instance", "[G28][guard][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const std::string path = leaf(game, a.id());
    write_file(dir.path / path, "{\"class\": ");

    a.set_color(rgb(0.25f, 0.5f, 0.75f));
    const std::vector<engine_core::SaveConflict> conflicts = save_conflicts(project);
    REQUIRE(conflicts.size() == 1);
    REQUIRE(conflicts[0].key.empty());
    REQUIRE(conflicts[0].disk == "can't be read");
    project.save(conflicts);
    REQUIRE(read_file(dir.path / path).find("\"Color\": [0.25, 0.5, 0.75]") != std::string::npos);
}

namespace {

// A copy: callers pass the scan_disk() temporary, which dies with the statement.
engine_core::SaveConflict only_row(const engine_core::DiskScan& scan) {
    REQUIRE(scan.conflicts.size() == 1);
    return scan.conflicts[0];
}

std::string box_dir(const DataModel& game, InstanceId box) { return "src/Box." + game.guid(box); }

}  // namespace

TEST_CASE("D1 a scan with nothing changed finds nothing", "[D1][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    add_part(game, 0, "A");
    add_script(game, 0, "Main", "print(1)\n");
    project.save();
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE_FALSE(scan.has_disk_changes);
}

TEST_CASE("D2 a property only the disk changed is a change to load, not a row", "[D2][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "Size", triple(2, 2, 2));
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE(scan.has_disk_changes);
}

TEST_CASE("D3 a property both sides changed differently is a row", "[D3][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    engine_core::GameObject& a = add_part(game, box, "A");
    project.save();
    const std::string path = box_dir(game, box) + "/A." + game.guid(a.id()) + ".json";
    edit_key(dir.path / path, "Color", triple(0, 0, 1));
    a.set_color(rgb(1.f, 0.f, 0.f));
    const engine_core::SaveConflict& row = only_row(project.scan_disk());
    REQUIRE(row.guid == game.guid(a.id()));
    REQUIRE(row.path == path);
    REQUIRE(row.kind == engine_core::SaveConflict::Kind::EditedOutside);
    REQUIRE(row.key == "Color");
    REQUIRE(row.studio == "1, 0, 0");
    REQUIRE(row.disk == "0, 0, 1");
    REQUIRE(row.name == "A");
    REQUIRE(row.where == "game.Box");
}

TEST_CASE("D4 a property both sides changed the same way is nothing", "[D4][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "Color", triple(0, 0, 1));
    a.set_color(rgb(0.f, 0.f, 1.f));
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE_FALSE(scan.has_disk_changes);
}

TEST_CASE("D5 a move on disk is a Parent change", "[D5][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "Keep");
    const InstanceId q = add_part(game, 0, "Q").id();
    add_part(game, q, "QKeep");
    const InstanceId loose = add_part(game, 0, "Loose").id();
    project.save();
    fs::rename(dir.path / leaf(game, loose),
               dir.path / box_dir(game, box) / ("Loose." + game.guid(loose) + ".json"));

    SECTION("the studio left it alone: a change to load") {
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE(scan.has_disk_changes);
    }
    SECTION("the studio moved it too: a row") {
        game.set_parent(loose, q);
        const engine_core::SaveConflict& row = only_row(project.scan_disk());
        REQUIRE(row.key == "Parent");
        REQUIRE(row.studio == "game.Q");
        REQUIRE(row.disk == "game.Box");
    }
}

TEST_CASE("D6 a source both sides changed is a row showing the first differing line", "[D6][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\nprint(2)\n");
    project.save();
    write_file(dir.path / leaf(game, main.id(), ".luau"), "print(1)\nprint(\"disk\")\n");
    main.set_source("print(1)\nprint(\"studio\")\n");
    const engine_core::SaveConflict& row = only_row(project.scan_disk());
    REQUIRE(row.key == "Source");
    REQUIRE(row.studio == "2: print(\"studio\")");
    REQUIRE(row.disk == "2: print(\"disk\")");
}

TEST_CASE("D7 an instance deleted on disk", "[D7][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    add_part(game, box, "Other");
    project.save();
    fs::remove_all(dir.path / box_dir(game, box));

    SECTION("the studio left it alone: a change to load") {
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE(scan.has_disk_changes);
    }
    SECTION("the studio changed something inside it: rows for it and for the child") {
        game.game_object(keep)->set_color(rgb(1.f, 0.f, 0.f));
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.size() == 2);
        std::set<std::string> names;
        for (const engine_core::SaveConflict& row : scan.conflicts) {
            REQUIRE(row.kind == engine_core::SaveConflict::Kind::DeletedOutside);
            REQUIRE(row.key.empty());
            REQUIRE(row.studio == "changed in the studio");
            REQUIRE(row.disk == "deleted");
            names.insert(row.name);
        }
        REQUIRE(names == std::set<std::string>{"Box", "Keep"});
    }
}

TEST_CASE("D8 an instance deleted in the studio", "[D8][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    const std::string path = leaf(game, a);

    SECTION("the disk left it alone: nothing, a save removes it") {
        game.destroy(a);
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE_FALSE(scan.has_disk_changes);
    }
    SECTION("the disk changed it: a row") {
        edit_key(dir.path / path, "Size", triple(2, 2, 2));
        game.destroy(a);
        const engine_core::SaveConflict& row = only_row(project.scan_disk());
        REQUIRE(row.kind == engine_core::SaveConflict::Kind::EditedOutside);
        REQUIRE(row.key.empty());
        REQUIRE(row.studio == "deleted");
        REQUIRE(row.disk == "changed on disk");
        REQUIRE(row.name == "A");
    }
}

TEST_CASE("D9 a class changed on disk", "[D9][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "class", engine_core::JsonValue::string("Folder"));

    SECTION("the studio left it alone: a change to load") {
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE(scan.has_disk_changes);
    }
    SECTION("the studio changed it: a row") {
        a.set_color(rgb(1.f, 0.f, 0.f));
        const engine_core::SaveConflict& row = only_row(project.scan_disk());
        REQUIRE(row.key == "class");
        REQUIRE(row.studio == "GameObject");
        REQUIRE(row.disk == "Folder");
    }
}

TEST_CASE("D10 an instance added on disk", "[D10][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    project.save();
    write_file(dir.path / box_dir(game, box) / "Added.added-0001.json", meta("Folder", "added-0001", "Added"));

    SECTION("under an instance the studio has: a change to load") {
        const engine_core::DiskScan scan = project.scan_disk();
        REQUIRE(scan.conflicts.empty());
        REQUIRE(scan.has_disk_changes);
    }
    SECTION("under one the studio deleted: a row for that one") {
        const std::string box_guid = game.guid(box);
        game.destroy(keep);
        game.destroy(box);
        const engine_core::SaveConflict& row = only_row(project.scan_disk());
        REQUIRE(row.guid == box_guid);
        REQUIRE(row.key.empty());
        REQUIRE(row.disk == "changed on disk");
    }
}

TEST_CASE("D11 a value a class rejects makes the scan throw", "[D11][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "Color", engine_core::JsonValue::string("red"));
    REQUIRE_THROWS_AS(project.scan_disk(), ProjectError);
}

TEST_CASE("D12 a file only reformatted is no change", "[D12][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    a.set_color(rgb(1.f, 0.f, 0.f));
    project.save();
    const std::string path = leaf(game, a.id());
    std::string wide = read_file(dir.path / path);
    for (std::size_t at = wide.find("\n  "); at != std::string::npos; at = wide.find("\n  ", at + 5)) {
        wide.replace(at, 3, "\n    ");
    }
    write_file(dir.path / path, wide);
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE_FALSE(scan.has_disk_changes);
}

TEST_CASE("D13 a key removed on disk is a change to load", "[D13][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    a.set_color(rgb(1.f, 0.f, 0.f));
    project.save();
    const fs::path file = dir.path / leaf(game, a.id());
    engine_core::JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(read_file(file), doc, error));
    REQUIRE(doc.erase("Color"));
    write_file(file, engine_core::write_json(doc));
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE(scan.has_disk_changes);
}

namespace {

bool has_key(const DataModel& object, const char* key) {
    engine_core::PropertyBag bag;
    object.save_properties(bag);
    return engine_core::bag_find(bag, key) != nullptr;
}

}  // namespace

TEST_CASE("A1 disk changes load as one undo step; undone, a save writes the studio's values", "[A1][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    edit_key(dir.path / leaf(game, a), "Size", triple(2, 2, 2));
    // A studio edit whose gesture is still open.
    b.set_color(rgb(1.f, 0.f, 0.f));

    const engine_core::DiskScan result = project.apply_disk();
    REQUIRE(result.loaded == std::vector<std::string>{"A"});
    REQUIRE(result.conflicts.empty());
    REQUIRE(has_key(*game.instance(a), "Size"));
    REQUIRE_FALSE(project.scan_disk().has_disk_changes);

    game.history().undo();
    REQUIRE_FALSE(has_key(*game.instance(a), "Size"));
    REQUIRE(b.color().r == 1.f);
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / leaf(game, a)).find("Size") == std::string::npos);
}

TEST_CASE("A2 each side of a row", "[A2][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    engine_core::GameObject& b = add_part(game, 0, "B");
    project.save();
    edit_key(dir.path / leaf(game, a.id()), "Color", triple(0, 0, 1));
    edit_key(dir.path / leaf(game, b.id()), "Color", triple(0, 0, 1));
    a.set_color(rgb(1.f, 0.f, 0.f));
    b.set_color(rgb(1.f, 0.f, 0.f));
    const std::vector<engine_core::SaveConflict> rows = project.scan_disk().conflicts;
    REQUIRE(rows.size() == 2);
    std::vector<engine_core::DiskChoice> choices;
    for (const engine_core::SaveConflict& row : rows) {
        choices.push_back({row, row.name == "A"});
    }

    const engine_core::DiskScan result = project.apply_disk(choices);
    REQUIRE(result.conflicts.empty());
    REQUIRE(result.skipped.empty());
    REQUIRE(a.color().b == 1.f);
    REQUIRE(b.color().r == 1.f);
    // B's studio value is settled: a save writes it over the disk's.
    REQUIRE(save_conflicts(project).empty());
    REQUIRE(read_file(dir.path / leaf(game, b.id())).find("\"Color\": [1, 0, 0]") != std::string::npos);
    REQUIRE(read_file(dir.path / leaf(game, a.id())).find("\"Color\": [0, 0, 1]") != std::string::npos);
}

TEST_CASE("A3 a row that changed after it was listed is skipped and listed again", "[A3][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    project.save();
    const fs::path file = dir.path / leaf(game, a.id());
    edit_key(file, "Color", triple(0, 0, 1));
    a.set_color(rgb(1.f, 0.f, 0.f));
    const std::vector<engine_core::SaveConflict> listed = project.scan_disk().conflicts;
    REQUIRE(listed.size() == 1);
    edit_key(file, "Color", triple(0, 1, 0));

    const engine_core::DiskScan result = project.apply_disk({{listed[0], true}});
    REQUIRE(result.skipped == listed);
    REQUIRE(result.conflicts.size() == 1);
    REQUIRE(result.conflicts[0].disk == "0, 1, 0");
    REQUIRE(a.color().r == 1.f);
}

TEST_CASE("A4 a loaded property keeps the instance's id", "[A4][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    const std::string guid = game.guid(a);
    project.save();
    edit_key(dir.path / leaf(game, a), "Size", triple(2, 2, 2));
    project.apply_disk();
    REQUIRE(game.find_guid(guid) == a);
}

TEST_CASE("A5 a rename and a move on disk load together, and the next save is quiet", "[A5][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "Keep");
    const InstanceId loose = add_part(game, 0, "Loose").id();
    project.save();
    const fs::path from = dir.path / leaf(game, loose);
    const fs::path to = dir.path / box_dir(game, box) / ("Tight." + game.guid(loose) + ".json");
    fs::rename(from, to);
    edit_key(to, "Name", engine_core::JsonValue::string("Tight"));

    const engine_core::DiskScan result = project.apply_disk();
    REQUIRE(result.conflicts.empty());
    REQUIRE(game.name(loose) == "Tight");
    REQUIRE(game.parent(loose) == box);
    project.save();
    // Only the old parent's file may be rewritten: when it listed its children
    // in order, the move left Loose in that list on disk.
    const std::vector<std::string> quiet;
    const std::vector<std::string> stale_list{"src/init.json"};
    REQUIRE((project.last_save().written == quiet || project.last_save().written == stale_list));
    REQUIRE(project.last_save().moved.empty());
    REQUIRE(project.last_save().removed.empty());
    REQUIRE_NOTHROW(Project::load(dir.path));
}

TEST_CASE("A6 an instance added on disk is made with its GUID, and the next save is quiet", "[A6][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "Keep");
    project.save();
    write_file(dir.path / box_dir(game, box) / "Added.added-0001.json", meta("Folder", "added-0001", "Added"));

    const engine_core::DiskScan result = project.apply_disk();
    REQUIRE(result.loaded == std::vector<std::string>{"Added"});
    const std::optional<InstanceId> added = game.find_guid("added-0001");
    REQUIRE(added.has_value());
    REQUIRE(game.parent(*added) == box);
    REQUIRE(std::string(game.instance(*added)->class_name()) == "Folder");
    project.save();
    REQUIRE(project.last_save().written.empty());
}

TEST_CASE("A7 an instance deleted on disk is destroyed, and one undo brings it back", "[A7][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    const std::string guid = game.guid(a);
    project.save();
    fs::remove(dir.path / leaf(game, a));

    project.apply_disk();
    REQUIRE_FALSE(game.find_guid(guid).has_value());
    game.history().undo();
    REQUIRE(game.find_guid(guid).has_value());
}

TEST_CASE("A8 rows for an instance deleted on disk", "[A8][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    add_part(game, box, "Other");
    project.save();
    const std::string box_guid = game.guid(box);
    const std::string keep_guid = game.guid(keep);
    fs::remove_all(dir.path / box_dir(game, box));
    game.game_object(keep)->set_color(rgb(1.f, 0.f, 0.f));
    const std::vector<engine_core::SaveConflict> rows = project.scan_disk().conflicts;
    REQUIRE(rows.size() == 2);

    SECTION("the studio's side keeps them, and a save writes them back") {
        std::vector<engine_core::DiskChoice> choices;
        for (const engine_core::SaveConflict& row : rows) {
            choices.push_back({row, false});
        }
        REQUIRE(project.apply_disk(choices).conflicts.empty());
        REQUIRE(save_conflicts(project).empty());
        Project loaded = Project::load(dir.path);
        REQUIRE(loaded.datamodel().find_guid(keep_guid).has_value());
    }
    SECTION("the disk's side on the parent removes the child with it") {
        const auto parent_row = std::find_if(rows.begin(), rows.end(),
                                             [&](const engine_core::SaveConflict& row) { return row.guid == box_guid; });
        REQUIRE(parent_row != rows.end());
        REQUIRE(project.apply_disk({{*parent_row, true}}).conflicts.empty());
        REQUIRE_FALSE(game.find_guid(box_guid).has_value());
        REQUIRE_FALSE(game.find_guid(keep_guid).has_value());
    }
}

TEST_CASE("A9 a row for an instance deleted in the studio and changed on disk", "[A9][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    const InstanceId keep = add_part(game, box, "Keep").id();
    project.save();
    const std::string box_guid = game.guid(box);
    const std::string keep_guid = game.guid(keep);
    const std::string folder = box_dir(game, box);
    write_file(dir.path / folder / "Added.added-0001.json", meta("Folder", "added-0001", "Added"));
    game.destroy(keep);
    game.destroy(box);
    const std::vector<engine_core::SaveConflict> rows = project.scan_disk().conflicts;
    REQUIRE(rows.size() == 1);

    SECTION("the disk's side brings it back, with everything under it") {
        REQUIRE(project.apply_disk({{rows[0], true}}).conflicts.empty());
        const std::optional<InstanceId> back = game.find_guid(box_guid);
        REQUIRE(back.has_value());
        REQUIRE(game.parent(*game.find_guid(keep_guid)) == *back);
        REQUIRE(game.parent(*game.find_guid("added-0001")) == *back);
    }
    SECTION("the studio's side lets the delete stand, and a save removes it all") {
        REQUIRE(project.apply_disk({{rows[0], false}}).conflicts.empty());
        REQUIRE(save_conflicts(project).empty());
        REQUIRE_FALSE(fs::exists(dir.path / folder));
    }
}

TEST_CASE("A10 a class changed on disk makes the instance again, keeping its children", "[A10][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    const InstanceId child = add_part(game, a, "Child").id();
    const std::string guid = game.guid(a);
    project.save();
    edit_key(dir.path / ("src/A." + guid + "/init.json"), "class", engine_core::JsonValue::string("Folder"));

    REQUIRE(project.apply_disk().conflicts.empty());
    const std::optional<InstanceId> made = game.find_guid(guid);
    REQUIRE(made.has_value());
    REQUIRE(std::string(game.instance(*made)->class_name()) == "Folder");
    REQUIRE(game.parent(child) == *made);
}

TEST_CASE("A11 a key removed on disk puts the class default back", "[A11][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::GameObject& a = add_part(game, 0, "A");
    a.set_color(rgb(1.f, 0.f, 0.f));
    project.save();
    const fs::path file = dir.path / leaf(game, a.id());
    engine_core::JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(read_file(file), doc, error));
    REQUIRE(doc.erase("Color"));
    write_file(file, engine_core::write_json(doc));

    project.apply_disk();
    REQUIRE_FALSE(has_key(a, "Color"));
}

TEST_CASE("A12 a source only the disk changed loads", "[A12][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    engine_core::Script& main = add_script(game, 0, "Main", "print(1)\n");
    project.save();
    write_file(dir.path / leaf(game, main.id(), ".luau"), "print(\"disk\")\n");
    project.apply_disk();
    REQUIRE(main.source() == "print(\"disk\")\n");
}

TEST_CASE("A13 a key the engine does not know loads as an extra, and the next save is quiet", "[A13][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId a = add_part(game, 0, "A").id();
    project.save();
    engine_core::JsonValue custom = engine_core::JsonValue::object();
    custom.set("x", engine_core::JsonValue::number(1));
    edit_key(dir.path / leaf(game, a), "Custom", custom);

    project.apply_disk();
    REQUIRE(engine_core::bag_find(game.extra_properties(a), "Custom") != nullptr);
    project.save();
    REQUIRE(project.last_save().written.empty());
}

TEST_CASE("A14 an apply during a test throws", "[A14][disk][project]") {
    TempDir dir;
    ScriptRig rig;
    {
        Project project = Project::create(dir.path, rig.game);
        add_part(rig.game, 0, "Door");
        project.save();
    }
    Project project = Project::load(dir.path, rig.game);
    rig.game.start_simulation();
    REQUIRE_THROWS_AS(project.apply_disk(), ProjectError);
    rig.game.stop_simulation();
}

TEST_CASE("A15 a sibling order only the disk changed loads", "[A15][disk][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId box = add_part(game, 0, "Box").id();
    add_part(game, box, "One");
    add_part(game, box, "Two");
    project.save();
    std::vector<std::string> order = child_guids(game, box);
    std::reverse(order.begin(), order.end());
    std::vector<engine_core::JsonValue> items;
    for (const std::string& guid : order) {
        items.push_back(engine_core::JsonValue::string(guid));
    }
    edit_key(dir.path / box_dir(game, box) / "init.json", "children", engine_core::JsonValue::array(std::move(items)));

    project.apply_disk();
    REQUIRE(child_guids(game, box) == order);
}
