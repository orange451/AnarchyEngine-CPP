#pragma once

// The Atmosphere's air, worked out with no GL context so it can be tested.
// atmosphere.glsl's atmosphereAt is these, line for line.
namespace engine_core {
struct VisualAtmosphere;
}

namespace runner {

// The Atmosphere, as the renderer reads it. The defaults draw no air.
struct SceneAtmosphere {
    // True only for an Atmosphere that is there, Enabled, and of Density above 0.
    bool enabled = false;
    float density = 0.25f;
    // Per unit.
    float heightFalloff = 0.01f;
    // World Y.
    float baseHeight = 0.f;
    // As the Color3 holds it (sRGB).
    float color[3] = {0.75f, 0.8f, 0.88f};
    float aerialPerspective = 0.5f;
    float sunScatter = 0.25f;
    float skyHaze = 1.f;
};

// The render snapshot's row, as the renderer reads it.
SceneAtmosphere ToSceneAtmosphere(const engine_core::VisualAtmosphere& row);

// The air's extinction per unit at Density 1, at BaseHeight. Tuned on Bald
// Eagle Valley: the default Density reads as a clear day, distant ridges hazed.
constexpr float kAtmosphereDensityScale = 0.016f;
// How far away the sky is taken to be, in units, when the air hazes it.
constexpr float kAtmosphereSkyDistance = 20000.f;
// The height term's exponent never goes past this, so a camera far below
// BaseHeight sees thick air, not an overflow.
constexpr float kAtmosphereMaxExponent = 80.f;
// Below this |falloff * dy * distance| the closed form's 0 / 0 is its series.
constexpr float kAtmosphereSeriesBelow = 1e-4f;

// How much light gets through distance units of air along a ray, 0 to 1. The
// air is sigma0 * exp(-falloff * y) thick at y units above BaseHeight; the
// ray starts heightAboveBase above it and climbs dy per unit (the world Y of
// its unit direction). Distance 0 is exactly 1.
float AtmosphereTransmittance(float sigma0, float falloff, float heightAboveBase, float dy, float distance);

// The glow toward the sun, 0 to 1, from the cosine between the ray and the
// way to the sun: pow(max(cosAngle, 0), 8).
float AtmosphereSunLobe(float cosAngle);

// The sky cube's mip the air takes its color from: the blurriest (maxLod)
// for air close by that lets most light through, the sharpest for distant air.
float AtmosphereSkyLod(float transmittance, float maxLod);

}  // namespace runner
