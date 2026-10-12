// The ValueBase classes, IntValue, NumberValue, StringValue, BoolValue, and
// ObjectValue: each one's Value, and how it is checked, undoes, saves, loads,
// and comes back at Stop.

#include "support.hpp"

#include "Folder.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace {

using engine_core::BoolValue;
using engine_core::InstanceId;
using engine_core::IntValue;
using engine_core::NumberValue;
using engine_core::ObjectValue;
using engine_core::StringValue;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

template <class T>
T& add_value(engine_core::DataModel& game, const char* name) {
    T& value = game.create<T>();
    game.set_name(value.id(), name);
    game.set_parent(value.id(), workspace_of(game));
    return value;
}

}  // namespace

TEST_CASE("VAL1 each value class is a ValueBase with its default, and saves nothing at it", "[value]") {
    SimRole role;
    engine_core::Game game;
    for (const char* name : {"IntValue", "NumberValue", "StringValue", "BoolValue", "ObjectValue"}) {
        INFO(name);
        REQUIRE(engine_core::project_class_known(name));
        REQUIRE(engine_core::lua_creatable_known(name));
        REQUIRE(engine_core::lua_class_inherits(name, "ValueBase"));
    }
    REQUIRE_FALSE(engine_core::lua_creatable_known("ValueBase"));

    IntValue& whole = add_value<IntValue>(game, "Int");
    NumberValue& number = add_value<NumberValue>(game, "Number");
    StringValue& text = add_value<StringValue>(game, "String");
    BoolValue& flag = add_value<BoolValue>(game, "Bool");
    ObjectValue& object = add_value<ObjectValue>(game, "Object");
    REQUIRE(whole.value() == 0);
    REQUIRE(number.value() == 0.0);
    REQUIRE(text.value().empty());
    REQUIRE_FALSE(flag.value());
    REQUIRE(object.value_id() == 0);
    engine_core::DataModel* const values[] = {&whole, &number, &text, &flag, &object};
    for (engine_core::DataModel* value : values) {
        engine_core::PropertyBag saved;
        value->save_properties(saved);
        REQUIRE(saved.empty());
    }
}

TEST_CASE("VAL2 IntValue rounds and clamps; numbers must be finite", "[value]") {
    SimRole role;
    engine_core::Game game;
    IntValue& whole = add_value<IntValue>(game, "Int");
    REQUIRE_FALSE(whole.set_value(2.4));
    REQUIRE(whole.value() == 2);
    REQUIRE_FALSE(whole.set_value(2.5));
    REQUIRE(whole.value() == 3);
    REQUIRE_FALSE(whole.set_value(-7.6));
    REQUIRE(whole.value() == -8);
    REQUIRE_FALSE(whole.set_value(1e300));
    REQUIRE(whole.value() == IntValue::kMaxInt);
    REQUIRE_FALSE(whole.set_value(-1e300));
    REQUIRE(whole.value() == -IntValue::kMaxInt);
    REQUIRE(*whole.set_value(std::nan("")) == "Value must be a finite number");
    REQUIRE(*whole.set_value(INFINITY) == "Value must be a finite number");
    REQUIRE(whole.value() == -IntValue::kMaxInt);

    NumberValue& number = add_value<NumberValue>(game, "Number");
    REQUIRE_FALSE(number.set_value(2.5));
    REQUIRE(number.value() == 2.5);
    REQUIRE(*number.set_value(-INFINITY) == "Value must be a finite number");
    REQUIRE(number.value() == 2.5);
}

TEST_CASE("VAL3 ObjectValue takes any instance, undoes, and heals when its target comes back", "[value]") {
    SimRole role;
    engine_core::Game game;
    ObjectValue& object = add_value<ObjectValue>(game, "Object");
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));

    begin_step(game, "Set Value");
    REQUIRE_FALSE(object.set_value(instance_slot(folder.id())));
    end_step(game);
    REQUIRE(object.value_id() == folder.id());
    game.history().undo();
    REQUIRE(object.value_id() == 0);
    game.history().redo();
    REQUIRE(object.value_id() == folder.id());

    const InstanceId folder_id = folder.id();
    begin_step(game, "Delete");
    game.destroy(folder_id);
    end_step(game);
    REQUIRE(object.value_id() == 0);
    REQUIRE(object.value().kind == engine_core::LuaSlot::Kind::Nil);
    game.history().undo();
    REQUIRE(object.value_id() == folder_id);

    REQUIRE_FALSE(object.set_value(engine_core::LuaSlot{}));
    REQUIRE(object.value_id() == 0);
}

TEST_CASE("VAL4 each Value undoes, and comes back at Stop", "[value]") {
    SimRole role;
    engine_core::Game game;
    IntValue& whole = add_value<IntValue>(game, "Int");
    StringValue& text = add_value<StringValue>(game, "String");
    BoolValue& flag = add_value<BoolValue>(game, "Bool");

    begin_step(game, "Set Values");
    REQUIRE_FALSE(whole.set_value(5));
    text.set_value("hello");
    flag.set_value(true);
    end_step(game);
    game.history().undo();
    REQUIRE(whole.value() == 0);
    REQUIRE(text.value().empty());
    REQUIRE_FALSE(flag.value());
    game.history().redo();
    REQUIRE(whole.value() == 5);
    REQUIRE(text.value() == "hello");
    REQUIRE(flag.value());

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(whole.set_value(9));
    text.set_value("changed");
    flag.set_value(false);
    game.stop_simulation();
    REQUIRE(whole.value() == 5);
    REQUIRE(text.value() == "hello");
    REQUIRE(flag.value());
}

TEST_CASE("VAL5 a project saves and loads every Value, an ObjectValue's by GUID", "[value]") {
    SimRole role;
    TempDir dir;
    std::string target_guid;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        engine_core::GameObject& part = create_part(game);
        game.set_name(part.id(), "Part");
        target_guid = game.guid(part.id());
        REQUIRE_FALSE(add_value<IntValue>(game, "Int").set_value(-42));
        REQUIRE_FALSE(add_value<NumberValue>(game, "Number").set_value(0.125));
        add_value<StringValue>(game, "String").set_value("a \"quoted\" line\nand another");
        add_value<BoolValue>(game, "Bool").set_value(true);
        REQUIRE_FALSE(add_value<ObjectValue>(game, "Object").set_value(instance_slot(part.id())));
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    const InstanceId ws = workspace_of(game);
    auto* whole = dynamic_cast<IntValue*>(game.instance(game.find_first_child(ws, "Int")));
    auto* number = dynamic_cast<NumberValue*>(game.instance(game.find_first_child(ws, "Number")));
    auto* text = dynamic_cast<StringValue*>(game.instance(game.find_first_child(ws, "String")));
    auto* flag = dynamic_cast<BoolValue*>(game.instance(game.find_first_child(ws, "Bool")));
    auto* object = dynamic_cast<ObjectValue*>(game.instance(game.find_first_child(ws, "Object")));
    REQUIRE(whole != nullptr);
    REQUIRE(number != nullptr);
    REQUIRE(text != nullptr);
    REQUIRE(flag != nullptr);
    REQUIRE(object != nullptr);
    REQUIRE(whole->value() == -42);
    REQUIRE(number->value() == 0.125);
    REQUIRE(text->value() == "a \"quoted\" line\nand another");
    REQUIRE(flag->value());
    REQUIRE(game.guid(object->value_id()) == target_guid);
}

TEST_CASE("VAL6 scripts make each Value and set it", "[value]") {
    ScriptRig rig;
    add_script(rig.game, "Values", R"(
        local int = Instance.new("IntValue", workspace)
        local number = Instance.new("NumberValue", workspace)
        local text = Instance.new("StringValue", workspace)
        local flag = Instance.new("BoolValue", workspace)
        local object = Instance.new("ObjectValue", workspace)
        _G.defaults = int.Value == 0 and number.Value == 0 and text.Value == "" and flag.Value == false
            and object.Value == nil and int:IsA("ValueBase") and object:IsA("ValueBase")
        int.Value = 3.7
        number.Value = 3.7
        text.Value = "hi"
        flag.Value = true
        object.Value = int
        _G.set = int.Value == 4 and number.Value == 3.7 and text.Value == "hi" and flag.Value == true
            and object.Value == int
        _G.changed = 0
        int.Changed:Connect(function(name) if name == "Value" then _G.changed += 1 end end)
        int.Value = 10
        object.Value = nil
        _G.refused = object.Value == nil
            and not pcall(function() int.Value = "seven" end)
            and not pcall(function() number.Value = 0 / 0 end)
            and not pcall(function() flag.Value = 1 end)
            and not pcall(function() object.Value = 5 end)
            and not pcall(function() Instance.new("ValueBase") end)
    )");
    rig.game.start_simulation();
    rig.frames(2, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"defaults", "set", "refused"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
    double changed = 0;
    REQUIRE(rig.runtime.global_number("changed", changed));
    REQUIRE(changed == 1);
}
