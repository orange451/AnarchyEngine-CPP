#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

namespace engine_core {

class Signal;

// Cooperative jobs. The engine resumes a phase; jobs must return.
// PreAnimation..Heartbeat run on SimulationThread.
// PreRender runs on RenderThread during the Prepare lock, before the copy.
//
// A job may call Signal::wait(). That parks the job on its own stack and
// resumes it at the start of a later simulation phase. PreRender jobs are
// not parked and cannot wait.
class TaskScheduler {
public:
    using Job = std::function<void(double dt)>;

    void reserve(std::size_t per_phase);
    // Larger priority runs first. The default matches a normal gameplay job.
    void bind(Phase phase, Job job, int priority = 2000);
    void run_phase(Phase phase, double dt);

    Phase current_phase() const { return current_phase_; }
    bool in_job() const;

    // Parks the running simulation job until `signal` fires once.
    void yield_for(Signal& signal);

private:
    friend void ae_fiber_main();
    static void fiber_main();
    enum class JobState { Idle, Running, Suspended, Ready };

    struct Entry {
        int priority = 2000;
        Job job;
        JobState state = JobState::Idle;
        std::uint64_t wait_id = 0;
        double dt = 0;
        std::exception_ptr error;
        void* sp = nullptr;
        std::unique_ptr<std::byte[]> stack;
        void* stack_bottom = nullptr;
        std::size_t stack_bytes = 0;
    };

    void resume_ready();
    void start_job(Entry& entry, double dt);
    void switch_to(Entry& entry);
    void switch_back(Entry& entry);
    void ensure_stack(Entry& entry);
    void arm_stack(Entry& entry);
    void mark_ready(std::uint64_t wait_id);

    static thread_local Entry* tls_entry_;
    static thread_local void* tls_scheduler_sp_;

    std::vector<Entry> jobs_[6];
    std::vector<int> order_[6];
    Phase current_phase_ = Phase::PreAnimation;
    std::uint64_t wait_serial_ = 0;
};

}  // namespace engine_core
