#version 330 core
// The split-sum lookup table: for each NdotV across and roughness up, the
// scale (r) and bias (g) on a surface's F0 that its reflection of the sky
// takes. Renderer puts environment.glsl in after the #version line.
in vec2 vUv;
out vec4 outColor;

const uint kSamples = 512u;

float geometryIbl(float NdotV, float NdotL, float a) {
    // Schlick-GGX with k = alpha / 2, as for image-based lighting.
    float k = a / 2.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

void main() {
    float NdotV = max(vUv.x, 1e-3);
    float roughness = vUv.y;
    float a = roughness * roughness;
    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    vec3 N = vec3(0.0, 0.0, 1.0);
    float scale = 0.0;
    float bias = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        vec3 H = importanceSampleGGX(hammersley(i, kSamples), N, max(a, 1e-4));
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);
        if (NdotL > 0.0) {
            float G = geometryIbl(NdotV, NdotL, a);
            float visibility = G * VdotH / max(NdotH * NdotV, 1e-6);
            float fresnel = pow(1.0 - VdotH, 5.0);
            scale += (1.0 - fresnel) * visibility;
            bias += fresnel * visibility;
        }
    }
    outColor = vec4(scale / float(kSamples), bias / float(kSamples), 0.0, 1.0);
}
