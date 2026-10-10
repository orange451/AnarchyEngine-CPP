// Lighting math shared by the light, IBL, and transparency passes: the
// legacy pbr.frag, pointlight.frag, and fresnel.frag, plus a SpotLight's cone.
// No #version: Renderer puts it in after the main file's. Everything is in
// view space, where the camera is at the origin. Diffuse and specular both
// leave out 1/pi, so their balance is right and a light's Intensity is in
// pi-scaled units.

uniform mat4 uInverseProjection;
// Lighting.ShadingModel is Fast: normalized Blinn-Phong with Kelemen's
// visibility instead of GGX and Smith. 0 is Standard.
uniform int uFastShading;

// The view-space point a G-buffer depth sample came from.
vec3 viewPositionAt(vec2 uv, float depth) {
    vec4 clip = vec4(vec3(uv, depth) * 2.0 - 1.0, 1.0);
    vec4 eye = uInverseProjection * clip;
    return eye.xyz / eye.w;
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    roughness = 1.0 - ((1.0 - roughness) * 0.94);
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (denom * denom);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), roughness) * geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// One light's Cook-Torrance contribution at P. cone is a SpotLight's cosines,
// outer then inner, around direction (where it points). A PointLight passes x
// below -1.5 and has no cone. Both fall off as inverse square, to nothing at radius. A
// DirectionalLight passes x below -3: it shines down direction on everything,
// with no position, radius, or falloff.
vec3 shadeLight(vec3 N, vec3 P, vec3 albedo, float metallic, float roughness, vec3 lightPosition,
                vec3 direction, vec2 cone, vec3 lightColor, float radius, float intensity) {
    vec3 L;
    float attenuation = 1.0;
    if (cone.x < -3.0) {
        L = -direction;
    } else {
        vec3 toLight = lightPosition - P;
        float distance = length(toLight);
        if (distance <= 0.0 || distance >= radius) {
            return vec3(0.0);
        }
        L = toLight / distance;
        // Inverse square, as light really falls off, windowed so it reaches
        // exactly nothing at radius (Karis 2013). Kept from blowing up right
        // at the light.
        float ratio = distance / radius;
        float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
        attenuation = window * window / max(distance * distance, 0.0625);
    }
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) {
        return vec3(0.0);
    }
    float spot = 1.0;
    if (cone.x > -1.5) {
        spot = smoothstep(cone.x, cone.y, dot(-L, direction));
        if (spot <= 0.0) {
            return vec3(0.0);
        }
    }

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 V = normalize(-P);
    vec3 H = normalize(L + V);
    float VdotH = max(dot(H, V), 0.0);
    vec3 F = fresnelSchlick(VdotH, F0);
    vec3 specular;
    if (uFastShading != 0) {
        // Blinn-Phong, its power matched to GGX's highlight size and on the
        // same scale as distributionGGX (no 1/pi). Kelemen's visibility, 1 over
        // 4 (V.H) squared, stands in for Smith over 4 N.L N.V.
        float a = roughness * roughness;
        float power = max(2.0 / max(a * a, 1e-4) - 2.0, 1.0);
        float NDF = (power + 2.0) * 0.5 * pow(max(dot(N, H), 0.0), power);
        specular = NDF * F / max(4.0 * VdotH * VdotH, 0.01);
    } else {
        float NDF = distributionGGX(N, H, roughness);
        float G = geometrySmith(N, V, L, roughness);
        specular = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 0.01);
    }
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 radiance = lightColor * attenuation * intensity * spot;
    return (kD * albedo + specular) * radiance * NdotL;
}

// A rim of sky light at grazing angles. viewDirection is the unit vector from
// the camera to the surface.
float calculateFresnel(vec3 viewDirection, vec3 N, float roughness, float metalness, float reflectiveness) {
    float power = 2.0 + (1.0 / (roughness * 4.0));
    float s = 1.0 - metalness + 0.125;
    float f = 1.0 - abs(dot(viewDirection, N));
    f = clamp(pow(f, power) * s, 0.0, 1.0);
    return f * mix(0.1, 1.0, reflectiveness);
}

// The legacy IBL term: what the sky and the ambient give a surface, before any light.
vec3 ambientLight(vec3 viewDirection, vec3 N, vec3 albedo, float metallic, float roughness, float reflectiveness,
                  vec3 ambient, vec3 skyRadiance) {
    vec3 kD = albedo * (1.0 - metallic);
    vec3 color = kD + mix(skyRadiance, kD * skyRadiance, metallic) * (0.05 + reflectiveness);
    color *= (1.0 + ambient) / 12.0;
    color += skyRadiance * calculateFresnel(viewDirection, N, roughness, metallic, reflectiveness) * ambient;
    return color;
}
