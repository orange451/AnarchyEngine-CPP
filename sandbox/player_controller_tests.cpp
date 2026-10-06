// PlayerController: a PhysicsBase that is an upright cylinder hovering
// StepHeight above the ground, slowed there by its own Friction.

#include "physics_rig.hpp"

#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace physics_rig;

TEST_CASE("C1 PlayerController properties are checked, saved, and come back at Stop", "[player]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("PlayerController"));
    REQUIRE(engine_core::lua_class_inherits("PlayerController", "PhysicsBase"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("PlayerController", "PhysicsObject"));
    for (const char* gone : {"Shape", "Size", "Mesh", "Bounciness", "AngularVelocity", "AngularDamping"}) {
        INFO(gone);
        REQUIRE(engine_core::lua_class_find("PlayerController", gone) == nullptr);
    }

    PlayerController& c = game.create<PlayerController>();
    game.set_parent(c.id(), workspace_of(game));
    REQUIRE(c.friction() == 8.0);
    REQUIRE(c.radius() == 0.5);
    REQUIRE(c.height() == 2.0);
    REQUIRE(c.step_height() == 0.4);
    REQUIRE(c.max_slope() == 45.0);
    REQUIRE_FALSE(c.on_ground());
    REQUIRE_FALSE(c.is_sliding());

    engine_core::PropertyBag saved;
    c.save_properties(saved);
    REQUIRE(saved.empty());

    // Each clamps against its own limits only.
    REQUIRE_FALSE(c.set_friction(-1.0));
    REQUIRE(c.friction() == 0.0);
    REQUIRE_FALSE(c.set_radius(0.0));
    REQUIRE(c.radius() == PlayerController::kMinSize);
    REQUIRE_FALSE(c.set_height(0.0));
    REQUIRE(c.height() == PlayerController::kMinSize);
    REQUIRE_FALSE(c.set_height(2.0));
    REQUIRE_FALSE(c.set_step_height(-1.0));
    REQUIRE(c.step_height() == 0.0);
    REQUIRE_FALSE(c.set_max_slope(120.0));
    REQUIRE(c.max_slope() == 89.0);
    REQUIRE_FALSE(c.set_max_slope(-5.0));
    REQUIRE(c.max_slope() == 0.0);
    REQUIRE(*c.set_radius(std::nan("")) == "Radius must be a finite number");

    // StepHeight above Height changes neither; only the hover gap is capped.
    REQUIRE_FALSE(c.set_step_height(3.0));
    REQUIRE(c.step_height() == 3.0);
    REQUIRE(c.height() == 2.0);
    REQUIRE(near(static_cast<float>(c.hover_gap()), 2.f - PlayerController::kMinSize, 1e-6f));
    REQUIRE_FALSE(c.set_step_height(0.4));
    REQUIRE(near(static_cast<float>(c.hover_gap()), 0.4f, 1e-6f));

    // OnGround and IsSliding are not saved, and false when not playing.
    engine_core::PropertyBag changed;
    c.save_properties(changed);
    REQUIRE(engine_core::bag_find(changed, "OnGround") == nullptr);
    REQUIRE(engine_core::bag_find(changed, "IsSliding") == nullptr);
    REQUIRE(engine_core::bag_find(changed, "MaxSlope") != nullptr);
    c.store_ground(true, false);
    REQUIRE_FALSE(c.on_ground());

    REQUIRE_FALSE(c.set_radius(0.5));
    game.capture_place();
    game.start_simulation();
    c.store_ground(true, false);
    REQUIRE(c.on_ground());
    REQUIRE_FALSE(c.set_radius(2.0));
    game.stop_simulation();
    REQUIRE(c.radius() == 0.5);
    REQUIRE_FALSE(c.on_ground());

    const engine_core::LuaField* slope = engine_core::lua_class_find("PlayerController", "MaxSlope");
    REQUIRE(slope != nullptr);
    REQUIRE(slope->slider());
}

TEST_CASE("C2 scripts make a PlayerController and cannot write its ground flags", "[player]") {
    ScriptRig rig;
    add_script(rig.game, "Controller", R"(
        local c = Instance.new("PlayerController", workspace)
        _G.default = c.Friction == 8 and c.Radius == 0.5 and c.Height == 2 and c.StepHeight == 0.4
            and c.MaxSlope == 45 and c.OnGround == false and c.IsSliding == false and c.Mass == 1
        _G.isa = c:IsA("PhysicsBase") and c:IsA("PVInstance") and not c:IsA("PhysicsObject")
        _G.readonly = not pcall(function() c.OnGround = true end)
            and not pcall(function() c.IsSliding = true end)
        _G.noshape = not pcall(function() return c.Shape end)
        _G.abstract = not pcall(function() Instance.new("PhysicsBase") end)
        c.StepHeight = 5
        _G.kept = c.StepHeight == 5 and c.Height == 2
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"default", "isa", "readonly", "noshape", "abstract", "kept"}) {
        INFO(name);
        bool value = false;
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}
