#pragma once

#include "AudioWorld.hpp"
#include "DataModel.hpp"
#include "Game.hpp"
#include "IClock.hpp"
#include "IRenderer.hpp"
#include "PhysicsWorld.hpp"
#include "SnapshotPump.hpp"
#include "TaskScheduler.hpp"

#include <memory>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace engine_core {

// RenderStepped's dt in the render loop. A frame that misses the 2 ms Prepare
// lock runs no RenderStepped, so its time carries to the next frame that does:
// dt-integrated motion keeps pace under load. Every pass adds its time; a
// prepared pass takes the sum, clamped at kMaxDt as one frame's dt always was.
// RenderThread only.
struct RenderStepTime {
    static constexpr double kMaxDt = 0.1;
    double pending = 0;

    void add(double seconds) {
        if (seconds > 0) {
            pending += seconds;
        }
    }
    // The carried time, or fallback when there is none. Starts the next sum.
    double take(double fallback) {
        const double dt = pending > kMaxDt ? kMaxDt : pending;
        pending = 0;
        return dt > 0.0 ? dt : fallback;
    }
};

// Two loops. SimulationThread steps the DataModel. RenderThread prepares a
// snapshot under a short write lock, then Perform/Present with the lock down.
// PostRender runs after Present, still on RenderThread, without the lock.
class ScriptAnalysis;
class ScriptRuntime;

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void set_renderer(IRenderer* renderer);
    void set_clock(IClock* clock);
    void set_timing(double render_dt, double physics_dt);
    // 0 runs as fast as the machine allows. set_pace_hz sets both loops.
    // The IDE paces the simulation at 60 and leaves the render loop uncapped.
    void set_pace_hz(double hz);
    void set_simulation_pace_hz(double hz);
    void set_render_pace_hz(double hz);
    // An uncapped render loop normally spins. With this set, it waits for
    // note_client_frame instead, so it stays with the window that is actually drawing.
    void set_render_client_sync(bool enabled);
    void note_client_frame();

    DataModel& datamodel() { return game_; }
    SnapshotPump& pump() { return pump_; }
    TaskScheduler& scheduler() { return scheduler_; }
    ScriptRuntime& scripts();
    ScriptAnalysis& analysis();
    const ScriptAnalysis& analysis() const;

    void start();
    void stop();

    // start() launches the threads and leaves the simulation paused.
    // resume() lets steps run. pause() stops them again. The render thread
    // keeps presenting the last snapshot either way.
    void resume();
    void pause();
    bool paused() const;

    // Runs fn on the DataModel from SimulationThread.
    // While the simulation is paused, the caller runs it under the write lock
    // so the change is visible before the next Test.
    void on_simulation(std::function<void(DataModel&)> fn);

    std::thread::id simulation_thread_id() const { return simulation_id_; }
    std::thread::id render_thread_id() const { return render_id_; }
    std::uint64_t present_count() const { return present_count_.load(); }
    std::uint64_t sim_frame_count() const { return sim_frames_.load(); }
    int last_substep_count() const { return last_substeps_.load(); }
    std::uint64_t published_frame() const { return pump_.published_frame(); }
    std::uint64_t last_contract_prepare_ns() const { return contract_prepare_ns_.load(); }
    std::uint64_t contract_count() const { return contract_count_.load(); }

private:
    void simulation_loop();
    void render_loop();
    void step_physics(double dt);
    void drain_edits();
    // Runs one step of a loop. A contract failure goes to on_contract. Any other
    // exception is reported to stderr and the console, and the loop goes on.
    template <typename Step, typename OnContract>
    void guarded_step(Step&& step, OnContract&& on_contract);
    void report_fault(const char* what);

    Game game_;
    SnapshotPump pump_;
    TaskScheduler scheduler_;
    std::unique_ptr<ScriptRuntime> scripts_;
    // PhysicsObject bodies, stepped in step_physics while the place plays.
    PhysicsWorld physics_;
    // SoundEmitter voices, stepped after the scripts each frame while the place plays.
    AudioWorld audio_;
    IRenderer* renderer_ = nullptr;
    IClock* clock_ = nullptr;
    double render_dt_ = 1.0 / 60.0;
    double physics_dt_ = 1.0 / 240.0;
    double simulation_pace_hz_ = 0;
    double render_pace_hz_ = 0;
    std::atomic<bool> render_client_sync_{false};
    // note_client_frame bumps client_frames_. The uncapped render loop waits on it
    // when render_client_sync_ is set. Tests leave the sync off.
    std::mutex client_frame_mu_;
    std::condition_variable client_frame_cv_;
    std::uint64_t client_frames_ = 0;

    std::thread simulation_;
    std::thread render_;
    std::thread::id simulation_id_{};
    std::thread::id render_id_{};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> present_count_{0};
    std::atomic<std::uint64_t> sim_frames_{0};
    std::atomic<int> last_substeps_{0};
    std::atomic<std::uint64_t> contract_prepare_ns_{0};
    std::atomic<std::uint64_t> contract_count_{0};

    std::mutex start_mu_;
    std::condition_variable start_cv_;
    bool simulation_ready_ = false;
    bool render_ready_ = false;
    bool start_release_ = false;

    // Guards paused_. SimulationThread waits on pause_cv_ while paused.
    // The UI thread takes it only to flip the flag or to publish a paused edit.
    // Hold is a store, or one paused edit.
    mutable std::mutex pause_mu_;
    std::condition_variable pause_cv_;
    bool paused_ = true;

    // Edits posted while the simulation is running. Drained on SimulationThread
    // at the start of the next step, under the write lock.
    std::mutex edit_mu_;
    std::vector<std::function<void(DataModel&)>> edits_;

    // When each recent fault was last reported.
    std::mutex fault_mu_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> recent_faults_;

    // Declared last so it is destroyed before the DataModel, after stop() joins
    // the simulation and render threads.
    std::unique_ptr<ScriptAnalysis> analysis_;
};

}  // namespace engine_core
