#version 330 core
// The lit opaque image screen-space reflections read: the light each
// surface took (and the sky) plus its own glow, averaged 2x2 into half size.
// Renderer builds the mips after.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uAccumulation;
uniform sampler2D uEmissiveLight;

void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * 2;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        ivec2 at = base + ivec2(i & 1, i >> 1);
        sum += max(texelFetch(uAccumulation, at, 0).rgb, 0.0) + texelFetch(uEmissiveLight, at, 0).rgb;
    }
    outColor = vec4(min(sum * 0.25, vec3(65000.0)), 1.0);
}
