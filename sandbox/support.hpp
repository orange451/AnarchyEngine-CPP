#pragma once

// Helpers the sandbox test files share.

#include "DataModel.hpp"
#include "DataModelLock.hpp"
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

    // One rendered frame, as Engine::render_loop runs it (Engine.cpp, the Prepare
    // block): a thread of its own in the render role, registered as the render
    // thread with threads running and this thread as the simulation thread, so
    // authorize and every SimulationThread guard see the real render thread. It
    // holds the write lock across the window, opens it, runs RenderStepped,
    // closes it, and takes its deferred violation as the engine does after the
    // lock, counting it in render_violations. The thread ids and threads_running
    // go back to what they were after.
    void render(double dt = 1.0 / 60.0) {
        const std::thread::id saved_sim = game.simulation_thread_id();
        const std::thread::id saved_render = game.render_thread_id();
        const bool saved_running = game.threads_running();
        const std::thread::id sim = std::this_thread::get_id();
        bool violated = false;
        const char* reason = nullptr;
        std::thread render_thread([&] {
            engine_core::set_thread_role(engine_core::ThreadRole::Render);
            game.set_thread_ids(sim, std::this_thread::get_id());
            game.set_threads_running(true);
            {
                engine_core::DataModelLock lock(game, engine_core::DataModelLock::Write);
                game.set_prerender_window(true);
                scheduler.run_phase(engine_core::Phase::RenderStepped, dt);
                game.set_prerender_window(false);
            }
            violated = game.take_deferred_violation(&reason);
            game.set_threads_running(saved_running);
            game.set_thread_ids(saved_sim, saved_render);
            engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
        });
        render_thread.join();
        if (violated) {
            ++render_violations;
            last_render_violation = reason != nullptr ? reason : "";
        }
    }

    // Refused writes the render thread deferred, one per render() call at most,
    // as the engine counts them, and the last one's reason.
    int render_violations = 0;
    std::string last_render_violation;
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
