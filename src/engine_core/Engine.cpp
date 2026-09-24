#include "Engine.hpp"

#include "DataModelLock.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace engine_core {

Engine::Engine() {
    pump_.reserve(DataModel::kMaxInstances);
    scheduler_.reserve(64);
    color_keys_.reserve(DataModel::kMaxInstances);
    model_.attach_scheduler(&scheduler_);
}

Engine::~Engine() { stop(); }

void Engine::set_renderer(IRenderer* renderer) { renderer_ = renderer; }

void Engine::set_clock(IClock* clock) { clock_ = clock; }

void Engine::set_timing(double render_dt, double physics_dt) {
    render_dt_ = render_dt;
    physics_dt_ = physics_dt;
}

void Engine::set_pace_hz(double hz) { pace_hz_ = hz; }

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
        model_.set_thread_ids(simulation_id_, render_id_);
        model_.set_threads_running(true);
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
    if (simulation_.joinable()) {
        simulation_.join();
    }
    if (render_.joinable()) {
        render_.join();
    }
    model_.set_threads_running(false);
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
            DataModelLock lock(model_, DataModelLock::Write);
            model_.drain_commands();
            scheduler_.run_phase(Phase::PreAnimation, render_dt_);
            // Deferred handlers run on this thread, still under the step lock,
            // after the phase that queued them and before Prepare can copy.
            model_.events().drain();
            accumulator += wall;
            constexpr int kMaxSubsteps = 32;
            while (accumulator >= physics_dt_ && substeps < kMaxSubsteps) {
                scheduler_.run_phase(Phase::PreSimulation, physics_dt_);
                model_.events().drain();
                scheduler_.run_phase(Phase::PhysicsSubstep, physics_dt_);
                model_.events().drain();
                step_physics(physics_dt_);
                scheduler_.run_phase(Phase::PostSimulation, physics_dt_);
                model_.events().drain();
                accumulator -= physics_dt_;
                ++substeps;
            }
            scheduler_.run_phase(Phase::Heartbeat, render_dt_);
            model_.events().drain();
        } catch (const ContractViolation&) {
            contract_count_.fetch_add(1);
        }
        if (model_.take_deferred_violation()) {
            contract_count_.fetch_add(1);
        }
        last_substeps_.store(substeps);
        sim_frames_.fetch_add(1);

        if (pace_hz_ > 0) {
            const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / pace_hz_));
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

    while (running_.load()) {
        const auto frame_start = std::chrono::steady_clock::now();
        bool prepared = false;
        bool saw_contract = false;
        std::uint64_t hold_ns = 0;
        {
            DataModelLock lock(model_, DataModelLock::Write, std::chrono::milliseconds(2));
            if (lock.owns()) {
                const auto hold_start = std::chrono::steady_clock::now();
                pump_.begin_prerender_window(model_);
                // Roblox order inside the pre-draw window: RenderStepped, then PreRender.
                // A failure in one does not skip the other or the copy.
                try {
                    scheduler_.run_phase(Phase::RenderStepped, render_dt_);
                } catch (const ContractViolation&) {
                    saw_contract = true;
                }
                try {
                    scheduler_.run_phase(Phase::PreRender, render_dt_);
                } catch (const ContractViolation&) {
                    saw_contract = true;
                }
                pump_.end_prerender_window(model_);
                // Copy even after a rejected PreRender write. Authorize fails before
                // mutation, so the queue still describes real sim state.
                try {
                    pump_.prepare_copy(model_);
                    prepared = true;
                } catch (const ContractViolation&) {
                    saw_contract = true;
                }
                hold_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - hold_start)
                        .count());
            }
        }
        if (model_.take_deferred_violation()) {
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
                const int batches = batch_colors(pump_.front());
                renderer_->perform(pump_.front(), batches);
                renderer_->present();
            } catch (const ContractViolation&) {
                // Perform is outside the pre-draw window. A DataModel write here is path D.
                contract_count_.fetch_add(1);
            }
        }
        // After Present the snapshot for this frame is already published.
        // PostRender does not hold the Prepare lock and is not part of the 2 ms budget.
        try {
            scheduler_.run_phase(Phase::PostRender, render_dt_);
        } catch (const ContractViolation&) {
            contract_count_.fetch_add(1);
        }
        present_count_.fetch_add(1);

        if (pace_hz_ > 0) {
            const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / pace_hz_));
            const auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < budget) {
                std::this_thread::sleep_for(budget - elapsed);
            }
        }
    }
}

void Engine::step_physics(double dt) { model_.integrate_simulated(dt); }

int Engine::batch_colors(const VisualSnapshot& snapshot) {
    color_keys_.clear();
    for (const VisualInstance& inst : snapshot.instances) {
        if (!inst.alive) {
            continue;
        }
        auto quantize = [](float channel) {
            float clamped = channel;
            if (clamped < 0.f) {
                clamped = 0.f;
            }
            if (clamped > 1.f) {
                clamped = 1.f;
            }
            return static_cast<std::uint32_t>(clamped * 255.f + 0.5f);
        };
        const std::uint32_t key =
            (quantize(inst.color.r) << 16u) | (quantize(inst.color.g) << 8u) | quantize(inst.color.b);
        if (color_keys_.size() == color_keys_.capacity()) {
            break;
        }
        color_keys_.push_back(key);
    }
    std::sort(color_keys_.begin(), color_keys_.end());
    const auto unique_end = std::unique(color_keys_.begin(), color_keys_.end());
    return static_cast<int>(unique_end - color_keys_.begin());
}

}  // namespace engine_core
