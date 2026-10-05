#include "ReflectionMath.hpp"

#include <algorithm>
#include <cmath>

namespace runner {
namespace {

// GLSL's smoothstep.
float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

}  // namespace

float EdgeFade(float u, float v) {
    const auto axis = [](float x) { return SmoothStep(0.f, 0.1f, x) * SmoothStep(0.f, 0.1f, 1.f - x); };
    return axis(u) * axis(v);
}

float DistanceFade(float distance, float maxDistance) {
    if (!(maxDistance > 0.f)) {
        return 0.f;
    }
    return 1.f - SmoothStep(0.75f * maxDistance, maxDistance, distance);
}

float RoughnessFade(float roughness, float maxRoughness) {
    if (!(maxRoughness > 0.f)) {
        return 0.f;
    }
    return 1.f - SmoothStep(0.8f * maxRoughness, maxRoughness, roughness);
}

float FacingFade(float reflectedZ) { return 1.f - SmoothStep(0.f, 0.5f, reflectedZ); }

float ConeLevel(float roughness, float hitDistance, float pixelsPerUnit, int levels) {
    const float radius = roughness * hitDistance * pixelsPerUnit;
    if (!(radius > 1.f)) {
        return 0.f;
    }
    return std::min(std::log2(radius), static_cast<float>(levels - 1));
}

}  // namespace runner
