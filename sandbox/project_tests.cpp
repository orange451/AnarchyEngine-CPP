#include "DataModel.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
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
#include <random>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using engine_core::DataModel;
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

engine_core::GameObject& add_part(DataModel& model, InstanceId parent, const char* name) {
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    model.set_name(part.id(), name);
    model.set_parent(part.id(), parent);
    return part;
}

engine_core::Script& add_script(DataModel& model, InstanceId parent, const char* name, const char* source) {
    engine_core::Script& script = model.create<engine_core::Script>();
    model.set_name(script.id(), name);
    script.set_source(source);
    model.set_parent(script.id(), parent);
    return script;
}

engine_core::ColorRgb rgb(float r, float g, float b) {
    engine_core::ColorRgb color;
    color.r = r;
    color.g = g;
    color.b = b;
    return color;
}

std::string leaf(const DataModel& model, InstanceId id, const char* ext = ".json") {
    return "src/" + engine_core::sanitize_file_name(model.name(id)) + "." + model.guid(id) + ext;
}

InstanceId by_guid(const DataModel& model, const std::string& guid) {
    const std::optional<InstanceId> id = model.find_guid(guid);
    REQUIRE(id.has_value());
    return *id;
}

std::vector<std::string> child_guids(const DataModel& model, InstanceId parent) {
    std::vector<std::string> out;
    for (InstanceId child : model.get_children(parent)) {
        out.push_back(model.guid(child));
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
        DataModel& model = project.datamodel();
        engine_core::GameObject& part = add_part(model, 0, "Part");
        part.set_color(rgb(0.25f, 0.5f, 0.1f));
        part.set_transform(engine_core::transform_translation(1.5f, -2.f, 0.1f));
        engine_core::Script& script = add_script(model, 0, "Main", source.c_str());
        part_guid = model.guid(part.id());
        script_guid = model.guid(script.id());
        root_guid = model.guid(0);
        project.save();
        REQUIRE(fs::exists(dir.path / leaf(model, part.id())));
        REQUIRE(fs::exists(dir.path / leaf(model, script.id(), ".luau")));
        REQUIRE(fs::exists(dir.path / leaf(model, script.id(), ".meta.json")));
        REQUIRE(read_file(dir.path / leaf(model, script.id(), ".luau")) == source);
        // Source is only in the .luau file.
        REQUIRE(read_file(dir.path / leaf(model, script.id(), ".meta.json")).find("print") == std::string::npos);
    }
    Project loaded = Project::load(dir.path);
    DataModel& model = loaded.datamodel();
    REQUIRE(model.guid(0) == root_guid);
    const InstanceId part = by_guid(model, part_guid);
    const InstanceId script = by_guid(model, script_guid);
    REQUIRE(model.name(part) == "Part");
    REQUIRE(model.name(script) == "Main");
    REQUIRE(model.parent(part) == 0);
    REQUIRE(std::string(model.instance(script)->class_name()) == "Script");
    REQUIRE(dynamic_cast<engine_core::Script*>(model.instance(script))->source() == source);
    const engine_core::GameObject* body = model.game_object(part);
    REQUIRE(body != nullptr);
    REQUIRE(body->color().r == 0.25f);
    REQUIRE(body->color().g == 0.5f);
    REQUIRE(body->color().b == 0.1f);
    REQUIRE(body->transform().m[12] == 1.5f);
    REQUIRE(body->transform().m[14] == 0.1f);
    REQUIRE(loaded.instance_for(part_guid) == part);
    // Nothing loaded is undoable, and Stop Play returns to this tree.
    REQUIRE_FALSE(model.history().can_undo().first);
}

TEST_CASE("P2 a second save with no edits writes nothing", "[P2][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& model = project.datamodel();
    engine_core::GameObject& folderish = add_part(model, 0, "Holder");
    add_part(model, folderish.id(), "Inner").set_color(rgb(0.f, 1.f, 0.f));
    add_script(model, 0, "Main", "print(1)\n");
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
    DataModel& model = project.datamodel();
    engine_core::GameObject& a = add_part(model, 0, "A");
    add_part(model, 0, "B");
    add_script(model, 0, "Main", "print(1)\n");
    project.save();
    const auto before = tree_files(dir.path);

    a.set_color(rgb(0.2f, 0.3f, 0.4f));
    project.save();
    const std::set<std::string> diff = changed(before, tree_files(dir.path));
    REQUIRE(diff == std::set<std::string>{leaf(model, a.id())});
    REQUIRE(project.last_save().written == std::vector<std::string>{leaf(model, a.id())});
    REQUIRE(read_file(dir.path / leaf(model, a.id())).find("\"Color\": [0.2, 0.3, 0.4]") != std::string::npos);
}

TEST_CASE("P4 a source edit rewrites only the .luau file", "[P4][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& model = project.datamodel();
    add_part(model, 0, "A");
    engine_core::Script& main = add_script(model, 0, "Main", "print(1)\n");
    project.save();
    const auto before = tree_files(dir.path);

    main.set_source("print(2)\n");
    project.save();
    REQUIRE(changed(before, tree_files(dir.path)) == std::set<std::string>{leaf(model, main.id(), ".luau")});
    REQUIRE(read_file(dir.path / leaf(model, main.id(), ".luau")) == "print(2)\n");
}

namespace {

struct ScriptRig {
    SimRole role;
    DataModel model;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;

    ScriptRig() {
        scheduler.reserve(16);
        model.attach_scheduler(&scheduler);
        runtime.attach(model, scheduler);
    }

    void frames(int count, double dt = 1.0 / 60.0) {
        for (int i = 0; i < count; ++i) {
            scheduler.run_phase(engine_core::Phase::Heartbeat, dt);
            model.events().drain();
            runtime.heartbeat(dt);
            model.events().drain();
        }
    }
};

}  // namespace

TEST_CASE("P5 a save during play writes the place, never play-only instances", "[P5][project]") {
    TempDir dir;
    ScriptRig rig;
    {
        Project project = Project::create(dir.path, rig.model);
        add_part(rig.model, 0, "Door");
        add_script(rig.model, 0, "Maker", R"(
            local made = Instance.new("GameObject")
            made.Name = "Session"
            made.Parent = script.Parent
            script.Parent:FindFirstChild("Door").Name = "Moved"
        )");
        project.save();
    }
    Project project = Project::load(dir.path, rig.model);
    const auto saved = tree_files(dir.path);
    const InstanceId door = rig.model.find_first_child(0, "Door");
    REQUIRE(door != 0);

    rig.model.start_simulation();
    rig.frames(1, 0.05);
    const InstanceId session = rig.model.find_first_child(0, "Session");
    REQUIRE(session != 0);
    REQUIRE(rig.model.name(door) == "Moved");

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

    rig.model.stop_simulation();
    REQUIRE_FALSE(rig.model.alive(session));
    REQUIRE(rig.model.name(door) == "Door");
    project.save();
    REQUIRE(project.last_save().written.empty());
    REQUIRE(tree_files(dir.path) == saved);
}

TEST_CASE("P6 destroying an authored part deletes its file", "[P6][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& model = project.datamodel();
    engine_core::GameObject& keep = add_part(model, 0, "Keep");
    engine_core::GameObject& gone = add_part(model, 0, "Gone");
    engine_core::GameObject& child = add_part(model, gone.id(), "Child");
    project.save();
    const std::string gone_dir = "src/Gone." + model.guid(gone.id());
    const std::string gone_init = gone_dir + "/init.json";
    const std::string child_file = gone_dir + "/Child." + model.guid(child.id()) + ".json";
    REQUIRE(fs::exists(dir.path / gone_init));
    REQUIRE(fs::exists(dir.path / child_file));

    model.destroy(child.id());
    model.destroy(gone.id());
    project.save();
    REQUIRE_FALSE(fs::exists(dir.path / gone_init));
    REQUIRE_FALSE(fs::exists(dir.path / child_file));
    // The emptied folder goes too.
    REQUIRE_FALSE(fs::exists(dir.path / gone_dir));
    REQUIRE(fs::exists(dir.path / leaf(model, keep.id())));
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
            DataModel& model = project.datamodel();
            for (int i = 0; i < 6; ++i) {
                add_part(model, 0, "Part");
            }
            order = child_guids(model, 0);
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
    REQUIRE_THROWS_AS(Project::create(root), ProjectError);
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
    DataModel& model = project.datamodel();
    const InstanceId part = by_guid(model, guid);
    const engine_core::JsonValue* value = engine_core::bag_find(model.extra_properties(part), "path");
    REQUIRE(value != nullptr);
    REQUIRE(value->as_string() == "textures/brick.png");

    // Untouched, it is not rewritten. Edited, the key survives the rewrite.
    project.save();
    REQUIRE(read_file(dir.path / path) == edited);
    model.game_object(part)->set_color(rgb(1.f, 0.f, 0.f));
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
        DataModel& model = project.datamodel();
        first = model.guid(add_part(model, 0, "Part").id());
        second = model.guid(add_part(model, 0, "Part").id());
        REQUIRE(first != second);
        project.save();
    }
    REQUIRE(fs::exists(dir.path / "src" / ("Part." + first + ".json")));
    REQUIRE(fs::exists(dir.path / "src" / ("Part." + second + ".json")));
    Project project = Project::load(dir.path);
    DataModel& model = project.datamodel();
    REQUIRE(model.get_children(0).size() == 2);
    REQUIRE(model.name(by_guid(model, first)) == "Part");
    REQUIRE(model.name(by_guid(model, second)) == "Part");
}

TEST_CASE("P12 adding a third Part adds one file and renames none", "[P12][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& model = project.datamodel();
    const InstanceId a = add_part(model, 0, "Part").id();
    const InstanceId b = add_part(model, 0, "Part").id();
    project.save();
    const auto before = tree_files(dir.path);
    const std::string a_file = leaf(model, a);
    const std::string b_file = leaf(model, b);

    const InstanceId c = add_part(model, 0, "Part").id();
    project.save();
    const auto after = tree_files(dir.path);
    REQUIRE(after.at(a_file) == before.at(a_file));
    REQUIRE(after.at(b_file) == before.at(b_file));
    REQUIRE(project.last_save().moved.empty());
    // One new instance file. The root's init.json may change only to record child order.
    std::set<std::string> diff = changed(before, after);
    diff.erase("src/init.json");
    REQUIRE(diff == std::set<std::string>{leaf(model, c)});
    REQUIRE(after.size() == before.size() + 1);
}

TEST_CASE("P13 two Scripts named Main keep their own source", "[P13][project]") {
    SimRole role;
    TempDir dir;
    std::string one;
    std::string two;
    {
        Project project = Project::create(dir.path);
        DataModel& model = project.datamodel();
        one = model.guid(add_script(model, 0, "Main", "return 'one'\n").id());
        two = model.guid(add_script(model, 0, "Main", "return 'two'\n").id());
        project.save();
    }
    Project project = Project::load(dir.path);
    DataModel& model = project.datamodel();
    auto source_of = [&model](const std::string& guid) {
        return dynamic_cast<engine_core::Script*>(model.instance(by_guid(model, guid)))->source();
    };
    REQUIRE(source_of(one) == "return 'one'\n");
    REQUIRE(source_of(two) == "return 'two'\n");
}

TEST_CASE("P14 a rename moves only that file and keeps the GUID", "[P14][project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& model = project.datamodel();
    const InstanceId floor = add_part(model, 0, "Part").id();
    const InstanceId other = add_part(model, 0, "Part").id();
    project.save();
    const std::string guid = model.guid(floor);
    const std::string old_file = leaf(model, floor);
    const std::string other_file = leaf(model, other);
    const std::string other_bytes = read_file(dir.path / other_file);

    model.set_name(floor, "Floor");
    project.save();
    const std::string new_file = "src/Floor." + guid + ".json";
    REQUIRE(model.guid(floor) == guid);
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
        DataModel& model = project.datamodel();
        slash = model.guid(add_part(model, 0, "/").id());
        mixed = model.guid(add_part(model, 0, "a:b*c?").id());
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
        DataModel model;
        const InstanceId keep = add_part(model, 0, "Keep").id();
        REQUIRE_THROWS_AS(Project::load(dir.path, model), ProjectError);
        REQUIRE(model.alive(keep));
        REQUIRE(model.find_first_child(0, "Keep") == keep);
    }
}

TEST_CASE("folders: first child, folder rename, script with children, undo of a delete", "[project]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path);
    DataModel& model = project.datamodel();
    engine_core::Folder& box = model.create<engine_core::Folder>();
    model.set_name(box.id(), "Box");
    model.set_parent(box.id(), 0);
    project.save();
    const std::string box_guid = model.guid(box.id());
    REQUIRE(fs::exists(dir.path / "src" / ("Box." + box_guid + ".json")));

    // The first child turns the leaf into a folder.
    const InstanceId inner = add_part(model, box.id(), "Inner").id();
    project.save();
    const std::string box_dir = "src/Box." + box_guid;
    REQUIRE_FALSE(fs::exists(dir.path / "src" / ("Box." + box_guid + ".json")));
    REQUIRE(fs::exists(dir.path / box_dir / "init.json"));
    REQUIRE(fs::exists(dir.path / box_dir / ("Inner." + model.guid(inner) + ".json")));

    // Renaming the folder moves its children with it.
    const std::string inner_bytes = read_file(dir.path / box_dir / ("Inner." + model.guid(inner) + ".json"));
    model.set_name(box.id(), "Crate");
    project.save();
    const std::string crate_dir = "src/Crate." + box_guid;
    REQUIRE_FALSE(fs::exists(dir.path / box_dir));
    REQUIRE(read_file(dir.path / crate_dir / ("Inner." + model.guid(inner) + ".json")) == inner_bytes);

    // A script with a child is a folder with init.luau and init.meta.json.
    engine_core::Script& main = add_script(model, 0, "Main", "print(1)\n");
    add_part(model, main.id(), "Handle");
    project.save();
    const std::string main_dir = "src/Main." + model.guid(main.id());
    REQUIRE(read_file(dir.path / main_dir / "init.luau") == "print(1)\n");
    REQUIRE(fs::exists(dir.path / main_dir / "init.meta.json"));

    // Undo of a delete brings back the same GUID, so the same file.
    model.history().end_gesture();
    const std::string inner_guid = model.guid(inner);
    model.destroy(inner);
    model.history().end_gesture();
    project.save();
    REQUIRE_FALSE(fs::exists(dir.path / crate_dir / ("Inner." + inner_guid + ".json")));
    model.history().undo();
    REQUIRE(model.guid(inner) == inner_guid);
    project.save();
    REQUIRE(read_file(dir.path / crate_dir / ("Inner." + inner_guid + ".json")) == inner_bytes);

    Project loaded = Project::load(dir.path);
    DataModel& again = loaded.datamodel();
    const InstanceId script = by_guid(again, model.guid(main.id()));
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
    DataModel model;
    const InstanceId part = add_part(model, 0, "Part").id();
    model.history().end_gesture();
    REQUIRE(model.history().can_undo().first);
    const std::string guid = model.guid(part);

    Project project = Project::adopt(dir.path / "Adopted", model);
    REQUIRE(&project.datamodel() == &model);
    REQUIRE(project.name() == "Adopted");
    REQUIRE(model.alive(part));
    REQUIRE(model.history().can_undo().first);
    REQUIRE(fs::exists(dir.path / "Adopted" / "project.json"));
    REQUIRE(fs::exists(dir.path / "Adopted" / "src" / ("Part." + guid + ".json")));
    REQUIRE_THROWS_AS(Project::adopt(dir.path / "Adopted", model), ProjectError);

    Project loaded = Project::load(dir.path / "Adopted");
    REQUIRE(loaded.datamodel().name(by_guid(loaded.datamodel(), guid)) == "Part");
}
