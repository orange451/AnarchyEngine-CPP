#include "OcclusionMath.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

OcclusionQuality QualitySettings(int quality, float pixelsPerPoint) {
    const int reduced = pixelsPerPoint >= kDenseDisplayPixelsPerPoint ? 4 : 2;
    switch (quality) {
        case 0:
            // Fewer slices, the same blur: a wider blur would cost more than the slice saves.
            return {reduced, 2, 4};
        case 2:
            return {1, 3, 4};
        default:
            return {reduced, 3, 4};
    }
}

float PixelRadius(float radius, float viewDepth, float projectionScale, float bufferHeight) {
    return std::min(radius * projectionScale / viewDepth, kOcclusionMaxRadiusFraction * bufferHeight);
}

float Falloff(float distance, float radius) {
    const float range = kOcclusionFalloffRange * radius;
    const float from = radius - range;
    return std::clamp(distance * (-1.f / range) + (from / range + 1.f), 0.f, 1.f);
}

float ArcVisibility(float n, float h0, float h1, float projectedLength) {
    const float arc0 = std::cos(n) + 2.f * h0 * std::sin(n) - std::cos(2.f * h0 - n);
    const float arc1 = std::cos(n) + 2.f * h1 * std::sin(n) - std::cos(2.f * h1 - n);
    return projectedLength * 0.25f * (arc0 + arc1);
}

float MultiBounce(float visibility, float albedo) {
    if (visibility >= 1.f) {
        return 1.f;
    }
    const float a = 2.0404f * albedo - 0.3324f;
    const float b = -4.7951f * albedo + 0.6417f;
    const float c = 2.7552f * albedo + 0.6903f;
    return std::max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

float SpecularOcclusion(float visibility, float NdotV, float roughness) {
    if (visibility >= 1.f) {
        return 1.f;
    }
    return std::clamp(std::pow(NdotV + visibility, std::exp2(-16.f * roughness - 1.f)) - 1.f + visibility, 0.f, 1.f);
}

}  // namespace runner
