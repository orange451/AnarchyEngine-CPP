#pragma once

namespace engine_core {

// A script sees this as a Color: a table with r, g, b, and a.
struct ColorRgb {
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;
};

}  // namespace engine_core
