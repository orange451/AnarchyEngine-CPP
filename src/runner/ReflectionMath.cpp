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

bool StepHits(float depthA, float depthB, float sceneDepth, float thickness) {
    const float nearest = std::min(depthA, depthB);
    const float farthest = std::max(depthA, depthB);
    return farthest >= sceneDepth && nearest <= sceneDepth + thickness;
}

// Premultiplied, so averaging texels at a silhouette averages their light and
// confidence alike, and the resolve stays linear in them.
ReflectionTexel TraceTexel(float hitColor, float confidence) { return {hitColor * confidence, confidence}; }

float ResolveReflection(float color, float intensity, ReflectionTexel traced, float weight, float skyLight) {
    return std::max(color + intensity * (weight * traced.light - traced.confidence * skyLight), 0.f);
}

bool BisectedHitHolds(float rayDepth, float sceneDepth, float thickness) {
    return rayDepth - sceneDepth <= thickness;
}

}  // namespace runner
