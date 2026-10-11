#pragma once

namespace engine_core {

class DataModel;

// Play mode's animation step: every Animator in Workspace whose AutoStep is
// true steps by dt. Does nothing while the place is not playing. Engine runs
// it once a frame, after the PreAnimation phase and its event drain.
// SimulationThread.
void step_animators(DataModel& game, double dt);

}  // namespace engine_core
