// read_property_json: one property's value as a save writes it, which
// replication sends and load_property applies
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "support.hpp"

#include "GameObject.hpp"
#include "Light.hpp"
#include "Script.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

using engine_core::JsonValue;

namespace {

// Every key save_properties writes, read back one at a time, is the same value.
// A Transform reads as each float's exact double, where a save writes its
// shortest decimal, so the two agree once each number is cast back to a float.
void require_matches_save(const engine_core::DataModel& object) {
    engine_core::PropertyBag saved;
    object.save_properties(saved);
    for (const auto& [key, value] : saved) {
        INFO(object.class_name() << "." << key);
        JsonValue read;
        REQUIRE(object.read_property_json(key, read));
        if (key == "Transform") {
            REQUIRE(read.is_array());
            REQUIRE(read.items().size() == value.items().size());
            for (std::size_t i = 0; i < value.items().size(); ++i) {
                REQUIRE(static_cast<float>(read.items()[i].as_number()) == static_cast<float>(value.items()[i].as_number()));
            }
            continue;
        }
        REQUIRE(read == value);
    }
}

}  // namespace

TEST_CASE("RP5 read_property_json gives what a save writes, at a default or not", "[replication][property]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId workspace = game.scene_service("Workspace");

    engine_core::GameObject& object = game.create<engine_core::GameObject>();
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 3.5f;
    moved.m[13] = 0.1f;   // no double holds 0.1 and the float's value at once
    object.set_transform(moved);
    game.set_simulated(object.id(), true);
    game.set_parent(object.id(), workspace);
    require_matches_save(object);
    JsonValue pose;
    REQUIRE(object.read_property_json("Transform", pose));
    REQUIRE(pose.items()[13].as_number() == static_cast<double>(0.1f));   // the float exactly

    engine_core::PointLight& light = game.create<engine_core::PointLight>();
    game.set_parent(light.id(), workspace);
    require_matches_save(light);
    JsonValue intensity;
    REQUIRE(light.read_property_json("Intensity", intensity));   // at its default, still read
    REQUIRE(intensity.is_number());

    engine_core::Script& script = add_script(game, "S", "-- s");
    script.set_enabled(false);
    require_matches_save(script);
    JsonValue enabled;
    REQUIRE(script.read_property_json("Enabled", enabled));
    REQUIRE(enabled == JsonValue::boolean(false));

    engine_core::ObjectValue& pointer = game.create<engine_core::ObjectValue>();
    game.set_parent(pointer.id(), workspace);
    engine_core::LuaSlot target;
    target.kind = engine_core::LuaSlot::Kind::Instance;
    target.id = object.id();
    REQUIRE_FALSE(pointer.set_value(target));
    require_matches_save(pointer);

    JsonValue none;
    REQUIRE_FALSE(object.read_property_json("Name", none));
    REQUIRE_FALSE(object.read_property_json("NoSuchProperty", none));
    REQUIRE_FALSE(script.read_property_json("Source", none));   // Source is not a saved property
}

TEST_CASE("RP6 load_property with read_property_json's value copies the state", "[replication][property]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId workspace = game.scene_service("Workspace");
    engine_core::PointLight& from = game.create<engine_core::PointLight>();
    game.set_parent(from.id(), workspace);
    engine_core::PointLight& to = game.create<engine_core::PointLight>();
    game.set_parent(to.id(), workspace);
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[14] = -2.0f;
    from.set_transform(moved);
    engine_core::PropertyBag saved;
    from.save_properties(saved);
    for (const auto& [key, value] : saved) {
        JsonValue read;
        REQUIRE(from.read_property_json(key, read));
        std::string error;
        REQUIRE(to.load_property(key, read, error));
        REQUIRE(error.empty());
    }
    engine_core::PropertyBag copied;
    to.save_properties(copied);
    REQUIRE(copied == saved);
}
