#version 330 core
// From linear light to the pane's colors (the legacy toneMap.frag): bloom,
// when there is any, then John Hable's filmic curve after Lighting.Exposure,
// then Gamma and Saturation, with a little noise against banding. The input
// and the output are premultiplied, and Renderer blends this over the pane's
// clear color.
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;
uniform float uExposure;
// 1 over Lighting.Gamma.
uniform float uInverseGamma;
uniform float uSaturation;
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
    vec3 c = toneMap(light * uExposure);
    c = pow(max(c / toneMap(vec3(W)), 0.0), vec3(uInverseGamma));
    // Noise dithering, scaled by coverage so the pane behind stays its exact color.
    c += (rand(vUv) - 0.5) * (0.5 / 255.0) * scene.a;
    c = mix(vec3(luma(c)), c, uSaturation);
    fragColor = vec4(clamp(c, 0.0, 1.0), scene.a);
}
