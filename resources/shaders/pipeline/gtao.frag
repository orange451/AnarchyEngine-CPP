#version 330 core
// Ground-truth ambient occlusion (Jimenez et al. 2016, after Intel's XeGTAO):
// how much of the sky above each opaque surface its neighbors leave open, 1
// open and 0 shut in, into an R8 buffer uOcclusionScale times smaller than
// the full-size buffers. Positions and steps are in full-size pixels; only
// the buffer is coarser. ao_blur.frag smooths the noise, and occlusion.glsl
// reads the result. Renderer puts lighting.glsl in after the #version line.
in vec2 vUv;
out vec4 outOcclusion;

uniform sampler2D uDepth;
uniform sampler2D uNormal;
// 1 over the full-size buffers' size in pixels.
uniform vec2 uTexel;
uniform float uOcclusionRadius;
// Full-size pixels per stud at view depth 1.
uniform float uProjectionScale;
// 1 or 2: this buffer's texel covers that many full-size pixels a side.
uniform float uOcclusionScale;
uniform float uSlices;

const float kPi = 3.14159265359;
const float kHalfPi = 1.57079632679;
const int kMaxSlices = 3;
const int kStepsPerSide = 6;
const float kMaxRadiusFraction = 0.25;
const float kFalloffRange = 0.6;

float sliceNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float stepNoise(vec2 pixel) {
    return fract(dot(pixel, vec2(0.7548776662, 0.5698402910)));
}

float safeAcos(float x) {
    return acos(clamp(x, -1.0, 1.0));
}

float horizonCosAt(vec2 uv, vec3 P, vec3 V, float low, float falloffMul, float falloffAdd) {
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
        return low;
    }
    ivec2 texel = ivec2(uv / uTexel);
    float depth = texelFetch(uDepth, texel, 0).r;
    if (depth >= 1.0) {
        return low;
    }
    vec3 delta = viewPositionAt((vec2(texel) + 0.5) * uTexel, depth) - P;
    float distance = length(delta);
    float cosine = dot(delta, V) / max(distance, 1e-6);
    float weight = clamp(distance * falloffMul + falloffAdd, 0.0, 1.0);
    return mix(low, cosine, weight);
}

void main() {
    // The full-size pixel this texel stands for.
    ivec2 pixel = ivec2(gl_FragCoord.xy) * int(uOcclusionScale);
    vec2 uv = (vec2(pixel) + 0.5) * uTexel;
    float depth = texelFetch(uDepth, pixel, 0).r;
    if (depth >= 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    vec3 P = viewPositionAt(uv, depth);
    vec3 V = normalize(-P);
    vec3 N = normalize(texelFetch(uNormal, pixel, 0).xyz);

    float radiusPixels = min(uOcclusionRadius * uProjectionScale / -P.z, kMaxRadiusFraction / uTexel.y);
    if (radiusPixels < 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    float falloffRange = kFalloffRange * uOcclusionRadius;
    float falloffFrom = uOcclusionRadius - falloffRange;
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;

    float angleNoise = sliceNoise(gl_FragCoord.xy);
    float offsetNoise = stepNoise(gl_FragCoord.xy);
    int slices = int(uSlices);
    float visibility = 0.0;
    for (int slice = 0; slice < kMaxSlices; ++slice) {
        if (slice >= slices) {
            break;
        }
        float phi = (float(slice) + angleNoise) * (kPi / uSlices);
        vec2 omega = vec2(cos(phi), sin(phi));
        vec3 direction = vec3(omega, 0.0);
        vec3 orthoDirection = direction - dot(direction, V) * V;
        vec3 axis = normalize(cross(orthoDirection, V));
        vec3 projectedNormal = N - axis * dot(N, axis);
        float projectedLength = length(projectedNormal);
        float signNormal = sign(dot(orthoDirection, projectedNormal));
        float cosNormal = clamp(dot(projectedNormal, V) / max(projectedLength, 1e-6), 0.0, 1.0);
        float n = signNormal * safeAcos(cosNormal);

        float low0 = cos(n + kHalfPi);
        float low1 = cos(n - kHalfPi);
        float horizonCos0 = low0;
        float horizonCos1 = low1;
        for (int s = 0; s < kStepsPerSide; ++s) {
            float t = (float(s) + offsetNoise) / float(kStepsPerSide);
            vec2 offset = omega * max(t * t * radiusPixels, float(s) + 1.0) * uTexel;
            horizonCos0 = max(horizonCos0, horizonCosAt(uv + offset, P, V, low0, falloffMul, falloffAdd));
            horizonCos1 = max(horizonCos1, horizonCosAt(uv - offset, P, V, low1, falloffMul, falloffAdd));
        }

        float h0 = -safeAcos(horizonCos1);
        float h1 = safeAcos(horizonCos0);
        h0 = n + clamp(h0 - n, -kHalfPi, kHalfPi);
        h1 = n + clamp(h1 - n, -kHalfPi, kHalfPi);
        float arc0 = cosNormal + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n);
        float arc1 = cosNormal + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n);
        visibility += projectedLength * 0.25 * (arc0 + arc1);
    }
    outOcclusion = vec4(clamp(visibility / uSlices, 0.0, 1.0));
}
