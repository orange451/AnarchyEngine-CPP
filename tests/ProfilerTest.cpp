// The profiler's recorder, without the engine: rings, frames, stats, pause,
// and the capture files and report built from a history.

#include "profiler/ProfileStats.hpp"
#include "profiler/Profiler.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

std::atomic<std::uint64_t> gNow{0};

std::uint64_t fake_clock() { return gNow.load(); }

void at(double ms) { gNow.store(static_cast<std::uint64_t>(ms * 1e6)); }

profiler::History live() {
    profiler::History out;
    profiler::with_live([&](const profiler::History& history) { out = history; });
    return out;
}

profiler::History view() {
    profiler::History out;
    profiler::with_view([&](const profiler::History& history) { out = history; });
    return out;
}

int row_of(const profiler::History& history, const char* name) {
    for (std::size_t index = 0; index < history.rows.size(); ++index) {
        if (history.rows[index] == name) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

// The scopes named name across the history, oldest first.
std::vector<profiler::ScopeRecord> named(const profiler::History& history, const char* name) {
    std::vector<profiler::ScopeRecord> out;
    for (const profiler::Frame& frame : history.frames) {
        for (const profiler::ScopeRecord& record : frame.scopes) {
            if (record.scope < history.scopes.size() && history.scopes[record.scope].name == name) {
                out.push_back(record);
            }
        }
    }
    return out;
}

// A thread that keeps its ring for the whole test, running what it is given in order.
class Worker {
public:
    explicit Worker(const char* row) : row_(row), thread_([this] { loop(); }) {}
    ~Worker() {
        run([this] { done_ = true; });
        thread_.join();
    }

    // Runs fn on the worker and waits for it.
    void run(std::function<void()> fn) {
        std::unique_lock<std::mutex> lock(mu_);
        job_ = std::move(fn);
        cv_.notify_all();
        cv_.wait(lock, [this] { return !job_; });
    }

private:
    void loop() {
        profiler::register_thread(row_);
        std::unique_lock<std::mutex> lock(mu_);
        while (true) {
            cv_.wait(lock, [this] { return static_cast<bool>(job_); });
            job_();
            job_ = nullptr;
            cv_.notify_all();
            if (done_) {
                return;
            }
        }
    }

    const char* row_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::function<void()> job_;
    bool done_ = false;
    std::thread thread_;
};

void fresh() {
    profiler::reset_for_testing();
    profiler::set_clock_for_testing(&fake_clock);
    at(0);
}

void testDisabledRecordsNothing() {
    fresh();
    const profiler::ScopeId a = profiler::intern("Off A", profiler::Group::Engine);
    profiler::frame_boundary();
    at(1);
    profiler::begin(a);
    at(2);
    profiler::end();
    at(16);
    profiler::frame_boundary();
    profiler::collect();
    expect(live().frames.empty(), "nothing records while no one has asked");
}

void testNesting() {
    fresh();
    profiler::acquire();
    const profiler::ScopeId a = profiler::intern("Nest A", profiler::Group::Engine);
    const profiler::ScopeId b = profiler::intern("Nest B", profiler::Group::Engine);
    const profiler::ScopeId c = profiler::intern("Nest C", profiler::Group::Engine);
    profiler::frame_boundary();
    at(1);
    profiler::begin(a);
    at(2);
    profiler::begin(b);
    at(3);
    profiler::begin(c);
    at(4);
    profiler::end();
    at(5);
    profiler::end();
    at(6);
    profiler::end();
    at(16);
    profiler::frame_boundary();
    profiler::collect();
    const profiler::History history = live();
    expect(history.frames.size() == 1, "one closed frame");
    const auto as = named(history, "Nest A");
    const auto bs = named(history, "Nest B");
    const auto cs = named(history, "Nest C");
    expect(as.size() == 1 && as[0].depth == 0 && as[0].start_ns == 1000000 && as[0].end_ns == 6000000,
           "the outer scope at depth 0, 1 to 6 ms");
    expect(bs.size() == 1 && bs[0].depth == 1, "the middle scope at depth 1");
    expect(cs.size() == 1 && cs[0].depth == 2 && cs[0].end_ns - cs[0].start_ns == 1000000,
           "the inner scope at depth 2, 1 ms long");
    expect(history.frames[0].start_ns == 0 && history.frames[0].end_ns == 16000000, "the frame spans 0 to 16 ms");
    profiler::release();
}

void testRowsAndOtherThreads() {
    fresh();
    profiler::acquire();
    const profiler::ScopeId step = profiler::intern("Worker step", profiler::Group::Engine);
    Worker sim("Sim");
    profiler::frame_boundary();
    at(10);
    sim.run([&] { profiler::begin(step); });
    at(16);
    profiler::frame_boundary();
    at(20);
    sim.run([] { profiler::end(); });
    at(32);
    profiler::frame_boundary();
    profiler::collect();
    const profiler::History history = live();
    expect(history.rows.size() >= 4 && history.rows[0] == "Sim" && history.rows[1] == "Render" &&
               history.rows[2] == "UI" && history.rows[3] == "GPU",
           "the rows start Sim, Render, UI, GPU");
    expect(history.frames.size() == 2, "two closed frames");
    const auto steps = named(history, "Worker step");
    expect(steps.size() == 1 && steps[0].row == row_of(history, "Sim"), "the step is on the Sim row");
    expect(!history.frames.empty() && history.frames[0].scopes.size() == 1,
           "a scope crossing a boundary is in the frame where it starts");
    profiler::release();
}

void testStats() {
    fresh();
    profiler::acquire();
    const profiler::ScopeId a = profiler::intern("Stat A", profiler::Group::Engine);
    profiler::frame_boundary();
    profiler::begin(a);
    at(2);
    profiler::end();
    at(16);
    profiler::frame_boundary();
    profiler::begin(a);
    at(18);
    profiler::end();
    at(19);
    profiler::begin(a);
    at(21);
    profiler::end();
    at(32);
    profiler::frame_boundary();
    profiler::collect();
    const profiler::History history = live();
    const std::vector<profiler::ScopeStat> stats = profiler::compute_stats(history, history.frames.size() - 1);
    const profiler::ScopeStat* found = nullptr;
    for (const profiler::ScopeStat& stat : stats) {
        if (history.scopes[stat.scope].name == "Stat A") {
            found = &stat;
        }
    }
    expect(found != nullptr, "the scope has a row");
    if (found != nullptr) {
        expect(std::abs(found->max_ms - 4.0) < 1e-9, "max is the frame with 4 ms in it");
        expect(std::abs(found->avg_ms - 3.0) < 1e-9, "avg over the frames is 3 ms");
        expect(std::abs(found->frame_ms - 4.0) < 1e-9, "the selected frame has 4 ms");
        expect(std::abs(found->calls_per_frame - 1.5) < 1e-9, "1.5 calls a frame");
        expect(std::abs(found->share - 3.0 / 16.0) < 1e-9, "3 ms of a 16 ms average frame");
    }
    expect(std::abs(profiler::frame_ms(history.frames[0]) - 16.0) < 1e-9, "a frame's length in ms");
    profiler::release();
}

void testOverflow() {
    fresh();
    profiler::acquire();
    const profiler::ScopeId a = profiler::intern("Flood", profiler::Group::Engine);
    profiler::frame_boundary();
    for (std::size_t index = 0; index < profiler::kRingEvents / 2 + 10; ++index) {
        profiler::begin(a);
        profiler::end();
    }
    at(16);
    profiler::frame_boundary();
    profiler::collect();
    expect(live().dropped >= 20, "events past the ring's size are dropped and counted");
    const profiler::ScopeId b = profiler::intern("After flood", profiler::Group::Engine);
    at(17);
    profiler::begin(b);
    at(18);
    profiler::end();
    at(32);
    profiler::frame_boundary();
    profiler::collect();
    expect(named(live(), "After flood").size() == 1, "the next frame records normally");
    profiler::release();
}

void testUnmatched() {
    fresh();
    const profiler::ScopeId a = profiler::intern("Half", profiler::Group::Engine);
    const profiler::ScopeId b = profiler::intern("Whole", profiler::Group::Engine);
    // Begun while off, ended once on: the end is ignored.
    profiler::begin(a);
    {
        profiler::Scope scope(a);
        profiler::acquire();
    }
    profiler::frame_boundary();
    profiler::end();
    at(1);
    profiler::begin(b);
    at(2);
    profiler::end();
    at(16);
    profiler::frame_boundary();
    profiler::collect();
    const profiler::History history = live();
    expect(named(history, "Half").empty(), "a scope begun while off records nothing");
    expect(named(history, "Whole").size() == 1 && named(history, "Whole")[0].depth == 0,
           "an end with nothing open is ignored");
    profiler::release();
}

void testPause() {
    fresh();
    profiler::acquire();
    for (int frame = 0; frame < 3; ++frame) {
        at(frame * 16.0);
        profiler::frame_boundary();
    }
    profiler::collect();
    profiler::set_paused(true);
    expect(profiler::paused(), "paused");
    for (int frame = 3; frame < 5; ++frame) {
        at(frame * 16.0);
        profiler::frame_boundary();
    }
    profiler::collect();
    expect(view().frames.size() == 2, "the view holds still while paused");
    expect(live().frames.size() == 4, "recording goes on underneath");
    profiler::set_paused(false);
    expect(view().frames.size() == 4, "resuming shows live again");
    profiler::release();
}

void testHistoryCap() {
    fresh();
    profiler::acquire();
    for (std::size_t frame = 0; frame < profiler::kHistoryFrames + 2; ++frame) {
        at(static_cast<double>(frame) * 16.0);
        profiler::frame_boundary();
        if (frame % 50 == 0) {
            profiler::collect();
        }
    }
    profiler::collect();
    const profiler::History history = live();
    expect(history.frames.size() == profiler::kHistoryFrames, "300 frames kept");
    expect(!history.frames.empty() && history.frames.front().start_ns == 16000000, "the oldest dropped first");
    profiler::release();
}

void testNames() {
    fresh();
    const profiler::ScopeId first = profiler::intern_keyed("guid-1", "Mover", profiler::Group::Script);
    const profiler::ScopeId again = profiler::intern_keyed("guid-1", "Runner", profiler::Group::Script);
    expect(first == again, "a Script keeps its id through a rename");
    expect(live().scopes[first].name == "Runner", "and takes the new name");
    expect(profiler::intern("Same", profiler::Group::Engine) == profiler::intern("Same", profiler::Group::Engine),
           "a name interns once");
    const profiler::ScopeId longer = profiler::intern(std::string(100, 'x'), profiler::Group::User);
    expect(live().scopes[longer].name.size() == profiler::kMaxNameBytes, "names are cut to 64 bytes");
    expect(profiler::intern_cause("Heartbeat") == profiler::intern_cause("Heartbeat"), "a cause interns once");
}

void testGpuScope() {
    fresh();
    profiler::acquire();
    const profiler::ScopeId pass = profiler::intern("Shadows", profiler::Group::Gpu);
    profiler::frame_boundary();
    at(16);
    profiler::frame_boundary();
    at(40);
    profiler::gpu_scope(pass, 0, 3000000, 5000000, 2);
    profiler::collect();
    const profiler::History history = live();
    const auto shadows = named(history, "Shadows");
    expect(shadows.size() == 1 && shadows[0].row == row_of(history, "GPU"), "a late GPU scope is on the GPU row");
    expect(!history.frames.empty() && history.frames[0].scopes.size() == 1, "in the frame it ran in");
    expect(history.gpu_lag_frames == 2, "the lag is kept");
    profiler::release();
}

void testMacro() {
    fresh();
    profiler::acquire();
    profiler::frame_boundary();
    at(1);
    {
        PROFILE_SCOPE("Macro scope", profiler::Group::Physics);
        at(3);
    }
    at(16);
    profiler::frame_boundary();
    profiler::collect();
    const profiler::History history = live();
    const auto found = named(history, "Macro scope");
    expect(found.size() == 1 && history.scopes[found[0].scope].group == profiler::Group::Physics,
           "PROFILE_SCOPE records its name and group");
    profiler::release();
}

}  // namespace

int main() {
    profiler::register_thread("Render");
    testDisabledRecordsNothing();
    testNesting();
    testRowsAndOtherThreads();
    testStats();
    testOverflow();
    testUnmatched();
    testPause();
    testHistoryCap();
    testNames();
    testGpuScope();
    testMacro();
    profiler::set_clock_for_testing(nullptr);
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return EXIT_FAILURE;
    }
    std::printf("ok\n");
    return EXIT_SUCCESS;
}
