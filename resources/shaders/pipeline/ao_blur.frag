#version 330 core
// One direction of the blur that smooths gtao.frag's noise, run across and
// then down, at the occlusion buffer's size. A neighbor counts less the
// farther its surface sits from the middle pixel's tangent plane, as a part
// of the middle's depth, and the more its normal turns away, so shade does
// not bleed across a silhouette or a crease. Positions come from the
// full-size depth buffer. Renderer puts lighting.glsl in after the #version
// line.
in vec2 vUv;
out vec4 outOcclusion;

uniform sampler2D uOcclusionSource;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
// 1 over the full-size buffers' size in pixels.
uniform vec2 uTexel;
uniform float uOcclusionScale;
// (1, 0) across, (0, 1) down.
uniform vec2 uBlurDirection;
uniform float uBlurRadius;

const int kMaxBlurRadius = 6;
const float kPlaneTolerance = 0.02;
const float kNormalPower = 8.0;

vec3 positionAt(ivec2 texel, out float depth) {
    ivec2 full = texel * int(uOcclusionScale);
    depth = texelFetch(uDepth, full, 0).r;
    return viewPositionAt((vec2(full) + 0.5) * uTexel, depth);
}

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float depth;
    vec3 P = positionAt(texel, depth);
    if (depth >= 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    vec3 N = decodeNormal(texelFetch(uNormal, texel * int(uOcclusionScale), 0));
    float tolerance = kPlaneTolerance * -P.z;
    ivec2 last = textureSize(uOcclusionSource, 0) - 1;
    ivec2 stride = ivec2(uBlurDirection);
    // A Gaussian whose sigma is half the radius.
    float sigma = 0.5 * uBlurRadius;
    float sum = texelFetch(uOcclusionSource, texel, 0).r;
    float total = 1.0;
    int radius = int(uBlurRadius);
    for (int i = 1; i <= kMaxBlurRadius; ++i) {
        if (i > radius) {
            break;
        }
        float gaussian = exp(-0.5 * float(i * i) / (sigma * sigma));
        for (int side = -1; side <= 1; side += 2) {
            ivec2 tap = clamp(texel + stride * (i * side), ivec2(0), last);
            float tapDepth;
            vec3 Ps = positionAt(tap, tapDepth);
            if (tapDepth >= 1.0) {
                continue;
            }
            float planeWeight = clamp(1.0 - abs(dot(Ps - P, N)) / tolerance, 0.0, 1.0);
            vec3 Ns = decodeNormal(texelFetch(uNormal, tap * int(uOcclusionScale), 0));
            float normalWeight = pow(clamp(dot(N, Ns), 0.0, 1.0), kNormalPower);
            float weight = gaussian * planeWeight * normalWeight;
            sum += texelFetch(uOcclusionSource, tap, 0).r * weight;
            total += weight;
        }
    }
    outOcclusion = vec4(sum / total);
}
