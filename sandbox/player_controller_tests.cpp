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

namespace {

// Each step, sets the controller's speed across the ground to (vx, vz),
// keeping what physics gave it up and down, then steps.
void walk(PhysicsRig& rig, PlayerController& c, float vx, float vz, double time) {
    const int count = static_cast<int>(time / kStep + 0.5);
    for (int i = 0; i < count; ++i) {
        REQUIRE_FALSE(c.set_velocity(Vec3{vx, c.velocity().y, vz}));
        rig.steps(1);
    }
}

// An anchored ramp rising toward +X at degrees, its top through the origin.
PhysicsObject& ramp(PhysicsRig& rig, double degrees) {
    const double angle = degrees * 3.14159265358979 / 180.0;
    Matrix4 where = engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, angle);
    // The box's top face passes through the origin: its center sits half
    // its thickness below, along the face's normal (-sin, cos, 0).
    where.m[12] = static_cast<float>(0.5 * std::sin(angle));
    where.m[13] = static_cast<float>(-0.5 * std::cos(angle));
    PhysicsObject& slope = rig.body(where, Vec3{30.f, 1.f, 8.f}, true);
    return slope;
}

}  // namespace

TEST_CASE("C5 a PlayerController hovers StepHeight above the floor, on ground", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PlayerController& c = rig.controller(at(0.f, 3.f, 0.f));
    rig.play();
    rig.seconds(3.0);
    INFO(y_of(c.transform()));
    // Its feet are on the floor: the cylinder floats 0.4 above them, within 1%.
    REQUIRE(near(y_of(c.transform()), 0.f, 0.004f));
    REQUIRE(near(c.velocity().y, 0.f, 0.01f));
    REQUIRE(c.on_ground());
    REQUIRE_FALSE(c.is_sliding());
}

TEST_CASE("C6 it walks up an edge as tall as StepHeight, and no taller", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));

    SECTION("an edge 0.9 of StepHeight is walked up") {
        rig.body(at(4.f, 0.18f, 0.f), Vec3{4.f, 0.36f, 4.f}, true);
        rig.play();
        rig.seconds(1.0);
        walk(rig, c, 2.f, 0.f, 2.5);
        INFO(x_of(c.transform()) << " " << y_of(c.transform()));
        REQUIRE(x_of(c.transform()) > 3.f);
        REQUIRE(near(y_of(c.transform()), 0.36f, 0.01f));
        REQUIRE(c.on_ground());
    }

    SECTION("an edge 1.1 of StepHeight blocks it") {
        rig.body(at(4.f, 0.22f, 0.f), Vec3{4.f, 0.44f, 4.f}, true);
        rig.play();
        rig.seconds(1.0);
        walk(rig, c, 2.f, 0.f, 2.5);
        INFO(x_of(c.transform()));
        // The ledge's face is at x = 2, and the cylinder's radius 0.5.
        REQUIRE(x_of(c.transform()) <= 1.51f);
        REQUIRE(near(y_of(c.transform()), 0.f, 0.01f));
    }
}

TEST_CASE("C7 an upward Velocity jumps, and the hover lets go", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
    rig.play();
    rig.seconds(1.0);
    REQUIRE(c.on_ground());
    REQUIRE_FALSE(c.set_velocity(Vec3{0.f, 5.f, 0.f}));
    float top = 0.f;
    bool left = false;
    for (int i = 0; i < 240; ++i) {
        rig.steps(1);
        top = std::max(top, y_of(c.transform()));
        left = left || !c.on_ground();
    }
    INFO(top);
    // 5 up under 9.81 rises 1.27; the hover pulling down would cut that short.
    REQUIRE(top > 1.15f);
    REQUIRE(left);
    rig.seconds(2.0);
    REQUIRE(c.on_ground());
}

TEST_CASE("C8 it stands still on a walkable slope and slides down a steep one", "[player]") {
    PhysicsRig rig;

    SECTION("30 degrees: on ground, and it does not creep") {
        ramp(rig, 30.0);
        PlayerController& c = rig.controller(at(0.f, 1.f, 0.f));
        rig.play();
        rig.seconds(1.0);
        const float x = x_of(c.transform());
        rig.seconds(2.0);
        INFO(x << " " << x_of(c.transform()));
        REQUIRE(near(x_of(c.transform()), x, 0.02f));
        REQUIRE(c.on_ground());
        REQUIRE_FALSE(c.is_sliding());
    }

    SECTION("60 degrees: sliding, and it goes down") {
        ramp(rig, 60.0);
        PlayerController& c = rig.controller(at(0.f, 1.f, 0.f));
        rig.play();
        bool slid = false;
        for (int i = 0; i < 480; ++i) {
            rig.steps(1);
            slid = slid || c.is_sliding();
            REQUIRE_FALSE(c.on_ground());
        }
        INFO(x_of(c.transform()));
        REQUIRE(slid);
        REQUIRE(x_of(c.transform()) < -1.f);
    }
}

TEST_CASE("C9 ground in odd places: none, anchored, StepHeight over Height, changes in play, Stop", "[player]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("in the air it is on nothing") {
        PlayerController& c = rig.controller(at(0.f, 20.f, 0.f));
        rig.play();
        rig.seconds(0.1);
        REQUIRE_FALSE(c.on_ground());
        REQUIRE_FALSE(c.is_sliding());
    }

    SECTION("anchored, it is never on ground") {
        PlayerController& c = rig.controller(at(0.f, 0.4f, 0.f));
        c.set_anchored(true);
        rig.play();
        rig.seconds(0.5);
        REQUIRE_FALSE(c.on_ground());
    }

    SECTION("StepHeight above Height: a thin slab at the top, its feet still on the floor") {
        PlayerController& c = rig.controller(at(0.f, 3.f, 0.f));
        REQUIRE_FALSE(c.set_step_height(3.0));
        rig.play();
        rig.seconds(3.0);
        INFO(y_of(c.transform()));
        REQUIRE(near(y_of(c.transform()), 0.f, 0.03f));
        REQUIRE(c.on_ground());
        REQUIRE(c.step_height() == 3.0);
    }

    SECTION("raising StepHeight during play lifts the cylinder, not the feet, and launches nothing") {
        PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
        rig.play();
        rig.seconds(1.0);
        REQUIRE_FALSE(c.set_step_height(0.8));
        float fastest = 0.f;
        for (int i = 0; i < 240; ++i) {
            rig.steps(1);
            fastest = std::max(fastest, std::fabs(c.velocity().y));
        }
        INFO(fastest << " " << y_of(c.transform()));
        REQUIRE(fastest < 5.f);
        REQUIRE(near(y_of(c.transform()), 0.f, 0.01f));
        REQUIRE(c.on_ground());
    }

    SECTION("Stop clears OnGround") {
        PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
        rig.play();
        rig.seconds(1.0);
        REQUIRE(c.on_ground());
        rig.game.stop_simulation();
        REQUIRE_FALSE(c.on_ground());
    }
}

TEST_CASE("C10 Friction slows it across the ground as exp(-Friction t), and only there", "[player]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("on the ground, Friction 8") {
        PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
        rig.play();
        rig.seconds(1.0);
        REQUIRE_FALSE(c.set_velocity(Vec3{4.f, 0.f, 0.f}));
        rig.seconds(0.25);
        INFO(c.velocity().x);
        REQUIRE(near(c.velocity().x, 4.f * std::exp(-8.f * 0.25f), 0.05f));
    }

    SECTION("on the ground, Friction 0") {
        PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
        REQUIRE_FALSE(c.set_friction(0.0));
        rig.play();
        rig.seconds(1.0);
        REQUIRE_FALSE(c.set_velocity(Vec3{4.f, 0.f, 0.f}));
        rig.seconds(0.5);
        REQUIRE(near(c.velocity().x, 4.f, 0.01f));
    }

    SECTION("in the air, nothing") {
        PlayerController& c = rig.controller(at(0.f, 30.f, 0.f));
        REQUIRE_FALSE(c.set_velocity(Vec3{4.f, 0.f, 0.f}));
        rig.play();
        rig.seconds(0.5);
        REQUIRE(near(c.velocity().x, 4.f, 0.01f));
    }
}

TEST_CASE("C11 a moving platform carries it", "[player]") {
    PhysicsRig rig;
    PhysicsObject& floor = rig.floor();
    REQUIRE_FALSE(floor.set_friction(0.0));
    PhysicsObject& platform = rig.body(at(0.f, 0.25f, 0.f), Vec3{6.f, 0.5f, 6.f}, false);
    REQUIRE_FALSE(platform.set_friction(0.0));
    REQUIRE_FALSE(platform.set_mass(10000.0));
    PlayerController& c = rig.controller(at(0.f, 1.f, 0.f));
    rig.play();
    rig.seconds(1.0);
    REQUIRE_FALSE(platform.set_velocity(Vec3{2.f, 0.f, 0.f}));
    rig.seconds(1.5);
    INFO(c.velocity().x << " " << x_of(c.transform()) << " " << x_of(platform.transform()));
    REQUIRE(near(c.velocity().x, 2.f, 0.05f));
    // It caught up exponentially, so it trails by speed / Friction.
    REQUIRE(near(x_of(c.transform()) - x_of(platform.transform()), -2.f / 8.f, 0.05f));
    REQUIRE(c.on_ground());
}

TEST_CASE("C12 it slides along a wall it is pushed into", "[player]") {
    PhysicsRig rig;
    rig.floor();
    // A wall whose face is at x = 1.
    rig.body(at(1.5f, 2.f, 0.f), Vec3{1.f, 4.f, 40.f}, true);
    PlayerController& c = rig.controller(at(0.f, 0.5f, 0.f));
    rig.play();
    rig.seconds(1.0);
    walk(rig, c, 3.f, 3.f, 1.0);
    INFO(x_of(c.transform()) << " " << z_of(c.transform()));
    REQUIRE(x_of(c.transform()) <= 0.51f);
    REQUIRE(z_of(c.transform()) > 2.5f);
}

TEST_CASE("C13 it pushes down on what it stands on", "[player]") {
    PhysicsRig rig;
    rig.floor();
    // A plank balanced on a ridge along Z.
    rig.body(at(0.f, 0.25f, 0.f), Vec3{0.2f, 0.5f, 4.f}, true);
    PhysicsObject& plank = rig.body(at(0.f, 0.6f, 0.f), Vec3{6.f, 0.2f, 2.f}, false);
    REQUIRE_FALSE(plank.set_mass(10.0));
    PlayerController& c = rig.controller(at(2.5f, 1.2f, 0.f));
    REQUIRE_FALSE(c.set_mass(50.0));
    rig.play();
    rig.seconds(2.0);
    // The plank's +X axis tips down toward the controller's end.
    INFO(plank.transform().m[1]);
    REQUIRE(plank.transform().m[1] < -0.05f);
}

TEST_CASE("C15 one PlayerController stands on another and the stack settles", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PlayerController& below = rig.controller(at(0.f, 0.5f, 0.f));
    PlayerController& above = rig.controller(at(0.f, 4.f, 0.f));
    rig.play();
    rig.seconds(4.0);
    INFO(y_of(below.transform()) << " " << y_of(above.transform()));
    REQUIRE(std::isfinite(y_of(above.transform())));
    REQUIRE(near(y_of(below.transform()), 0.f, 0.03f));
    // The lower one's top is at 2; the upper one's feet hover on it.
    REQUIRE(near(y_of(above.transform()), 2.f, 0.05f));
    REQUIRE(above.on_ground());
    REQUIRE(near(above.velocity().y, 0.f, 0.05f));
}

TEST_CASE("C16 a heavy PlayerController on a light one settles", "[player]") {
    for (const double ratio : {10.0, 80.0}) {
        INFO("mass ratio " << ratio);
        PhysicsRig rig;
        rig.floor();
        PlayerController& below = rig.controller(at(0.f, 0.5f, 0.f));
        PlayerController& above = rig.controller(at(0.f, 3.f, 0.f));
        REQUIRE_FALSE(above.set_mass(ratio));
        rig.play();
        rig.seconds(4.0);
        float fastest = 0.f;
        for (int i = 0; i < 240; ++i) {
            rig.steps(1);
            fastest = std::max(fastest, std::fabs(above.velocity().y));
        }
        INFO(y_of(below.transform()) << " " << y_of(above.transform()) << " " << fastest);
        REQUIRE(std::isfinite(y_of(above.transform())));
        REQUIRE(fastest < 0.05f);
        // It stands on the lower one's head, which its weight presses down.
        REQUIRE(near(y_of(above.transform()) - y_of(below.transform()), 2.f, 0.05f));
        REQUIRE(y_of(below.transform()) > -0.45f);
        REQUIRE(above.on_ground());
    }
}

TEST_CASE("C17 walking across a light crate does not drag it along", "[player]") {
    PhysicsRig rig;
    rig.floor();
    PhysicsObject& crate = rig.body(at(0.f, 0.5f, 0.f), Vec3{6.f, 1.f, 6.f}, false);
    PlayerController& c = rig.controller(at(-2.f, 1.5f, 0.f));
    rig.play();
    rig.seconds(1.0);
    const float start = x_of(crate.transform());
    walk(rig, c, 4.f, 0.f, 0.8);
    INFO(x_of(c.transform()) << " " << x_of(crate.transform()) - start);
    REQUIRE(x_of(c.transform()) > 0.5f);
    REQUIRE(std::fabs(x_of(crate.transform()) - start) < 0.05f);
}

TEST_CASE("C18 landing stops the fall at once", "[player]") {
    PhysicsRig rig;
    rig.floor();

    SECTION("dropped from 3, it stops dead at its hover height on the step it lands") {
        PlayerController& c = rig.controller(at(0.f, 3.f, 0.f));
        rig.play();
        float lowest = 10.f;
        int landed = -1;
        for (int i = 0; i < 480; ++i) {
            rig.steps(1);
            lowest = std::min(lowest, y_of(c.transform()));
            if (landed < 0 && c.on_ground()) {
                landed = i;
                INFO("landed at step " << i << " y " << y_of(c.transform()) << " vy " << c.velocity().y);
                REQUIRE(std::fabs(c.velocity().y) < 0.1f);
                REQUIRE(near(y_of(c.transform()), 0.f, 0.01f));
            }
        }
        REQUIRE(landed > 0);
        INFO(lowest);
        REQUIRE(lowest > -0.01f);
    }

    SECTION("in the air above the ground it is not yet on it") {
        PlayerController& c = rig.controller(at(0.f, 0.3f, 0.f));
        rig.play();
        rig.steps(1);
        REQUIRE_FALSE(c.on_ground());
    }
}

TEST_CASE("C19 walking under a sloped overhang stops it there, not pushed into the ground", "[player]") {
    PhysicsRig rig;
    rig.floor();
    ramp(rig, 30.0);
    PlayerController& c = rig.controller(at(9.f, 0.5f, 0.f));
    rig.play();
    rig.seconds(0.5);
    float lowest = 10.f;
    for (int i = 0; i < 480; ++i) {
        REQUIRE_FALSE(c.set_velocity(Vec3{-4.f, c.velocity().y, 0.f}));
        rig.steps(1);
        lowest = std::min(lowest, y_of(c.transform()));
    }
    INFO(x_of(c.transform()) << " lowest " << lowest);
    REQUIRE(lowest > -0.02f);
    REQUIRE(x_of(c.transform()) > 5.6f);
    REQUIRE(c.on_ground());
}

namespace {

// Where the feet stand on a ramp(rig, degrees) at x: level with the ramp
// under the probe's ring at its uphill side, 0.95 of Radius 0.5 toward +X.
float on_ramp(double degrees, float x) {
    return (x + 0.475f) * static_cast<float>(std::tan(degrees * 3.14159265358979 / 180.0));
}

}  // namespace

TEST_CASE("C20 on slopes it stands on the ground, never sunk into it", "[player]") {
    PhysicsRig rig;

    SECTION("walking up a 30 degree slope keeps it at its hover height") {
        ramp(rig, 30.0);
        PlayerController& c = rig.controller(at(-6.f, on_ramp(30.0, -6.f) + 0.1f, 0.f));
        rig.play();
        rig.seconds(1.0);
        float worst = 0.f;
        for (int i = 0; i < 480; ++i) {
            REQUIRE_FALSE(c.set_velocity(Vec3{3.f, c.velocity().y, 0.f}));
            rig.steps(1);
            REQUIRE(c.on_ground());
            worst = std::min(worst, y_of(c.transform()) - on_ramp(30.0, x_of(c.transform())));
        }
        INFO(x_of(c.transform()) << " sunk " << worst);
        REQUIRE(x_of(c.transform()) > -1.f);
        REQUIRE(worst > -0.02f);
    }

    SECTION("sliding down a 60 degree slope keeps it at its hover height") {
        ramp(rig, 60.0);
        PlayerController& c = rig.controller(at(0.f, 1.f, 0.f));
        rig.play();
        int slid = 0;
        float worst = 0.f;
        for (int i = 0; i < 240; ++i) {
            rig.steps(1);
            if (c.is_sliding()) {
                ++slid;
                worst = std::min(worst, y_of(c.transform()) - on_ramp(60.0, x_of(c.transform())));
            }
        }
        INFO(x_of(c.transform()) << " sunk " << worst << " sliding steps " << slid);
        REQUIRE(slid > 120);
        REQUIRE(x_of(c.transform()) < -1.f);
        REQUIRE(worst > -0.02f);
    }

    SECTION("sent into a 60 degree slope, it rides up it only as far as its speed pays for") {
        ramp(rig, 60.0);
        PlayerController& c = rig.controller(at(-2.f, on_ramp(60.0, -2.f) + 0.1f, 0.f));
        rig.play();
        REQUIRE_FALSE(c.set_velocity(Vec3{4.f, 0.f, 0.f}));
        const float start = y_of(c.transform());
        float highest = -10.f;
        for (int i = 0; i < 480; ++i) {
            rig.steps(1);
            highest = std::max(highest, y_of(c.transform()));
        }
        INFO(start << " to " << highest);
        // Of 4 into the slope, 4 cos 60 is along it: that rises 0.2.
        REQUIRE(highest < start + 0.25f);
        REQUIRE(highest > start + 0.1f);
    }

    SECTION("sliding off a 60 degree slope onto a floor it lands on, not in") {
        ramp(rig, 60.0);
        rig.body(at(0.f, -6.5f, 0.f), Vec3{40.f, 1.f, 40.f}, true);
        PlayerController& c = rig.controller(at(0.f, 1.f, 0.f));
        rig.play();
        float lowest = 10.f;
        bool landed = false;
        for (int i = 0; i < 960; ++i) {
            rig.steps(1);
            landed = landed || c.on_ground();
            if (landed) {
                lowest = std::min(lowest, y_of(c.transform()));
            }
        }
        INFO(x_of(c.transform()) << " lowest " << lowest);
        REQUIRE(landed);
        REQUIRE(lowest > -6.02f);
    }
}

TEST_CASE("C21 walking along a step at a shallow angle climbs it without a bounce", "[player]") {
    // The step turned about Y by these, so its edge meets the probe's ring
    // square to a side, along one, and between.
    const float turns[] = {0.f, 11.25f, 5.f, 30.f};
    const float angles[] = {2.f, 4.f, 7.f, 11.f, 16.f, 25.f};
    for (float turn : turns) {
        const float yaw = turn * 3.14159265f / 180.f;
        // The step's own X and Z, in the world.
        const Vec3 along{std::cos(yaw), 0.f, -std::sin(yaw)};
        const Vec3 across{std::sin(yaw), 0.f, std::cos(yaw)};
        auto place = [&](float x, float y, float z) {
            Matrix4 where = engine_core::matrix4_axis_angle(Vec3{0.f, 1.f, 0.f}, yaw);
            where.m[12] = along.x * x + across.x * z;
            where.m[13] = y;
            where.m[14] = along.z * x + across.z * z;
            return where;
        };
        for (float degrees : angles) {
            PhysicsRig rig;
            rig.body(place(40.f, -0.5f, 0.f), Vec3{200.f, 1.f, 40.f}, true);
            // A step 0.3 tall, its edge along its X at its z = 1.
            rig.body(place(40.f, 0.15f, 3.f), Vec3{200.f, 0.3f, 4.f}, true);
            const Matrix4 start = place(-8.f, 0.5f, 0.f);
            PlayerController& c = rig.controller(at(start.m[12], 0.5f, start.m[14]));
            rig.play();
            rig.seconds(0.5);
            const float radians = degrees * 3.14159265f / 180.f;
            const float vx = 4.f * std::cos(radians);
            const float vz = 4.f * std::sin(radians);
            auto inward = [&] { return x_of(c.transform()) * across.x + z_of(c.transform()) * across.z; };
            float back = 0.f;
            float highest = 0.f;
            float z = inward();
            const int count = static_cast<int>(1.6f / vz / static_cast<float>(kStep));
            for (int i = 0; i < count; ++i) {
                REQUIRE_FALSE(c.set_velocity(Vec3{along.x * vx + across.x * vz, c.velocity().y,
                                                  along.z * vx + across.z * vz}));
                rig.steps(1);
                back = std::max(back, z - inward());
                z = inward();
                highest = std::max(highest, y_of(c.transform()));
            }
            INFO(turn << " turn, " << degrees << " degrees: back " << back << " highest " << highest << " in "
                      << inward() << " y " << y_of(c.transform()));
            CHECK(back < 1e-3f);
            CHECK(highest < 0.32f);
            CHECK(near(y_of(c.transform()), 0.3f, 0.01f));
        }
    }
}

TEST_CASE("C22 walking off the side of a slope it falls, no faster than gravity", "[player]") {
    const float headings[] = {0.f, 20.f, -20.f, 45.f, -45.f};
    for (float degrees : headings) {
        PhysicsRig rig;
        ramp(rig, 30.0);
        // A floor 3 below the ramp's top, which the ramp's side at x = -3 stands 1.27 over.
        rig.body(at(0.f, -3.5f, 0.f), Vec3{60.f, 1.f, 60.f}, true);
        PlayerController& c = rig.controller(at(-3.f, on_ramp(30.0, -3.f) + 0.05f, 2.f));
        rig.play();
        rig.seconds(0.5);
        const float radians = degrees * 3.14159265f / 180.f;
        float fastest = 0.f;
        float highest = y_of(c.transform());
        for (int i = 0; i < 600; ++i) {
            REQUIRE_FALSE(c.set_velocity(Vec3{4.f * std::sin(radians), c.velocity().y, 4.f * std::cos(radians)}));
            rig.steps(1);
            fastest = std::max(fastest, -c.velocity().y);
            highest = std::max(highest, y_of(c.transform()));
        }
        // Falling from its highest to the floor, gravity takes it to this,
        // with a little over for its speed down the ramp as it leaves.
        const float fall = std::sqrt(2.f * 9.81f * (highest + 3.f)) + 0.5f;
        INFO(degrees << " degrees: fastest down " << fastest << " of " << fall << ", ends at " << x_of(c.transform())
                     << " " << y_of(c.transform()) << " " << z_of(c.transform()));
        CHECK(fastest < fall);
        CHECK(near(y_of(c.transform()), -3.f, 0.01f));
    }
}

TEST_CASE("C23 stepping down a step it drops smoothly, still on ground", "[player]") {
    PhysicsRig rig;
    rig.floor();
    rig.body(at(-3.f, 0.15f, 0.f), Vec3{6.f, 0.3f, 6.f}, true);
    PlayerController& c = rig.controller(at(-1.f, 0.6f, 0.f));
    rig.play();
    rig.seconds(1.0);
    REQUIRE(near(y_of(c.transform()), 0.3f, 0.01f));
    bool always = true;
    float fastest = 0.f;
    float lowest = 10.f;
    float y = y_of(c.transform());
    for (int i = 0; i < 480; ++i) {
        REQUIRE_FALSE(c.set_velocity(Vec3{3.f, c.velocity().y, 0.f}));
        rig.steps(1);
        always = always && c.on_ground();
        fastest = std::max(fastest, (y - y_of(c.transform())) / static_cast<float>(kStep));
        y = y_of(c.transform());
        lowest = std::min(lowest, y);
    }
    INFO("fastest down " << fastest << " lowest " << lowest << " y " << y_of(c.transform()));
    REQUIRE(always);
    // Gravity alone, over 0.3, reaches 2.43.
    REQUIRE(fastest < 2.6f);
    REQUIRE(fastest > 0.5f);
    REQUIRE(lowest > -0.005f);
    REQUIRE(near(y_of(c.transform()), 0.f, 0.005f));
}

namespace {

// The playground's purple ramp: 8 long, 0.5 thick, 4 wide, 55 degrees up
// toward +X, its top face meeting the floor at x = 4.
void steep_ramp(PhysicsRig& rig) {
    Matrix4 where = engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 55.0 * 3.14159265358979 / 180.0);
    where.m[12] = 6.4990935f;
    where.m[13] = 3.133214f;
    where.m[14] = 12.f;
    rig.body(where, Vec3{8.f, 0.5f, 4.f}, true);
}

// As the playground's script drives it: on ground its speed across is what
// is asked, and it jumps once its feet pass jump_x; in the air it steers
// toward what is asked; surfing, it is left alone, so only the slope moves
// it. Gives its highest feet.
float drive(PhysicsRig& rig, PlayerController& c, Vec3 wish, float jump_x, double time) {
    float highest = y_of(c.transform());
    bool jumped = false;
    const int count = static_cast<int>(time / kStep + 0.5);
    for (int i = 0; i < count; ++i) {
        const Vec3 v = c.velocity();
        if (c.on_ground()) {
            float up = v.y;
            if (!jumped && x_of(c.transform()) >= jump_x) {
                jumped = true;
                up = 6.5f;
            }
            REQUIRE_FALSE(c.set_velocity(Vec3{wish.x, up, wish.z}));
        } else if (!c.is_sliding()) {
            const float k = std::min(1.f, 4.f * static_cast<float>(kStep));
            REQUIRE_FALSE(c.set_velocity(Vec3{v.x + (wish.x - v.x) * k, v.y, v.z + (wish.z - v.z) * k}));
        }
        rig.steps(1);
        highest = std::max(highest, y_of(c.transform()));
    }
    return highest;
}

}  // namespace

TEST_CASE("C24 walking into the foot of a steep slope it surfs up it, as far as its speed pays for", "[player]") {
    const float speeds[] = {6.f, 11.f};
    const float headings[] = {0.f, 30.f, -30.f, 60.f, -60.f};
    const float lanes[] = {12.f, 10.1f, 13.9f, 9.6f, 14.4f};
    const float along = std::cos(55.f * 3.14159265f / 180.f);
    for (float speed : speeds) {
        for (float degrees : headings) {
            for (float lane : lanes) {
                PhysicsRig rig;
                rig.body(at(0.f, -0.5f, 0.f), Vec3{80.f, 1.f, 80.f}, true);
                steep_ramp(rig);
                const float radians = degrees * 3.14159265f / 180.f;
                const Vec3 wish{speed * std::cos(radians), 0.f, speed * std::sin(radians)};
                // Start 2.5 back from the foot along the way it walks.
                PlayerController& c = rig.controller(at(4.f - 2.5f * std::cos(radians), 0.f, lane - 2.5f * std::sin(radians)));
                rig.play();
                rig.seconds(0.2);
                const float highest = drive(rig, c, wish, 1e9f, 2.0);
                INFO(speed << " speed, " << degrees << " degrees, lane " << lane << ": highest " << highest
                           << " ends at " << x_of(c.transform()) << " " << y_of(c.transform()) << " "
                           << z_of(c.transform()));
                // It gains no energy: no higher than all of its speed would
                // rise. A little more for the hover's corner at the foot.
                CHECK(highest < speed * speed / (2.f * 9.81f) + 0.1f);
                if (lane > 10.f && lane < 14.f) {
                    // Onto the slope's face, of its speed up the fall line the
                    // part along the slope is left: that rises u^2 / 2g. Over
                    // the side's edge, whose normal leans, some speed across
                    // turns up it too.
                    const float u = wish.x * along;
                    CHECK(highest < u * u / (2.f * 9.81f) + 0.1f);
                }
                if (speed == 11.f && degrees == 0.f && lane == 12.f) {
                    // Running straight in, it goes well up the slope.
                    CHECK(highest > 1.f);
                }
            }
        }
    }
}

TEST_CASE("C25 jumping into a steep slope it surfs up it and gains no energy", "[player]") {
    const float speeds[] = {6.f, 11.f};
    const float jumps[] = {2.4f, 2.8f, 3.2f, 3.5f};
    for (float speed : speeds) {
        for (float jump_x : jumps) {
            PhysicsRig rig;
            rig.body(at(0.f, -0.5f, 0.f), Vec3{80.f, 1.f, 80.f}, true);
            steep_ramp(rig);
            PlayerController& c = rig.controller(at(-1.f, 0.f, 12.f));
            rig.play();
            rig.seconds(0.2);
            // Runs at speed, jumps once its feet pass jump_x, and is left alone
            // after. Its energy per mass, speed and height, from the jump on.
            bool jumped = false;
            float start = 0.f;
            float most = 0.f;
            float highest = 0.f;
            bool surfed = false;
            const int count = static_cast<int>(6.0 / kStep + 0.5);
            for (int i = 0; i < count; ++i) {
                if (!jumped && c.on_ground()) {
                    float up = c.velocity().y;
                    if (x_of(c.transform()) >= jump_x) {
                        jumped = true;
                        up = 6.5f;
                        start = 0.5f * (speed * speed + up * up) + 9.81f * y_of(c.transform());
                    }
                    REQUIRE_FALSE(c.set_velocity(Vec3{speed, up, 0.f}));
                }
                rig.steps(1);
                if (jumped) {
                    const Vec3 v = c.velocity();
                    most = std::max(most, 0.5f * (v.x * v.x + v.y * v.y + v.z * v.z) + 9.81f * y_of(c.transform()));
                    highest = std::max(highest, y_of(c.transform()));
                    surfed = surfed || c.is_sliding();
                }
            }
            INFO(speed << " speed, jump at " << jump_x << ": energy " << start << " to at most " << most
                       << ", highest " << highest << " ends at " << x_of(c.transform()) << " "
                       << y_of(c.transform()));
            REQUIRE(jumped);
            CHECK(surfed);
            // The hover's lift off the slope may add a little height.
            CHECK(most < start * 1.03f);
            if (speed == 11.f) {
                // Its run carries it higher than the jump alone, 2.15.
                CHECK(highest > 2.4f);
            }
            // It slides back down to the floor, or goes over the top onto the
            // slope's end, which is gentle enough to stand on.
            CHECK(c.on_ground());
        }
    }
}

TEST_CASE("C26 jumping just before a steep slope, its feet stay out of it", "[player]") {
    const float speeds[] = {6.f, 11.f};
    // The slope's face: up toward +X at 55 degrees from the foot at x = 4.
    const float s = std::sin(55.f * 3.14159265f / 180.f);
    const float k = std::cos(55.f * 3.14159265f / 180.f);
    for (float speed : speeds) {
        for (float jump_x = 2.8f; jump_x <= 3.45f; jump_x += 0.05f) {
            PhysicsRig rig;
            rig.body(at(0.f, -0.5f, 0.f), Vec3{80.f, 1.f, 80.f}, true);
            steep_ramp(rig);
            PlayerController& c = rig.controller(at(-1.f, 0.f, 12.f));
            rig.play();
            rig.seconds(0.2);
            const float r = static_cast<float>(c.radius());
            // As drive() does, and after the jump, the deepest its feet's
            // forward edge goes into the face, while over it: the face ends
            // 8 along, at x = 4 + 8 cos 55.
            bool jumped = false;
            float deepest = -1.f;
            const int count = static_cast<int>(2.0 / kStep + 0.5);
            for (int i = 0; i < count; ++i) {
                const Vec3 v = c.velocity();
                if (c.on_ground()) {
                    float up = v.y;
                    if (!jumped && x_of(c.transform()) >= jump_x) {
                        jumped = true;
                        up = 6.5f;
                    }
                    REQUIRE_FALSE(c.set_velocity(Vec3{speed, up, 0.f}));
                } else if (!c.is_sliding()) {
                    const float steer = std::min(1.f, 4.f * static_cast<float>(kStep));
                    REQUIRE_FALSE(c.set_velocity(Vec3{v.x + (speed - v.x) * steer, v.y, v.z}));
                }
                rig.steps(1);
                if (jumped && x_of(c.transform()) + r < 4.f + 8.f * k) {
                    deepest = std::max(deepest, s * (x_of(c.transform()) + r - 4.f) - k * y_of(c.transform()));
                }
            }
            INFO(speed << " speed, jump at " << jump_x << ": feet " << deepest << " into the slope");
            REQUIRE(jumped);
            // Unjumped, walking up to the foot, it stands as close: the probe
            // looks a little inside the cylinder's edge. The gap under the
            // cylinder would take its feet 0.23 in.
            CHECK(deepest < 0.05f);
        }
    }
}
