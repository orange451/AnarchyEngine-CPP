#include "TaskScheduler.hpp"

#include "Contract.hpp"

#include <algorithm>

namespace engine_core {

void TaskScheduler::reserve(std::size_t per_phase) {
    for (std::vector<Entry>& phase : jobs_) {
        phase.reserve(per_phase);
    }
}

void TaskScheduler::bind(Phase phase, Job job, int priority) {
    std::vector<Entry>& list = jobs_[static_cast<int>(phase)];
    if (list.size() == list.capacity()) {
        contract_fail("phase job capacity exhausted");
    }
    list.push_back(Entry{priority, std::move(job)});
    std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
        return a.priority > b.priority;
    });
}

void TaskScheduler::run_phase(Phase phase, double dt) {
    if (phase == Phase::PreRender) {
        if (thread_role() != ThreadRole::Render) {
            contract_fail("PreRender runs on RenderThread");
        }
    } else if (thread_role() != ThreadRole::Simulation) {
        contract_fail("gameplay phases run on SimulationThread");
    }
    for (Entry& entry : jobs_[static_cast<int>(phase)]) {
        entry.job(dt);
    }
}

}  // namespace engine_core
