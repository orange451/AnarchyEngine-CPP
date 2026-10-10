// A Material's surface at one fragment, shared by deferred.frag and
// forward.frag. No #version: Renderer puts it in after the main file's.

uniform sampler2D uDiffuse;
uniform sampler2D uNormalMap;
uniform sampler2D uRoughnessMap;
uniform sampler2D uMetalnessMap;
uniform sampler2D uEmissiveMap;
// The Material's Color, white for no Material. Its alpha is unused.
uniform vec4 uColor;
uniform vec3 uEmissive;
uniform float uMetalness;
uniform float uRoughness;
uniform float uReflectivity;
// 1 when the Material has a NormalTexture.
uniform float uNormalMapEnabled;
// 1 when the Material has an EmissiveTexture. Most have none, and skip the read.
uniform float uEmissiveMapEnabled;

struct Surface {
    vec3 albedo;
    float alpha;
    vec3 normal;
    float metalness;
    float roughness;
    float reflectivity;
    vec3 emissive;
};

// The derivative-based tangent frame (Christian Schüler, "Followup: Normal
// Mapping Without Precomputed Tangents"), as the legacy normalmap.frag used.
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    return mat3(T * invmax, B * invmax, N);
}

// Colors arrive as the sRGB values a picker and an image hold. Lighting is
// done linear, and the tone map puts gamma back.
vec3 toLinear(vec3 srgb) {
    return pow(max(srgb, vec3(0.0)), vec3(2.2));
}

Surface readSurface(vec3 viewPosition, vec3 viewNormal, vec2 uv, vec4 vertexColor, bool frontFacing) {
    Surface s;
    vec4 diffuse = texture(uDiffuse, uv);
    s.albedo = toLinear(diffuse.rgb) * toLinear(uColor.rgb) * vertexColor.rgb;
    s.alpha = diffuse.a * vertexColor.a;

    // Lit from both sides: a face seen from behind turns its normal to the camera.
    vec3 N = normalize(viewNormal);
    if (!frontFacing) {
        N = -N;
    }
    if (uNormalMapEnabled > 0.5) {
        // X and Y only (a BC5 map holds no Z): Z is what keeps it unit length.
        vec2 xy = texture(uNormalMap, uv).rg * 2.0 - 1.0;
        vec3 map = vec3(xy, sqrt(max(0.0, 1.0 - dot(xy, xy))));
        N = normalize(cotangentFrame(N, viewPosition, uv) * map);
    }
    s.normal = N;

    s.metalness = texture(uMetalnessMap, uv).r * uMetalness;
    s.roughness = max(0.05, texture(uRoughnessMap, uv).r * uRoughness);
    s.reflectivity = uReflectivity;
    s.emissive = toLinear(uEmissive);
    if (uEmissiveMapEnabled > 0.5) {
        s.emissive *= toLinear(texture(uEmissiveMap, uv).rgb);
    }
    return s;
}
