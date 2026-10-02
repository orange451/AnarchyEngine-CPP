// How much of one light reaches a point, 0 to 1, from its shadow map. No
// #version: Renderer puts it in after lighting.glsl. light.frag works in view
// space and the maps in world space, so uInverseView takes points there.
// ShadowRenderer draws the maps; ShadowMath's CubeFaceUv and CubeDepth are
// the C++ twins of the PointLight lookup below.

uniform mat4 uInverseView;
// ShadowLookup::Kind: 0 none, 1 SpotLight, 2 PointLight, 3 DirectionalLight.
uniform int uShadowKind;
// Every PointLight's and SpotLight's tiles, on the atlas's pages (layers).
uniform sampler2DArrayShadow uShadowAtlas;
// The DirectionalLight's cascades.
uniform sampler2DArrayShadow uShadowCascades;
// World to a map's clip space: the SpotLight's, or each cascade's.
uniform mat4 uShadowMatrix[4];
uniform int uShadowCascadeCount;
// A PointLight's or SpotLight's world position, and its map's near plane and Radius.
uniform vec3 uShadowLight;
uniform vec2 uShadowNearFar;
// World units across a texel: x per unit of distance from a PointLight or
// SpotLight; one per cascade for a DirectionalLight.
uniform vec4 uShadowTexel;
// One texel of the texture the light reads, in its 0 to 1 coordinates.
uniform float uShadowTexelUv;
// A SpotLight's tile ([0]) or a PointLight's six, in GL's cube-face order:
// the corner (xy) and size (z) in a page's 0 to 1 coordinates, and the page (w).
uniform vec4 uShadowTiles[6];
// How far a PointLight's 90 degrees reach across its faces' tiles.
uniform float uShadowFaceScale;

// How far along its normal a point moves before it is looked up, in texels:
// enough that a surface does not shadow itself.
const float kNormalOffset = 1.5;
// The outer part of each cascade that fades into the next.
const float kCascadeBlend = 0.1;

// 3 by 3 taps at uv on tile, each a hardware 2 by 2 comparison, kept two
// texels inside the tile so none reads another light's map.
float atlasTaps(vec2 uv, vec4 tile, float depth) {
    vec2 low = tile.xy + vec2(2.0 * uShadowTexelUv);
    vec2 high = tile.xy + vec2(tile.z) - vec2(2.0 * uShadowTexelUv);
    vec2 at = tile.xy + uv * tile.z;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(uShadowAtlas, vec4(clamp(at + vec2(x, y) * uShadowTexelUv, low, high), tile.w, depth));
        }
    }
    return lit / 9.0;
}

float cascadeTaps(vec3 s, float layer) {
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(uShadowCascades, vec4(s.xy + vec2(x, y) * uShadowTexelUv, layer, s.z));
        }
    }
    return lit / 9.0;
}

// Cascade i's clip-space point for W, moved off the surface by that cascade's texel.
vec3 cascadePoint(int i, vec3 W, vec3 N) {
    return (uShadowMatrix[i] * vec4(W + N * (uShadowTexel[i] * kNormalOffset), 1.0)).xyz;
}

// The first cascade that holds W, faded into the next over its edge, and
// out to no shadow past the last.
float cascadeShadow(vec3 W, vec3 N) {
    // The taps reach two texels out.
    float limit = 1.0 - 4.0 * uShadowTexelUv;
    for (int i = 0; i < uShadowCascadeCount; ++i) {
        vec3 c = cascadePoint(i, W, N);
        float edge = max(abs(c.x), abs(c.y));
        if (edge >= limit || c.z > 1.0) {
            continue;
        }
        float lit = cascadeTaps(c * 0.5 + 0.5, float(i));
        float blend = (edge - (limit - kCascadeBlend)) / kCascadeBlend;
        if (blend > 0.0) {
            float next = 1.0;
            if (i + 1 < uShadowCascadeCount) {
                vec3 n = cascadePoint(i + 1, W, N);
                // Only a next cascade that holds the point too; else this one alone.
                next = max(abs(n.x), abs(n.y)) < limit && n.z <= 1.0 ? cascadeTaps(n * 0.5 + 0.5, float(i + 1)) : lit;
            }
            lit = mix(lit, next, blend);
        }
        return lit;
    }
    return 1.0;
}

// A PointLight's: the face v is most along, as ShadowMath's CubeFaceUv picks it.
float pointShadow(vec3 v) {
    vec3 a = abs(v);
    int face;
    float sc;
    float tc;
    float major;
    if (a.x >= a.y && a.x >= a.z) {
        major = a.x;
        face = v.x > 0.0 ? 0 : 1;
        sc = v.x > 0.0 ? -v.z : v.z;
        tc = -v.y;
    } else if (a.y >= a.z) {
        major = a.y;
        face = v.y > 0.0 ? 2 : 3;
        sc = v.x;
        tc = v.y > 0.0 ? v.z : -v.z;
    } else {
        major = a.z;
        face = v.z > 0.0 ? 4 : 5;
        sc = v.z > 0.0 ? v.x : -v.x;
        tc = -v.y;
    }
    vec2 uv = vec2(sc, tc) / major * uShadowFaceScale * 0.5 + 0.5;
    float n = uShadowNearFar.x;
    float f = uShadowNearFar.y;
    // The face's own perspective depth, as ShadowMath's CubeDepth.
    float depth = ((f + n) / (f - n) - 2.0 * f * n / ((f - n) * major)) * 0.5 + 0.5;
    return atlasTaps(uv, uShadowTiles[face], depth);
}

// P and N in view space, as light.frag has them.
float shadowFactor(vec3 P, vec3 N) {
    if (uShadowKind == 0) {
        return 1.0;
    }
    vec3 W = (uInverseView * vec4(P, 1.0)).xyz;
    vec3 Nw = normalize(mat3(uInverseView) * N);
    if (uShadowKind == 3) {
        return cascadeShadow(W, Nw);
    }
    float distance = length(W - uShadowLight);
    vec3 at = W + Nw * (uShadowTexel.x * distance * kNormalOffset);
    if (uShadowKind == 2) {
        return pointShadow(at - uShadowLight);
    }
    vec4 clip = uShadowMatrix[0] * vec4(at, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 s = clip.xyz / clip.w * 0.5 + 0.5;
    return atlasTaps(s.xy, uShadowTiles[0], s.z);
}
