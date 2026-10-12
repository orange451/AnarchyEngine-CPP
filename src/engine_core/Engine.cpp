#include "profiler/Profiler.hpp"
#include "Engine.hpp"

#include "AnimatorStep.hpp"
#include "Assemblies.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "MeshEditors.hpp"
#include "PoseBlend.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace engine_core {
namespace {

// How often a paused engine steps the command line's and the plugins' threads.
constexpr std::chrono::milliseconds kToolInterval(16);

// How long a client-synced simulation waits for a paint before it steps anyway,
// so a window that stops painting still runs the place at about 60 Hz.
constexpr std::chrono::microseconds kSimulationClientFallback(16667);

// Sleeps until the next slot of a fixed hz schedule. Sleeping for the budget
// less the step's own work dropped each wake-up's lateness, and a sleep on
// Windows can wake a whole 15.6 ms tick late, so 60 Hz ran near 32. On a fixed
// schedule a late wake is made up by the next one. A step more than a slot
// behind starts the schedule over rather than running a burst to catch up.
// next is the schedule; an empty one starts at from.
void WaitForSlot(std::chrono::steady_clock::time_point& next, std::chrono::steady_clock::time_point from, double hz) {
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / hz));
    if (next == std::chrono::steady_clock::time_point{}) {
        next = from;
    }
    next += interval;
    const auto now = std::chrono::steady_clock::now();
    if (now - next > interval) {
        next = now;
    } else if (now < next) {
        std::this_thread::sleep_until(next);
    }
}

}  // namespace

Engine::Engine() {
    pump_.reserve(DataModel::kMaxInstances);
    scheduler_.reserve(64);
    game_.attach_scheduler(&scheduler_);
    scripts_ = std::make_unique<ScriptRuntime>();
    scripts_->attach(game_, scheduler_);
    // The engine starts paused, so play handlers wait for resume() like its
    // signals do, even across a start_simulation that comes first.
    scripts_->set_render_paused(paused_);
    // Physics warnings, such as a Hull that fell back to a Box, go to the console.
    physics_.set_warning_sink([this](const std::string& text) {
        scripts_->append_output(ScriptRuntime::OutputKind::Print, text);
    });
    game_.set_physics(&physics_);
    // So do audio warnings, such as a Sound whose file cannot be played.
    audio_.set_warning_sink([this](const std::string& text) {
        scripts_->append_output(ScriptRuntime::OutputKind::Print, text);
    });
    // Each Terrain terrain_ meshes gets a body, made in physics_.sync.
    physics_.set_terrain_world(&terrain_);
    // Task 5: rebuild_look's layer_of source.
    terrain_.set_terrain_textures(&textures_);
    // The engine's snapshot carries each Terrain's chunks and look, read
    // from terrain_ by SnapshotPump::resolve_terrains.
    pump_.set_terrain_world(&terrain_);
    // So do instances' warnings, such as a Terrain whose voxel file is missing.
    game_.set_warning_sink([this](const std::string& text) {
        scripts_->append_output(ScriptRuntime::OutputKind::Print, text);
    });
    analysis_ = std::make_unique<ScriptAnalysis>(game_);
}

ScriptRuntime& Engine::scripts() { return *scripts_; }

ScriptAnalysis& Engine::analysis() { return *analysis_; }

const ScriptAnalysis& Engine::analysis() const { return *analysis_; }

Engine::~Engine() {
    stop();
    // game_ is declared before physics_, so it is destroyed after physics_
    // goes away. Clear the pointer here so no one can reach a dangling world
    // during teardown. This runs after stop() has joined the engine thread,
    // so nothing else touches game_ and no lock is needed.
    game_.set_physics(nullptr);
    // terrain_ is declared after physics_ and pump_, so it goes first.
    physics_.set_terrain_world(nullptr);
    pump_.set_terrain_world(nullptr);
}

void Engine::set_renderer(IRenderer* renderer) { renderer_ = renderer; }

void Engine::set_clock(IClock* clock) { clock_ = clock; }

void Engine::set_timing(double render_dt, double physics_dt) {
    render_dt_ = render_dt;
    physics_dt_ = physics_dt;
}

void Engine::set_pace_hz(double hz) { pace_hz_ = hz; }

void Engine::set_client_sync(bool enabled) { client_sync_.store(enabled); }

void Engine::note_client_frame() {
    {
        std::lock_guard<std::mutex> guard(client_frame_mu_);
        ++client_frames_;
    }
    // The engine loop may be waiting on this paint.
    client_frame_cv_.notify_all();
}

void Engine::start() {
    if (running_.load()) {
        throw std::logic_error("Engine already started");
    }
    {
        std::lock_guard<std::mutex> guard(start_mu_);
        simulation_ready_ = false;
        start_release_ = false;
    }
    running_.store(true);
    simulation_ = std::thread([this] { engine_loop(); });
    {
        std::unique_lock<std::mutex> guard(start_mu_);
        start_cv_.wait(guard, [&] { return simulation_ready_; });
        game_.set_engine_thread(simulation_id_);
        game_.set_threads_running(true);
        start_release_ = true;
    }
    start_cv_.notify_all();
}

void Engine::resume() {
    // The frames drawn while paused are not the first step's RenderStepped time.
    scripts_->drop_render_frames();
    {
        std::lock_guard<std::mutex> guard(pause_mu_);
        paused_ = false;
    }
    // A resumed session's window handlers resume delivery with its signals.
    scripts_->set_render_paused(false);
    pause_cv_.notify_all();
}

void Engine::pause() {
    {
        std::lock_guard<std::mutex> guard(pause_mu_);
        paused_ = true;
    }
    // A paused session's window handlers wait with its signals.
    scripts_->set_render_paused(true);
    // Paused sounds wait where they are; the first step after resume plays them on.
    audio_.suspend();
}

bool Engine::paused() const {
    std::lock_guard<std::mutex> guard(pause_mu_);
    return paused_;
}

void Engine::on_simulation(std::function<void(DataModel&)> fn) {
    if (!fn) {
        return;
    }
    if (!running_.load() || std::this_thread::get_id() == simulation_id_) {
        fn(game_);
        return;
    }
    // pause_mu_ is released by the sim thread while it waits. Holding it here
    // keeps that wait from ending, so the write lock is not the step lock.
    std::unique_lock<std::mutex> pause_lock(pause_mu_);
    if (paused_) {
        DataModelLock lock(game_, DataModelLock::Write);
        game_.perform_paused_edit(fn);
        return;
    }
    std::lock_guard<std::mutex> guard(edit_mu_);
    edits_.push_back(std::move(fn));
}

template <typename Step, typename OnContract>
void Engine::guarded_step(Step&& step, OnContract&& on_contract) {
    try {
        step();
    } catch (const ContractViolation&) {
        on_contract();
    } catch (const std::exception& error) {
        report_fault(error.what());
    } catch (...) {
        report_fault("an exception that is not a std::exception");
    }
}

void Engine::report_terrain_failures() {
    const std::uint64_t failures = terrain_.mesh_failures();
    if (failures <= terrain_failures_seen_) {
        return;   // no rise since the last check
    }
    terrain_failures_seen_ = failures;
    const std::string text = "Terrain meshing failed: " + terrain_.last_mesh_failure();
    report_fault(text.c_str());
}

void Engine::report_fault(const char* what) {
    const std::string text = std::string("Engine: ") + (what != nullptr ? what : "unknown error");
    const auto now = std::chrono::steady_clock::now();
    {
        // A fault that repeats every frame, on either thread, is said once every
        // few seconds rather than once a frame.
        std::lock_guard<std::mutex> guard(fault_mu_);
        const auto seen = recent_faults_.find(text);
        if (seen != recent_faults_.end() && now - seen->second < std::chrono::seconds(5)) {
            return;
        }
        if (recent_faults_.size() >= 64) {
            recent_faults_.clear();
        }
        recent_faults_[text] = now;
    }
    std::fprintf(stderr, "%s\n", text.c_str());
    if (scripts_) {
        scripts_->append_output(ScriptRuntime::OutputKind::Error, text);
    }
}

void Engine::drain_edits() {
    std::vector<std::function<void(DataModel&)>> batch;
    {
        std::lock_guard<std::mutex> guard(edit_mu_);
        if (edits_.empty()) {
            return;
        }
        batch.swap(edits_);
    }
    for (const std::function<void(DataModel&)>& fn : batch) {
        // An edit that fails is reported; the edits queued after it still run.
        guarded_step([&] { fn(game_); }, [this] { contract_count_.fetch_add(1); });
    }
}

void Engine::stop() {
    running_.store(false);
    {
        std::lock_guard<std::mutex> guard(start_mu_);
        start_release_ = true;
    }
    {
        std::lock_guard<std::mutex> guard(pause_mu_);
        paused_ = false;
    }
    // Kept with paused_, so a later start() delivers as it steps.
    scripts_->set_render_paused(false);
    start_cv_.notify_all();
    pause_cv_.notify_all();
    client_frame_cv_.notify_all();
    if (simulation_.joinable()) {
        simulation_.join();
    }
    game_.set_threads_running(false);
}

void Engine::engine_loop() {
    set_thread_role(ThreadRole::Simulation);
    profiler::register_thread("Engine");
    {
        std::unique_lock<std::mutex> guard(start_mu_);
        simulation_id_ = std::this_thread::get_id();
        simulation_ready_ = true;
        start_cv_.notify_all();
        start_cv_.wait(guard, [&] { return start_release_; });
    }
    if (!running_.load()) {
        return;
    }

    auto last = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_frame;
    bool was_paused = true;
    while (running_.load()) {
        // Sample before the frame so a paint that arrives during it is not missed.
        std::uint64_t client_seen = 0;
        const bool wait_for_client = client_sync_.load() && !(pace_hz_ > 0.0);
        if (wait_for_client) {
            std::lock_guard<std::mutex> guard(client_frame_mu_);
            client_seen = client_frames_;
        }
        const auto frame_start = std::chrono::steady_clock::now();
        double wall = std::chrono::duration<double>(frame_start - last).count();
        last = frame_start;
        const bool paused_now = paused();
        // The time spent paused is not a simulation step, nor slots to catch up on.
        if (was_paused && !paused_now) {
            wall = 0;
            next_frame = {};
        }
        was_paused = paused_now;
        run_frame(std::clamp(wall, 0.0, 0.1));

        if (pace_hz_ > 0) {
            WaitForSlot(next_frame, frame_start, pace_hz_);
        } else if (wait_for_client) {
            // Steps with the window's paint. stop() wakes this wait as well.
            std::unique_lock<std::mutex> guard(client_frame_mu_);
            client_frame_cv_.wait_until(guard, frame_start + kSimulationClientFallback, [&] {
                return !running_.load() || client_frames_ != client_seen;
            });
        } else if (paused_now) {
            // Unpaced and paused: the editor's tick rate, woken early by resume or stop.
            std::unique_lock<std::mutex> guard(pause_mu_);
            pause_cv_.wait_for(guard, kToolInterval, [&] { return !paused_ || !running_.load(); });
        }
    }
}

void Engine::run_frame(double wall) {
    static const profiler::ScopeId kFrame = profiler::intern("Frame", profiler::Group::Engine);
    static const profiler::ScopeId kLockWait = profiler::intern("Lock wait", profiler::Group::Engine);
    profiler::frame_boundary();
    // Read before the lock: a paused edit holds pause_mu_ while it waits for the lock.
    const bool paused_now = paused();
    const bool timing = profiler::enabled();
    if (timing) {
        profiler::begin(kFrame);
        profiler::begin(kLockWait);
    }
    double window_dt = wall;
    std::uint64_t hold_ns = 0;
    bool saw_contract = false;
    {
        DataModelLock lock(game_, DataModelLock::Write);
        if (timing) {
            profiler::end();
        }
        if (paused_now) {
            run_stopped_tick();
        } else {
            const double step_wall = clock_ != nullptr ? clock_->delta_seconds() : wall;
            // RenderStepped gets the frame's dt, the one Heartbeat gets.
            const bool measured = client_sync_.load() && !(pace_hz_ > 0.0);
            window_dt = measured && step_wall > 0.0 ? step_wall : render_dt_;
            guarded_step([&] { run_step(step_wall); }, [&] { contract_count_.fetch_add(1); });
            if (game_.take_deferred_violation()) {
                contract_count_.fetch_add(1);
            }
            sim_frames_.fetch_add(1);
        }
        const auto hold_start = std::chrono::steady_clock::now();
        {
            PROFILE_SCOPE("Prepare", profiler::Group::Render);
            run_window(window_dt);
        }
        hold_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - hold_start)
                .count());
        saw_contract = game_.take_deferred_violation();
    }
    if (saw_contract) {
        contract_count_.fetch_add(1);
        contract_prepare_ns_.store(hold_ns);
    }
    {
        // The buffer copy needs no DataModel state, so it is out of the lock.
        PROFILE_SCOPE("Publish", profiler::Group::Engine);
        pump_.finish_copy();
        pump_.publish();
    }
    // Perform is after the frame's lock. A DataModel write there is refused.
    if (renderer_ != nullptr) {
        guarded_step(
            [&] {
                {
                    PROFILE_SCOPE("Perform", profiler::Group::Render);
                    renderer_->perform(pump_.front());
                }
                PROFILE_SCOPE("Present", profiler::Group::Render);
                renderer_->present();
            },
            [this] { contract_count_.fetch_add(1); });
    }
    if (timing) {
        profiler::end();
    }
    present_count_.fetch_add(1);
}

void Engine::run_stopped_tick() {
    const auto now = std::chrono::steady_clock::now();
    const bool first = last_tool_ == std::chrono::steady_clock::time_point{};
    if (!first && now - last_tool_ < kToolInterval) {
        return;
    }
    const double dt = first ? render_dt_ : std::min(0.1, std::chrono::duration<double>(now - last_tool_).count());
    last_tool_ = now;
    // Paused, the play step waits, but the command line's and the plugins'
    // threads keep time. Stopped, Custom PhysicsObjects' Meshes are split into
    // convex pieces, and bodies follow the tree and the GameObjects they move
    // without being simulated. A paused test writes no files and moves no body.
    guarded_step(
        [&] {
            PROFILE_SCOPE("Stopped physics", profiler::Group::Physics);
            if (!game_.simulation_running()) {
                // The last tick's tool and command line edits, first,
                // so the pieces below are cut from the newest geometry.
                game_.mesh_editors().step(game_, std::chrono::steady_clock::now());
                decomposer_.update(game_);
                // Textures before terrain_: a Material newly seen this tick
                // already has its layer index (TerrainTextures::layer_of) by
                // the time rebuild_look runs inside terrain_.update().
                // Terrain meshes before physics syncs, as the running tick
                // also orders it (see below): a Terrain's shape and collider
                // need to exist before physics_.sync looks for them, or a
                // Terrain created this tick still shows up in TerrainWorld's
                // views with no collider built yet, for one tick.
                textures_.update(game_);
                terrain_.update(game_);
                report_terrain_failures();
                terrain_.attach_textures(textures_);
                physics_.sync(game_);
                // Any RigidConstraint sub a write left off its parent's
                // pivot goes back on it; a consistent place writes nothing.
                if (game_.assemblies().follow(game_, 0) > 0) {
                    game_.history().mark_dirty();
                }
            }
        },
        [&] { contract_count_.fetch_add(1); });
    if (!scripts_ || !scripts_->tools_open()) {
        return;
    }
    guarded_step(
        [&] {
            PROFILE_SCOPE("Tool step", profiler::Group::Engine);
            scripts_->step_tools(dt);
        },
        [&] { contract_count_.fetch_add(1); });
}

void Engine::run_step(double wall) {
    PROFILE_SCOPE("Simulation step", profiler::Group::Engine);
    // A step that follows the paint has no fixed length, so it is given the
    // time it actually covers. A paced step keeps render_dt.
    const bool measured = client_sync_.load() && !(pace_hz_ > 0.0);
    const double step_dt = measured && wall > 0.0 ? wall : render_dt_;
    int substeps = 0;
    {
        PROFILE_SCOPE("Commands", profiler::Group::Engine);
        game_.drain_commands();
        drain_edits();
    }
    {
        PROFILE_SCOPE("PreAnimation", profiler::Group::Engine);
        scheduler_.run_phase(Phase::PreAnimation, step_dt);
    }
    {
        // Deferred handlers run on this thread, still under the step lock,
        // after the phase that queued them and before Prepare can copy.
        PROFILE_SCOPE("Events", profiler::Group::Engine);
        game_.events().drain();
    }
    {
        // After the frame's input and its handlers (a script's Play
        // or Stop counts this frame), before physics moves anything.
        PROFILE_SCOPE("Animation", profiler::Group::Engine);
        step_animators(game_, step_dt);
    }
    {
        // Once per frame, not per substep: the camera for job
        // ordering only needs to move once a frame, and meshing
        // is already off this thread (TerrainMesher's workers).
        // textures_ first: see the stopped tick's own comment above.
        PROFILE_SCOPE("Terrain", profiler::Group::Engine);
        {
            PROFILE_SCOPE("Terrain textures", profiler::Group::Engine);
            textures_.update(game_);
        }
        terrain_.update(game_);
        report_terrain_failures();
        terrain_.attach_textures(textures_);
    }
    accumulator_ += wall;
    constexpr int kMaxSubsteps = 32;
    if (accumulator_ >= physics_dt_) {
        PROFILE_SCOPE("Physics", profiler::Group::Physics);
        while (accumulator_ >= physics_dt_ && substeps < kMaxSubsteps) {
            PROFILE_SCOPE("Substep", profiler::Group::Physics);
            // The last substep's motion is what the window draws through.
            const bool last = accumulator_ - physics_dt_ < physics_dt_ || substeps + 1 == kMaxSubsteps;
            if (last) {
                game_.pose_blend().arm();
            }
            scheduler_.run_phase(Phase::PreSimulation, physics_dt_);
            game_.events().drain();
            scheduler_.run_phase(Phase::PhysicsSubstep, physics_dt_);
            game_.events().drain();
            {
                PROFILE_SCOPE("Box3D", profiler::Group::Physics);
                step_physics(physics_dt_);
            }
            game_.pose_blend().disarm();
            scheduler_.run_phase(Phase::PostSimulation, physics_dt_);
            game_.events().drain();
            accumulator_ -= physics_dt_;
            ++substeps;
        }
    }
    {
        PROFILE_SCOPE("Heartbeat", profiler::Group::Engine);
        scheduler_.run_phase(Phase::Heartbeat, step_dt);
        // Stepping instances under the root step in this phase. Bound
        // Heartbeat jobs stay for callers that are not instances.
        game_.step_instances(step_dt);
        game_.events().drain();
    }
    // Same dt Heartbeat jobs just received. Scripts resume after that drain.
    if (scripts_) {
        {
            PROFILE_SCOPE("Scripts", profiler::Group::Engine);
            scripts_->heartbeat(step_dt);
        }
        // The command line and plugins keep time with the play step.
        PROFILE_SCOPE("Tools", profiler::Group::Engine);
        scripts_->step_tools(step_dt);
    }
    {
        PROFILE_SCOPE("Events", profiler::Group::Engine);
        game_.events().drain();
    }
    {
        // This frame's MeshEditor edits, drawn from the next frame.
        PROFILE_SCOPE("Mesh edits", profiler::Group::Engine);
        game_.mesh_editors().step(game_, std::chrono::steady_clock::now());
    }
    {
        // After the scripts' writes (a character turned to face
        // where it goes), what a hand holds is drawn on the hand.
        PROFILE_SCOPE("Held", profiler::Group::Engine);
        game_.assemblies().place_held(game_);
    }
    // After the scripts, so a Play, Stop, or Destroy this frame is heard this frame.
    PROFILE_SCOPE("Audio", profiler::Group::Engine);
    audio_.step(game_, step_dt);
    last_substeps_.store(substeps);
}

void Engine::run_window(double dt) {
    pump_.begin_prerender_window(game_);
    // Bodies the last substep moved show where they are between it and the
    // next, until the copy below has taken them.
    swap_in_blend(game_, std::clamp(accumulator_ / physics_dt_, 0.0, 1.0));
    // Roblox order inside the pre-draw window: RenderStepped, then PreRender.
    // A failure in one does not skip the other or the copy.
    const auto contract = [this] { contract_count_.fetch_add(1); };
    guarded_step(
        [&] {
            PROFILE_SCOPE("RenderStepped", profiler::Group::Engine);
            scheduler_.run_phase(Phase::RenderStepped, dt);
        },
        contract);
    guarded_step(
        [&] {
            PROFILE_SCOPE("PreRender", profiler::Group::Engine);
            scheduler_.run_phase(Phase::PreRender, dt);
        },
        contract);
    pump_.end_prerender_window(game_);
    // Copy even after a rejected PreRender write. Authorize fails before
    // mutation, so the queue still describes real sim state.
    guarded_step(
        [&] {
            PROFILE_SCOPE("Snapshot copy", profiler::Group::Engine);
            pump_.take_changes(game_);
        },
        contract);
    // Its own step, so a failed copy cannot leave blended poses in the sim.
    guarded_step([&] { swap_out_blend(game_); }, contract);
}

void Engine::step_physics(double dt) {
    // Members are known before integrate_simulated skips them.
    game_.assemblies().sync_play(game_);
    game_.integrate_simulated(dt);
    physics_.step(game_, dt);
}

}  // namespace engine_core
