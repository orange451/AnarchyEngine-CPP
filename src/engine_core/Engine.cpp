#include "profiler/Profiler.hpp"
#include "Engine.hpp"

#include "DataModelLock.hpp"
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
    analysis_ = std::make_unique<ScriptAnalysis>(game_);
}

ScriptRuntime& Engine::scripts() { return *scripts_; }

ScriptAnalysis& Engine::analysis() { return *analysis_; }

const ScriptAnalysis& Engine::analysis() const { return *analysis_; }

Engine::~Engine() {
    stop();
    // game_ is declared before physics_, so it is destroyed after physics_
    // goes away. Clear the pointer here so no one can reach a dangling world
    // during teardown. This runs after stop() has joined the simulation and
    // render threads, so nothing else touches game_ and no lock is needed.
    game_.set_physics(nullptr);
}

void Engine::set_renderer(IRenderer* renderer) { renderer_ = renderer; }

void Engine::set_clock(IClock* clock) { clock_ = clock; }

void Engine::set_timing(double render_dt, double physics_dt) {
    render_dt_ = render_dt;
    physics_dt_ = physics_dt;
}

void Engine::set_pace_hz(double hz) {
    simulation_pace_hz_ = hz;
    render_pace_hz_ = hz;
}

void Engine::set_simulation_pace_hz(double hz) { simulation_pace_hz_ = hz; }

void Engine::set_render_pace_hz(double hz) { render_pace_hz_ = hz; }

void Engine::set_render_client_sync(bool enabled) { render_client_sync_.store(enabled); }

void Engine::set_simulation_client_sync(bool enabled) { simulation_client_sync_.store(enabled); }

void Engine::note_client_frame() {
    {
        std::lock_guard<std::mutex> guard(client_frame_mu_);
        ++client_frames_;
    }
    // Both loops may be waiting on the same paint.
    client_frame_cv_.notify_all();
}

void Engine::start() {
    if (running_.load()) {
        throw std::logic_error("Engine already started");
    }
    {
        std::lock_guard<std::mutex> guard(start_mu_);
        simulation_ready_ = false;
        render_ready_ = false;
        start_release_ = false;
    }
    running_.store(true);
    simulation_ = std::thread([this] { simulation_loop(); });
    render_ = std::thread([this] { render_loop(); });
    {
        std::unique_lock<std::mutex> guard(start_mu_);
        start_cv_.wait(guard, [&] { return simulation_ready_ && render_ready_; });
        game_.set_thread_ids(simulation_id_, render_id_);
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
    if (render_.joinable()) {
        render_.join();
    }
    game_.set_threads_running(false);
}

void Engine::simulation_loop() {
    set_thread_role(ThreadRole::Simulation);
    profiler::register_thread("Sim");
    static const profiler::ScopeId kLockWait = profiler::intern("Lock wait", profiler::Group::Engine);
    static const profiler::ScopeId kPaintWait = profiler::intern("Paint wait", profiler::Group::Engine);
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
    std::chrono::steady_clock::time_point next_step;
    double accumulator = 0;
    while (running_.load()) {
        bool woke = false;
        {
            std::unique_lock<std::mutex> pause_lock(pause_mu_);
            if (paused_) {
                // Paused, the play step waits, but the command line's and the plugins'
                // threads keep time. The tool step takes the write lock, as a paused edit
                // does, so the two take turns. pause_mu_ is let go meanwhile.
                auto last_tool = std::chrono::steady_clock::now();
                while (!pause_cv_.wait_for(pause_lock, kToolInterval, [&] { return !paused_ || !running_.load(); })) {
                    const auto now = std::chrono::steady_clock::now();
                    const double dt = std::min(0.1, std::chrono::duration<double>(now - last_tool).count());
                    last_tool = now;
                    pause_lock.unlock();
                    // Stopped, Custom PhysicsObjects' Meshes are split into convex
                    // pieces, and bodies follow the tree without being simulated.
                    // A paused test writes no files.
                    guarded_step(
                        [&] {
                            PROFILE_SCOPE("Stopped physics", profiler::Group::Physics);
                            DataModelLock lock(game_, DataModelLock::Write);
                            if (!game_.simulation_running()) {
                                decomposer_.update(game_);
                                physics_.sync(game_);
                            }
                        },
                        [&] { contract_count_.fetch_add(1); });
                    if (!scripts_ || !scripts_->tools_open()) {
                        pause_lock.lock();
                        continue;
                    }
                    guarded_step(
                        [&] {
                            PROFILE_SCOPE("Tool step", profiler::Group::Engine);
                            DataModelLock lock(game_, DataModelLock::Write);
                            scripts_->step_tools(dt);
                        },
                        [&] { contract_count_.fetch_add(1); });
                    pause_lock.lock();
                }
                woke = true;
            }
        }
        if (!running_.load()) {
            break;
        }
        // The time spent paused is not a simulation step.
        if (woke && clock_ == nullptr) {
            last = std::chrono::steady_clock::now();
        }
        // Nor slots to catch up on.
        if (woke) {
            next_step = {};
        }

        // Sample before the step so a paint that arrives during it is not missed.
        std::uint64_t client_seen = 0;
        const bool wait_for_client = simulation_client_sync_.load() && !(simulation_pace_hz_ > 0.0);
        if (wait_for_client) {
            std::lock_guard<std::mutex> guard(client_frame_mu_);
            client_seen = client_frames_;
        }
        const auto frame_start = std::chrono::steady_clock::now();
        double wall = render_dt_;
        if (clock_ != nullptr) {
            wall = clock_->delta_seconds();
        } else {
            const auto now = std::chrono::steady_clock::now();
            wall = std::chrono::duration<double>(now - last).count();
            last = now;
            if (wall < 0) {
                wall = 0;
            }
            if (wall > 0.1) {
                wall = 0.1;
            }
        }

        // A step that follows the paint has no fixed length, so it is given the
        // time it actually covers. A paced step keeps render_dt.
        const double step_dt = wait_for_client && wall > 0.0 ? wall : render_dt_;

        int substeps = 0;
        guarded_step(
            [&] {
                PROFILE_SCOPE("Simulation step", profiler::Group::Engine);
                const bool timing = profiler::enabled();
                if (timing) {
                    profiler::begin(kLockWait);
                }
                DataModelLock lock(game_, DataModelLock::Write);
                if (timing) {
                    profiler::end();
                }
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
                accumulator += wall;
                constexpr int kMaxSubsteps = 32;
                if (accumulator >= physics_dt_) {
                    PROFILE_SCOPE("Physics", profiler::Group::Physics);
                    while (accumulator >= physics_dt_ && substeps < kMaxSubsteps) {
                        PROFILE_SCOPE("Substep", profiler::Group::Physics);
                        scheduler_.run_phase(Phase::PreSimulation, physics_dt_);
                        game_.events().drain();
                        scheduler_.run_phase(Phase::PhysicsSubstep, physics_dt_);
                        game_.events().drain();
                        {
                            PROFILE_SCOPE("Box3D", profiler::Group::Physics);
                            step_physics(physics_dt_);
                        }
                        scheduler_.run_phase(Phase::PostSimulation, physics_dt_);
                        game_.events().drain();
                        accumulator -= physics_dt_;
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
                // After the scripts, so a Play, Stop, or Destroy this frame is heard this frame.
                PROFILE_SCOPE("Audio", profiler::Group::Engine);
                audio_.step(game_, step_dt);
            },
            [&] { contract_count_.fetch_add(1); });
        if (game_.take_deferred_violation()) {
            contract_count_.fetch_add(1);
        }
        last_substeps_.store(substeps);
        sim_frames_.fetch_add(1);

        if (simulation_pace_hz_ > 0) {
            WaitForSlot(next_step, frame_start, simulation_pace_hz_);
        } else if (wait_for_client) {
            // Steps with the window's paint, as the uncapped render loop does.
            // stop() wakes this wait as well.
            profiler::Scope wait(kPaintWait);
            std::unique_lock<std::mutex> guard(client_frame_mu_);
            client_frame_cv_.wait_until(guard, frame_start + kSimulationClientFallback, [&] {
                return !running_.load() || client_frames_ != client_seen;
            });
        }
    }
}

void Engine::render_loop() {
    set_thread_role(ThreadRole::Render);
    profiler::register_thread("Render");
    static const profiler::ScopeId kStep = profiler::intern("Render step", profiler::Group::Render);
    static const profiler::ScopeId kPrepare = profiler::intern("Prepare", profiler::Group::Render);
    static const profiler::ScopeId kLockWait = profiler::intern("Lock wait", profiler::Group::Engine);
    static const profiler::ScopeId kPaintWait = profiler::intern("Paint wait", profiler::Group::Engine);
    {
        std::unique_lock<std::mutex> guard(start_mu_);
        render_id_ = std::this_thread::get_id();
        render_ready_ = true;
        start_cv_.notify_all();
        start_cv_.wait(guard, [&] { return start_release_; });
    }
    if (!running_.load()) {
        return;
    }

    auto last_frame = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_frame;
    RenderStepTime step_time;
    while (running_.load()) {
        // A frame is one pass of this loop, from one Prepare to the next.
        profiler::frame_boundary();
        // Sample before the work so a paint that arrives during the step is not missed.
        std::uint64_t client_seen = 0;
        const bool wait_for_client = render_client_sync_.load() && !(render_pace_hz_ > 0.0);
        if (wait_for_client) {
            std::lock_guard<std::mutex> guard(client_frame_mu_);
            client_seen = client_frames_;
        }
        const auto frame_start = std::chrono::steady_clock::now();
        double frame_dt = std::chrono::duration<double>(frame_start - last_frame).count();
        last_frame = frame_start;
        step_time.add(frame_dt);
        if (frame_dt < 0) {
            frame_dt = 0;
        }
        if (frame_dt > 0.1) {
            frame_dt = 0.1;
        }
        if (!(frame_dt > 0.0)) {
            frame_dt = render_dt_;
        }
        bool prepared = false;
        bool saw_contract = false;
        std::uint64_t hold_ns = 0;
        const bool timing = profiler::enabled();
        // The frame's work, Prepare to PostRender, as one scope; the wait for the
        // next paint after it is not part of it.
        if (timing) {
            profiler::begin(kStep);
            profiler::begin(kPrepare);
            profiler::begin(kLockWait);
        }
        {
            DataModelLock lock(game_, DataModelLock::Write, std::chrono::milliseconds(2));
            if (timing) {
                profiler::end();
            }
            if (lock.owns()) {
                const auto hold_start = std::chrono::steady_clock::now();
                // Includes the frames since the last prepared one, which ran no RenderStepped.
                const double step_dt = step_time.take(render_dt_);
                pump_.begin_prerender_window(game_);
                // Roblox order inside the pre-draw window: RenderStepped, then PreRender.
                // A failure in one does not skip the other or the copy.
                const auto contract = [&saw_contract] { saw_contract = true; };
                guarded_step(
                    [&] {
                        PROFILE_SCOPE("RenderStepped", profiler::Group::Engine);
                        scheduler_.run_phase(Phase::RenderStepped, step_dt);
                    },
                    contract);
                guarded_step(
                    [&] {
                        PROFILE_SCOPE("PreRender", profiler::Group::Engine);
                        scheduler_.run_phase(Phase::PreRender, step_dt);
                    },
                    contract);
                pump_.end_prerender_window(game_);
                // Copy even after a rejected PreRender write. Authorize fails before
                // mutation, so the queue still describes real sim state.
                guarded_step(
                    [&] {
                        PROFILE_SCOPE("Snapshot copy", profiler::Group::Engine);
                        pump_.take_changes(game_);
                        prepared = true;
                    },
                    contract);
                hold_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - hold_start)
                        .count());
            }
        }
        if (game_.take_deferred_violation()) {
            saw_contract = true;
        }
        if (saw_contract) {
            contract_count_.fetch_add(1);
            contract_prepare_ns_.store(hold_ns);
        }
        if (prepared) {
            // The buffer copy needs no DataModel state, so it is out of the lock.
            PROFILE_SCOPE("Publish", profiler::Group::Engine);
            pump_.finish_copy();
            pump_.publish();
        }
        if (timing) {
            profiler::end();
        }
        // Perform is outside the pre-draw window. A DataModel write there is path D.
        const auto late_contract = [this] { contract_count_.fetch_add(1); };
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
                late_contract);
        }
        // After Present the snapshot for this frame is already published.
        // PostRender does not hold the Prepare lock and is not part of the 2 ms budget.
        guarded_step(
            [&] {
                PROFILE_SCOPE("PostRender", profiler::Group::Engine);
                scheduler_.run_phase(Phase::PostRender, frame_dt);
            },
            late_contract);
        if (timing) {
            profiler::end();
        }
        present_count_.fetch_add(1);

        if (render_pace_hz_ > 0) {
            WaitForSlot(next_frame, frame_start, render_pace_hz_);
        } else if (wait_for_client) {
            // The window paints much slower than an empty step. Waiting here keeps
            // the step with that paint. The timeout only covers a window that is
            // not painting; stop() wakes this wait as well. Recorded, so the frame
            // shows the step waiting for the paint rather than a gap after it.
            profiler::Scope wait(kPaintWait);
            std::unique_lock<std::mutex> guard(client_frame_mu_);
            client_frame_cv_.wait_for(guard, std::chrono::milliseconds(50), [&] {
                return !running_.load() || client_frames_ != client_seen;
            });
        }
    }
}

void Engine::step_physics(double dt) {
    game_.integrate_simulated(dt);
    physics_.step(game_, dt);
}

}  // namespace engine_core
