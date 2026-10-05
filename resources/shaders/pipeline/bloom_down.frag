#version 330 core
// One step down bloom's chain (Jimenez 2014's 13-tap filter): the level above,
// read bilinearly, into one half its size. The first step, from the merge
// image, also keeps only the light above the threshold (bloom.glsl), and
// weights each of the filter's five boxes by 1 / (1 + luma), a Karis
// average, so one very bright pixel cannot flicker as the camera moves.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uSource;
// One texel of uSource.
uniform vec2 uTexel;
// 1 on the first step.
uniform float uPrefilter;
uniform float uThreshold;

// Half float's largest value. A driver may store light past it as infinity,
// and one infinite tap would turn the Karis weights, and so a whole patch of
// the bloom, into NaN; Apple's saturates instead.
const float kMaxHalf = 65000.0;

vec3 tap(float x, float y) {
    vec3 c = texture(uSource, vUv + vec2(x, y) * uTexel).rgb;
    return any(isnan(c)) ? vec3(0.0) : clamp(c, 0.0, kMaxHalf);
}

float luma(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    vec3 a = tap(-2.0, 2.0);
    vec3 b = tap(0.0, 2.0);
    vec3 c = tap(2.0, 2.0);
    vec3 d = tap(-2.0, 0.0);
    vec3 e = tap(0.0, 0.0);
    vec3 f = tap(2.0, 0.0);
    vec3 g = tap(-2.0, -2.0);
    vec3 h = tap(0.0, -2.0);
    vec3 i = tap(2.0, -2.0);
    vec3 j = tap(-1.0, 1.0);
    vec3 k = tap(1.0, 1.0);
    vec3 l = tap(-1.0, -1.0);
    vec3 m = tap(1.0, -1.0);
    // Five overlapping boxes: the middle one weighs half, each corner one an eighth.
    vec3 boxes[5] = vec3[5]((j + k + l + m) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25,
                            (d + e + g + h) * 0.25, (e + f + h + i) * 0.25);
    float weights[5] = float[5](0.5, 0.125, 0.125, 0.125, 0.125);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int n = 0; n < 5; ++n) {
        vec3 box = boxes[n];
        float w = weights[n];
        if (uPrefilter > 0.5) {
            box = brightPart(box, uThreshold);
            w /= 1.0 + luma(box);
        }
        sum += box * w;
        total += w;
    }
    outColor = vec4(sum / total, 1.0);
}
