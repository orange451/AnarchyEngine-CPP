// The GameObject field is a PhysicsObject's alone: a Brush is what it draws
// and moves no GameObject, and a PlayerController moves only the GameObject
// it is under.

#include "physics_rig.hpp"
#include "support.hpp"

#include "Brush.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::GameObject;
using engine_core::JsonValue;

using namespace physics_rig;

bool has_field(const char* class_name, const char* field) {
    for (const engine_core::LuaField& saved : engine_core::lua_saved_fields(class_name)) {
        if (std::string(saved.name) == field) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("PG1 only a PhysicsObject has a GameObject field", "[physics][gameobject]") {
    REQUIRE(has_field("PhysicsObject", "GameObject"));
    REQUIRE_FALSE(has_field("PhysicsBase", "GameObject"));
    REQUIRE_FALSE(has_field("Brush", "GameObject"));
    REQUIRE_FALSE(has_field("PlayerController", "GameObject"));
}

TEST_CASE("PG2 a Brush under a GameObject moves nothing, and falls from where it is", "[physics][gameobject]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    part.set_transform(at(0.f, 20.f, 0.f));
    engine_core::Brush& brush = rig.game.create<engine_core::Brush>();
    brush.set_anchored(false);
    REQUIRE_FALSE(brush.set_transform(at(5.f, 20.f, 0.f)));
    rig.game.set_parent(brush.id(), part.id());
    REQUIRE(brush.driven_game_object() == 0);
    rig.play();
    rig.seconds(0.5);
    REQUIRE(near(x_of(brush.transform()), 5.f, 1e-3f));
    REQUIRE(y_of(brush.transform()) < 19.f);
    REQUIRE(near(y_of(part.transform()), 20.f, 1e-5f));
}

TEST_CASE("PG3 a PlayerController moves the GameObject it is under", "[physics][gameobject]") {
    PhysicsRig rig;
    GameObject& character = create_part(rig.game);
    engine_core::PlayerController& player = rig.game.create<engine_core::PlayerController>();
    rig.game.set_parent(player.id(), character.id());
    REQUIRE(player.driven_game_object() == character.id());
}

TEST_CASE("PG4 an old file's GameObject link on a Brush or a PlayerController is kept aside and links nothing",
          "[physics][gameobject]") {
    PhysicsRig rig;
    GameObject& part = create_part(rig.game);
    const JsonValue link = JsonValue::string(rig.game.guid(part.id()));
    engine_core::Brush& brush = rig.game.create<engine_core::Brush>();
    rig.game.set_parent(brush.id(), workspace_of(rig.game));
    engine_core::PlayerController& player = rig.game.create<engine_core::PlayerController>();
    rig.game.set_parent(player.id(), workspace_of(rig.game));
    std::string error;
    REQUIRE_FALSE(brush.load_property("GameObject", link, error));
    REQUIRE_FALSE(player.load_property("GameObject", link, error));
    REQUIRE(error.empty());
    REQUIRE(brush.driven_game_object() == 0);
    REQUIRE(player.driven_game_object() == 0);
    // A PhysicsObject still takes it.
    engine_core::PhysicsObject& body = rig.game.create<engine_core::PhysicsObject>();
    rig.game.set_parent(body.id(), workspace_of(rig.game));
    REQUIRE(body.load_property("GameObject", link, error));
    REQUIRE(error.empty());
    REQUIRE(body.driven_game_object() == part.id());
}
