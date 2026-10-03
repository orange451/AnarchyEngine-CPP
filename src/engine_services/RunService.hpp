#pragma once

#include "Events.hpp"
#include "types.hpp"

#include <atomic>
#include <cstdint>

namespace engine_core {

// game:GetService("RunService"). The phases a script can connect to, each a
// host-owned Signal fired with that step's dt. PreRender is declared to scripts
// but blocked, so it has no signal.
//
// RenderStepped is the engine phase on RenderThread, but its script signal fires
// on SimulationThread: the render job only counts the frame's time with
// note_frame, and the next step fires the signal once for the frames drawn since,
// with their summed time. Scripts never run on RenderThread.
//
// SimulationThread is the only caller, except of note_frame and drop_frames. Between bind and
// release the signals belong to one world's EventQueue, and they must outlive that use.
class RunService {
public:
    RunService() = default;
    RunService(const RunService&) = delete;
    RunService& operator=(const RunService&) = delete;

    void bind(EventQueue& events);
    // Does nothing unless bound.
    void release(EventQueue& events);

    // Null for a phase scripts cannot connect to.
    Signal* signal(Phase phase);
    // Queues this phase's signal with dt. Nothing when the phase has no bound signal.
    void fire(EventQueue& events, Phase phase, double dt);
    // The dt this phase was fired with last. A listener reads it when the queue
    // delivers, which can be after another phase has fired.
    double dt(Phase phase) const { return dt_[static_cast<int>(phase)]; }

    // A rendered frame of dt seconds. RenderThread calls it from its RenderStepped job.
    void note_frame(double dt);
    // Fires RenderStepped once for the frames noted since the last call, with
    // their time summed. Nothing when no frame was drawn.
    void fire_render_stepped(EventQueue& events);
    // Started fires when a play session opens, after the place is captured;
    // Stopped after Stop has restored it. Neither carries arguments.
    Signal* started() { return bound_ ? &started_ : nullptr; }
    Signal* stopped() { return bound_ ? &stopped_ : nullptr; }
    // Forgets the frames noted so far. Any thread may call it.
    void drop_frames() { frame_ns_.store(0, std::memory_order_relaxed); }

private:
    Signal pre_animation_;
    Signal pre_simulation_;
    Signal post_simulation_;
    Signal heartbeat_;
    Signal render_stepped_;
    Signal started_;
    Signal stopped_;
    double dt_[kPhaseCount] = {};
    // Nanoseconds of the frames noted and not yet fired. A frame counts at least 1,
    // so a frame noted with dt 0 is still a frame.
    std::atomic<std::uint64_t> frame_ns_{0};
    bool bound_ = false;
};

}  // namespace engine_core
