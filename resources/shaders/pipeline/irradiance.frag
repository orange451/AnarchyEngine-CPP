#version 330 core
// One face of the irradiance cube: the light the environment gives a surface
// facing each way, cosine weighted, divided by pi so that a surface's diffuse
// light is this times its albedo. Renderer puts environment.glsl in after the
// #version line.
in vec2 vUv;
out vec4 outColor;

uniform samplerCube uEnvironment;
uniform int uFace;
// The environment cube's width in texels at mip 0.
uniform float uEnvironmentSize;

// Samples per texel: the fewer, the blurrier the mip each one reads.
uniform int uSamples;

void main() {
    vec3 N = normalize(cubeDirection(uFace, vUv));
    vec3 T;
    vec3 B;
    basis(N, T, B);
    vec3 sum = vec3(0.0);
    uint samples = uint(max(uSamples, 1));
    for (uint i = 0u; i < samples; ++i) {
        vec2 xi = hammersley(i, samples);
        // Cosine weighted, so the average of the samples is the integral over pi.
        float phi = 2.0 * kPi * xi.x;
        float cosTheta = sqrt(1.0 - xi.y);
        float sinTheta = sqrt(xi.y);
        vec3 L = T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + N * cosTheta;
        float lod = sampleLod(cosTheta / kPi, float(samples), uEnvironmentSize);
        sum += textureLod(uEnvironment, L, lod).rgb;
    }
    outColor = vec4(sum / float(samples), 1.0);
}
