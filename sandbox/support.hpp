#pragma once

// Helpers the sandbox test files share.

#include "DataModel.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "types.hpp"

#include <atomic>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <thread>

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
            runtime.step_tools(dt);
            game.events().drain();
        }
    }

    // One rendered frame: RenderStepped run on a thread of its own in the render
    // role, inside the prerender window, as the engine's render thread runs it.
    void render(double dt = 1.0 / 60.0) {
        std::thread render_thread([&] {
            engine_core::set_thread_role(engine_core::ThreadRole::Render);
            game.set_prerender_window(true);
            scheduler.run_phase(engine_core::Phase::RenderStepped, dt);
            game.set_prerender_window(false);
            engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
        });
        render_thread.join();
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

// A Script directly under Workspace, where scripts run, beside the parts a test adds there.
inline engine_core::Script& add_script(engine_core::DataModel& game, const char* name, const char* source) {
    return add_script(game, game.scene_service("Workspace"), name, source);
}

// A plain instance that steps on Heartbeat, turning 90 degrees a simulation
// second, so a test can see which instances step. No engine class steps.
class Spinner : public engine_core::DataModel {
public:
    Spinner(engine_core::DataModel::ChildTag tag, engine_core::DataModel::State& state, engine_core::InstanceId id)
        : DataModel(tag, state, id) {}

    const char* class_name() const override { return "Spinner"; }
    bool steps() const override { return true; }
    void step(double dt) override { degrees_.store(degrees_.load() + dt * 90.0); }
    // A dead id reads as 0. Another thread may read it while Heartbeat steps.
    double degrees() const { return alive(id()) ? degrees_.load() : 0.0; }

protected:
    void on_release() override { degrees_.store(0.0); }
    void on_reuse() override { degrees_.store(0.0); }

private:
    std::atomic<double> degrees_{0.0};
};

// Where tests put instances that would sit under game: the Workspace service.
inline engine_core::InstanceId workspace_of(const engine_core::DataModel& game) {
    return game.scene_service("Workspace");
}

// A GameObject under Workspace, so the render snapshot has a row for it.
inline engine_core::GameObject& create_part(engine_core::DataModel& game) {
    engine_core::GameObject& object = game.create_game_object();
    game.set_parent(object.id(), workspace_of(game));
    return object;
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
