#include "runner/UiFrameProfile.hpp"

#include "profiler/Profiler.hpp"

#include "jadefx/stage/Stage.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int gFailures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
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

// The scope named name in frame, or nullptr.
const profiler::ScopeRecord* find(const profiler::History& history, const profiler::Frame& frame,
                                  const std::string& name) {
    for (const profiler::ScopeRecord& record : frame.scopes) {
        if (record.scope < history.scopes.size() && history.scopes[record.scope].name == name) {
            return &record;
        }
    }
    return nullptr;
}

double ms(const profiler::ScopeRecord& record) { return static_cast<double>(record.end_ns - record.start_ns) * 1e-6; }

}  // namespace

// The window's frame phases land on the UI row as scopes named for what the
// thread was doing, so the time between two Scene View paints is accounted for.
int RunUiFrameProfileTests() {
    gFailures = 0;
    profiler::reset_for_testing();
    profiler::set_clock_for_testing(&fake_clock);
    at(0);
    jadefx::Stage stage;
    runner::profile_ui_frames(stage);
    profiler::acquire();

    using jadefx::FramePhase;
    profiler::frame_boundary();
    at(1);
    stage.notePhase(FramePhase::Layout, true);
    at(3);
    stage.notePhase(FramePhase::Layout, false);
    stage.notePhase(FramePhase::Render, true);
    at(4);
    stage.notePhase(FramePhase::Render, false);
    at(8);
    stage.notePhase(FramePhase::Wait, true);
    at(12);
    stage.notePhase(FramePhase::Wait, false);
    at(16);
    profiler::frame_boundary();
    profiler::collect();
    {
        const profiler::History history = live();
        expect(history.frames.size() == 1, "one frame of window phases");
        if (!history.frames.empty()) {
            const profiler::Frame& frame = history.frames.front();
            const profiler::ScopeRecord* layout = find(history, frame, "Styles and layout");
            const profiler::ScopeRecord* paint = find(history, frame, "UI paint");
            const profiler::ScopeRecord* wait = find(history, frame, "Frame wait");
            expect(layout != nullptr && ms(*layout) == 2.0, "the layout phase is a 2 ms scope");
            expect(paint != nullptr && ms(*paint) == 1.0, "the paint phase is a 1 ms scope");
            expect(wait != nullptr && ms(*wait) == 4.0, "the wait for the next frame is a 4 ms scope");
            expect(layout != nullptr && layout->row < history.rows.size() && history.rows[layout->row] == "UI",
                   "the phases are on the UI row");
            expect(paint != nullptr && history.scopes[paint->scope].group == profiler::Group::Render,
                   "the paint is a Render scope");
            expect(layout != nullptr && layout->depth == 0 && paint != nullptr && paint->depth == 0,
                   "the phases follow one another at the top of the row");
        }
    }

    // Recording turned on inside a phase: that phase's end closes nothing, so a
    // scope begun after the recording started runs its full length.
    profiler::release();
    at(20);
    profiler::frame_boundary();
    at(21);
    stage.notePhase(FramePhase::Events, true);
    profiler::acquire();
    profiler::frame_boundary();
    const profiler::ScopeId outer = profiler::intern("Outer", profiler::Group::Engine);
    at(22);
    profiler::begin(outer);
    at(23);
    stage.notePhase(FramePhase::Events, false);
    at(26);
    profiler::end();
    at(36);
    profiler::frame_boundary();
    profiler::collect();
    {
        const profiler::History history = live();
        expect(!history.frames.empty(), "a frame after recording resumed");
        if (!history.frames.empty()) {
            const profiler::Frame& frame = history.frames.back();
            const profiler::ScopeRecord* record = find(history, frame, "Outer");
            expect(record != nullptr && ms(*record) == 4.0, "a phase begun while off does not end the scope begun after");
            expect(find(history, frame, "Input and tasks") == nullptr, "and records nothing itself");
        }
    }

    profiler::release();
    profiler::reset_for_testing();
    profiler::set_clock_for_testing(nullptr);
    return gFailures;
}
