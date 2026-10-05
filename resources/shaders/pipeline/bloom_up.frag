#version 330 core
// One step up bloom's chain: the smaller level through a 3x3 tent uRadius
// texels wide, which Renderer adds (blend ONE, ONE) into the next larger
// level, still holding its own step down.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uSource;
// One texel of uSource.
uniform vec2 uTexel;
uniform float uRadius;

vec3 tap(float x, float y) {
    return texture(uSource, vUv + vec2(x, y) * uTexel * uRadius).rgb;
}

void main() {
    vec3 sum = tap(0.0, 0.0) * 4.0;
    sum += (tap(-1.0, 0.0) + tap(1.0, 0.0) + tap(0.0, -1.0) + tap(0.0, 1.0)) * 2.0;
    sum += tap(-1.0, -1.0) + tap(1.0, -1.0) + tap(-1.0, 1.0) + tap(1.0, 1.0);
    outColor = vec4(sum / 16.0, 1.0);
}
