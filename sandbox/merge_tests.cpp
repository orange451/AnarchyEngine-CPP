#include "JsonMerge.hpp"
#include "Contract.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "TestTriangle.hpp"
#include "types.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

using engine_core::JsonValue;
using engine_core::KeyChange;

namespace {

JsonValue json(const char* text) {
    JsonValue out;
    std::string error;
    REQUIRE(engine_core::parse_json(text, out, error));
    return out;
}

std::map<std::string, KeyChange> changes(const JsonValue& base, const JsonValue& disk, const JsonValue& studio) {
    std::map<std::string, KeyChange> out;
    for (const engine_core::KeyMerge& merged : engine_core::merge_keys(base, disk, studio)) {
        out[merged.key] = merged.change;
    }
    return out;
}

}  // namespace

TEST_CASE("M1 merge_keys sorts every key into how it changed", "[M1][merge]") {
    const JsonValue base = json(R"({"class": "GameObject", "id": "a", "Name": "A", "Color": [1, 0, 0],
                                   "Size": [2, 2, 2], "Gone": true, "Kept": 1})");
    const JsonValue disk = json(R"({"class": "GameObject", "id": "a", "Name": "A", "Color": [0, 0, 1],
                                   "Size": [2, 2, 2], "Kept": 2, "New": 1})");
    const JsonValue studio = json(R"({"class": "GameObject", "id": "a", "Name": "B", "Color": [0, 1, 0],
                                     "Size": [2, 2, 2], "Gone": true, "Kept": 2, "New": 1})");
    const std::map<std::string, KeyChange> expected = {
        {"class", KeyChange::Unchanged}, {"Name", KeyChange::StudioOnly}, {"Color", KeyChange::Conflict},
        {"Size", KeyChange::Unchanged},  {"Gone", KeyChange::DiskOnly},   {"Kept", KeyChange::Agreed},
        {"New", KeyChange::Agreed},
    };
    REQUIRE(changes(base, disk, studio) == expected);
}

TEST_CASE("M2 merge_keys lists keys in byte order, without id", "[M2][merge]") {
    const JsonValue same = json(R"({"id": "x", "class": "C", "a": 1, "B": 2})");
    std::vector<std::string> keys;
    for (const engine_core::KeyMerge& merged : engine_core::merge_keys(same, same, same)) {
        keys.push_back(merged.key);
    }
    REQUIRE(keys == std::vector<std::string>{"B", "a", "class"});
}

TEST_CASE("M3 same_value compares as the file writes values", "[M3][merge]") {
    const JsonValue one = json(R"({"n": [1.0, 2], "o": {"a": 1, "b": 2}})");
    const JsonValue two = json(R"({"n": [1, 2], "o": {"b": 2, "a": 1}})");
    REQUIRE(engine_core::same_value(one.find("n"), two.find("n")));
    REQUIRE(engine_core::same_value(one.find("o"), two.find("o")));
    REQUIRE(engine_core::same_value(nullptr, nullptr));
    REQUIRE_FALSE(engine_core::same_value(one.find("n"), nullptr));
    REQUIRE_FALSE(engine_core::same_value(one.find("n"), one.find("o")));
}

TEST_CASE("M4 display_value writes a value on one line", "[M4][merge]") {
    REQUIRE(engine_core::display_value(nullptr) == "(default)");
    const JsonValue color = json("[0.25, 0.5, 0.75]");
    REQUIRE(engine_core::display_value(&color) == "0.25, 0.5, 0.75");
    const JsonValue yes = JsonValue::boolean(true);
    REQUIRE(engine_core::display_value(&yes) == "true");
    const JsonValue name = JsonValue::string("Part");
    REQUIRE(engine_core::display_value(&name) == "Part");
    const JsonValue three = JsonValue::number(3);
    REQUIRE(engine_core::display_value(&three) == "3");
    const JsonValue nested = json(R"({"a": 1, "b": [1, "x"]})");
    const std::string text = engine_core::display_value(&nested);
    REQUIRE(text.find('\n') == std::string::npos);
    REQUIRE(text.front() == '{');
    REQUIRE(text.find("\"a\": 1") != std::string::npos);
}

namespace {

struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

std::vector<std::string> default_keys(const engine_core::DataModel& object) {
    engine_core::PropertyBag defaults;
    object.default_properties(defaults);
    std::vector<std::string> out;
    for (const JsonValue::Member& member : defaults) {
        out.push_back(member.first);
    }
    return out;
}

}  // namespace

TEST_CASE("M5 default_properties is what save_properties leaves out", "[M5][merge]") {
    SimRole role;
    engine_core::Game game;
    std::vector<engine_core::DataModel*> objects = {
        &game.create(), &game.create<engine_core::GameObject>(), &game.create<engine_core::Script>(),
        &game.create<engine_core::ModuleScript>(), &game.create<engine_core::Folder>(),
        &game.create<engine_core::TestTriangle>()};
    for (engine_core::DataModel* object : objects) {
        INFO(object->class_name());
        engine_core::PropertyBag saved;
        object->save_properties(saved);
        REQUIRE(saved.empty());
        engine_core::PropertyBag defaults;
        object->default_properties(defaults);
        for (const JsonValue::Member& member : defaults) {
            std::string error;
            REQUIRE(object->load_property(member.first, member.second, error));
            REQUIRE(error.empty());
        }
        object->save_properties(saved);
        REQUIRE(saved.empty());
    }
    REQUIRE(default_keys(game).empty());
    REQUIRE(default_keys(*objects[0]) == std::vector<std::string>{"Simulated", "VisualOnly"});
    REQUIRE(default_keys(*objects[1]) ==
            std::vector<std::string>{"Color", "Simulated", "Size", "Transform", "VisualOnly"});
    REQUIRE(default_keys(*objects[2]) == std::vector<std::string>{"Enabled", "Simulated", "VisualOnly"});
    REQUIRE(default_keys(*objects[5]) == std::vector<std::string>{"Position", "Simulated", "VisualOnly"});
}
