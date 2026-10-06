#include "SkyMath.hpp"

#include <algorithm>
#include <cmath>

namespace runner {
namespace {

constexpr double kDegrees = 3.14159265358979323846 / 180.0;

float Smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

}  // namespace

SkyVector SunDirection(double timeOfDay, double latitudeDegrees) {
    const double hour = (timeOfDay - 12.0) * 15.0 * kDegrees;
    const double phi = latitudeDegrees * kDegrees;
    // Around the celestial equator, then tilted about X by the latitude.
    return {static_cast<float>(-std::sin(hour)), static_cast<float>(std::cos(hour) * std::cos(phi)),
            static_cast<float>(std::cos(hour) * std::sin(phi))};
}

void StarFrame(double timeOfDay, double latitudeDegrees, float out[9]) {
    const double hour = (timeOfDay - 12.0) * 15.0 * kDegrees;
    const double phi = latitudeDegrees * kDegrees;
    const double ch = std::cos(hour);
    const double sh = std::sin(hour);
    const double cp = std::cos(phi);
    const double sp = std::sin(phi);
    // The latitude's tilt undone, then the hour's turn about the pole undone.
    const double rows[3][3] = {{ch, sh * cp, sh * sp}, {-sh, ch * cp, ch * sp}, {0.0, -sp, cp}};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            out[column * 3 + row] = static_cast<float>(rows[row][column]);
        }
    }
}

float ElevationDegrees(SkyVector toward) {
    const float length = std::sqrt(toward.x * toward.x + toward.y * toward.y + toward.z * toward.z);
    if (!(length > 0.f)) {
        return 0.f;
    }
    return static_cast<float>(std::asin(std::clamp(toward.y / length, -1.f, 1.f)) / kDegrees);
}

float BodyFade(float elevationDegrees) { return Smoothstep(0.f, 6.f, elevationDegrees); }

float StarVisibility(float sunElevationDegrees) { return Smoothstep(-2.f, -12.f, sunElevationDegrees); }

void Transmittance(float elevationDegrees, float out[3]) {
    // Kasten and Young's air mass (1989), relative to straight overhead.
    const double zenith = 90.0 - std::clamp(static_cast<double>(elevationDegrees), 0.0, 90.0);
    const double mass =
        1.0 / (std::cos(zenith * kDegrees) + 0.50572 * std::pow(96.07995 - zenith, -1.6364));
    const double overhead = 1.0 / (1.0 + 0.50572 * std::pow(96.07995, -1.6364));
    const double extra = mass - overhead;
    for (int channel = 0; channel < 3; ++channel) {
        const double depth = static_cast<double>(kRayleigh[channel]) * kRayleighHeight +
                             static_cast<double>(kMie) * 1.1 * kMieHeight;
        out[channel] = static_cast<float>(std::exp(-depth * extra));
    }
}

float CloudDimming(float cover, float density) {
    return 1.f - kCloudDimming * std::clamp(cover, 0.f, 1.f) * std::clamp(density, 0.f, 1.f);
}

SkyState ComputeSky(double timeOfDay, double latitudeDegrees, double brightness, double cloudCover,
                    double cloudDensity) {
    SkyState state;
    state.sun = SunDirection(timeOfDay, latitudeDegrees);
    state.moon = {-state.sun.x, -state.sun.y, -state.sun.z};
    StarFrame(timeOfDay, latitudeDegrees, state.starFrame);
    const float sunUp = ElevationDegrees(state.sun);
    const float moonUp = -sunUp;
    state.starVisibility = StarVisibility(sunUp);
    const float dimming = CloudDimming(static_cast<float>(cloudCover), static_cast<float>(cloudDensity));
    const float strength = static_cast<float>(std::max(brightness, 0.0));

    float sunAir[3];
    float moonAir[3];
    Transmittance(sunUp, sunAir);
    Transmittance(moonUp, moonAir);
    for (int channel = 0; channel < 3; ++channel) {
        state.sunColor[channel] = sunUp > -1.f ? sunAir[channel] * kSunDiscRadiance : 0.f;
        state.moonColor[channel] = moonUp > -1.f ? moonAir[channel] * kMoonTint[channel] * kMoonDiscRadiance : 0.f;
    }

    // Both fade to nothing as they cross the horizon together, so the light never jumps.
    SkyLight& light = state.light;
    if (sunUp > 0.f) {
        light.fromSun = true;
        light.toward = state.sun;
        std::copy(sunAir, sunAir + 3, light.color);
        light.intensity = strength * BodyFade(sunUp) * dimming;
    } else {
        light.fromSun = false;
        light.toward = state.moon;
        for (int channel = 0; channel < 3; ++channel) {
            light.color[channel] = moonAir[channel] * kMoonTint[channel];
        }
        light.intensity = strength * kMoonBrightnessScale * BodyFade(moonUp) * dimming;
    }
    return state;
}

EnvironmentSizes EnvironmentSizesFor(int quality) {
    if (quality == 0) {
        return {128, 128};
    }
    if (quality == 2) {
        return {512, 512};
    }
    return {256, 256};
}

bool operator==(const SkyLightingKey& a, const SkyLightingKey& b) {
    return a.timeOfDay == b.timeOfDay && a.latitude == b.latitude && a.cloudCover == b.cloudCover &&
           a.cloudDensity == b.cloudDensity && a.quality == b.quality;
}

bool LightingDue(const SkyLightingKey& made, const SkyLightingKey& now, double madeSeconds, double nowSeconds,
                 bool windy, bool everMade) {
    if (!everMade || made.quality != now.quality) {
        return true;
    }
    const double since = nowSeconds - madeSeconds;
    if (since < 0.0) {
        return true;
    }
    if (made != now) {
        return since >= kLightingChangeSeconds;
    }
    return windy && since >= kLightingDriftSeconds;
}

LightingSlice LightingSliceAt(int slice) {
    // The sky's faces are cheap beside the filtering, which the last three share.
    switch (slice) {
    case 0:
        return {0, 3, false, 0u};
    case 1:
        return {3, 6, false, 0u};
    case 2:
        return {6, 6, true, 0b000001u};
    case 3:
        return {6, 6, false, 0b111100u};
    // Mip 1 has three quarters of the filtered texels, so it gets a slice to itself.
    case 4:
        return {6, 6, false, 0b000010u};
    default:
        return {};
    }
}

}  // namespace runner
