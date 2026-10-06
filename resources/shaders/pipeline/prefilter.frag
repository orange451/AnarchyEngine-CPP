#version 330 core
// One face of one mip of the prefiltered cube: the environment as a surface of
// uRoughness reflects it, for split-sum image-based lighting (Karis, "Real
// Shading in Unreal Engine 4", 2013). Renderer puts environment.glsl in after
// the #version line.
in vec2 vUv;
out vec4 outColor;

uniform samplerCube uEnvironment;
uniform int uFace;
uniform float uRoughness;
// The environment cube's width in texels at mip 0.
uniform float uEnvironmentSize;

// Samples per texel: the fewer, the blurrier the mip each one reads.
uniform int uSamples;

void main() {
    // Taken as seen straight on: N, V, and R are one direction.
    vec3 N = normalize(cubeDirection(uFace, vUv));
    if (uRoughness <= 0.0) {
        outColor = vec4(textureLod(uEnvironment, N, 0.0).rgb, 1.0);
        return;
    }
    float a = uRoughness * uRoughness;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    uint samples = uint(max(uSamples, 1));
    for (uint i = 0u; i < samples; ++i) {
        vec3 H = importanceSampleGGX(hammersley(i, samples), N, a);
        vec3 L = normalize(2.0 * dot(N, H) * H - N);
        float NdotL = dot(N, L);
        if (NdotL > 0.0) {
            float NdotH = max(dot(N, H), 0.0);
            // With V equal to N, the pdf's NdotH over 4 HdotV is a quarter.
            float lod = sampleLod(ggx(NdotH, a) * 0.25, float(samples), uEnvironmentSize);
            sum += textureLod(uEnvironment, L, lod).rgb * NdotL;
            weight += NdotL;
        }
    }
    outColor = vec4(sum / max(weight, 1e-4), 1.0);
}
