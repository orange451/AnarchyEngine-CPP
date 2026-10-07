#pragma once

// The physics test rig: a Game, a physics world stepped by hand at the
// engine's 240 Hz, and the warnings it gave.

#include "support.hpp"

#include "PhysicsObject.hpp"
#include "PhysicsWorld.hpp"
#include "PlayerController.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace physics_rig {

using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::PhysicsObject;
using engine_core::PlayerController;
using engine_core::Vec3;

constexpr double kStep = 1.0 / 240.0;

inline Matrix4 at(float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); }

inline float x_of(const Matrix4& m) { return m.m[12]; }
inline float y_of(const Matrix4& m) { return m.m[13]; }
inline float z_of(const Matrix4& m) { return m.m[14]; }

inline bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

inline float column_length(const Matrix4& m, int column) {
    const float* axis = m.m + column * 4;
    return std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
}

struct PhysicsRig {
    SimRole role;
    engine_core::Game game;
    engine_core::PhysicsWorld physics;
    std::vector<std::string> warnings;

    PhysicsRig() {
        physics.set_warning_sink([this](const std::string& text) { warnings.push_back(text); });
    }

    PhysicsObject& body(Matrix4 where, Vec3 size, bool anchored, InstanceId parent = 0) {
        PhysicsObject& object = game.create<PhysicsObject>();
        REQUIRE_FALSE(object.set_transform(where));
        REQUIRE_FALSE(object.set_size(size));
        object.set_anchored(anchored);
        game.set_parent(object.id(), parent != 0 ? parent : workspace_of(game));
        return object;
    }

    // A wide anchored floor whose top is at y = 0.
    PhysicsObject& floor() { return body(at(0.f, -0.5f, 0.f), Vec3{40.f, 1.f, 40.f}, true); }

    // A PlayerController with its defaults, its feet at where.
    PlayerController& controller(Matrix4 where, InstanceId parent = 0) {
        PlayerController& object = game.create<PlayerController>();
        REQUIRE_FALSE(object.set_transform(where));
        game.set_parent(object.id(), parent != 0 ? parent : workspace_of(game));
        return object;
    }

    void play() {
        game.capture_place();
        game.start_simulation();
    }

    void steps(int count) {
        for (int i = 0; i < count; ++i) {
            physics.step(game, kStep);
        }
    }

    void seconds(double time) { steps(static_cast<int>(time / kStep + 0.5)); }

    // What the Engine's stopped tick does: bodies follow the tree, nothing moves.
    void sync_steps(int count) {
        for (int i = 0; i < count; ++i) {
            physics.sync(game);
        }
    }
};

}  // namespace physics_rig
