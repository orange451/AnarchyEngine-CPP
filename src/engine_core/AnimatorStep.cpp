#include "AnimatorStep.hpp"

#include "Animator.hpp"
#include "DataModel.hpp"

#include <vector>

namespace engine_core {

void step_animators(DataModel& game, double dt) {
    if (!game.simulation_running()) {
        return;
    }
    // Kept between frames, so a step allocates only while the count grows.
    static thread_local std::vector<InstanceId> ids;
    game.animators(ids);
    for (const InstanceId id : ids) {
        auto* animator = dynamic_cast<Animator*>(game.instance(id));
        if (animator != nullptr && animator->auto_step()) {
            animator->step(dt);
        }
    }
}

}  // namespace engine_core
