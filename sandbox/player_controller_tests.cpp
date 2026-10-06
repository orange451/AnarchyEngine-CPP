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

TEST_CASE("C18 landing stops the fall at once; only stepping up is smoothed", "[player]") {
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

    SECTION("walking off a 0.3 step it keeps to the ground and drops at once") {
        rig.body(at(-3.f, 0.15f, 0.f), Vec3{6.f, 0.3f, 6.f}, true);
        PlayerController& c = rig.controller(at(-1.f, 0.6f, 0.f));
        rig.play();
        rig.seconds(1.0);
        REQUIRE(near(y_of(c.transform()), 0.3f, 0.01f));
        bool always = true;
        float x_down = 0.f;
        for (int i = 0; i < 240; ++i) {
            REQUIRE_FALSE(c.set_velocity(Vec3{3.f, c.velocity().y, 0.f}));
            rig.steps(1);
            always = always && c.on_ground();
            if (x_down == 0.f && y_of(c.transform()) < 0.01f) {
                x_down = x_of(c.transform());
            }
        }
        INFO(x_down);
        REQUIRE(always);
        // Down once its probe's ring, 0.95 of Radius, is past the edge at x = 0.
        REQUIRE(x_down > 0.45f);
        REQUIRE(x_down < 0.55f);
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
