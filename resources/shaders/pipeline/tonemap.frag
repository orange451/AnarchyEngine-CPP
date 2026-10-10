#version 330 core
// From linear light to the pane's colors (the legacy toneMap.frag): bloom,
// when there is any, then Lighting.Exposure and the Lighting.ToneMapping
// curve (Classic: John Hable's filmic curve and Gamma; Cinematic: fitted ACES
// and the sRGB encode, Gamma adjusting it around 2.2), then Saturation, with
// a little noise against banding. The input and the output are premultiplied,
// and Renderer blends this over the pane's clear color.
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;
uniform float uExposure;
// 1 over Lighting.Gamma.
uniform float uInverseGamma;
uniform float uSaturation;
// Lighting.ToneMapping: 0 Classic (Hable, then Gamma), 1 Cinematic.
uniform int uCinematic;
// Bloom's chain at its largest level, the sum of every level drawn.
uniform sampler2D uBloom;
// BloomEffect.Intensity, or 0 with no bloom this frame.
uniform float uBloomIntensity;
// 1 over the number of levels summed into uBloom.
uniform float uBloomLevelScale;
uniform float uBloomThreshold;

const float A = 0.15;
const float B = 0.50;
const float C = 0.10;
const float D = 0.20;
const float E = 0.02;
const float F = 0.30;
const float W = 2.0;

vec3 toneMap(vec3 x) {
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// Lighting.ToneMapping Cinematic: Narkowicz's fit of the ACES film curve,
// which keeps more contrast in the mids and rolls highlights off sooner than
// Hable's, then the exact sRGB encode in place of Gamma.
vec3 acesFitted(vec3 x) {
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

vec3 linearToSrgb(vec3 c) {
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, lessThanEqual(c, vec3(0.0031308)));
}

float luma(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

float rand(vec2 co) {
    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec4 scene = texture(uScene, vUv);
    vec3 light = max(scene.rgb, 0.0);
    // The bright part moves into the blurred copy: at Threshold 0 a plain mix
    // toward it, above it only the light over the threshold. Alpha is left as
    // it is, so where nothing was drawn the halo adds over the pane.
    if (uBloomIntensity > 0.0) {
        vec3 bloom = texture(uBloom, vUv).rgb * uBloomLevelScale;
        light = max(light + uBloomIntensity * (bloom - brightPart(light, uBloomThreshold)), 0.0);
    }
    vec3 c;
    if (uCinematic != 0) {
        c = linearToSrgb(clamp(acesFitted(light * uExposure), 0.0, 1.0));
        // Gamma as an adjustment around the sRGB curve: at 2.2 it changes nothing.
        c = pow(c, vec3(2.2 * uInverseGamma));
    } else {
        c = toneMap(light * uExposure);
        c = pow(max(c / toneMap(vec3(W)), 0.0), vec3(uInverseGamma));
    }
    // Noise dithering, scaled by coverage so the pane behind stays its exact color.
    c += (rand(vUv) - 0.5) * (0.5 / 255.0) * scene.a;
    c = mix(vec3(luma(c)), c, uSaturation);
    fragColor = vec4(clamp(c, 0.0, 1.0), scene.a);
}
