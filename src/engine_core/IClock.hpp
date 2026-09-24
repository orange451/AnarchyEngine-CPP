#pragma once

namespace engine_core {

// SimulationThread only. delta_seconds() is the wall step added to the
// physics accumulator. RenderThread does not read this.
class IClock {
public:
    virtual ~IClock() = default;
    virtual double delta_seconds() = 0;
};

}  // namespace engine_core
