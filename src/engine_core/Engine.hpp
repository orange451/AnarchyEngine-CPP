#pragma once

#include "DataModel.hpp"
#include "IClock.hpp"
#include "IRenderer.hpp"
#include "SnapshotPump.hpp"
#include "TaskScheduler.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace engine_core {

// Two loops. SimulationThread steps the DataModel. RenderThread prepares a
// snapshot under a short write lock, then Perform/Present with the lock down.
// PostRender runs after Present, still on RenderThread, without the lock.
class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void set_renderer(IRenderer* renderer);
    void set_clock(IClock* clock);
    void set_timing(double render_dt, double physics_dt);
    // 0 runs as fast as the machine allows. The IDE runner passes 60.
    void set_pace_hz(double hz);

    DataModel& datamodel() { return model_; }
    SnapshotPump& pump() { return pump_; }
    TaskScheduler& scheduler() { return scheduler_; }

    void start();
    void stop();

    // start() launches the threads and leaves the simulation paused.
    // resume() lets steps run. pause() stops them again. The render thread
    // keeps presenting the last snapshot either way.
    void resume();
    void pause();
    bool paused() const;

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
    void pace(double hz_anchor_seconds) const;

    DataModel model_;
    SnapshotPump pump_;
    TaskScheduler scheduler_;
    IRenderer* renderer_ = nullptr;
    IClock* clock_ = nullptr;
    double render_dt_ = 1.0 / 60.0;
    double physics_dt_ = 1.0 / 240.0;
    double pace_hz_ = 0;

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
    // The UI thread takes it only to flip the flag. Hold is a store.
    mutable std::mutex pause_mu_;
    std::condition_variable pause_cv_;
    bool paused_ = true;
};

}  // namespace engine_core
