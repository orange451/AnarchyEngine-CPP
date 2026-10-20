// Attributes: named values any instance can carry, set and read from Lua,
// saved as one hidden JSON property, undone, and restored at Stop.

#include "support.hpp"

#include "Attributes.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "InstanceFile.hpp"
#include "Project.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "PropertyBag.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using engine_core::LuaSlot;

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

}  // namespace

TEST_CASE("ATR1 attribute names and kinds are checked", "[attributes]") {
    REQUIRE(!engine_core::attribute_name_error("Health"));
    REQUIRE(!engine_core::attribute_name_error("a_1"));
    REQUIRE(engine_core::attribute_name_error("") == "an attribute name is letters, digits, and underscores, up to 100");
    REQUIRE(engine_core::attribute_name_error("has space"));
    REQUIRE(engine_core::attribute_name_error("dot.ted"));
    REQUIRE(!engine_core::attribute_name_error(std::string(100, 'a')));
    REQUIRE(engine_core::attribute_name_error(std::string(101, 'a')));
    REQUIRE(!engine_core::attribute_value_error(number_slot(3)));
    REQUIRE(!engine_core::attribute_value_error(LuaSlot{}));
    LuaSlot signal;
    signal.kind = LuaSlot::Kind::Signal;
    REQUIRE(engine_core::attribute_value_error(signal) ==
            "an attribute holds a number, boolean, string, Vector2, Vector3, Color3, Matrix4, or Instance");
    LuaSlot curve;
    curve.kind = LuaSlot::Kind::NumberCurve;
    REQUIRE(engine_core::attribute_value_error(curve));
    LuaSlot item;
    item.kind = LuaSlot::Kind::Enum;
    REQUIRE(engine_core::attribute_value_error(item));
}

TEST_CASE("ATR2 attributes round-trip through JSON", "[attributes]") {
    engine_core::AttributeMap map;
    map["Count"] = number_slot(3);
    LuaSlot flag;
    flag.kind = LuaSlot::Kind::Bool;
    flag.flag = true;
    map["Open"] = flag;
    LuaSlot text;
    text.kind = LuaSlot::Kind::String;
    text.text = "x";
    map["Label"] = text;
    LuaSlot flat;
    flat.kind = LuaSlot::Kind::Vec2;
    flat.vec = {1.f, 2.f, 0.f};
    map["Scroll"] = flat;
    LuaSlot where;
    where.kind = LuaSlot::Kind::Vec3;
    where.vec = {1.f, 2.f, 3.f};
    map["Where"] = where;
    LuaSlot tint;
    tint.kind = LuaSlot::Kind::Color;
    tint.color = {1.f, 0.f, 0.f, 1.f};
    map["Tint"] = tint;
    LuaSlot frame;
    frame.kind = LuaSlot::Kind::Matrix4;
    frame.transform = engine_core::matrix4_identity();
    map["Frame"] = frame;
    LuaSlot target;
    target.kind = LuaSlot::Kind::Instance;
    target.text = "0123456789abcdef0123456789abcdef";
    map["Target"] = target;

    const std::string json = engine_core::attributes_to_json(map);
    engine_core::AttributeMap back;
    std::string error;
    REQUIRE(engine_core::attributes_from_json(json, back, error));
    INFO(json);
    REQUIRE(error.empty());
    REQUIRE(back.size() == 8);
    REQUIRE(back["Count"].kind == LuaSlot::Kind::Number);
    REQUIRE(back["Count"].number == 3);
    REQUIRE(back["Open"].flag);
    REQUIRE(back["Label"].text == "x");
    REQUIRE(back["Scroll"].kind == LuaSlot::Kind::Vec2);
    REQUIRE(back["Scroll"].vec.y == 2.f);
    REQUIRE(back["Where"].vec.z == 3.f);
    REQUIRE(back["Tint"].kind == LuaSlot::Kind::Color);
    REQUIRE(back["Tint"].color.r == 1.f);
    REQUIRE(back["Frame"].kind == LuaSlot::Kind::Matrix4);
    REQUIRE(back["Frame"].transform.m[15] == 1.f);
    REQUIRE(back["Target"].kind == LuaSlot::Kind::Instance);
    REQUIRE(back["Target"].text == "0123456789abcdef0123456789abcdef");

    REQUIRE(engine_core::attributes_to_json({}).empty());
    REQUIRE(engine_core::attributes_from_json("", back, error));
    REQUIRE(back.empty());
    REQUIRE(engine_core::attributes_from_json("null", back, error));
    REQUIRE(back.empty());
    REQUIRE(!engine_core::attributes_from_json("[1]", back, error));
}

TEST_CASE("ATR3 a bad entry is skipped and named, the rest load", "[attributes]") {
    engine_core::AttributeMap back;
    std::string error;
    REQUIRE(engine_core::attributes_from_json(
        R"({"X":{"type":"Color3","value":"red"},"Y":{"type":"number","value":2},"bad name":{"type":"number","value":1},"Z":{"type":"Signal","value":1}})",
        back, error));
    REQUIRE(back.size() == 1);
    REQUIRE(back["Y"].number == 2);
    REQUIRE(error.find("attribute X") != std::string::npos);
    REQUIRE(error.find("attribute bad name") != std::string::npos);
    REQUIRE(error.find("attribute Z") != std::string::npos);
}

TEST_CASE("ATR4 the Attributes property is hidden, saved, and reads the map", "[attributes]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::LuaField* field = engine_core::lua_class_find("DataModel", "Attributes");
    REQUIRE(field != nullptr);
    REQUIRE(field->hidden);
    REQUIRE(field->saved);
    REQUIRE(field->json_text);
    engine_core::GameObject& part = create_part(game);
    REQUIRE(part.attributes().empty());
    REQUIRE(!part.set_attribute("Health", number_slot(5)));
    LuaSlot out;
    REQUIRE(field->read(game, part, out));
    REQUIRE(out.text == R"({"Health":{"type":"number","value":5}})");
    REQUIRE(part.attribute("Health").kind == LuaSlot::Kind::Number);
    REQUIRE(part.attribute("Health").number == 5);
    REQUIRE(part.attribute("Missing").kind == LuaSlot::Kind::Nil);

    // The property's write replaces the whole map, as a load does.
    LuaSlot in;
    in.kind = LuaSlot::Kind::String;
    in.text = R"({"Speed":{"type":"number","value":2}})";
    REQUIRE(field->write(game, part, in));
    REQUIRE(part.attributes().size() == 1);
    REQUIRE(part.attribute("Speed").number == 2);
    in.text.clear();
    REQUIRE(field->write(game, part, in));
    REQUIRE(part.attributes().empty());
}

TEST_CASE("ATR5 set_attribute refuses what the spec refuses", "[attributes]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    REQUIRE(game.set_attribute("X", number_slot(1)) == "game cannot hold attributes");
    REQUIRE(game.attributes().empty());
    REQUIRE(part.set_attribute("bad name", number_slot(1)) ==
            "an attribute name is letters, digits, and underscores, up to 100");
    LuaSlot signal;
    signal.kind = LuaSlot::Kind::Signal;
    REQUIRE(part.set_attribute("X", signal) ==
            "an attribute holds a number, boolean, string, Vector2, Vector3, Color3, Matrix4, or Instance");
    LuaSlot gone;
    gone.kind = LuaSlot::Kind::Instance;
    gone.id = 99999;
    REQUIRE(part.set_attribute("X", gone) == "the instance is gone");
    REQUIRE(part.attributes().empty());
}

TEST_CASE("ATR6 an Instance attribute is held by GUID and reads nil once destroyed", "[attributes]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    engine_core::GameObject& other = create_part(game);
    const engine_core::InstanceId other_id = other.id();
    const std::string other_guid = game.guid(other_id);
    LuaSlot ref;
    ref.kind = LuaSlot::Kind::Instance;
    ref.id = other_id;
    REQUIRE(!part.set_attribute("Target", ref));
    REQUIRE(part.attribute("Target").kind == LuaSlot::Kind::Instance);
    REQUIRE(part.attribute("Target").id == other_id);
    REQUIRE(part.attributes().at("Target").text == other_guid);
    game.destroy(other_id);
    REQUIRE(part.attribute("Target").kind == LuaSlot::Kind::Nil);
    REQUIRE(part.attributes().count("Target") == 1);
}

TEST_CASE("ATR7 set, replace, and remove each undo as one step, kinds exact", "[attributes]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    engine_core::GameObject& other = create_part(game);
    LuaSlot ref;
    ref.kind = LuaSlot::Kind::Instance;
    ref.id = other.id();
    begin_step(game);
    REQUIRE(!part.set_attribute("X", ref));
    end_step(game);
    begin_step(game);
    REQUIRE(!part.set_attribute("X", number_slot(7)));
    end_step(game);
    begin_step(game);
    REQUIRE(!part.set_attribute("X", LuaSlot{}));
    end_step(game);
    REQUIRE(part.attributes().empty());
    game.history().undo();
    REQUIRE(part.attribute("X").kind == LuaSlot::Kind::Number);
    REQUIRE(part.attribute("X").number == 7);
    game.history().undo();
    REQUIRE(part.attribute("X").kind == LuaSlot::Kind::Instance);
    REQUIRE(part.attribute("X").id == other.id());
    game.history().undo();
    REQUIRE(part.attributes().empty());
    REQUIRE(!game.history().can_undo().first);
    game.history().redo();
    REQUIRE(part.attribute("X").kind == LuaSlot::Kind::Instance);
    game.history().redo();
    REQUIRE(part.attribute("X").number == 7);
}

TEST_CASE("ATR8 an equal value records nothing and fires nothing", "[attributes]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    int fired = 0;
    game.event_signal(part.id(), "AttributeChanged").connect([&](engine_core::InstanceId, engine_core::Field) {
        ++fired;
    });
    begin_step(game);
    REQUIRE(!part.set_attribute("X", number_slot(7)));
    end_step(game);
    game.events().drain();
    REQUIRE(fired == 1);
    REQUIRE(game.history().can_undo().first);
    begin_step(game);
    REQUIRE(!part.set_attribute("X", number_slot(7)));
    end_step(game);
    game.events().drain();
    REQUIRE(fired == 1);
    REQUIRE(!part.set_attribute("Absent", LuaSlot{}));
    game.events().drain();
    REQUIRE(fired == 1);
    // Only the first set was a step: one undo empties the map.
    game.history().undo();
    REQUIRE(part.attributes().empty());
    REQUIRE(!game.history().can_undo().first);
}

TEST_CASE("ATR9 AttributeChanged carries the name and Changed says Attributes", "[attributes]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = create_part(game);
    std::string named;
    std::string changed;
    game.event_signal(part.id(), "AttributeChanged").connect([&](engine_core::InstanceId, engine_core::Field) {
        const engine_core::EventArgs* args = game.events().current_args();
        REQUIRE(args != nullptr);
        REQUIRE(args->size() == 1);
        named = args->at(0).text;
    });
    game.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field) {
        const char* name = engine_core::lua_property_name(static_cast<std::uint32_t>(game.events().payload()));
        changed = name != nullptr ? name : "";
    });
    LuaSlot flag;
    flag.kind = LuaSlot::Kind::Bool;
    flag.flag = true;
    REQUIRE(!part.set_attribute("Open", flag));
    game.events().drain();
    REQUIRE(named == "Open");
    REQUIRE(changed == "Attributes");
}

namespace {

LuaSlot color_slot(float r, float g, float b) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Color;
    slot.color = {r, g, b, 1.f};
    return slot;
}

LuaSlot instance_slot(engine_core::InstanceId id) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::InstanceId named_part(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name) {
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), parent);
    game.set_name(part.id(), name);
    return part.id();
}

// The file a project saved the instance with this GUID in.
std::filesystem::path saved_file(const std::filesystem::path& root, const std::string& guid) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.find(guid) != std::string::npos && name.size() > 5 &&
            name.substr(name.size() - 5) == ".json") {
            return entry.path();
        }
    }
    return {};
}

}  // namespace

TEST_CASE("ATR10 attributes save with the place and load back, a dead target as its GUID", "[attributes][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const engine_core::InstanceId workspace = game.scene_service("Workspace");
    const engine_core::InstanceId holder = named_part(game, workspace, "Holder");
    const engine_core::InstanceId target = named_part(game, workspace, "Target");
    const std::string target_guid = game.guid(target);
    REQUIRE(!game.instance(holder)->set_attribute("Health", number_slot(5)));
    REQUIRE(!game.instance(holder)->set_attribute("Tint", color_slot(0.25f, 0.5f, 1.f)));
    REQUIRE(!game.instance(holder)->set_attribute("Aim", instance_slot(target)));
    game.destroy(target);
    project.save();

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    const engine_core::InstanceId loaded = other.find_first_child(other.scene_service("Workspace"), "Holder");
    REQUIRE(loaded != 0);
    const engine_core::DataModel& back = *other.instance(loaded);
    REQUIRE(back.attributes().size() == 3);
    REQUIRE(back.attribute("Health").number == 5);
    REQUIRE(back.attribute("Tint").kind == LuaSlot::Kind::Color);
    REQUIRE(back.attribute("Tint").color.g == 0.5f);
    REQUIRE(back.attributes().at("Aim").text == target_guid);
    REQUIRE(back.attribute("Aim").kind == LuaSlot::Kind::Nil);
}

TEST_CASE("ATR11 a hand-edited bad entry loads the rest", "[attributes][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const engine_core::InstanceId holder = named_part(game, game.scene_service("Workspace"), "Holder");
    REQUIRE(!game.instance(holder)->set_attribute("Y", number_slot(1)));
    project.save();

    const std::filesystem::path file = saved_file(dir.path, game.guid(holder));
    REQUIRE(!file.empty());
    std::stringstream text;
    text << std::ifstream(file).rdbuf();
    engine_core::JsonValue json;
    std::string error;
    REQUIRE(engine_core::parse_json(text.str(), json, error));
    engine_core::JsonValue* properties = nullptr;
    for (auto& member : const_cast<std::vector<engine_core::JsonValue::Member>&>(json.members())) {
        if (member.first == "properties") {
            properties = &member.second;
        }
    }
    engine_core::JsonValue edited;
    REQUIRE(engine_core::parse_json(R"({"X":{"type":"Color3","value":"red"},"Y":{"type":"number","value":2}})",
                                    edited, error));
    (properties != nullptr ? *properties : json).set("Attributes", edited);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << engine_core::write_json(json);

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    const engine_core::InstanceId loaded = other.find_first_child(other.scene_service("Workspace"), "Holder");
    REQUIRE(loaded != 0);
    REQUIRE(other.instance(loaded)->attributes().size() == 1);
    REQUIRE(other.instance(loaded)->attribute("Y").number == 2);
}

TEST_CASE("ATR12 Stop puts attributes back", "[attributes]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId holder = named_part(game, game.scene_service("Workspace"), "Holder");
    REQUIRE(!game.instance(holder)->set_attribute("Health", number_slot(5)));
    game.capture_place();
    game.start_simulation();
    REQUIRE(!game.instance(holder)->set_attribute("Health", number_slot(9)));
    REQUIRE(!game.instance(holder)->set_attribute("Extra", number_slot(1)));
    game.stop_simulation();
    REQUIRE(game.instance(holder)->attributes().size() == 1);
    REQUIRE(game.instance(holder)->attribute("Health").number == 5);
}

TEST_CASE("ATR13 Clone and paste copy attributes and remap an Instance inside the set", "[attributes][copy]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    const engine_core::InstanceId workspace = workspace_of(game);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace);
    const engine_core::InstanceId a = named_part(game, folder.id(), "A");
    const engine_core::InstanceId b = named_part(game, folder.id(), "B");
    const engine_core::InstanceId outside = named_part(game, workspace, "Outside");
    REQUIRE(!game.instance(a)->set_attribute("Target", instance_slot(b)));
    REQUIRE(!game.instance(a)->set_attribute("Away", instance_slot(outside)));
    REQUIRE(!game.instance(a)->set_attribute("Health", number_slot(3)));

    auto check = [&](engine_core::InstanceId copy) {
        REQUIRE(copy != 0);
        const engine_core::InstanceId a2 = game.find_first_child(copy, "A");
        const engine_core::InstanceId b2 = game.find_first_child(copy, "B");
        REQUIRE(a2 != 0);
        REQUIRE(b2 != 0);
        REQUIRE(a2 != a);
        REQUIRE(game.instance(a2)->attribute("Target").id == b2);
        REQUIRE(game.instance(a2)->attribute("Away").id == outside);
        REQUIRE(game.instance(a2)->attribute("Health").number == 3);
        // The source keeps its own.
        REQUIRE(game.instance(a)->attribute("Target").id == b);
    };

    check(engine_core::clone_tree(game, folder.id()));

    const engine_core::JsonValue file = engine_core::write_instance_file({engine_core::copy_tree(game, folder.id())});
    std::vector<engine_core::CopiedNode> roots;
    std::string error;
    REQUIRE(engine_core::read_instance_file(file, roots, error));
    std::vector<engine_core::InstanceId> made;
    engine_core::CopyMap map;
    REQUIRE(engine_core::paste_copies(game, roots, workspace, &made, nullptr, &map));
    engine_core::remap_references(game, map);
    REQUIRE(made.size() == 1);
    check(made[0]);
}

namespace {

std::vector<std::string> printed(const engine_core::ScriptRuntime::OutputBatch& batch) {
    std::vector<std::string> out;
    for (const auto& line : batch.lines) {
        out.push_back(line.text);
    }
    return out;
}

bool has_line(const std::vector<std::string>& lines, const std::string& text) {
    return std::find(lines.begin(), lines.end(), text) != lines.end();
}

bool has_prefix(const std::vector<std::string>& lines, const std::string& prefix) {
    for (const std::string& line : lines) {
        if (line.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("ATR14 scripts set, get, list, and remove attributes", "[attributes][lua]") {
    ScriptRig rig;
    rig.game.set_name(create_part(rig.game).id(), "Part");
    rig.runtime.run_chunk(R"(
        local p = game.Workspace.Part
        p:SetAttribute("Health", 5)
        p:SetAttribute("Tint", Color3.new(1, 0, 0))
        p:SetAttribute("Where", Vector3.new(1, 2, 3))
        p:SetAttribute("Flat", Vector2.new(4, 5))
        p:SetAttribute("Open", true)
        p:SetAttribute("Label", "hi")
        p:SetAttribute("Target", game.Workspace)
        print(p:GetAttribute("Health"), p:GetAttribute("Tint").R, p:GetAttribute("Where").Z, p:GetAttribute("Flat").Y,
              p:GetAttribute("Open"), p:GetAttribute("Label"), p:GetAttribute("Target").Name)
        local n = 0
        for k, v in pairs(p:GetAttributes()) do n += 1 end
        print("count", n, p:GetAttributes().Health)
        p:SetAttribute("Health", nil)
        print("removed", p:GetAttribute("Health"))
        print("missing", p:GetAttribute("Nothing"))
        print("name", pcall(function() p:SetAttribute("bad name", 1) end))
        print("kind", pcall(function() p:SetAttribute("X", p.Changed) end))
        print("root", pcall(function() game:SetAttribute("X", 1) end))
    )");
    const std::vector<std::string> out = printed(rig.runtime.drain_output());
    INFO(rig.runtime.last_error());
    for (const std::string& line : out) {
        UNSCOPED_INFO(line);
    }
    REQUIRE(has_line(out, "5\t1\t3\t5\ttrue\thi\tWorkspace\n"));
    REQUIRE(has_line(out, "count\t7\t5\n"));
    REQUIRE(has_line(out, "removed\tnil\n"));
    REQUIRE(has_line(out, "missing\tnil\n"));
    REQUIRE(has_prefix(out, "name\tfalse\t"));
    REQUIRE(has_prefix(out, "kind\tfalse\t"));
    REQUIRE(has_prefix(out, "root\tfalse\t"));
    REQUIRE(rig.game.instance(rig.game.find_first_child(workspace_of(rig.game), "Part"))->attributes().size() == 6);
}

TEST_CASE("ATR15 AttributeChanged and GetAttributeChangedSignal fire for the right name", "[attributes][lua]") {
    ScriptRig rig;
    rig.game.set_name(create_part(rig.game).id(), "Part");
    rig.runtime.run_chunk(R"(
        local p = game.Workspace.Part
        p.AttributeChanged:Connect(function(name) print("any", name) end)
        p:GetAttributeChangedSignal("A"):Connect(function(...) print("A changed", select("#", ...)) end)
        p:GetAttributeChangedSignal("B"):Connect(function() print("B changed") end)
        p:SetAttribute("A", 1)
        p:SetAttribute("A", 1)
        p:SetAttribute("B", nil)
        p:SetAttribute("B", true)
    )");
    rig.frames(1);
    const std::vector<std::string> out = printed(rig.runtime.drain_output());
    INFO(rig.runtime.last_error());
    REQUIRE(out == std::vector<std::string>{"any\tA\n", "A changed\t0\n", "any\tB\n", "B changed\n"});
}

TEST_CASE("ATR16 strict script analysis accepts the attribute members", "[attributes][lua]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Uses", R"(--!strict
local p = workspace
p:SetAttribute("Health", 5)
local health = p:GetAttribute("Health")
local all = p:GetAttributes()
print(health, all.Health)
p.AttributeChanged:Connect(function(name: string) print(name) end)
p:GetAttributeChangedSignal("Health"):Connect(function() end)
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
