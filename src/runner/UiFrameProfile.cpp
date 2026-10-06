#include "UiFrameProfile.hpp"

#include "profiler/Profiler.hpp"

#include "jadefx/stage/Stage.hpp"

#include <array>
#include <cstddef>

namespace runner {

namespace {

profiler::ScopeId phase_scope(jadefx::FramePhase phase) {
    using profiler::Group;
    static const profiler::ScopeId kWait = profiler::intern("Frame wait", Group::Engine);
    static const profiler::ScopeId kPoll = profiler::intern("OS events", Group::Engine);
    static const profiler::ScopeId kEvents = profiler::intern("Input and tasks", Group::Engine);
    static const profiler::ScopeId kLayout = profiler::intern("Styles and layout", Group::Engine);
    static const profiler::ScopeId kRender = profiler::intern("UI paint", Group::Render);
    static const profiler::ScopeId kTail = profiler::intern("Frame tail", Group::Engine);
    static const profiler::ScopeId kSwap = profiler::intern("Swap", Group::Render);
    switch (phase) {
        case jadefx::FramePhase::Wait:
            return kWait;
        case jadefx::FramePhase::Poll:
            return kPoll;
        case jadefx::FramePhase::Events:
            return kEvents;
        case jadefx::FramePhase::Layout:
            return kLayout;
        case jadefx::FramePhase::Render:
            return kRender;
        case jadefx::FramePhase::Tail:
            return kTail;
        case jadefx::FramePhase::Swap:
            return kSwap;
    }
    return kWait;
}

constexpr std::size_t kPhaseCount = static_cast<std::size_t>(jadefx::FramePhase::Swap) + 1;

}  // namespace

void register_ui_thread() {
    static thread_local bool registered = false;
    if (!registered) {
        profiler::register_thread("UI");
        registered = true;
    }
}

void profile_ui_frames(jadefx::Stage& stage) {
    register_ui_thread();
    // As profiler::Scope does: a phase begun while recording was off ends
    // without an end(), so recording turned on inside it, as the profiler's key
    // is in the Events phase, does not end a scope another begin opened.
    stage.setFramePhaseHook([open = std::array<bool, kPhaseCount>{}](jadefx::FramePhase phase, bool begin) mutable {
        const std::size_t at = static_cast<std::size_t>(phase);
        if (at >= kPhaseCount) {
            return;
        }
        if (begin) {
            open[at] = profiler::enabled();
            if (open[at]) {
                profiler::begin(phase_scope(phase));
            }
        } else if (open[at]) {
            open[at] = false;
            profiler::end();
        }
    });
}

}  // namespace runner
