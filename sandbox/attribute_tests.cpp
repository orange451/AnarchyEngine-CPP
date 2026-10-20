// Attributes: named values any instance can carry, set and read from Lua,
// saved as one hidden JSON property, undone, and restored at Stop.

#include "support.hpp"

#include "Attributes.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

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
