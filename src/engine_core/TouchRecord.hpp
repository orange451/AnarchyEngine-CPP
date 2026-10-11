#pragma once

#include "types.hpp"

namespace engine_core {

// What a TouchObject says (docs/superpowers/specs/2026-10-11-touched-design.md
// §1): when, with what, where, which way (away from the body it was given
// to), and how fast the other was moving relative to it there.
struct TouchRecord {
    double time = 0.0;
    InstanceId instance = 0;
    Vec3 point{};
    Vec3 normal{};
    Vec3 relative_velocity{};
};

}  // namespace engine_core
