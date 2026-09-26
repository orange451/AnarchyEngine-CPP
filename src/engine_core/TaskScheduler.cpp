#include "TaskScheduler.hpp"

#include "Contract.hpp"
#include "Events.hpp"

#include <algorithm>
#include <numeric>

namespace engine_core {
namespace {

#if defined(__aarch64__)
constexpr std::size_t kFrameBytes = 160;

// Callee-saved GPRs and d8-d15. x30 of a fresh frame is the entry point.
extern "C" __attribute__((naked)) void ae_fiber_swap(void**, void**) {
    __asm__ volatile(
        "stp x19, x20, [sp, #-16]!\n"
        "stp x21, x22, [sp, #-16]!\n"
        "stp x23, x24, [sp, #-16]!\n"
        "stp x25, x26, [sp, #-16]!\n"
        "stp x27, x28, [sp, #-16]!\n"
        "stp x29, x30, [sp, #-16]!\n"
        "stp d8, d9, [sp, #-16]!\n"
        "stp d10, d11, [sp, #-16]!\n"
        "stp d12, d13, [sp, #-16]!\n"
        "stp d14, d15, [sp, #-16]!\n"
        "mov x2, sp\n"
        "str x2, [x0]\n"
        "ldr x2, [x1]\n"
        "mov sp, x2\n"
        "ldp d14, d15, [sp], #16\n"
        "ldp d12, d13, [sp], #16\n"
        "ldp d10, d11, [sp], #16\n"
        "ldp d8, d9, [sp], #16\n"
        "ldp x29, x30, [sp], #16\n"
        "ldp x27, x28, [sp], #16\n"
        "ldp x25, x26, [sp], #16\n"
        "ldp x23, x24, [sp], #16\n"
        "ldp x21, x22, [sp], #16\n"
        "ldp x19, x20, [sp], #16\n"
        "ret\n");
}

void write_frame(void* sp, void (*entry)()) {
    auto* words = static_cast<std::uint64_t*>(sp);
    for (int i = 0; i < 20; ++i) {
        words[i] = 0;
    }
    // Pair 4 is x29, x30. x30 is the high half.
    words[9] = reinterpret_cast<std::uint64_t>(entry);
}

#elif defined(__x86_64__) && !defined(_WIN32)
constexpr std::size_t kFrameBytes = 64;

extern "C" __attribute__((naked)) void ae_fiber_swap(void**, void**) {
    __asm__ volatile(
        "pushq %rbx\n"
        "pushq %rbp\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        "movq %rsp, (%rdi)\n"
        "movq (%rsi), %rsp\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %rbp\n"
        "popq %rbx\n"
        "ret\n");
}

void write_frame(void* sp, void (*entry)()) {
    auto* words = static_cast<std::uint64_t*>(sp);
    words[0] = 0;
    words[1] = 0;
    words[2] = 0;
    words[3] = 0;
    words[4] = 0;
    words[5] = 0;
    words[6] = reinterpret_cast<std::uint64_t>(entry);
}

#else
#define AE_NO_FIBER 1
#endif

}  // namespace

void ae_fiber_main() { TaskScheduler::fiber_main(); }

namespace {

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define AE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define AE_ASAN 1
#endif

#if defined(AE_ASAN)
extern "C" void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
extern "C" void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, std::size_t* size_old);
extern "C" void __asan_unpoison_memory_region(void* addr, std::size_t size);
#endif

}  // namespace

thread_local TaskScheduler::Entry* TaskScheduler::tls_entry_ = nullptr;
thread_local void* TaskScheduler::tls_scheduler_sp_ = nullptr;

namespace {

thread_local Phase tls_phase = Phase::PreAnimation;

bool is_render_phase(Phase phase) {
    return phase == Phase::RenderStepped || phase == Phase::PreRender || phase == Phase::PostRender;
}

const char* render_phase_thread_message(Phase phase) {
    switch (phase) {
    case Phase::RenderStepped:
        return "RenderStepped runs on RenderThread";
    case Phase::PreRender:
        return "PreRender runs on RenderThread";
    case Phase::PostRender:
        return "PostRender runs on RenderThread";
    default:
        return "render phases run on RenderThread";
    }
}

}  // namespace

void TaskScheduler::fiber_main() {
#if defined(AE_NO_FIBER)
    contract_fail("finished simulation job resumed");
#else
    Entry* entry = tls_entry_;
#if defined(AE_ASAN)
    // Scheduler called start_switch before entering this fresh frame.
    __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
    try {
        entry->job(entry->dt);
        entry->state = JobState::Idle;
    } catch (...) {
        entry->error = std::current_exception();
        entry->state = JobState::Idle;
    }
#if defined(AE_ASAN)
    void* fake = nullptr;
    __sanitizer_start_switch_fiber(&fake, nullptr, 0);
#endif
    ae_fiber_swap(&entry->sp, &tls_scheduler_sp_);
#if defined(AE_ASAN)
    __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
    contract_fail("finished simulation job resumed");
#endif
}

Phase TaskScheduler::current_phase() const { return tls_phase; }

void TaskScheduler::reserve(std::size_t per_phase) {
    for (int i = 0; i < kPhaseCount; ++i) {
        jobs_[i].reserve(per_phase);
        order_[i].reserve(per_phase);
    }
}

void TaskScheduler::bind(Phase phase, Job job, int priority) { bind_job(phase, std::move(job), priority, true); }

void TaskScheduler::bind_session(Phase phase, Job job, int priority) {
    bind_job(phase, std::move(job), priority, false);
}

void TaskScheduler::bind_job(Phase phase, Job job, int priority, bool permanent) {
    std::vector<Entry>& list = jobs_[static_cast<int>(phase)];
    std::vector<int>& order = order_[static_cast<int>(phase)];
    if (list.size() == list.capacity()) {
        contract_fail("phase job capacity exhausted");
    }
    const int slot = static_cast<int>(list.size());
    list.push_back(Entry{});
    list.back().priority = priority;
    list.back().permanent = permanent;
    list.back().job = std::move(job);
    order.push_back(slot);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return list[static_cast<std::size_t>(a)].priority > list[static_cast<std::size_t>(b)].priority;
    });
}

void TaskScheduler::cancel_session_jobs() {
    for (int phase = 0; phase < kPhaseCount; ++phase) {
        std::vector<Entry>& list = jobs_[phase];
        bool holds_current = false;
        if (tls_entry_ != nullptr) {
            for (Entry& entry : list) {
                if (&entry == tls_entry_) {
                    holds_current = true;
                    break;
                }
            }
        }
        // Render jobs run without a fiber. One may be inside its std::function on the
        // render thread while stop runs here, so those entries stay put.
        if (holds_current || is_render_phase(static_cast<Phase>(phase))) {
            for (Entry& entry : list) {
                if (entry.permanent) {
                    continue;
                }
                entry.retired = true;
                if (&entry != tls_entry_) {
                    entry.state = JobState::Idle;
                }
            }
            continue;
        }
        const std::size_t capacity = list.capacity();
        std::vector<Entry> kept;
        kept.reserve(capacity);
        for (int slot : order_[phase]) {
            Entry& entry = list[static_cast<std::size_t>(slot)];
            if (entry.permanent && !entry.retired) {
                kept.push_back(std::move(entry));
                continue;
            }
            const bool parked = entry.state == JobState::Running || entry.state == JobState::Suspended ||
                                entry.state == JobState::Ready;
            if (!parked) {
                continue;
            }
            entry.retired = true;
            entry.state = JobState::Idle;
            kept.push_back(std::move(entry));
        }
        list.swap(kept);
        if (list.capacity() < capacity) {
            list.reserve(capacity);
        }
        order_[phase].resize(list.size());
        std::iota(order_[phase].begin(), order_[phase].end(), 0);
    }
}

bool TaskScheduler::in_job() const { return tls_entry_ != nullptr; }

void TaskScheduler::mark_ready(std::uint64_t wait_id) {
    for (std::vector<Entry>& phase : jobs_) {
        for (Entry& entry : phase) {
            if (!entry.retired && entry.wait_id == wait_id && entry.state == JobState::Suspended) {
                entry.state = JobState::Ready;
            }
        }
    }
}

void TaskScheduler::yield_for(Signal& signal) {
    if (tls_entry_ == nullptr || thread_role() != ThreadRole::Simulation) {
        contract_fail("Signal::wait() yields the current simulation job only");
    }
#if defined(AE_NO_FIBER)
    (void)signal;
    contract_fail("Signal::wait() yields the current simulation job only");
#else
    Entry& entry = *tls_entry_;
    entry.wait_id = ++wait_serial_;
    entry.state = JobState::Suspended;
    const std::uint64_t wait_id = entry.wait_id;
    signal.once([this, wait_id](InstanceId, Field) { mark_ready(wait_id); });
    if (entry.state == JobState::Suspended) {
        switch_back(entry);
    }
#endif
}

void TaskScheduler::ensure_stack(Entry& entry) {
#if defined(AE_NO_FIBER)
    (void)entry;
#else
    if (entry.stack) {
        return;
    }
    constexpr std::size_t kBytes = 256 * 1024;
    constexpr std::size_t kAlign = 16;
    entry.stack = std::unique_ptr<std::byte[]>(new std::byte[kBytes + kAlign]);
    const auto raw = reinterpret_cast<std::uintptr_t>(entry.stack.get());
    const auto aligned = (raw + kAlign - 1) & ~(static_cast<std::uintptr_t>(kAlign) - 1);
    entry.stack_bottom = reinterpret_cast<void*>(aligned);
    entry.stack_bytes = kBytes - (aligned - raw);
    entry.stack_bytes &= ~static_cast<std::size_t>(15);
#endif
}

void TaskScheduler::arm_stack(Entry& entry) {
#if defined(AE_NO_FIBER)
    (void)entry;
#else
#if defined(AE_ASAN)
    // A previous visit to this stack poisons the high end as a stack redzone.
    __asan_unpoison_memory_region(entry.stack_bottom, entry.stack_bytes);
#endif
    auto top = reinterpret_cast<std::uintptr_t>(entry.stack_bottom) + entry.stack_bytes;
    top &= ~static_cast<std::uintptr_t>(15);
#if defined(__x86_64__) && !defined(_WIN32)
    // Return address sits 16 bytes below the top so `ret` enters with rsp % 16 == 8.
    top -= 16;
#endif
    void* sp = reinterpret_cast<void*>(top - kFrameBytes);
    write_frame(sp, &ae_fiber_main);
    entry.sp = sp;
#endif
}

void TaskScheduler::switch_to(Entry& entry) {
#if defined(AE_NO_FIBER)
    (void)entry;
#else
    tls_entry_ = &entry;
#if defined(AE_ASAN)
    void* fake = nullptr;
    __sanitizer_start_switch_fiber(&fake, entry.stack_bottom, entry.stack_bytes);
#endif
    ae_fiber_swap(&tls_scheduler_sp_, &entry.sp);
#if defined(AE_ASAN)
    __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
    tls_entry_ = nullptr;
    if (entry.error) {
        std::exception_ptr error = std::move(entry.error);
        std::rethrow_exception(error);
    }
#endif
}

void TaskScheduler::switch_back(Entry& entry) {
#if defined(AE_NO_FIBER)
    (void)entry;
#else
#if defined(AE_ASAN)
    void* fake = nullptr;
    __sanitizer_start_switch_fiber(&fake, nullptr, 0);
#endif
    ae_fiber_swap(&entry.sp, &tls_scheduler_sp_);
#if defined(AE_ASAN)
    __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
#endif
}

void TaskScheduler::start_job(Entry& entry, double dt) {
#if defined(AE_NO_FIBER)
    entry.job(dt);
#else
    ensure_stack(entry);
    arm_stack(entry);
    entry.dt = dt;
    entry.error = nullptr;
    entry.state = JobState::Running;
    switch_to(entry);
#endif
}

void TaskScheduler::resume_ready() {
    for (std::vector<Entry>& phase : jobs_) {
        for (Entry& entry : phase) {
            if (entry.retired || entry.state != JobState::Ready) {
                continue;
            }
            entry.state = JobState::Running;
            switch_to(entry);
        }
    }
}

void TaskScheduler::run_phase(Phase phase, double dt) {
    if (is_render_phase(phase)) {
        if (thread_role() != ThreadRole::Render) {
            contract_fail(render_phase_thread_message(phase));
        }
    } else if (thread_role() != ThreadRole::Simulation) {
        contract_fail("gameplay phases run on SimulationThread");
    }
    tls_phase = phase;
    const int phase_index = static_cast<int>(phase);
    // A parked simulation job resumes on the simulation thread only.
    // Render phases call the job directly and never switch fibers.
    if (!is_render_phase(phase)) {
        resume_ready();
    }
    const std::vector<int>& order = order_[phase_index];
    for (int slot : order) {
        Entry& entry = jobs_[phase_index][static_cast<std::size_t>(slot)];
        if (entry.retired || !entry.job) {
            continue;
        }
        if (is_render_phase(phase)) {
            entry.job(dt);
            continue;
        }
        if (entry.state == JobState::Suspended || entry.state == JobState::Ready) {
            continue;
        }
        start_job(entry, dt);
    }
}

}  // namespace engine_core
