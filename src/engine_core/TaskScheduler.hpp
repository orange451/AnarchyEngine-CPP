#pragma once

#include "types.hpp"

#include <functional>
#include <vector>

namespace engine_core {

// Cooperative jobs. The engine resumes a phase; jobs must return.
// PreAnimation..Heartbeat run on SimulationThread.
// PreRender runs on RenderThread during the Prepare lock, before the copy.
class TaskScheduler {
public:
    using Job = std::function<void(double dt)>;

    void reserve(std::size_t per_phase);
    // Larger priority runs first. The default matches a normal gameplay job.
    void bind(Phase phase, Job job, int priority = 2000);
    void run_phase(Phase phase, double dt);

private:
    struct Entry {
        int priority = 2000;
        Job job;
    };

    std::vector<Entry> jobs_[6];
};

}  // namespace engine_core
