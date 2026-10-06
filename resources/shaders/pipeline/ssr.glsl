// Screen-space reflections' fades, cone level and mip chain blur: ReflectionMath, line for
// line, which the sandbox tests pin. No #version: Renderer puts it in after
// the main file's.

const int kReflectionMaxSteps = 32;
const int kReflectionRefineSteps = 4;

float edgeFadeAxis(float x) {
    return smoothstep(0.0, 0.1, x) * smoothstep(0.0, 0.1, 1.0 - x);
}

float edgeFade(vec2 uv) {
    return edgeFadeAxis(uv.x) * edgeFadeAxis(uv.y);
}

float distanceFade(float distance, float maxDistance) {
    if (maxDistance <= 0.0) {
        return 0.0;
    }
    return 1.0 - smoothstep(0.75 * maxDistance, maxDistance, distance);
}

float roughnessFade(float roughness, float maxRoughness) {
    if (maxRoughness <= 0.0) {
        return 0.0;
    }
    return 1.0 - smoothstep(0.8 * maxRoughness, maxRoughness, roughness);
}

float facingFade(float reflectedZ) {
    return 1.0 - smoothstep(0.0, 0.5, reflectedZ);
}

bool stepHits(float depthA, float depthB, float sceneDepth, float thickness) {
    float nearest = min(depthA, depthB);
    float farthest = max(depthA, depthB);
    return farthest >= sceneDepth && nearest <= sceneDepth + thickness;
}

bool bisectedHitHolds(float rayDepth, float sceneDepth, float thickness) {
    return rayDepth - sceneDepth <= thickness;
}

float coneLevel(float roughness, float hitDistance, float pixelsPerUnit, float levels) {
    float radius = roughness * hitDistance * pixelsPerUnit;
    if (radius <= 1.0) {
        return 0.0;
    }
    return min(log2(radius), levels - 1.0);
}

const int kReflectionBlurRadius = 2;

float pyramidTapWeight(int offset, float luma, bool firefly) {
    // A Gaussian with a sigma of one texel, normalized over -2 to 2.
    const float kWeights[kReflectionBlurRadius + 1] = float[kReflectionBlurRadius + 1](0.402620, 0.244201, 0.054489);
    float w = kWeights[min(abs(offset), kReflectionBlurRadius)];
    return firefly ? w / (1.0 + max(luma, 0.0)) : w;
}
