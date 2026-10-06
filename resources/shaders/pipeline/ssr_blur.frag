#version 330 core
// One half of building a level of the lit image's mip chain, which rough
// reflections read: a Gaussian blur one texel of the level wide, across
// (uBlurDirection) one axis. The first half reads the level above, so its
// bilinear taps also halve it; the second reads the first half's result at
// the level's own size. The first level down weights each tap by 1 / (1 +
// luma), a Karis average, so a glint one texel wide cannot become a bright
// smear. Renderer puts ssr.glsl in after the #version line.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uSource;
// The level of uSource the taps read.
uniform float uSourceLevel;
// One texel of the level being drawn, along the blur's axis, in uv.
uniform vec2 uBlurDirection;
// 1 for the first level down.
uniform float uPrefilter;

// Half float's largest value; see bloom_down.frag.
const float kMaxHalf = 65000.0;

void main() {
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int offset = -kReflectionBlurRadius; offset <= kReflectionBlurRadius; ++offset) {
        vec3 c = textureLod(uSource, vUv + uBlurDirection * float(offset), uSourceLevel).rgb;
        c = any(isnan(c)) ? vec3(0.0) : clamp(c, 0.0, kMaxHalf);
        float w = pyramidTapWeight(offset, dot(c, vec3(0.2126, 0.7152, 0.0722)), uPrefilter > 0.5);
        sum += c * w;
        total += w;
    }
    outColor = vec4(sum / total, 1.0);
}
