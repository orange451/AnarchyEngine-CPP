#version 330 core
// One face of the environment cube, from the Skybox's equirectangular image.
// Renderer puts environment.glsl in after the #version line.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uSky;
uniform int uFace;
// The cube's width in texels.
uniform float uEnvironmentSize;

void main() {
    // A face spans a quarter of the image's width. The image's mip nearest
    // the cube's texels averages a large image rather than skipping over it.
    float lod = max(log2(float(textureSize(uSky, 0).x) / 4.0 / uEnvironmentSize), 0.0);
    vec3 direction = cubeDirection(uFace, vUv);
    outColor = vec4(min(textureLod(uSky, equirectUv(direction), lod).rgb, vec3(kMaxHalf)), 1.0);
}
