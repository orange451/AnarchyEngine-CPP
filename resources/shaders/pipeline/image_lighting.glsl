// What the sky gives a surface, shared by ibl.frag and forward.frag. With a
// Skybox it is split-sum image-based lighting from its irradiance and
// prefiltered cubes, plus Lighting.Ambient as a flat fill; with none it is
// the legacy stand-in (lighting.glsl's ambientLight). skyReflection is the
// part of it a surface reflects, which merge.frag's screen-space reflections
// swap for what is on screen. No #version: Renderer puts it in after
// lighting.glsl.

// 1 with a Skybox.
uniform float uSkyEnabled;
uniform samplerCube uIrradiance;
uniform samplerCube uPrefiltered;
uniform sampler2D uBrdf;
// From view space to the sky's, with the camera's turn and the Rotation.
uniform mat3 uViewToSky;
// Exposure times Tint, linear.
uniform vec3 uSkyColor;
// The Skybox's LightScale: how much of the sky's light surfaces take. The sky
// drawn behind them (sky.frag) does not read it.
uniform float uSkyLightScale;
// The prefiltered cube's last mip, which roughness 1 reads.
uniform float uPrefilteredMaxLod;

// The reflection skyLight adds, and its weight: the share of light from the
// reflected direction a surface sends to the eye. Screen-space reflections
// swap light for traced light at the same weight. With a Skybox it is the
// split-sum term; with none, the legacy stand-in's Fresnel term for the
// light, and an analytic split-sum weight.
struct SkyReflection {
    vec3 light;
    vec3 weight;
};

// What skyLight and skyReflection share with a Skybox.
struct SkySurface {
    vec3 F0;
    vec3 F;
    vec2 brdf;
};

SkySurface skySurface(vec3 viewDirection, vec3 N, vec3 albedo, float metallic, float roughness, float reflectivity) {
    SkySurface s;
    float NdotV = clamp(dot(N, -viewDirection), 1e-4, 1.0);
    // A dielectric's F0 from Reflectivity, as Filament's reflectance: 0.5 is the usual 0.04.
    s.F0 = mix(vec3(0.16 * reflectivity * reflectivity), albedo, metallic);
    // Schlick's Fresnel, flattened by roughness (Lagarde), for how much is left to diffuse.
    s.F = s.F0 + (max(vec3(1.0 - roughness), s.F0) - s.F0) * pow(1.0 - NdotV, 5.0);
    s.brdf = texture(uBrdf, vec2(NdotV, roughness)).rg;
    return s;
}

SkyReflection skyReflection(vec3 viewDirection, vec3 N, vec3 albedo, float metallic, float roughness,
                            float reflectivity, vec3 ambient, vec3 skyRadiance) {
    SkyReflection r;
    if (uSkyEnabled < 0.5) {
        // The stand-in's reflection is what ambientLight added, so it is
        // what a traced reflection takes away. Its legacy Fresnel gives a
        // mirror next to nothing, though, so traced light is weighted
        // physically: Karis's analytic fit of the split-sum table (2014),
        // which needs no table.
        r.light = skyRadiance * calculateFresnel(viewDirection, N, roughness, metallic, reflectivity) * ambient;
        float NdotV = clamp(dot(N, -viewDirection), 1e-4, 1.0);
        vec3 F0 = mix(vec3(0.16 * reflectivity * reflectivity), albedo, metallic);
        vec4 fit = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
        float a004 = min(fit.x * fit.x, exp2(-9.28 * NdotV)) * fit.x + fit.y;
        vec2 brdf = vec2(-1.04, 1.04) * a004 + fit.zw;
        r.weight = F0 * brdf.x + brdf.y;
        return r;
    }
    SkySurface s = skySurface(viewDirection, N, albedo, metallic, roughness, reflectivity);
    vec3 R = reflect(viewDirection, N);
    vec3 reflected = textureLod(uPrefiltered, uViewToSky * R, roughness * uPrefilteredMaxLod).rgb;
    r.weight = s.F0 * s.brdf.x + s.brdf.y;
    r.light = reflected * r.weight * uSkyColor * uSkyLightScale;
    return r;
}

// viewDirection is the unit vector from the camera to the surface.
vec3 skyLight(vec3 viewDirection, vec3 N, vec3 albedo, float metallic, float roughness, float reflectivity,
              vec3 ambient, vec3 skyRadiance) {
    if (uSkyEnabled < 0.5) {
        return ambientLight(viewDirection, N, albedo, metallic, roughness, reflectivity, ambient, skyRadiance);
    }
    SkySurface s = skySurface(viewDirection, N, albedo, metallic, roughness, reflectivity);
    vec3 kD = (vec3(1.0) - s.F) * (1.0 - metallic);
    vec3 irradiance = texture(uIrradiance, uViewToSky * N).rgb;
    SkyReflection r = skyReflection(viewDirection, N, albedo, metallic, roughness, reflectivity, ambient, skyRadiance);
    vec3 sky = kD * albedo * irradiance * uSkyColor * uSkyLightScale + r.light;
    // Lighting.Ambient lights every surface alike, as light from no direction.
    return sky + albedo * (1.0 - metallic) * ambient / kPi;
}
