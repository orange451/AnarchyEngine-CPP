#pragma once

#include "Events.hpp"
#include "types.hpp"

namespace engine_core {

// game:GetService("RunService"). The simulation phases a script can connect to,
// each a host-owned Signal fired with that step's dt. PreRender and
// RenderStepped are declared to scripts but blocked, so they have no signal.
//
// SimulationThread is the only caller. Between bind and release the signals
// belong to one world's EventQueue, and they must outlive that use.
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
    // The dt of the phase fired last. A listener reads it when the queue delivers.
    double dt() const { return dt_; }

private:
    Signal pre_animation_;
    Signal pre_simulation_;
    Signal post_simulation_;
    Signal heartbeat_;
    double dt_ = 0;
    bool bound_ = false;
};

}  // namespace engine_core
