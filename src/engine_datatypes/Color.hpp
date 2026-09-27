#pragma once

namespace engine_core {

// Engine storage. A script sees GameObject.Color as a Color3, which has no alpha.
struct ColorRgb {
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;
};

}  // namespace engine_core
