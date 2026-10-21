// The Ocean's vertex stage: moves the sea mesh by the swells, every frame.
// The waves are afl_ext's "Very fast procedural ocean" (afl_ext 2017-2024,
// MIT License): a sum of sharp sine waves, each dragging the next. The
// fragment stage sums more of them for the ripples.

group Waves;
uniform float WaveHeight : range(0, 5) = 2.0;
uniform float WaveScale : range(1, 200) = 4.0;
uniform float Choppiness : range(0, 1) = 0.38;
uniform float Speed : range(0, 4) = 1.0;

// Out past this many units from the mesh's middle the swells die down, so
// the mesh's far, wide rings stay flat to the horizon.
const float kCalmFrom = 60.0;
const float kCalmTo = 220.0;

// One wave's height and slope along direction.
vec2 wavedx(vec2 position, vec2 direction, float frequency, float timeshift) {
    float x = dot(direction, position) * frequency + timeshift;
    float wave = exp(sin(x) - 1.0);
    float dx = wave * cos(x);
    return vec2(wave, -dx);
}

// The sea's height at position, 0 to 1, from octaves waves.
float waves(vec2 position, int octaves) {
    float phaseShift = length(position) * 0.1;
    float iter = 0.0;
    float frequency = 1.0;
    float timeMultiplier = 2.0;
    float weight = 1.0;
    float sumOfValues = 0.0;
    float sumOfWeights = 0.0;
    for (int i = 0; i < octaves; i++) {
        vec2 direction = vec2(sin(iter), cos(iter));
        vec2 res = wavedx(position, direction, frequency, TIME * Speed * timeMultiplier + phaseShift);
        position += direction * res.y * weight * Choppiness;
        sumOfValues += res.x * weight;
        sumOfWeights += weight;
        weight = mix(weight, 0.0, 0.2);
        frequency *= 1.18;
        timeMultiplier *= 1.07;
        iter += 1232.399963;
    }
    return sumOfValues / sumOfWeights;
}

void vertex() {
    vec2 xz = VERTEX.xz;
    float calm = 1.0 - smoothstep(kCalmFrom, kCalmTo, length(xz));
    const float e = 0.1;
    float h = waves(xz / WaveScale, 12);
    float hx = waves((xz + vec2(e, 0.0)) / WaveScale, 12);
    float hz = waves((xz + vec2(0.0, e)) / WaveScale, 12);
    float lift = WaveHeight * calm;
    VERTEX.y += lift * (h - 0.5);
    NORMAL = normalize(vec3((h - hx) * lift, e, (h - hz) * lift));
}
