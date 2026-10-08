#version 330 core
// The G-buffer for a terrain LOD node (level 0: a chunk). Up to 4 materials
// per pixel, from the mesh's interpolated Ids and weights (Task 2's border
// split keeps one shared Id set per triangle), blended by a height-aware
// triplanar sampler reading each Terrain's own texture arrays (Task 5/6):
// Surface A (color RGB, sRGB + height A) and Surface B (tangent-space normal
// XY, Z reconstructed, + roughness B + metalness A), looked up per material
// through the Terrain's 256 x 4 RGBA32F look table (row 0 color, row 1
// metalness/roughness/reflectivity, row 2 layer/TextureScale/BlendSharpness/
// HeightStrength, row 3 reserved). With no arrays published yet (uHasSurface
// 0) this draws flat-colored from the look alone, exactly as before --
// terrain must never draw black or vanish. Writes the same four targets,
// packed the same way, as deferred.frag.
in vec3 vLocalPosition;
in vec3 vLocalNormal;
in vec3 vViewPosition;
in vec3 vViewNormal;
flat in uvec4 vIds;
in vec4 vWeights;

uniform sampler2D uTerrainLook;
uniform sampler2DArray uSurfaceA;
uniform sampler2DArray uSurfaceB;
uniform sampler2D uNoise;
// Whether a per-Terrain array pair is bound and ready: 0 draws flat colors
// from the look table alone, as the pre-Task-6 shader always did.
uniform int uHasSurface;
// Lighting.TerrainQuality: 0 Low, 1 Medium, 2 High.
uniform int uTerrainQuality;
// This draw's LOD node level (0 for a chunk): gates quality falloffs at
// level >= 2, independent of uLodLevel's debug tint below.
uniform int uNodeLevel;
// A LOD node cross-fading with another level: a 4 x 4 ordered dither, the
// one fading in and the one fading out testing the same threshold the
// opposite ways, so together they cover every pixel once.
uniform float uFade;   // 1 = fully drawn
uniform int uFadeIn;   // 1: this draw is fading in; 0: fading out
// A debug view (SetTerrainLodColors): the node's level, 0 to 7, tints it in
// that level's color; -1 draws the look table's colors.
uniform int uLodLevel;
// mat3(uView) * aNormalMatrix: constant across one instanced draw (both
// factors are per-instance), so a perturbed LOCAL-space normal can be turned
// into view space per pixel without passing the model matrix itself through.
flat in mat3 vNormalToView;

const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
// sRGB 0 to 255, as Renderer.cpp's kTerrainLodColors.
const vec3 kLodColors[8] = vec3[8](vec3(230.0, 40.0, 40.0), vec3(245.0, 145.0, 30.0), vec3(235.0, 225.0, 40.0),
                                   vec3(50.0, 190.0, 60.0), vec3(40.0, 200.0, 220.0), vec3(50.0, 80.0, 230.0),
                                   vec3(140.0, 60.0, 200.0), vec3(235.0, 60.0, 200.0));
layout (location = 0) out vec4 gAlbedo;
layout (location = 1) out vec4 gNormal;
// Metalness, roughness, reflectivity.
layout (location = 2) out vec4 gMaterial;
layout (location = 3) out vec4 gEmissive;

vec3 toLinear(vec3 srgb) { return pow(max(srgb, vec3(0.0)), vec3(2.2)); }

struct Look {
    vec3 color;
    float metalness, roughness, reflectivity;
    float layer, scale, sharpness, heightStrength;
};

Look fetchLook(int id) {
    Look look;
    vec4 c = texelFetch(uTerrainLook, ivec2(id, 0), 0);
    vec4 s = texelFetch(uTerrainLook, ivec2(id, 1), 0);
    vec4 t = texelFetch(uTerrainLook, ivec2(id, 2), 0);
    look.color = c.rgb;
    look.metalness = s.r;
    look.roughness = max(0.05, s.g);
    look.reflectivity = s.b;
    look.layer = t.r;
    look.scale = max(t.g, 0.0001);
    look.sharpness = clamp(t.b, 0.0, 1.0);
    look.heightStrength = max(t.a, 0.0);
    return look;
}

vec3 unpackNormalRG(vec2 rg) {
    vec2 xy = rg * 2.0 - 1.0;
    float z = sqrt(max(0.0, 1.0 - dot(xy, xy)));
    return vec3(xy, z);
}

// Blends one material's A/B surfaces over up to 3 triplanar projections
// (aw's nonzero components), each read at localPos / scale (plus a small
// anti-tiling offset from a low-frequency noise texture), and each
// projection's tangent-space normal reoriented ("whiteout") onto its own
// world axis before the projections are summed.
void sampleMaterialTriplanar(float layer, float scale, vec3 n, vec3 localPos, vec3 aw, float mipBias,
                              bool antiTiling, bool normalsOn, out vec3 color, out float height,
                              out vec3 normalLocal, out float rough, out float metal) {
    color = vec3(0.0);
    height = 0.0;
    normalLocal = vec3(0.0);
    rough = 0.0;
    metal = 0.0;
    if (aw.x > 0.0) {
        vec2 uv = localPos.zy / scale;
        if (antiTiling) {
            vec3 noiseSample = texture(uNoise, localPos.zy / (scale * 40.0)).rgb;
            uv += (noiseSample.rg - 0.5) * 0.5;
        }
        vec4 a = texture(uSurfaceA, vec3(uv, layer), mipBias);
        vec4 b = texture(uSurfaceB, vec3(uv, layer), mipBias);
        color += aw.x * a.rgb;
        height += aw.x * a.a;
        rough += aw.x * b.b;
        metal += aw.x * b.a;
        vec3 axisNormal = normalsOn ? unpackNormalRG(b.rg) : vec3(0.0, 0.0, 1.0);
        normalLocal += aw.x * vec3(axisNormal.z * sign(n.x), axisNormal.y, axisNormal.x);
    }
    if (aw.y > 0.0) {
        vec2 uv = localPos.xz / scale;
        if (antiTiling) {
            vec3 noiseSample = texture(uNoise, localPos.xz / (scale * 40.0)).rgb;
            uv += (noiseSample.rg - 0.5) * 0.5;
        }
        vec4 a = texture(uSurfaceA, vec3(uv, layer), mipBias);
        vec4 b = texture(uSurfaceB, vec3(uv, layer), mipBias);
        color += aw.y * a.rgb;
        height += aw.y * a.a;
        rough += aw.y * b.b;
        metal += aw.y * b.a;
        vec3 axisNormal = normalsOn ? unpackNormalRG(b.rg) : vec3(0.0, 0.0, 1.0);
        normalLocal += aw.y * vec3(axisNormal.x, axisNormal.z * sign(n.y), axisNormal.y);
    }
    if (aw.z > 0.0) {
        vec2 uv = localPos.xy / scale;
        if (antiTiling) {
            vec3 noiseSample = texture(uNoise, localPos.xy / (scale * 40.0)).rgb;
            uv += (noiseSample.rg - 0.5) * 0.5;
        }
        vec4 a = texture(uSurfaceA, vec3(uv, layer), mipBias);
        vec4 b = texture(uSurfaceB, vec3(uv, layer), mipBias);
        color += aw.z * a.rgb;
        height += aw.z * a.a;
        rough += aw.z * b.b;
        metal += aw.z * b.a;
        vec3 axisNormal = normalsOn ? unpackNormalRG(b.rg) : vec3(0.0, 0.0, 1.0);
        normalLocal += aw.z * vec3(axisNormal.x, axisNormal.y, axisNormal.z * sign(n.z));
    }
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
    if (uFadeIn == 1 ? threshold > uFade : threshold <= 1.0 - uFade) discard;

    if (uHasSurface == 0) {
        // Arrays not ready (or none published): flat colors from the look
        // table alone, the dominant Id, exactly as the pre-Task-6 shader drew.
        Look look0 = fetchLook(int(vIds.x));
        vec3 albedo = look0.color;
        if (uLodLevel >= 0) {
            albedo = mix(albedo, kLodColors[uLodLevel] / 255.0, 0.85);
        }
        vec3 N = normalize(vViewNormal);
        if (!gl_FrontFacing) N = -N;
        gAlbedo = vec4(toLinear(albedo), 1.0);
        gNormal = vec4(N, 1.0);
        gMaterial = vec4(look0.metalness, look0.roughness, look0.reflectivity, 1.0);
        gEmissive = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Triplanar axis weights, shared by every material (they depend only on
    // the surface normal): sharpened (fixed power 4) so most surfaces settle
    // on one or two projections, capped by quality and by far LOD nodes.
    vec3 n = normalize(vLocalNormal);
    if (!gl_FrontFacing) n = -n;
    vec3 aw = pow(abs(n), vec3(4.0));
    float awSum = aw.x + aw.y + aw.z;
    aw = awSum > 1e-6 ? aw / awSum : vec3(1.0, 0.0, 0.0);

    int qualityCap = uTerrainQuality <= 0 ? 1 : (uTerrainQuality == 1 ? 2 : 3);
    int cap = uNodeLevel >= 2 ? min(qualityCap, 2) : qualityCap;
    if (cap <= 2) {
        if (aw.x <= aw.y && aw.x <= aw.z) aw.x = 0.0;
        else if (aw.y <= aw.x && aw.y <= aw.z) aw.y = 0.0;
        else aw.z = 0.0;
    }
    if (cap <= 1) {
        if (aw.x >= aw.y && aw.x >= aw.z) { aw.y = 0.0; aw.z = 0.0; }
        else if (aw.y >= aw.x && aw.y >= aw.z) { aw.x = 0.0; aw.z = 0.0; }
        else { aw.x = 0.0; aw.y = 0.0; }
    }
    aw.x = aw.x < 0.05 ? 0.0 : aw.x;
    aw.y = aw.y < 0.05 ? 0.0 : aw.y;
    aw.z = aw.z < 0.05 ? 0.0 : aw.z;
    float awSum2 = aw.x + aw.y + aw.z;
    aw = awSum2 > 1e-6 ? aw / awSum2 : aw;

    bool heightBlendOn = uTerrainQuality != 0;
    bool antiTiling = uTerrainQuality != 0;
    bool normalsOn = uTerrainQuality == 2 || uNodeLevel < 2;
    float mipBias = uNodeLevel >= 2 ? 1.0 : 0.0;

    vec3 colors[4];
    vec3 normals[4];
    float roughs[4];
    float metals[4];
    float refls[4];
    float scores[4];
    float bands[4];
    bool materialActive[4];
    float maxScore = -1.0e9;

    for (int c = 0; c < 4; ++c) {
        float w = vWeights[c];
        materialActive[c] = w >= 0.01;
        if (!materialActive[c]) {
            continue;
        }
        Look look = fetchLook(int(vIds[c]));
        vec3 sColor;
        float sHeight;
        vec3 sNormal;
        float sRough;
        float sMetal;
        sampleMaterialTriplanar(look.layer, look.scale, n, vLocalPosition, aw, mipBias, antiTiling, normalsOn, sColor,
                                sHeight, sNormal, sRough, sMetal);
        colors[c] = sColor * look.color;
        roughs[c] = clamp(sRough * look.roughness, 0.03, 1.0);
        metals[c] = clamp(sMetal * look.metalness, 0.0, 1.0);
        refls[c] = look.reflectivity;
        normals[c] = sNormal;
        bands[c] = max((1.0 - look.sharpness) * 0.5, 0.001);
        scores[c] = heightBlendOn ? (w + sHeight * look.heightStrength) : w;
        maxScore = max(maxScore, scores[c]);
    }

    float finalWeights[4];
    float weightSum = 0.0;
    for (int c = 0; c < 4; ++c) {
        if (!materialActive[c]) {
            finalWeights[c] = 0.0;
            continue;
        }
        float fw = heightBlendOn ? max(scores[c] - (maxScore - bands[c]), 0.0) : vWeights[c];
        finalWeights[c] = fw;
        weightSum += fw;
    }

    vec3 albedo = vec3(0.0);
    vec3 normalLocalOut = vec3(0.0);
    float roughOut = 0.0;
    float metalOut = 0.0;
    float reflOut = 0.0;
    if (weightSum > 1.0e-5) {
        for (int c = 0; c < 4; ++c) {
            if (!materialActive[c]) {
                continue;
            }
            float fw = finalWeights[c] / weightSum;
            albedo += fw * colors[c];
            normalLocalOut += fw * normals[c];
            roughOut += fw * roughs[c];
            metalOut += fw * metals[c];
            reflOut += fw * refls[c];
        }
    }

    if (uLodLevel >= 0) {
        albedo = mix(albedo, kLodColors[uLodLevel] / 255.0, 0.85);
    }
    vec3 nLocalOut = length(normalLocalOut) > 1.0e-6 ? normalize(normalLocalOut) : n;
    vec3 Nview = normalize(vNormalToView * nLocalOut);

    gAlbedo = vec4(toLinear(albedo), 1.0);
    gNormal = vec4(Nview, 1.0);
    gMaterial = vec4(metalOut, max(0.05, roughOut), reflOut, 1.0);
    gEmissive = vec4(0.0, 0.0, 0.0, 1.0);
}
