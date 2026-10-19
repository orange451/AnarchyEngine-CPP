#include "AtmosphereMath.hpp"

#include "SnapshotPump.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

SceneAtmosphere ToSceneAtmosphere(const engine_core::VisualAtmosphere& row) {
    SceneAtmosphere out;
    out.enabled = row.present && row.enabled && row.density > 0.f;
    out.density = row.density;
    out.heightFalloff = row.height_falloff;
    out.baseHeight = row.base_height;
    out.color[0] = row.color.r;
    out.color[1] = row.color.g;
    out.color[2] = row.color.b;
    out.aerialPerspective = row.aerial_perspective;
    out.sunScatter = row.sun_scatter;
    out.skyHaze = row.sky_haze;
    return out;
}

float AtmosphereTransmittance(float sigma0, float falloff, float heightAboveBase, float dy, float distance) {
    if (distance <= 0.f) {
        return 1.f;
    }
    // The air where the ray starts, along the ray's first unit.
    const float start = sigma0 * std::exp(std::min(-falloff * heightAboveBase, kAtmosphereMaxExponent));
    // (1 - exp(-x)) / x: how the air thins or thickens over the ray, as a share of the start's.
    const float x = falloff * dy * distance;
    float spread;
    if (std::fabs(x) < kAtmosphereSeriesBelow) {
        spread = 1.f - 0.5f * x;
    } else {
        // Climbing down far enough, exp(-x) overflows; the air there is past all light anyway.
        spread = -std::expm1(std::min(-x, kAtmosphereMaxExponent)) / x;
    }
    const float depth = start * distance * spread;
    return std::exp(-std::min(depth, kAtmosphereMaxExponent));
}

float AtmosphereSunLobe(float cosAngle) {
    const float c = std::max(cosAngle, 0.f);
    const float c2 = c * c;
    const float c4 = c2 * c2;
    return c4 * c4;
}

float AtmosphereSkyLod(float transmittance, float maxLod) { return maxLod * transmittance; }

}  // namespace runner
