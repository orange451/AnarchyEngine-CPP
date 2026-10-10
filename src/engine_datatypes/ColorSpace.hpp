#pragma once

#include <cmath>

namespace engine_core {

// The sRGB transfer curve, exactly as surface.glsl's toLinear: a color as a
// picker or an image holds it, made linear for lighting. Not a 2.2 power,
// so darks are not crushed. Any Color3 shown to a person is sRGB; anything
// multiplied into light must go through this first.
// Lighting.Ambient is not decoded yet; see docs/reviews/2026-10-10-code-audit.md.
inline float srgb_to_linear(float c) {
    if (c <= 0.f) {
        return 0.f;
    }
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

}  // namespace engine_core
