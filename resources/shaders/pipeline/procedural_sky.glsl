// The DynamicSky toward any world direction: a single-scattering atmosphere
// (Nishita 1993; the sun's glow after https://www.shadertoy.com/view/3djSzz)
// lit by the sun and, at night, the moon; clouds on a layer overhead (after
// https://www.shadertoy.com/view/wslyWs); stars; and the sun and moon. No
// #version: Renderer puts it in after the main file's, after environment.glsl
// (kPi, kMaxHalf, basis). SkyMath.hpp holds the CPU side of the same numbers.

// Toward each body, world space, unit length.
uniform vec3 uSunDirection;
uniform vec3 uMoonDirection;
// World directions into the stars' frame, which turns with the hour.
uniform mat3 uStarFrame;
// 0 by day, 1 at night.
uniform float uStarVisibility;
// The sky's light: toward it, and its color times its intensity, linear.
uniform vec3 uBodyLightDirection;
uniform vec3 uBodyLightColor;
// Each disc's radiance, linear, reddened by the air in front of it.
uniform vec3 uSunColor;
uniform vec3 uMoonColor;
uniform float uCloudCover;
uniform float uCloudDensity;
// How far the clouds have drifted, in studs across X and Z.
uniform vec2 uCloudOffset;
// The tangent of each disc's half angle.
uniform float uSunSize;
uniform float uMoonSize;
uniform float uSunTextureEnabled;
uniform float uMoonTextureEnabled;
uniform sampler2D uSunTexture;
uniform sampler2D uMoonTexture;

const float kPlanetRadius = 6371e3;
const float kAtmosphereRadius = 6471e3;
// Per meter at sea level, and the scale heights; SkyMath.hpp has the same.
const vec3 kRayleigh = vec3(5.5e-6, 13.0e-6, 22.4e-6);
const float kRayleighHeight = 8e3;
const float kMie = 21e-6;
const float kMieHeight = 1.2e3;
const float kMieG = 0.758;
const float kSunRadiance = 22.0;
// The moon lights the sky this much as the sun does: far more than nature, so a night is not black.
const float kMoonSkyRadiance = kSunRadiance * 0.03;
// A dark blue floor under everything at night.
const vec3 kNightSky = vec3(0.002, 0.003, 0.006);
const int kViewSteps = 12;
const int kLightSteps = 4;
// The cloud layer's height and a cloud's size, in studs.
const float kCloudHeight = 200.0;
const float kCloudFeature = 120.0;
const float kStarCells = 150.0;

// Where a ray from origin along unit dir enters (x) and leaves (y) a sphere
// about the planet's center; x > y when it misses.
vec2 sphereHits(vec3 origin, vec3 dir, float radius) {
    float b = dot(dir, origin);
    float c = dot(origin, origin) - radius * radius;
    float d = b * b - c;
    if (d < 0.0) {
        return vec2(1e9, -1e9);
    }
    d = sqrt(d);
    return vec2(-b - d, -b + d);
}

// The light the air scatters toward the eye along dir, from a body toward light.
vec3 atmosphere(vec3 dir, vec3 light, float radiance) {
    vec3 origin = vec3(0.0, kPlanetRadius + 2.0, 0.0);
    float span = sphereHits(origin, dir, kAtmosphereRadius).y;
    vec2 ground = sphereHits(origin, dir, kPlanetRadius);
    if (ground.x > 0.0 && ground.x < ground.y) {
        span = min(span, ground.x);
    }
    float stepLength = span / float(kViewSteps);
    float mu = dot(dir, light);
    float phaseRayleigh = 3.0 / (16.0 * kPi) * (1.0 + mu * mu);
    float g2 = kMieG * kMieG;
    float phaseMie = 3.0 / (8.0 * kPi) * ((1.0 - g2) * (1.0 + mu * mu)) /
                     ((2.0 + g2) * pow(max(1.0 + g2 - 2.0 * kMieG * mu, 1e-4), 1.5));
    vec3 rayleigh = vec3(0.0);
    vec3 mie = vec3(0.0);
    float depthRayleigh = 0.0;
    float depthMie = 0.0;
    for (int i = 0; i < kViewSteps; ++i) {
        vec3 at = origin + dir * (stepLength * (float(i) + 0.5));
        float height = length(at) - kPlanetRadius;
        float densityRayleigh = exp(-height / kRayleighHeight) * stepLength;
        float densityMie = exp(-height / kMieHeight) * stepLength;
        depthRayleigh += densityRayleigh;
        depthMie += densityMie;
        // In the planet's shadow, this point sees no light.
        vec2 blocked = sphereHits(at, light, kPlanetRadius);
        if (blocked.x > 0.0 && blocked.x < blocked.y) {
            continue;
        }
        float lightStep = sphereHits(at, light, kAtmosphereRadius).y / float(kLightSteps);
        float lightRayleigh = 0.0;
        float lightMie = 0.0;
        for (int j = 0; j < kLightSteps; ++j) {
            vec3 lit = at + light * (lightStep * (float(j) + 0.5));
            float litHeight = length(lit) - kPlanetRadius;
            lightRayleigh += exp(-litHeight / kRayleighHeight) * lightStep;
            lightMie += exp(-litHeight / kMieHeight) * lightStep;
        }
        vec3 attenuation =
            exp(-(kRayleigh * (depthRayleigh + lightRayleigh) + kMie * 1.1 * (depthMie + lightMie)));
        rayleigh += densityRayleigh * attenuation;
        mie += densityMie * attenuation;
    }
    return radiance * (phaseRayleigh * kRayleigh * rayleigh + phaseMie * kMie * mie);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float hash13(vec3 p) {
    vec3 p3 = fract(p * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
               mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

// 0 to 1, five octaves, each turned so their grids do not line up.
float fbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    mat2 turn = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 5; ++i) {
        sum += amplitude * valueNoise(p);
        p = turn * p;
        amplitude *= 0.5;
    }
    return sum / 0.96875;
}

// The clouds toward dir: their light in rgb, and how much of what is behind them they hide in a.
vec4 clouds(vec3 dir, vec3 skyAround) {
    if (dir.y < 0.01 || uCloudCover <= 0.0) {
        return vec4(0.0);
    }
    vec2 at = (dir.xz / dir.y * kCloudHeight + uCloudOffset) / kCloudFeature;
    float shape = fbm(at);
    float edge = 1.0 - uCloudCover;
    float coverage = smoothstep(edge, edge + 0.3, shape);
    if (coverage <= 0.0) {
        return vec4(0.0);
    }
    float opacity = coverage * mix(0.4, 1.0, uCloudDensity) * smoothstep(0.01, 0.12, dir.y);
    // Thicker toward the light is darker: the shape a little way toward it, against here.
    vec2 toward = uBodyLightDirection.xz;
    float lean = length(toward);
    toward = lean > 1e-4 ? toward / lean : vec2(0.0);
    float ahead = fbm(at + toward * 0.2);
    float shade = clamp(1.0 - (ahead - shape) * 4.0 * mix(0.5, 1.5, uCloudDensity), 0.2, 1.0);
    // Lit as a white matte surface, plus the sky around it; denser is grayer.
    vec3 light = uBodyLightColor / kPi * shade * mix(1.0, 0.55, uCloudDensity * coverage) + skyAround * 0.6;
    return vec4(light, opacity);
}

vec3 stars(vec3 dir) {
    vec3 s = uStarFrame * dir;
    vec3 cell = floor(s * kStarCells);
    float pick = hash13(cell);
    if (pick < 0.996) {
        return vec3(0.0);
    }
    vec3 jitter = vec3(hash13(cell + 1.7), hash13(cell + 3.1), hash13(cell + 5.9)) - 0.5;
    vec3 center = normalize((cell + 0.5 + jitter * 0.5) / kStarCells);
    float away = length(center - s) * kStarCells;
    float point = 1.0 - smoothstep(0.0, 0.35, away);
    float strength = (pick - 0.996) / 0.004;
    vec3 tint = mix(vec3(1.0, 0.85, 0.7), vec3(0.75, 0.85, 1.0), hash13(cell + 9.2));
    return tint * point * mix(0.3, 3.0, strength * strength);
}

// A sun or moon of half-angle tangent size toward toward, seen along dir: its
// shading in rgb and how much of dir it covers in a. With a texture, the
// texture's color (made linear) and alpha; without, a disc darkening toward
// its edge, mottled for the moon.
vec4 body(vec3 dir, vec3 toward, float size, float textured, sampler2D image, float mottled) {
    float c = dot(dir, toward);
    vec3 T;
    vec3 B;
    basis(toward, T, B);
    vec2 plane = vec2(dot(dir, T), dot(dir, B)) / (max(c, 1e-4) * size);
    float r = length(plane);
    float soft = max(fwidth(r), 1e-3);
    if (c <= 0.0) {
        return vec4(0.0);
    }
    if (textured > 0.5) {
        vec2 uv = plane * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            return vec4(0.0);
        }
        vec4 texel = textureLod(image, uv, 0.0);
        return vec4(pow(texel.rgb, vec3(2.2)), texel.a);
    }
    float cover = 1.0 - smoothstep(1.0 - soft, 1.0 + soft, r);
    float limb = sqrt(max(1.0 - r * r, 0.0));
    float shade = mix(0.4, 1.0, limb) * mix(1.0, 0.75 + 0.25 * valueNoise(plane * 4.0 + 7.0), mottled);
    return vec4(vec3(shade), cover);
}

// The sky's linear radiance toward dir (unit length, world space). detail
// adds the stars and the sun and moon discs, which the lighting cube leaves
// out: the sky's light already gives the sun's highlight.
vec3 proceduralSky(vec3 dir, bool detail) {
    // At and below the horizon, the horizon the same way round (straight
    // down, any); the ground darkens it below.
    vec3 up = dir;
    if (dir.y <= 0.0) {
        float across = length(dir.xz);
        up = across > 1e-4 ? vec3(dir.x / across, 0.0, dir.z / across) : vec3(0.0, 0.0, -1.0);
    }
    up = normalize(vec3(up.x, up.y + 1e-4, up.z));
    vec3 air = atmosphere(up, uSunDirection, kSunRadiance);
    if (uStarVisibility > 0.0) {
        air += atmosphere(up, uMoonDirection, kMoonSkyRadiance);
    }
    air += kNightSky;
    vec3 color = air + uMoonColor * 0.02 * pow(max(dot(up, uMoonDirection), 0.0), 600.0);
    if (detail) {
        color += stars(up) * uStarVisibility;
        vec4 sun = body(up, uSunDirection, uSunSize, uSunTextureEnabled, uSunTexture, 0.0);
        color += sun.rgb * uSunColor * sun.a;
        vec4 moon = body(up, uMoonDirection, uMoonSize, uMoonTextureEnabled, uMoonTexture, 1.0);
        color = mix(color, air + moon.rgb * uMoonColor, moon.a);
    }
    vec4 cloud = clouds(up, air);
    color = mix(color, cloud.rgb, cloud.a);
    float ground = 1.0 - smoothstep(-0.25, 0.0, dir.y);
    color *= mix(1.0, 0.25, ground);
    return min(color, vec3(kMaxHalf));
}
