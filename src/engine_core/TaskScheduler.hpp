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
// RenderStepped and PreRender run on RenderThread during the Prepare lock,
// before the copy. PostRender runs on RenderThread after Present, lock down.
//
// A job may call Signal::wait(). That parks the job on its own stack and
// resumes it at the start of a later simulation phase. Render-thread jobs
// are not parked and cannot wait.
class TaskScheduler {
public:
    using Job = std::function<void(double dt)>;

    void reserve(std::size_t per_phase);
    // Larger priority runs first. The default matches a normal gameplay job.
    // bind() is engine-permanent: stop_simulation leaves it in place.
    void bind(Phase phase, Job job, int priority = 2000);
    // Dropped by stop_simulation. A script job uses this, not bind().
    void bind_session(Phase phase, Job job, int priority = 2000);
    void cancel_session_jobs();
    void run_phase(Phase phase, double dt);

    // The phase this thread is inside. Render and simulation overlap after
    // Present, so this is not a single engine-wide value.
    Phase current_phase() const;
    bool in_job() const;

    // Parks the running simulation job until `signal` fires once.
    void yield_for(Signal& signal);
    // Whether yield_for can park a job. The fiber switch is written for arm64
    // and for x86-64 outside Windows. Elsewhere it is a contract failure.
    static bool can_suspend();

private:
    friend void ae_fiber_main();
    static void fiber_main();
    enum class JobState { Idle, Running, Suspended, Ready };

    struct Entry {
        int priority = 2000;
        bool permanent = true;
        // Session jobs cancelled while parked stay here so their stack is not freed
        // under a suspended frame. run_phase skips them.
        bool retired = false;
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

    void bind_job(Phase phase, Job job, int priority, bool permanent);

    void resume_ready();
    void start_job(Entry& entry, double dt);
    void switch_to(Entry& entry);
    void switch_back(Entry& entry);
    void ensure_stack(Entry& entry);
    void arm_stack(Entry& entry);
    void mark_ready(std::uint64_t wait_id);

    static thread_local Entry* tls_entry_;
    static thread_local void* tls_scheduler_sp_;

    // Each entry has its own allocation. A parked job's frame refers to its
    // entry, so cancel_session_jobs can rebuild a list without moving one.
    std::vector<std::unique_ptr<Entry>> jobs_[kPhaseCount];
    std::vector<int> order_[kPhaseCount];
    std::uint64_t wait_serial_ = 0;
};

}  // namespace engine_core
