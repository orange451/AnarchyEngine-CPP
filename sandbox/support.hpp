#pragma once

// Helpers the sandbox test files share.

#include "DataModel.hpp"
#include "Game.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "types.hpp"

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

// A fresh directory under the system temp dir, removed at the end of the test.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        std::random_device device;
        path = std::filesystem::temp_directory_path() /
               ("ae-project-" + std::to_string(device()) + std::to_string(device()));
        std::filesystem::remove_all(path);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path operator/(const char* child) const { return path / child; }
};

// Marks this thread as SimulationThread while it lives.
struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

// A game with a script runtime and no engine threads. frames steps Heartbeat by hand.
struct ScriptRig {
    SimRole role;
    engine_core::Game game;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;

    ScriptRig() {
        scheduler.reserve(16);
        game.attach_scheduler(&scheduler);
        runtime.attach(game, scheduler);
    }

    void frames(int count, double dt = 1.0 / 60.0) {
        for (int i = 0; i < count; ++i) {
            scheduler.run_phase(engine_core::Phase::Heartbeat, dt);
            game.events().drain();
            runtime.heartbeat(dt);
            game.events().drain();
        }
    }
};

inline engine_core::Script& add_script(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name,
                                       const char* source) {
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), name);
    script.set_source(source);
    game.set_parent(script.id(), parent);
    return script;
}

// A Script directly under the game.
inline engine_core::Script& add_script(engine_core::DataModel& game, const char* name, const char* source) {
    return add_script(game, game.id(), name, source);
}

// Opaque: alpha is 1.
inline engine_core::ColorRgb rgb(float r, float g, float b) {
    engine_core::ColorRgb color;
    color.r = r;
    color.g = g;
    color.b = b;
    color.a = 1.f;
    return color;
}
