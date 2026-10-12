#pragma once

namespace engine_core {

// Engine thread only. delta_seconds() is the wall step added to the
// physics accumulator.
class IClock {
public:
    virtual ~IClock() = default;
    virtual double delta_seconds() = 0;
};

}  // namespace engine_core
