// The Atmosphere's air, shared by merge.frag (opaque surfaces, terrain, and
// the sky), forward.frag, and particle.frag. atmosphereTransmittance is
// AtmosphereMath's AtmosphereTransmittance, line for line, but for (1 -
// exp(-x)) / x, which GLSL has no expm1 for. Its uniforms are
// AtmosphereUniforms' and need nothing from another library. No #version:
// Renderer and ParticleRenderer put it in after the #version line.

// 1 draws air; 0 leaves every color as it was.
uniform float uAtmosphereEnabled;
// Extinction per unit at BaseHeight, and how fast it thins going up, per unit.
uniform float uAtmosphereSigma0;
uniform float uAtmosphereFalloff;
// The camera's height above BaseHeight.
uniform float uAtmosphereHeight;
// World up, in view space.
uniform vec3 uAtmosphereUp;
// Color, linear.
uniform vec3 uAtmosphereColor;
uniform float uAtmosphereAerial;
uniform float uAtmosphereSkyHaze;
// How far away the sky is taken to be, in units.
uniform float uAtmosphereSkyDistance;
// Toward the light that glows through the air, view space, and its color
// times its intensity times SunScatter.
uniform vec3 uAtmosphereLightDirection;
uniform vec3 uAtmosphereLightColor;
// The sky's prefiltered cube: 1 when it is there to sample, from view space
// into its frame, what its texels are multiplied by, and its last mip.
uniform float uAtmosphereSkyReady;
uniform mat3 uAtmosphereViewToSky;
uniform vec3 uAtmosphereSkyColor;
uniform float uAtmosphereSkyMaxLod;
uniform samplerCube uAtmosphereSky;

const float kAtmosphereMaxDepth = 80.0;
const float kAtmosphereSeriesBelow = 1e-4;

// How much light gets through distance units of air along a ray that climbs
// dy per unit, from the camera.
float atmosphereTransmittance(float dy, float distance) {
    if (distance <= 0.0 || uAtmosphereSigma0 <= 0.0) {
        return 1.0;
    }
    // (1 - exp(-y)) / y: how the air thins over the ray, 0 to 1.
    float x = uAtmosphereFalloff * dy * distance;
    float y = abs(x);
    float shape = y < kAtmosphereSeriesBelow ? 1.0 - 0.5 * y : (1.0 - exp(-y)) / y;
    // As a log, so no one part overflows or underflows on its own.
    float logDepth = log(uAtmosphereSigma0 * distance) - uAtmosphereFalloff * uAtmosphereHeight + max(-x, 0.0) +
                     log(shape);
    float depth = exp(min(logDepth, log(kAtmosphereMaxDepth)));
    return exp(-depth);
}

// The air between the camera and a point distance units away along the unit
// view-space viewDirection: rgb is the light it adds, a how much of the
// point's own light gets through. A color becomes color * a + rgb. With no
// air, (0, 0, 0, 1).
vec4 atmosphereAt(vec3 viewDirection, float distance) {
    if (uAtmosphereEnabled < 0.5) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    float through = atmosphereTransmittance(dot(viewDirection, uAtmosphereUp), distance);
    vec3 air = uAtmosphereColor;
    if (uAtmosphereSkyReady > 0.5 && uAtmosphereAerial > 0.0) {
        // Air close by takes the sky's blurry average, distant air the sharp horizon.
        vec3 sky = textureLod(uAtmosphereSky, uAtmosphereViewToSky * viewDirection,
                              uAtmosphereSkyMaxLod * through).rgb * uAtmosphereSkyColor;
        air = mix(air, sky, uAtmosphereAerial);
    }
    float glow = max(dot(viewDirection, uAtmosphereLightDirection), 0.0);
    glow *= glow;
    glow *= glow;
    glow *= glow;
    air += uAtmosphereLightColor * glow;
    return vec4(air * (1.0 - through), through);
}

// atmosphereAt for a point in view space.
vec4 atmosphereAtPoint(vec3 viewPosition) {
    float distance = length(viewPosition);
    return atmosphereAt(viewPosition / max(distance, 1e-6), distance);
}
