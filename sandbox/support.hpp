#pragma once

// Helpers the sandbox test files share.

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "MeshEditors.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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

// This process's id, for temp folders two sandbox runs at once must not share.
inline std::string process_id() {
#ifdef _WIN32
    return std::to_string(_getpid());
#else
    return std::to_string(getpid());
#endif
}

// Lets fill change mesh's geometry at once, through its MeshEditor: stopped,
// into its file (which the Mesh draws again); playing, into the session's
// copy. Returns why it could not, as Mesh::edit_geometry did before MeshEditor.
inline std::optional<std::string> edit_geometry(engine_core::DataModel& game, engine_core::Mesh& mesh,
                                                const std::function<void(anarchy::amesh::Data&)>& fill) {
    std::string error;
    const std::shared_ptr<engine_core::MeshEditor> editor = game.mesh_editors().open(game, mesh, error);
    if (editor == nullptr) {
        return error;
    }
    anarchy::amesh::Data data = editor->build();
    fill(data);
    editor->clear();
    editor->add_shape([&data](anarchy::amesh::Data& out) { out = data; });
    if (game.simulation_running()) {
        game.mesh_editors().commit(game);
    } else {
        game.mesh_editors().write_all(game, true);
    }
    return std::nullopt;
}

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

    // The time frames gives MeshEditors::step, which moves dt a frame from 0.
    engine_core::MeshEditors::Clock::time_point clock{};

    void frames(int count, double dt = 1.0 / 60.0) {
        for (int i = 0; i < count; ++i) {
            scheduler.run_phase(engine_core::Phase::Heartbeat, dt);
            game.events().drain();
            runtime.heartbeat(dt);
            runtime.step_tools(dt);
            game.events().drain();
            // After the scripts and tools, as Engine steps it.
            game.mesh_editors().step(game, clock);
            clock += std::chrono::duration_cast<engine_core::MeshEditors::Clock::duration>(
                std::chrono::duration<double>(dt));
        }
    }

    // One rendered frame's window, as Engine::run_window runs it, on this
    // thread as the engine thread with threads running, so authorize and every
    // engine-thread guard see a real window. It holds the write lock across the
    // window, opens it, runs RenderStepped, closes it, and takes its deferred
    // violation as the engine does after the lock, counting it in
    // render_violations. The engine thread and threads_running go back to what
    // they were after.
    void render(double dt = 1.0 / 60.0) {
        const std::thread::id saved_engine = game.simulation_thread_id();
        const bool saved_running = game.threads_running();
        bool violated = false;
        const char* reason = nullptr;
        game.set_engine_thread(std::this_thread::get_id());
        game.set_threads_running(true);
        {
            engine_core::DataModelLock lock(game, engine_core::DataModelLock::Write);
            game.set_prerender_window(true);
            scheduler.run_phase(engine_core::Phase::RenderStepped, dt);
            game.set_prerender_window(false);
        }
        violated = game.take_deferred_violation(&reason);
        game.set_threads_running(saved_running);
        game.set_engine_thread(saved_engine);
        if (violated) {
            ++render_violations;
            last_render_violation = reason != nullptr ? reason : "";
        }
    }

    // Refused writes the window deferred, one per render() call at most,
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

// A test's stand-in for an IDE command: the writes between begin_step and
// end_step are one undo step. A write outside them is not undoable.
inline void begin_step(engine_core::DataModel& game, std::string name = "Edit") {
    REQUIRE(game.history().try_begin_recording(std::move(name)).has_value());
}

// Commits the step begin_step opened.
inline void end_step(engine_core::DataModel& game) { game.history().seal_edit_recording(); }

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
