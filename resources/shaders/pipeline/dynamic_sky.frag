#version 330 core
// The DynamicSky behind everything, where no opaque surface was drawn, added
// into the accumulation buffer so it is tone mapped with the rest. Renderer
// puts lighting.glsl, environment.glsl, and procedural_sky.glsl in after the
// #version line.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uDepth;
// From view space to the world's: a DynamicSky does not turn.
uniform mat3 uViewToSky;
// The DynamicSky's brightness against the picture proceduralSky draws, the same each channel.
uniform vec3 uSkyColor;

void main() {
    if (texture(uDepth, vUv).r < 1.0) {
        discard;
    }
    vec3 direction = normalize(uViewToSky * viewPositionAt(vUv, 1.0));
    outColor = vec4(min(proceduralSky(direction, true) * uSkyColor, vec3(kMaxHalf)), 1.0);
}
