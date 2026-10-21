// The Ocean's look. The same waves as the vertex stage (afl_ext's "Very fast
// procedural ocean", afl_ext 2017-2024, MIT License), summed to
// DetailOctaves for the ripples' normal, which smooths toward flat with
// distance so the far sea does not shimmer. The sky it mirrors is the scene's.

group Waves;
uniform float WaveHeight : range(0, 5) = 2.0;
uniform float WaveScale : range(1, 200) = 4.0;
uniform float Choppiness : range(0, 1) = 0.38;
uniform float Speed : range(0, 4) = 1.0;

group Look;
uniform int DetailOctaves : range(1, 48) = 24;
uniform vec3 DeepColor : color = vec3(0.01, 0.04, 0.07);
uniform vec3 ShallowColor : color = vec3(0.06, 0.28, 0.32);
uniform vec3 FoamColor : color = vec3(0.92, 0.95, 1.0);
uniform float FoamAmount : range(0, 1) = 0.3;
// Light through the crests, as the original's scattering term.
uniform float Scatter : range(0, 4) = 1.0;

vec2 wavedx(vec2 position, vec2 direction, float frequency, float timeshift) {
    float x = dot(direction, position) * frequency + timeshift;
    float wave = exp(sin(x) - 1.0);
    float dx = wave * cos(x);
    return vec2(wave, -dx);
}

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

void surface() {
    vec2 xz = WORLD_POSITION.xz;
    // The farther, the flatter, measured in wavelengths, as the original; and
    // the fewer ripples summed, which far off would only shimmer.
    float far = distance(WORLD_POSITION, CAMERA_POSITION) / WaveScale;
    float flatten = min(1.0, sqrt(far * 0.01) * 1.1);
    int octaves = max(6, int(float(DetailOctaves) * (1.0 - 0.7 * flatten)));
    float e = 0.02 * (1.0 + far * 0.05);
    float h = waves(xz / WaveScale, octaves);
    float hx = waves((xz + vec2(e, 0.0)) / WaveScale, octaves);
    float hz = waves((xz + vec2(0.0, e)) / WaveScale, octaves);
    vec3 n = normalize(vec3((h - hx) * WaveHeight, e, (h - hz) * WaveHeight));
    n = normalize(mix(n, vec3(0.0, 1.0, 0.0), 0.9 * flatten));
    NORMAL = normalize(mat3(VIEW) * n);

    // Foam on the sharpest crests.
    float foam = FoamAmount * smoothstep(0.55, 0.9, h) * smoothstep(0.0, 0.25, 1.0 - n.y + h * 0.2);
    ALBEDO = mix(DeepColor, ShallowColor, h * h);
    ALBEDO = mix(ALBEDO, FoamColor, foam);
    ROUGHNESS = mix(0.06, 0.5, foam);
    METALNESS = 0.0;
    REFLECTIVITY = 1.0;
    EMISSIVE = ShallowColor * Scatter * 0.25 * (0.2 + h) * h;
}
