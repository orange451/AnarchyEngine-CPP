#pragma once

#include "AudioWorld.hpp"
#include "ParticleWorld.hpp"
#include "ConvexDecomposition.hpp"
#include "DataModel.hpp"
#include "Game.hpp"
#include "IClock.hpp"
#include "INetworkPump.hpp"
#include "IRenderer.hpp"
#include "PhysicsWorld.hpp"
#include "SnapshotPump.hpp"
#include "TaskScheduler.hpp"
#include "TerrainTextures.hpp"
#include "TerrainWorld.hpp"

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

// One engine thread runs each frame under one DataModel write lock: the sim
// step (or, paused, the stopped tick), then the render window (RenderStepped,
// PreRender) and the snapshot copy. Perform and Present follow with the lock
// down. The UI thread draws from the published snapshot.
class ScriptAnalysis;
class ScriptRuntime;

// How an Engine is built. A headless server turns the audio device off.
struct EngineOptions {
    // False: SoundEmitters still step and mix offline, but no device opens and
    // nothing is heard.
    bool audio_device = true;
};

class Engine {
public:
    Engine();
    // An Engine built to the options, such as a headless server with no audio device.
    explicit Engine(const EngineOptions& options);
    ~Engine();
    bool has_audio_device() const { return audio_device_; }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void set_renderer(IRenderer* renderer);
    void set_clock(IClock* clock);
    // A server or client session, pumped at each step's ends. Null to stop.
    // Set before start(), or on the engine thread between pumps (a join that
    // starts or ends during play); the engine does not own it.
    void set_network(INetworkPump* pump);
    // The clock a session's now is on, seconds on the steady clock: the engine
    // pumps a session with it, and whoever connects one starts it on the same.
    static double network_now();
    void set_timing(double render_dt, double physics_dt);
    // Frames a second. 0 runs as fast as the machine allows, or, with client
    // sync, once per paint. The IDE leaves it at 0 and syncs to the window.
    void set_pace_hz(double hz);
    // An unpaced loop normally spins. With this set, it waits for
    // note_client_frame instead, one frame per paint, so it runs as often as
    // the window draws. Its steps then take the measured time as dt, not
    // render_dt. A window that stops painting still gets about 60 frames a second.
    void set_client_sync(bool enabled);
    void note_client_frame();
    // One frame on the calling thread: the sim step (or, paused, the stopped
    // tick), then the window and the copy, then Perform and Present. wall is
    // the time since the last frame. engine_loop calls it after its wait;
    // tests call it on an engine that was never started.
    void run_frame(double wall);

    DataModel& datamodel() { return game_; }
    // Every ParticleEmitter's simulation; engine thread, or a test with the engine stopped.
    const ParticleWorld& particles() const { return particles_; }
    SnapshotPump& pump() { return pump_; }
    TaskScheduler& scheduler() { return scheduler_; }
    // For PhysicsWorld's terrain bodies (set_terrain_world) and the snapshot
    // pump's resolve_terrains.
    TerrainWorld& terrain_world() { return terrain_; }
    // Task 5: the Configure Terrain tab's "N MB textures" figure
    // (TerrainTextures::memory_bytes) and its revision.
    TerrainTextures& terrain_textures() { return textures_; }
    ScriptRuntime& scripts();
    ScriptAnalysis& analysis();
    const ScriptAnalysis& analysis() const;

    void start();
    void stop();

    // start() launches the engine thread and leaves the simulation paused.
    // resume() lets steps run. pause() stops them again. Paused, each frame
    // still runs the window and publishes a snapshot.
    void resume();
    void pause();
    bool paused() const;

    // Runs fn on the DataModel from the engine thread.
    // While the simulation is paused, the caller runs it under the write lock
    // so the change is visible before the next Test.
    void on_simulation(std::function<void(DataModel&)> fn);

    // The engine thread.
    std::thread::id simulation_thread_id() const { return simulation_id_; }
    std::uint64_t present_count() const { return present_count_.load(); }
    std::uint64_t sim_frame_count() const { return sim_frames_.load(); }
    int last_substep_count() const { return last_substeps_.load(); }
    std::uint64_t published_frame() const { return pump_.published_frame(); }
    std::uint64_t last_contract_prepare_ns() const { return contract_prepare_ns_.load(); }
    std::uint64_t contract_count() const { return contract_count_.load(); }

private:
    void engine_loop();
    // The sim step's body, under the frame's lock.
    void run_step(double wall);
    // The stopped tick: tools' meshes, convex pieces, terrain, and physics that
    // follows the tree, and the plugins' and command line's tool step.
    void run_stopped_tick();
    // RenderStepped, PreRender, and the copy, under the frame's lock.
    void run_window(double dt);
    void step_physics(double dt);
    void drain_edits();
    // Runs one step of a loop. A contract failure goes to on_contract. Any other
    // exception is reported to stderr and the console, and the loop goes on.
    template <typename Step, typename OnContract>
    void guarded_step(Step&& step, OnContract&& on_contract);
    void report_fault(const char* what);
    // Checks terrain_'s failure count against what was last seen and reports
    // a rise once through report_fault, the way other faults are reported --
    // TerrainMesher's worker threads never log directly (see its header).
    // Called right after each terrain_.update(game_), on SimulationThread.
    void report_terrain_failures();

    Game game_;
    SnapshotPump pump_;
    TaskScheduler scheduler_;
    std::unique_ptr<ScriptRuntime> scripts_;
    // PhysicsObject bodies, stepped in step_physics while the place plays,
    // and synced (not stepped) by the stopped tick, so they follow the tree
    // in edit mode. Registered with game_ so scripts reach it (Raycast).
    PhysicsWorld physics_;
    // Splits Custom PhysicsObjects' Meshes into convex pieces while stopped.
    ConvexDecomposer decomposer_;
    // Meshes every Terrain in Workspace, playing or stopped, building each
    // chunk's collider on its workers for physics_'s Terrain bodies.
    TerrainWorld terrain_{PhysicsWorld::build_terrain_collider};
    // Task 5: builds and publishes each Terrain's texture arrays, off
    // SimulationThread on its own worker. Wired into terrain_ once
    // (set_terrain_textures) so TerrainWorld::rebuild_look can query
    // layer_of; updated just before terrain_ each tick so a Material newly
    // seen already has its layer index by the time rebuild_look runs.
    TerrainTextures textures_;
    std::uint64_t terrain_failures_seen_ = 0;   // report_terrain_failures' high-water mark
    // Whether audio_ may open an output device; declared first so audio_ is built after it.
    bool audio_device_ = true;
    // SoundEmitter voices, stepped after the scripts each frame while the place plays.
    AudioWorld audio_;
    // Every ParticleEmitter in Workspace, stepped once a frame in run_frame,
    // stopped or playing; frozen while play is paused.
    ParticleWorld particles_;
    // The dt run_step last stepped with, which particles_ takes in play.
    double last_step_dt_ = 1.0 / 60.0;
    IRenderer* renderer_ = nullptr;
    IClock* clock_ = nullptr;
    INetworkPump* network_ = nullptr;
    double render_dt_ = 1.0 / 60.0;
    double physics_dt_ = 1.0 / 240.0;
    double pace_hz_ = 0;
    std::atomic<bool> client_sync_{false};
    // Wall time not yet stepped by physics. Engine thread only.
    double accumulator_ = 0;
    // When the stopped tick last ran. Engine thread only.
    std::chrono::steady_clock::time_point last_tool_{};
    // note_client_frame bumps client_frames_. The unpaced loop waits on it
    // when client sync is set. Tests leave the sync off.
    std::mutex client_frame_mu_;
    std::condition_variable client_frame_cv_;
    std::uint64_t client_frames_ = 0;

    std::thread simulation_;
    std::thread::id simulation_id_{};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> present_count_{0};
    std::atomic<std::uint64_t> sim_frames_{0};
    std::atomic<int> last_substeps_{0};
    std::atomic<std::uint64_t> contract_prepare_ns_{0};
    std::atomic<std::uint64_t> contract_count_{0};

    std::mutex start_mu_;
    std::condition_variable start_cv_;
    bool simulation_ready_ = false;
    bool start_release_ = false;

    // Guards paused_. An unpaced engine thread waits on pause_cv_ while paused.
    // The UI thread takes it only to flip the flag or to publish a paused edit.
    // Hold is a store, or one paused edit.
    mutable std::mutex pause_mu_;
    std::condition_variable pause_cv_;
    bool paused_ = true;

    // Edits posted while the simulation is running. Drained on the engine thread
    // at the start of the next step, under the write lock.
    std::mutex edit_mu_;
    std::vector<std::function<void(DataModel&)>> edits_;

    // When each recent fault was last reported.
    std::mutex fault_mu_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> recent_faults_;

    // Declared last so it is destroyed before the DataModel, after stop() joins
    // the engine thread.
    std::unique_ptr<ScriptAnalysis> analysis_;
};

}  // namespace engine_core
