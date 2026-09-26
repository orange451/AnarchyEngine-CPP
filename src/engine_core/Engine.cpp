#include "Engine.hpp"

#include "DataModelLock.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

namespace engine_core {

Engine::Engine() {
    pump_.reserve(DataModel::kMaxInstances);
    scheduler_.reserve(64);
    game_.attach_scheduler(&scheduler_);
    scripts_ = std::make_unique<ScriptRuntime>();
    scripts_->attach(game_, scheduler_);
    analysis_ = std::make_unique<ScriptAnalysis>(game_);
}

ScriptRuntime& Engine::scripts() { return *scripts_; }

ScriptAnalysis& Engine::analysis() { return *analysis_; }

const ScriptAnalysis& Engine::analysis() const { return *analysis_; }

Engine::~Engine() { stop(); }

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

void Engine::note_client_frame() {
    {
        std::lock_guard<std::mutex> guard(client_frame_mu_);
        ++client_frames_;
    }
    client_frame_cv_.notify_one();
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
    {
        std::lock_guard<std::mutex> guard(pause_mu_);
        paused_ = false;
    }
    pause_cv_.notify_all();
}

void Engine::pause() {
    std::lock_guard<std::mutex> guard(pause_mu_);
    paused_ = true;
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
        fn(game_);
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
    double accumulator = 0;
    while (running_.load()) {
        bool woke = false;
        {
            std::unique_lock<std::mutex> pause_lock(pause_mu_);
            if (paused_) {
                pause_cv_.wait(pause_lock, [&] { return !paused_ || !running_.load(); });
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

        int substeps = 0;
        try {
            DataModelLock lock(game_, DataModelLock::Write);
            game_.drain_commands();
            drain_edits();
            scheduler_.run_phase(Phase::PreAnimation, render_dt_);
            // Deferred handlers run on this thread, still under the step lock,
            // after the phase that queued them and before Prepare can copy.
            game_.events().drain();
            accumulator += wall;
            constexpr int kMaxSubsteps = 32;
            while (accumulator >= physics_dt_ && substeps < kMaxSubsteps) {
                scheduler_.run_phase(Phase::PreSimulation, physics_dt_);
                game_.events().drain();
                scheduler_.run_phase(Phase::PhysicsSubstep, physics_dt_);
                game_.events().drain();
                step_physics(physics_dt_);
                scheduler_.run_phase(Phase::PostSimulation, physics_dt_);
                game_.events().drain();
                accumulator -= physics_dt_;
                ++substeps;
            }
            scheduler_.run_phase(Phase::Heartbeat, render_dt_);
            // Descendants of the root step in this phase. Bound Heartbeat jobs
            // stay for callers that are not instances.
            game_.step_descendants(render_dt_);
            game_.events().drain();
            // Same dt Heartbeat jobs just received. Scripts resume after that drain.
            if (scripts_) {
                scripts_->heartbeat(render_dt_);
            }
            game_.events().drain();
        } catch (const ContractViolation&) {
            contract_count_.fetch_add(1);
        }
        if (game_.take_deferred_violation()) {
            contract_count_.fetch_add(1);
        }
        last_substeps_.store(substeps);
        sim_frames_.fetch_add(1);

        if (simulation_pace_hz_ > 0) {
            const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / simulation_pace_hz_));
            const auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < budget) {
                std::this_thread::sleep_for(budget - elapsed);
            }
        }
    }
}

void Engine::render_loop() {
    set_thread_role(ThreadRole::Render);
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
    while (running_.load()) {
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
        {
            DataModelLock lock(game_, DataModelLock::Write, std::chrono::milliseconds(2));
            if (lock.owns()) {
                const auto hold_start = std::chrono::steady_clock::now();
                pump_.begin_prerender_window(game_);
                // Roblox order inside the pre-draw window: RenderStepped, then PreRender.
                // A failure in one does not skip the other or the copy.
                try {
                    scheduler_.run_phase(Phase::RenderStepped, frame_dt);
                } catch (const ContractViolation&) {
                    saw_contract = true;
                }
                try {
                    scheduler_.run_phase(Phase::PreRender, frame_dt);
                } catch (const ContractViolation&) {
                    saw_contract = true;
                }
                pump_.end_prerender_window(game_);
                // Copy even after a rejected PreRender write. Authorize fails before
                // mutation, so the queue still describes real sim state.
                try {
                    pump_.prepare_copy(game_);
                    prepared = true;
                } catch (const ContractViolation&) {
                    saw_contract = true;
                }
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
            pump_.publish();
        }
        if (renderer_ != nullptr) {
            try {
                renderer_->perform(pump_.front());
                renderer_->present();
            } catch (const ContractViolation&) {
                // Perform is outside the pre-draw window. A DataModel write here is path D.
                contract_count_.fetch_add(1);
            }
        }
        // After Present the snapshot for this frame is already published.
        // PostRender does not hold the Prepare lock and is not part of the 2 ms budget.
        try {
            scheduler_.run_phase(Phase::PostRender, frame_dt);
        } catch (const ContractViolation&) {
            contract_count_.fetch_add(1);
        }
        present_count_.fetch_add(1);

        if (render_pace_hz_ > 0) {
            const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / render_pace_hz_));
            const auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < budget) {
                std::this_thread::sleep_for(budget - elapsed);
            }
        } else if (wait_for_client) {
            // The window paints much slower than an empty step. Waiting here keeps
            // the step with that paint. The timeout only covers a window that is
            // not painting; stop() wakes this wait as well.
            std::unique_lock<std::mutex> guard(client_frame_mu_);
            client_frame_cv_.wait_for(guard, std::chrono::milliseconds(50), [&] {
                return !running_.load() || client_frames_ != client_seen;
            });
        }
    }
}

void Engine::step_physics(double dt) { game_.integrate_simulated(dt); }

}  // namespace engine_core
