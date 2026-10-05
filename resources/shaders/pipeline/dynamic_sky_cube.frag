#version 330 core
// One face of the DynamicSky's environment cube, which EnvironmentMap filters
// into the sky's light and reflections: no stars and no sun or moon disc.
// Renderer puts environment.glsl and procedural_sky.glsl in after the
// #version line.
in vec2 vUv;
out vec4 outColor;

uniform int uFace;

void main() {
    outColor = vec4(proceduralSky(normalize(cubeDirection(uFace, vUv)), false), 1.0);
}
