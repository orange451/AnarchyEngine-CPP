// SaveService: named stores of plain values, one JSON file each under the
// per-user folder, written within a second of a change and at Stop.

#include "support.hpp"

#include "FileBytes.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SaveStore.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "UserData.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using engine_core::JsonValue;
using engine_core::SaveStores;
using std::chrono::milliseconds;

std::vector<std::string> output(ScriptRig& rig) {
    std::vector<std::string> out;
    for (const auto& line : rig.runtime.drain_output().lines) {
        UNSCOPED_INFO(line.text);
        out.push_back(line.text);
    }
    return out;
}

bool has_line(const std::vector<std::string>& lines, const std::string& text) {
    return std::find(lines.begin(), lines.end(), text) != lines.end();
}

// A line that starts with prefix and holds part.
bool line_has(const std::vector<std::string>& lines, const std::string& prefix, const std::string& part) {
    for (const std::string& line : lines) {
        if (line.rfind(prefix, 0) == 0 && line.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string file_text(const fs::path& path) {
    std::string bytes;
    std::string error;
    REQUIRE(engine_core::read_file(path, bytes, error));
    return bytes;
}

// Runs source as a script in a play session of rig's place, then stops.
std::vector<std::string> play(ScriptRig& rig, const char* source) {
    engine_core::Script& script = add_script(rig.game, "Saves", source);
    rig.game.start_simulation();
    rig.frames(3);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    std::vector<std::string> out = output(rig);
    rig.game.stop_simulation();
    rig.game.destroy(script.id());
    return out;
}

// Sets an environment variable for one test, and puts it back after.
struct ScopedEnv {
    const char* name;
    std::optional<std::string> old;

    ScopedEnv(const char* variable, const std::string& value) : name(variable) {
        if (const char* was = std::getenv(name)) {
            old = was;
        }
        set(value.c_str());
    }

    ~ScopedEnv() {
        if (old) {
            set(old->c_str());
        } else {
#ifdef _WIN32
            _putenv_s(name, "");
#else
            unsetenv(name);
#endif
        }
    }

    void set(const char* value) const {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
};

}  // namespace

TEST_CASE("SV1 a store name is 1 to 50 letters, digits, _ and -", "[saves]") {
    REQUIRE(!engine_core::store_name_error("Progress"));
    REQUIRE(!engine_core::store_name_error("slot_1-b"));
    REQUIRE(!engine_core::store_name_error(std::string(50, 'a')));
    REQUIRE(engine_core::store_name_error(""));
    REQUIRE(engine_core::store_name_error(std::string(51, 'a')));
    REQUIRE(engine_core::store_name_error("has space"));
    REQUIRE(engine_core::store_name_error("dot.json"));
    REQUIRE(engine_core::store_name_error("../up"));
    REQUIRE(engine_core::store_name_error("slash/x"));

    SaveStores stores;
    std::string error;
    REQUIRE(stores.open("bad name", error) == nullptr);
    REQUIRE(!error.empty());
    error.clear();
    engine_core::SaveStore* first = stores.open("Good", error);
    REQUIRE(first != nullptr);
    REQUIRE(error.empty());
    REQUIRE(stores.open("Good", error) == first);

    ScriptRig rig;
    const std::vector<std::string> out = play(rig, R"(
        local SaveService = game:GetService("SaveService")
        print("bad", pcall(function() SaveService:Open("no spaces") end))
        print("empty", pcall(function() SaveService:Open("") end))
        print("good", typeof(SaveService:Open("Good")))
    )");
    REQUIRE(line_has(out, "bad\tfalse\t", "letters"));
    REQUIRE(line_has(out, "empty\tfalse\t", "letters"));
    REQUIRE(has_line(out, "good\tSaveStore\n"));
}

TEST_CASE("SV2 Set, Get, Remove, Keys, Clear, and Get's default", "[saves]") {
    ScriptRig rig;
    const std::vector<std::string> out = play(rig, R"(
        local SaveService = game:GetService("SaveService")
        local store = SaveService:Open("Main")
        print("missing", store:Get("coins"), store:Get("coins", 7))
        store:Set("coins", 5)
        store:Set("name", "Ada")
        store:Set("alive", false)
        print("got", store:Get("coins", 7), store:Get("name"), store:Get("alive", true))
        print("keys", table.concat(store:Keys(), ","))
        store:Remove("name")
        store:Remove("name")
        store:Set("alive", nil)
        print("after", store:Get("name"), store:Get("alive"), table.concat(store:Keys(), ","))
        print("same", SaveService:Open("Main"):Get("coins"))
        local t = {1, 2}
        store:Set("list", t)
        t[1] = 99
        local copy = store:Get("list")
        copy[2] = 99
        print("copy", store:Get("list")[1], store:Get("list")[2])
        store:Clear()
        print("cleared", #store:Keys(), store:Get("coins"))
        print("emptykey", pcall(function() store:Set("", 1) end))
        print("longkey", pcall(function() store:Get(string.rep("k", 101)) end))
        print("okkey", pcall(function() store:Set(string.rep("k", 100), 1) end))
    )");
    REQUIRE(has_line(out, "missing\tnil\t7\n"));
    REQUIRE(has_line(out, "got\t5\tAda\tfalse\n"));
    REQUIRE(has_line(out, "keys\talive,coins,name\n"));
    REQUIRE(has_line(out, "after\tnil\tnil\tcoins\n"));
    REQUIRE(has_line(out, "same\t5\n"));
    REQUIRE(has_line(out, "copy\t1\t2\n"));
    REQUIRE(has_line(out, "cleared\t0\tnil\n"));
    REQUIRE(line_has(out, "emptykey\tfalse\t", "key"));
    REQUIRE(line_has(out, "longkey\tfalse\t", "key"));
    REQUIRE(has_line(out, "okkey\ttrue\n"));
}

TEST_CASE("SV3 values round-trip through a file and a fresh SaveStores", "[saves]") {
    TempDir dir;
    {
        ScriptRig rig;
        rig.game.saves().set_directory(dir.path);
        play(rig, R"(
            local store = game:GetService("SaveService"):Open("World")
            store:Set("nested", {name = "Ada", stats = {hp = 10, flags = {true, false}}, empty = {}})
            store:Set("list", {1, 2.5, "three"})
            store:Set("pos", Vector3.new(1, 2.5, -3))
            store:Set("flat", Vector2.new(4, 5))
            store:Set("tint", Color3.new(0.25, 0.5, 1))
            store:Set("frame", Matrix4.new(1, 2, 3))
        )");
        // Stop wrote it.
    }
    const fs::path file = dir.path / "world.json";
    REQUIRE(fs::exists(file));
    JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(file_text(file), doc, error));
    const JsonValue* pos = doc.find("pos");
    REQUIRE(pos != nullptr);
    REQUIRE(pos->find("$type") != nullptr);
    REQUIRE(pos->find("$type")->as_string() == "Vector3");
    REQUIRE(pos->find("value")->items().size() == 3);
    REQUIRE(pos->find("value")->items()[1].as_number() == 2.5);
    REQUIRE(doc.find("frame")->find("value")->items().size() == 16);
    REQUIRE(doc.find("flat")->find("$type")->as_string() == "Vector2");
    REQUIRE(doc.find("tint")->find("$type")->as_string() == "Color3");
    REQUIRE(doc.find("nested")->find("empty")->is_array());

    ScriptRig rig;
    rig.game.saves().set_directory(dir.path);
    const std::vector<std::string> out = play(rig, R"(
        local store = game:GetService("SaveService"):Open("World")
        local nested = store:Get("nested")
        print("nested", nested.name, nested.stats.hp, nested.stats.flags[1], nested.stats.flags[2], #nested.empty)
        local list = store:Get("list")
        print("list", #list, list[1], list[2], list[3])
        print("pos", typeof(store:Get("pos")), store:Get("pos") == Vector3.new(1, 2.5, -3))
        print("flat", typeof(store:Get("flat")), store:Get("flat") == Vector2.new(4, 5))
        print("tint", typeof(store:Get("tint")), store:Get("tint") == Color3.new(0.25, 0.5, 1))
        print("frame", typeof(store:Get("frame")), store:Get("frame").Position == Vector3.new(1, 2, 3))
    )");
    REQUIRE(has_line(out, "nested\tAda\t10\ttrue\tfalse\t0\n"));
    REQUIRE(has_line(out, "list\t3\t1\t2.5\tthree\n"));
    REQUIRE(has_line(out, "pos\tVector3\ttrue\n"));
    REQUIRE(has_line(out, "flat\tVector2\ttrue\n"));
    REQUIRE(has_line(out, "tint\tColor3\ttrue\n"));
    REQUIRE(has_line(out, "frame\tMatrix4\ttrue\n"));
}

TEST_CASE("SV4 values that cannot be saved raise, naming what they are", "[saves]") {
    ScriptRig rig;
    const std::vector<std::string> out = play(rig, R"(
        local store = game:GetService("SaveService"):Open("Bad")
        print("function", pcall(function() store:Set("k", print) end))
        print("instance", pcall(function() store:Set("k", workspace) end))
        print("thread", pcall(function() store:Set("k", coroutine.create(print)) end))
        local cyclic = {}
        cyclic.self = cyclic
        print("cyclic", pcall(function() store:Set("k", cyclic) end))
        print("mixed", pcall(function() store:Set("k", {1, 2, x = 3}) end))
        print("holes", pcall(function() store:Set("k", {[1] = 1, [3] = 3}) end))
        print("boolkey", pcall(function() store:Set("k", {[true] = 1}) end))
        print("nan", pcall(function() store:Set("k", 0 / 0) end))
        print("inf", pcall(function() store:Set("k", math.huge) end))
        print("deepnan", pcall(function() store:Set("k", {a = {Vector3.new(0 / 0, 0, 0)}}) end))
        print("type", pcall(function() store:Set("k", {["$type"] = "Vector3", value = {1, 2, 3}}) end))
        local deep = {}
        local at = deep
        for i = 1, 40 do at.next = {} at = at.next end
        print("deep", pcall(function() store:Set("k", deep) end))
        print("nested", pcall(function() store:Set("k", {inner = {print}}) end))
        print("kept", #store:Keys())
    )");
    REQUIRE(line_has(out, "function\tfalse\t", "function"));
    REQUIRE(line_has(out, "instance\tfalse\t", "Instance"));
    REQUIRE(line_has(out, "thread\tfalse\t", "thread"));
    REQUIRE(line_has(out, "cyclic\tfalse\t", "a saved table cannot contain itself"));
    REQUIRE(line_has(out, "mixed\tfalse\t", "keys"));
    REQUIRE(line_has(out, "holes\tfalse\t", "keys"));
    REQUIRE(line_has(out, "boolkey\tfalse\t", "keys"));
    REQUIRE(line_has(out, "nan\tfalse\t", "finite"));
    REQUIRE(line_has(out, "inf\tfalse\t", "finite"));
    REQUIRE(line_has(out, "deepnan\tfalse\t", "finite"));
    REQUIRE(line_has(out, "type\tfalse\t", "$type"));
    REQUIRE(line_has(out, "deep\tfalse\t", "32"));
    REQUIRE(line_has(out, "nested\tfalse\t", "function"));
    REQUIRE(has_line(out, "kept\t0\n"));
}

TEST_CASE("SV5 a change is written once a second has passed, or at flush, with no temp file left", "[saves]") {
    TempDir dir;
    SaveStores stores;
    stores.set_directory(dir.path);
    std::string error;
    engine_core::SaveStore* store = stores.open("Timed", error);
    REQUIRE(store != nullptr);
    const fs::path file = dir.path / "timed.json";
    const SaveStores::Clock::time_point t0{std::chrono::seconds(100)};

    // Nothing changed: nothing written.
    stores.flush_due(t0);
    stores.flush_due(t0 + std::chrono::seconds(5));
    REQUIRE(!fs::exists(file));

    store->set("a", JsonValue::number(1));
    stores.flush_due(t0 + std::chrono::seconds(10));
    REQUIRE(!fs::exists(file));
    stores.flush_due(t0 + std::chrono::seconds(10) + milliseconds(500));
    REQUIRE(!fs::exists(file));
    store->set("b", JsonValue::number(2));
    stores.flush_due(t0 + std::chrono::seconds(10) + milliseconds(999));
    REQUIRE(!fs::exists(file));
    stores.flush_due(t0 + std::chrono::seconds(11));
    REQUIRE(fs::exists(file));
    REQUIRE(!fs::exists(dir.path / "Timed.json.tmp"));
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        files.push_back(entry.path());
    }
    REQUIRE(files == std::vector<fs::path>{file});
    JsonValue doc;
    REQUIRE(engine_core::parse_json(file_text(file), doc, error));
    REQUIRE(doc.find("a")->as_number() == 1);
    REQUIRE(doc.find("b")->as_number() == 2);

    // Unchanged since: not rewritten.
    fs::remove(file);
    stores.flush_due(t0 + std::chrono::seconds(20));
    stores.flush();
    REQUIRE(!fs::exists(file));

    // flush writes at once.
    store->set("c", JsonValue::string("x"));
    stores.flush();
    REQUIRE(fs::exists(file));
    REQUIRE(!fs::exists(dir.path / "Timed.json.tmp"));
    REQUIRE(stores.take_messages().empty());
}

TEST_CASE("SV6 a corrupt file opens empty, says why, and is kept until a Set", "[saves]") {
    TempDir dir;
    fs::create_directories(dir.path);
    const fs::path file = dir.path / "broken.json";
    const std::string corrupt = "{\"coins\": 5,,, not json";
    std::string error;
    REQUIRE(engine_core::write_file(file, corrupt, error));
    const fs::path wrong = dir.path / "wrong.json";
    REQUIRE(engine_core::write_file(wrong, "[1, 2]", error));

    ScriptRig rig;
    rig.game.saves().set_directory(dir.path);
    engine_core::Script& script = add_script(rig.game, "Saves", R"(
        local store = game:GetService("SaveService"):Open("Broken")
        print("opened", #store:Keys(), store:Get("coins"))
        store:Remove("coins")
        print("wrong", #game:GetService("SaveService"):Open("Wrong"):Keys())
    )");
    rig.game.start_simulation();
    rig.frames(3);
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> out = output(rig);
    REQUIRE(has_line(out, "opened\t0\tnil\n"));
    REQUIRE(has_line(out, "wrong\t0\n"));
    const std::vector<std::string> messages = rig.game.saves().take_messages();
    REQUIRE(messages.size() == 2);
    REQUIRE(messages[0].find("broken.json") != std::string::npos);
    REQUIRE(messages[1].find("wrong.json") != std::string::npos);
    REQUIRE(rig.game.saves().take_messages().empty());

    // Time passes and Stop flushes: the bad file is still there, untouched.
    rig.game.saves().flush_due(SaveStores::Clock::now() + std::chrono::hours(1));
    rig.game.stop_simulation();
    REQUIRE(file_text(file) == corrupt);
    REQUIRE(file_text(wrong) == "[1, 2]");
    rig.game.destroy(script.id());

    // The first Set replaces it.
    play(rig, R"(
        game:GetService("SaveService"):Open("Broken"):Set("coins", 9)
    )");
    JsonValue doc;
    REQUIRE(engine_core::parse_json(file_text(file), doc, error));
    REQUIRE(doc.find("coins")->as_number() == 9);
}

TEST_CASE("SV7 a folder that cannot be written says so, raises nothing, and retries", "[saves]") {
    TempDir dir;
    fs::create_directories(dir.path);
    // A regular file where the folder should be: no platform can write under it.
    const fs::path blocked = dir.path / "blocked";
    std::string error;
    REQUIRE(engine_core::write_file(blocked, "a file", error));

    ScriptRig rig;
    rig.game.saves().set_directory(blocked);
    engine_core::Script& script = add_script(rig.game, "Saves", R"(
        local store = game:GetService("SaveService"):Open("Data")
        store:Set("coins", 3)
        print("value", store:Get("coins"))
    )");
    rig.game.start_simulation();
    rig.frames(3);
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(has_line(output(rig), "value\t3\n"));
    rig.game.saves().flush();
    std::vector<std::string> messages = rig.game.saves().take_messages();
    REQUIRE(messages.size() == 1);
    REQUIRE(messages[0].find("data") != std::string::npos);

    // Still dirty: a later flush tries again.
    REQUIRE_NOTHROW(rig.game.stop_simulation());
    REQUIRE(rig.runtime.last_error().empty());
    rig.game.destroy(script.id());
    fs::remove(blocked);
    rig.game.saves().flush();
    REQUIRE(fs::exists(blocked / "data.json"));
}

TEST_CASE("SV8 with no folder, values last the session only", "[saves]") {
    ScriptRig rig;
    REQUIRE(rig.game.saves().directory().empty());
    play(rig, R"(
        game:GetService("SaveService"):Open("Session"):Set("coins", 4)
    )");
    rig.game.saves().flush();
    REQUIRE(rig.game.saves().take_messages().empty());
    // A second Test in the same session still sees it.
    const std::vector<std::string> out = play(rig, R"(
        print("again", game:GetService("SaveService"):Open("Session"):Get("coins"))
    )");
    REQUIRE(has_line(out, "again\t4\n"));

    // A new session does not.
    ScriptRig other;
    const std::vector<std::string> fresh = play(other, R"(
        print("fresh", game:GetService("SaveService"):Open("Session"):Get("coins"))
    )");
    REQUIRE(has_line(fresh, "fresh\tnil\n"));

    // A handle from before the folder changed raises rather than reach a store
    // that went. (From no folder the stores are kept: SV12.)
    TempDir dir;
    TempDir other_dir;
    rig.game.saves().set_directory(dir.path);
    engine_core::Script& script = add_script(rig.game, "Holds", R"(
        local store = game:GetService("SaveService"):Open("Session")
        task.wait()
        task.wait()
        print("gone", pcall(function() return store:Get("coins") end))
    )");
    rig.game.start_simulation();
    rig.frames(1);
    rig.game.saves().set_directory(other_dir.path);
    rig.frames(3);
    REQUIRE(line_has(output(rig), "gone\tfalse\t", "SaveStore is gone"));
    rig.game.stop_simulation();
    rig.game.destroy(script.id());
}

TEST_CASE("SV9 strict script analysis accepts the SaveService and SaveStore members", "[saves]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Uses", R"(--!strict
local SaveService = game:GetService("SaveService")
local store = SaveService:Open("Progress")
store:Set("coins", 5)
local coins = store:Get("coins", 0)
local other = store:Get("other")
store:Remove("coins")
local keys = store:Keys()
local first: string = keys[1]
store:Clear()
print(coins, other, first, #keys)
)");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!analysis.idle()) {
        analysis.pump();
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    std::string problems;
    for (const engine_core::Diagnostic& diagnostic : analysis.diagnostics(script.id())) {
        if (diagnostic.severity == engine_core::Severity::Error || diagnostic.severity == engine_core::Severity::Warning) {
            problems += diagnostic.code + " @" + std::to_string(diagnostic.range.start.line) + " " + diagnostic.message + "\n";
        }
    }
    INFO(problems);
    REQUIRE(problems.empty());
}

TEST_CASE("SV10 loading a project points its saves at the user folder for its gameId", "[saves][project]") {
    SimRole role;
    TempDir user;
    TempDir dir;
    const ScopedEnv env("ANARCHY_USER_DATA", engine_core::utf8_path(user.path));
    REQUIRE(engine_core::user_data_directory() == user.path);
    std::string game_id;
    {
        engine_core::Game game;
        engine_core::Project project = engine_core::Project::create(dir.path, game);
        game_id = project.game_id();
        REQUIRE(!game_id.empty());
        // A new project has its id, so its saves have their folder.
        REQUIRE(game.saves().directory() == user.path / "saves" / game_id);
    }
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::load(dir.path, game);
    REQUIRE(project.game_id() == game_id);
    REQUIRE(game.saves().directory() == user.path / "saves" / game_id);
    std::string error;
    engine_core::SaveStore* store = game.saves().open("Slot", error);
    REQUIRE(store != nullptr);
    store->set("coins", JsonValue::number(12));
    game.saves().flush();
    REQUIRE(game.saves().take_messages().empty());
    JsonValue doc;
    REQUIRE(engine_core::parse_json(file_text(user.path / "saves" / game_id / "slot.json"), doc, error));
    REQUIRE(doc.find("coins")->as_number() == 12);
}

TEST_CASE("SV11 a new place no longer writes into the last project's saves", "[saves][project]") {
    SimRole role;
    TempDir user;
    TempDir dir;
    const ScopedEnv env("ANARCHY_USER_DATA", engine_core::utf8_path(user.path));
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const fs::path file = user.path / "saves" / project.game_id() / "slot.json";
    std::string error;
    game.saves().open("Slot", error)->set("coins", JsonValue::number(12));
    game.saves().flush();
    const std::string before = file_text(file);

    engine_core::Project::reset_place(game);
    REQUIRE(game.saves().directory().empty());
    game.saves().open("Slot", error)->set("coins", JsonValue::number(99));
    game.saves().flush();
    REQUIRE(file_text(file) == before);
}

TEST_CASE("SV12 stores kept in memory move to the first folder they are given", "[saves]") {
    TempDir dir;
    SaveStores stores;
    std::string error;
    engine_core::SaveStore* store = stores.open("Keep", error);
    REQUIRE(store != nullptr);
    store->set("coins", JsonValue::number(7));
    const std::uint64_t generation = stores.generation();
    stores.set_directory(dir.path / "saves");
    REQUIRE(stores.generation() == generation);
    REQUIRE(stores.open("Keep", error) == store);
    REQUIRE(store->get("coins")->as_number() == 7);
    stores.flush();
    REQUIRE(stores.take_messages().empty());
    JsonValue doc;
    REQUIRE(engine_core::parse_json(file_text(dir.path / "saves" / "keep.json"), doc, error));
    REQUIRE(doc.find("coins")->as_number() == 7);
}

TEST_CASE("SV13 store names ignore case, and Windows device names are refused", "[saves]") {
    for (const char* device : {"con", "PRN", "Aux", "nul", "com1", "COM9", "lpt1", "Lpt9"}) {
        INFO(device);
        REQUIRE(engine_core::store_name_error(device));
    }
    for (const char* plain : {"com0", "com10", "console", "lpt", "nulls"}) {
        INFO(plain);
        REQUIRE(!engine_core::store_name_error(plain));
    }
    TempDir dir;
    SaveStores stores;
    stores.set_directory(dir.path);
    std::string error;
    engine_core::SaveStore* upper = stores.open("Player", error);
    REQUIRE(upper != nullptr);
    REQUIRE(stores.open("player", error) == upper);
    REQUIRE(stores.open("PLAYER", error) == upper);
    upper->set("level", JsonValue::number(3));
    stores.flush();
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        files.push_back(entry.path().filename().string());
    }
    REQUIRE(files == std::vector<std::string>{"player.json"});
}
