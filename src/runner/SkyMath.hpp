#pragma once

// The DynamicSky's sun, moon, stars, light, and lighting refreshes, worked
// out with no GL context so they can be tested. procedural_sky.glsl copies
// the atmosphere's numbers. World space: +Y up, north -Z, east +X; a
// direction points toward the body.
namespace runner {

// The moon's light is the sun's Brightness times this.
constexpr float kMoonBrightnessScale = 0.1f;
// Full, dense cloud takes this much of the light away.
constexpr float kCloudDimming = 0.7f;
// Each disc's radiance before the air reddens it, linear.
constexpr float kSunDiscRadiance = 40.f;
constexpr float kMoonDiscRadiance = 1.5f;
constexpr float kMoonTint[3] = {0.75f, 0.82f, 1.f};
// Units from the camera the sky's light shadows, as a DirectionalLight's default.
constexpr float kSkyShadowDistance = 100.f;
// The lighting cube: no sooner after a change, and this often while clouds drift.
constexpr double kLightingChangeSeconds = 0.05;
constexpr double kLightingDriftSeconds = 0.25;
// Rayleigh (per meter, sea level) and Mie extinction, and their scale heights
// in meters, as procedural_sky.glsl has them.
constexpr float kRayleigh[3] = {5.5e-6f, 13.0e-6f, 22.4e-6f};
constexpr float kRayleighHeight = 8e3f;
constexpr float kMie = 21e-6f;
constexpr float kMieHeight = 1.2e3f;

struct SkyVector {
    float x = 0.f;
    float y = 1.f;
    float z = 0.f;
};

// Toward the sun at timeOfDay hours (0 to 24) and latitudeDegrees, at the
// equinox: east at 6, highest at 12 (90 - |latitude| up, south for a
// northern latitude), west at 18.
SkyVector SunDirection(double timeOfDay, double latitudeDegrees);
// Column-major: world directions into the stars' frame, which turns with the
// hour. The sun is always (0, 1, 0) in it and the celestial pole (0, 0, -1).
void StarFrame(double timeOfDay, double latitudeDegrees, float out[9]);
// Degrees above the horizon, -90 to 90.
float ElevationDegrees(SkyVector toward);
// How much a body above the horizon lights the scene: 0 at or below 0
// degrees, 1 from 6, smooth between.
float BodyFade(float elevationDegrees);
// How much the stars show: 0 with the sun above -2 degrees, 1 below -12.
float StarVisibility(float sunElevationDegrees);
// How much of a body's light reaches the ground through the air at this
// elevation, per channel, relative to straight overhead (1, 1, 1). Below the
// horizon is the horizon's.
void Transmittance(float elevationDegrees, float out[3]);
// 1 - kCloudDimming * cover * density.
float CloudDimming(float cover, float density);

// The sky's directional light.
struct SkyLight {
    SkyVector toward;
    // Linear, at most 1 per channel.
    float color[3] = {1.f, 1.f, 1.f};
    float intensity = 0.f;
    bool fromSun = true;
};

struct SkyState {
    SkyVector sun;
    SkyVector moon{0.f, -1.f, 0.f};
    float starFrame[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    float starVisibility = 0.f;
    // From the sun while it is above the horizon, the moon otherwise.
    SkyLight light;
    // Each disc's linear radiance, reddened by the air; 0 once its center is 1 degree below
    // the horizon, by which point a 2-degree disc has sunk behind the ground.
    float sunColor[3] = {0.f, 0.f, 0.f};
    float moonColor[3] = {0.f, 0.f, 0.f};
};

SkyState ComputeSky(double timeOfDay, double latitudeDegrees, double brightness, double cloudCover,
                    double cloudDensity);

// The lighting cube's face widths, in texels, for Enum.EffectQuality's
// value: Low 0, Medium 1, High 2. Anything else is Medium.
struct EnvironmentSizes {
    int environment = 256;
    int prefiltered = 256;
};
EnvironmentSizes EnvironmentSizesFor(int quality);

// What the lighting cube is drawn from.
struct SkyLightingKey {
    float timeOfDay = 0.f;
    float latitude = 0.f;
    float cloudCover = 0.f;
    float cloudDensity = 0.f;
    int quality = 1;
};
bool operator==(const SkyLightingKey& a, const SkyLightingKey& b);
inline bool operator!=(const SkyLightingKey& a, const SkyLightingKey& b) { return !(a == b); }

// Whether to draw the lighting cube again now. made is what it was drawn
// from, at madeSeconds; everMade is false before the first.
bool LightingDue(const SkyLightingKey& made, const SkyLightingKey& now, double madeSeconds, double nowSeconds,
                 bool windy, bool everMade);

// A redraw of the lighting cube spread over frames, so that no one frame pays
// for all of it: slices 0 to kLightingSlices - 1, one a frame, in order. A
// slice draws the sky's faces [faceBegin, faceEnd); then, with diffuse, the
// environment cube's mipmap and the irradiance; then the prefiltered mips
// whose bits are set in levels. Any other slice does nothing.
constexpr int kLightingSlices = 5;
struct LightingSlice {
    int faceBegin = 0;
    int faceEnd = 0;
    bool diffuse = false;
    unsigned levels = 0;
};
LightingSlice LightingSliceAt(int slice);

}  // namespace runner
