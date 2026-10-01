// What the sky gives a surface, shared by ibl.frag and forward.frag. With a
// Skybox it is split-sum image-based lighting from its irradiance and
// prefiltered cubes, plus Lighting.Ambient as a flat fill; with none it is
// the legacy stand-in (lighting.glsl's ambientLight). No #version: Renderer
// puts it in after lighting.glsl.

// 1 with a Skybox.
uniform float uSkyEnabled;
uniform samplerCube uIrradiance;
uniform samplerCube uPrefiltered;
uniform sampler2D uBrdf;
// From view space to the sky's, with the camera's turn and the Rotation.
uniform mat3 uViewToSky;
// Exposure times Tint, linear.
uniform vec3 uSkyColor;
// The prefiltered cube's last mip, which roughness 1 reads.
uniform float uPrefilteredMaxLod;

// viewDirection is the unit vector from the camera to the surface.
vec3 skyLight(vec3 viewDirection, vec3 N, vec3 albedo, float metallic, float roughness, float reflectivity,
              vec3 ambient, vec3 skyRadiance) {
    if (uSkyEnabled < 0.5) {
        return ambientLight(viewDirection, N, albedo, metallic, roughness, reflectivity, ambient, skyRadiance);
    }
    vec3 V = -viewDirection;
    float NdotV = clamp(dot(N, V), 1e-4, 1.0);
    // A dielectric's F0 from Reflectivity, as Filament's reflectance: 0.5 is the usual 0.04.
    vec3 F0 = mix(vec3(0.16 * reflectivity * reflectivity), albedo, metallic);
    // Schlick's Fresnel, flattened by roughness (Lagarde), for how much is left to diffuse.
    vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec2 brdf = texture(uBrdf, vec2(NdotV, roughness)).rg;
    vec3 irradiance = texture(uIrradiance, uViewToSky * N).rgb;
    vec3 R = reflect(viewDirection, N);
    vec3 reflected = textureLod(uPrefiltered, uViewToSky * R, roughness * uPrefilteredMaxLod).rgb;
    vec3 sky = (kD * albedo * irradiance + reflected * (F0 * brdf.x + brdf.y)) * uSkyColor;
    // Lighting.Ambient lights every surface alike, as light from no direction.
    return sky + albedo * (1.0 - metallic) * ambient / kPi;
}
