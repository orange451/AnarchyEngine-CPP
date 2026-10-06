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

namespace {

// Its +Y axis, which an upright Transform keeps straight up.
bool upright(const Matrix4& m) { return near(m.m[4], 0.f, 1e-4f) && near(m.m[5], 1.f, 1e-4f) && near(m.m[6], 0.f, 1e-4f); }

}  // namespace

TEST_CASE("C3 a PlayerController is an upright cylinder that never turns", "[player]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("with StepHeight 0 it falls and rests with its feet on the floor") {
        PlayerController& c = rig.controller(at(0.f, 3.f, 0.f));
        REQUIRE_FALSE(c.set_step_height(0.0));
        rig.play();
        rig.seconds(2.0);
        INFO(y_of(c.transform()));
        REQUIRE(near(y_of(c.transform()), 0.f, 0.02f));
        REQUIRE(rig.physics.has_body(c.id()));
    }

    SECTION("a spinning box knocks it aside but does not turn it") {
        PlayerController& c = rig.controller(at(0.f, 0.f, 0.f));
        REQUIRE_FALSE(c.set_step_height(0.0));
        PhysicsObject& box = rig.body(at(-3.f, 1.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
        REQUIRE_FALSE(box.set_velocity(Vec3{8.f, 0.f, 0.f}));
        REQUIRE_FALSE(box.set_angular_velocity(Vec3{3.f, 5.f, 20.f}));
        rig.play();
        rig.seconds(2.0);
        REQUIRE(x_of(c.transform()) > 0.05f);
        REQUIRE(upright(c.transform()));
        REQUIRE(near(c.transform().m[0], 1.f, 1e-4f));
    }

    SECTION("a written Transform keeps its position and its turn about Y only") {
        PlayerController& c = rig.controller(at(0.f, 0.f, 0.f));
        Matrix4 turned = engine_core::matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, 0.7);
        turned.m[12] = 1.f;
        turned.m[13] = 2.f;
        turned.m[14] = 3.f;
        REQUIRE_FALSE(c.set_transform(turned));
        REQUIRE(near(c.transform().m[0], std::cos(0.7f), 1e-4f));
        Matrix4 tilted = engine_core::matrix4_axis_angle(Vec3{1.f, 0.f, 0.f}, 0.5);
        tilted.m[12] = 4.f;
        REQUIRE_FALSE(c.set_transform(tilted));
        REQUIRE(upright(c.transform()));
        REQUIRE(x_of(c.transform()) == 4.f);
    }

    SECTION("anchored, it is a static cylinder a box lands on") {
        PlayerController& c = rig.controller(at(0.f, 0.f, 0.f));
        c.set_anchored(true);
        PhysicsObject& box = rig.body(at(0.f, 4.f, 0.f), Vec3{0.5f, 0.5f, 0.5f}, false);
        rig.play();
        rig.seconds(2.0);
        INFO(y_of(box.transform()));
        REQUIRE(near(y_of(box.transform()), 2.25f, 0.03f));
        REQUIRE(y_of(c.transform()) == 0.f);
    }

    SECTION("it moves its GameObject parent, upright, from its feet") {
        engine_core::GameObject& part = create_part(rig.game);
        part.set_transform(at(0.f, 3.f, 0.f));
        PlayerController& c = rig.controller(at(0.f, 0.f, 0.f), part.id());
        REQUIRE_FALSE(c.set_step_height(0.0));
        rig.play();
        rig.seconds(2.0);
        REQUIRE(near(y_of(part.transform()), 0.f, 0.02f));
        // A script moving the GameObject, tilted, moves the body upright.
        Matrix4 tilted = engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 0.6);
        tilted.m[12] = 5.f;
        tilted.m[13] = 1.f;
        part.set_transform(tilted);
        rig.seconds(2.0);
        INFO(x_of(part.transform()) << " " << y_of(part.transform()));
        REQUIRE(near(x_of(part.transform()), 5.f, 0.05f));
        REQUIRE(near(y_of(part.transform()), 0.f, 0.02f));
        REQUIRE(upright(part.transform()));
    }
    REQUIRE(rig.warnings.empty());
}

TEST_CASE("C4 a PlayerController's outline is its cylinder and a line up from its feet", "[player]") {
    PhysicsRig rig;
    PlayerController& c = rig.controller(at(0.f, 0.f, 0.f));
    REQUIRE_FALSE(c.set_radius(0.5));
    REQUIRE_FALSE(c.set_height(2.0));
    REQUIRE_FALSE(c.set_step_height(0.4));
    std::vector<Vec3> lines;
    engine_core::PhysicsWorld::collision_outline(c, lines);
    REQUIRE(lines.size() >= 2 * 16 * 3);
    REQUIRE(lines.size() % 2 == 0);
    bool feet = false;
    for (const Vec3& p : lines) {
        const float out = std::sqrt(p.x * p.x + p.z * p.z);
        REQUIRE(out <= 0.5f + 1e-4f);
        REQUIRE(p.y >= -1e-4f);
        REQUIRE(p.y <= 2.f + 1e-4f);
        // Only the line from the feet comes below the cylinder.
        if (p.y < 0.4f - 1e-4f) {
            REQUIRE(out < 1e-4f);
        }
        feet = feet || (out < 1e-4f && p.y < 1e-4f);
    }
    REQUIRE(feet);
}
