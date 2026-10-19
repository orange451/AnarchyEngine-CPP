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
    if (distance <= 0.f || sigma0 <= 0.f) {
        return 1.f;
    }
    // (1 - exp(-y)) / y for y = |falloff * dy * distance|: how the air thins
    // over the ray, as a share of the start's, 0 to 1, never overflowing.
    const float x = falloff * dy * distance;
    const float y = std::fabs(x);
    const float shape = y < kAtmosphereSeriesBelow ? 1.f - 0.5f * y : -std::expm1(-y) / y;
    // The air along the ray, as a log, so no one part overflows or underflows
    // on its own: the air where the ray starts, its length, its shape, and,
    // climbing down, the exp(|x|) the air thickens by.
    const float logDepth = std::log(sigma0 * distance) - falloff * heightAboveBase + std::max(-x, 0.f) + std::log(shape);
    const float depth = std::exp(std::min(logDepth, std::log(kAtmosphereMaxDepth)));
    return std::exp(-depth);
}

float AtmosphereSunLobe(float cosAngle) {
    const float c = std::max(cosAngle, 0.f);
    const float c2 = c * c;
    const float c4 = c2 * c2;
    return c4 * c4;
}

float AtmosphereSkyLod(float transmittance, float maxLod) { return maxLod * transmittance; }

}  // namespace runner
