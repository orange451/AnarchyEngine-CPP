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
uniform sampler2DArray uSurfaceC;
uniform sampler2D uNoise;
// Whether a per-Terrain array pair is bound and ready: 0 draws flat colors
// from the look table alone, as the pre-Task-6 shader always did.
uniform int uHasSurface;
// Lighting.TerrainQuality: 0 Low, 1 Medium, 2 High.
uniform int uTerrainQuality;
// This draw's LOD node level (0 for a chunk): used only by uLodLevel's
// debug tint below. The quality falloffs that used to gate on "level >= 2"
// (mip bias, normal strength, the third triplanar projection) no longer
// read this: a node's level is a property of the mesh LOD picked for a
// whole node, so two adjacent nodes at different levels (a just-split
// parent next to a child, or two nodes a different pixel error put at
// different levels) could sit at nearly the same camera distance and still
// shade on opposite sides of a hard step -- a visible straight seam across
// otherwise-flat ground. uDetailFade0/uDetailFade1 below replace it with a
// per-pixel ramp over the fragment's own view-space distance, which is
// continuous by construction: no two neighboring pixels, whatever node
// either belongs to, can be on opposite sides of a discontinuity.
// The view-space distance band (units) the far falloffs ramp smoothly
// across: at or inside uDetailFade0 every falloff is fully off (as a near,
// level-0 node always drew); at or beyond uDetailFade1 each is fully on
// (as a level >= 2 node always drew, before this fix). Renderer.cpp sets
// these from the terrain's own chunk size, independent of which node a
// pixel happens to be inside.
uniform float uDetailFade0;
uniform float uDetailFade1;
// Test-only (TX-R8): -1 leaves distT (below) on the usual per-pixel
// uDetailFade0/uDetailFade1 ramp; 0 to 1 pins it directly, so a test built
// at one fixed, close camera distance can still force and check the fully
// far falloffs.
uniform float uDetailFadeOverride;
// A LOD node cross-fading with another level: a 4 x 4 ordered dither, the
// one fading in and the one fading out testing the same threshold the
// opposite ways, so together they cover every pixel once.
uniform float uFade;   // 1 = fully drawn
uniform int uFadeIn;   // 1: this draw is fading in; 0: fading out
// A debug view (SetTerrainLodColors): the node's level, 0 to 7, tints it in
// that level's color; -1 draws the look table's colors.
uniform int uLodLevel;
// Test-only (Task 7, TX-R7): -1 leaves anti-tiling to uTerrainQuality as
// usual; 0 or 1 forces it off or on, so a test can isolate anti-tiling from
// quality's other falloffs.
uniform int uAntiTilingOverride;
// Test-only (Task 7, TX-R6/TX-R8): while 1, this pixel's shaded color is
// replaced by a flat color encoding the number of active triplanar
// projections (after quality's and the far-LOD cap's falloffs) -- red 1,
// green 2, blue 3 -- written to the emissive target alone, so a test can
// read it back independent of lighting, shadows, or tonemapping.
uniform int uProjectionDebug;
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

// The exact sRGB curve, as surface.glsl has it.
vec3 toLinear(vec3 srgb) {
    srgb = max(srgb, vec3(0.0));
    return mix(pow((srgb + 0.055) / 1.055, vec3(2.4)), srgb / 12.92, lessThanEqual(srgb, vec3(0.04045)));
}

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
//
// ddxLocal/ddyLocal (dFdx/dFdy of vLocalPosition) are computed once in
// main(), outside any data-dependent branch, and passed in: GLSL's implicit
// per-pixel texture() LOD needs screen-space derivatives of the texture
// coordinate, computed across a 2x2 pixel quad, and is undefined when the
// branch that calls it (aw.x > 0.0, or the per-material loop above) is not
// taken uniformly by every pixel in that quad -- exactly what happens right
// at a triplanar axis's weight threshold, where a neighboring pixel can
// cross from "projection active" to "not," and the undefined LOD often came
// out far too coarse (a flat, blurred patch with the triangle's own hard
// edges, not a smooth transition). Each axis instead builds its own
// derivative from the one taken outside any branch, so textureGrad's LOD
// stays correct however the blend or the quality cap varies pixel to pixel.
// normalStrength: 0 draws every projection's normal flat (vec3(0,0,1), as
// the old uNodeLevel >= 2 && quality != High path did), 1 draws the sampled
// normal at full strength (as every other path did); continuous in between
// so the fade has no step. See uDetailFade0/uDetailFade1's comment.
void sampleMaterialTriplanar(float layer, float scale, vec3 n, vec3 localPos, vec3 ddxLocal, vec3 ddyLocal, vec3 aw,
                              float mipBias, bool antiTiling, float normalStrength, out vec3 color, out float height,
                              out vec3 normalLocal, out float rough, out float metal) {
    color = vec3(0.0);
    height = 0.0;
    normalLocal = vec3(0.0);
    rough = 0.0;
    metal = 0.0;
    float mipScale = exp2(mipBias);
    if (aw.x > 0.0) {
        vec2 uv = localPos.zy / scale;
        vec2 ddxUv = ddxLocal.zy / scale * mipScale;
        vec2 ddyUv = ddyLocal.zy / scale * mipScale;
        if (antiTiling) {
            vec3 noiseSample = textureLod(uNoise, localPos.zy / (scale * 40.0), 0.0).rgb;
            uv += (noiseSample.rg - 0.5) * 0.5;
        }
        vec4 a = textureGrad(uSurfaceA, vec3(uv, layer), ddxUv, ddyUv);
        vec4 b = vec4(textureGrad(uSurfaceB, vec3(uv, layer), ddxUv, ddyUv).rg,
                      textureGrad(uSurfaceC, vec3(uv, layer), ddxUv, ddyUv).rg);
        color += aw.x * a.rgb;
        height += aw.x * a.a;
        rough += aw.x * b.b;
        metal += aw.x * b.a;
        vec3 axisNormal = mix(vec3(0.0, 0.0, 1.0), unpackNormalRG(b.rg), normalStrength);
        vec3 tn = vec3(axisNormal.xy + n.zy, abs(axisNormal.z) * n.x);
        normalLocal += aw.x * tn.zyx;
    }
    if (aw.y > 0.0) {
        vec2 uv = localPos.xz / scale;
        vec2 ddxUv = ddxLocal.xz / scale * mipScale;
        vec2 ddyUv = ddyLocal.xz / scale * mipScale;
        if (antiTiling) {
            vec3 noiseSample = textureLod(uNoise, localPos.xz / (scale * 40.0), 0.0).rgb;
            uv += (noiseSample.rg - 0.5) * 0.5;
        }
        vec4 a = textureGrad(uSurfaceA, vec3(uv, layer), ddxUv, ddyUv);
        vec4 b = vec4(textureGrad(uSurfaceB, vec3(uv, layer), ddxUv, ddyUv).rg,
                      textureGrad(uSurfaceC, vec3(uv, layer), ddxUv, ddyUv).rg);
        color += aw.y * a.rgb;
        height += aw.y * a.a;
        rough += aw.y * b.b;
        metal += aw.y * b.a;
        vec3 axisNormal = mix(vec3(0.0, 0.0, 1.0), unpackNormalRG(b.rg), normalStrength);
        vec3 tn = vec3(axisNormal.xy + n.xz, abs(axisNormal.z) * n.y);
        normalLocal += aw.y * tn.xzy;
    }
    if (aw.z > 0.0) {
        vec2 uv = localPos.xy / scale;
        vec2 ddxUv = ddxLocal.xy / scale * mipScale;
        vec2 ddyUv = ddyLocal.xy / scale * mipScale;
        if (antiTiling) {
            vec3 noiseSample = textureLod(uNoise, localPos.xy / (scale * 40.0), 0.0).rgb;
            uv += (noiseSample.rg - 0.5) * 0.5;
        }
        vec4 a = textureGrad(uSurfaceA, vec3(uv, layer), ddxUv, ddyUv);
        vec4 b = vec4(textureGrad(uSurfaceB, vec3(uv, layer), ddxUv, ddyUv).rg,
                      textureGrad(uSurfaceC, vec3(uv, layer), ddxUv, ddyUv).rg);
        color += aw.z * a.rgb;
        height += aw.z * a.a;
        rough += aw.z * b.b;
        metal += aw.z * b.a;
        vec3 axisNormal = mix(vec3(0.0, 0.0, 1.0), unpackNormalRG(b.rg), normalStrength);
        vec3 tn = vec3(axisNormal.xy + n.xy, abs(axisNormal.z) * n.z);
        normalLocal += aw.z * tn.xyz;
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
        gNormal = encodeNormal(N);
        gMaterial = vec4(look0.metalness, look0.roughness, look0.reflectivity, 1.0);
        gEmissive = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Triplanar axis weights, shared by every material (they depend only on
    // the surface normal): sharpened (fixed power 4) so most surfaces settle
    // on one or two projections, capped by quality and faded down further
    // at distance (High only -- see distT below).
    vec3 n = normalize(vLocalNormal);
    if (!gl_FrontFacing) n = -n;
    vec3 aw = pow(abs(n), vec3(4.0));
    float awSum = aw.x + aw.y + aw.z;
    aw = awSum > 1e-6 ? aw / awSum : vec3(1.0, 0.0, 0.0);

    // 0 within uDetailFade0, 1 at or beyond uDetailFade1, ramping linearly
    // between: the single per-pixel distance signal every far falloff below
    // reads instead of this draw's uNodeLevel, so two pixels at the same
    // camera distance always shade the same whichever LOD node drew them --
    // see uDetailFade0/uDetailFade1's declaration comment.
    float viewDist = length(vViewPosition);
    float distT = uDetailFadeOverride >= 0.0
                      ? clamp(uDetailFadeOverride, 0.0, 1.0)
                      : clamp((viewDist - uDetailFade0) / max(uDetailFade1 - uDetailFade0, 1e-4), 0.0, 1.0);

    int qualityCap = uTerrainQuality <= 0 ? 1 : (uTerrainQuality == 1 ? 2 : 3);
    if (qualityCap <= 2) {
        // Low/Medium: always at most 2 projections. A quality setting, not
        // a per-pixel falloff -- it never varies with camera distance, so
        // it can never seam between nodes on its own.
        if (aw.x <= aw.y && aw.x <= aw.z) aw.x = 0.0;
        else if (aw.y <= aw.x && aw.y <= aw.z) aw.y = 0.0;
        else aw.z = 0.0;
    } else {
        // High: the third (smallest) projection's weight fades to 0 evenly
        // across the detail band -- replaces the old "uNodeLevel >= 2 caps
        // at 2" step, which could put two adjacent, same-distance nodes on
        // opposite sides of a hard 3-vs-2-projection edge.
        float keep = 1.0 - distT;
        if (aw.x <= aw.y && aw.x <= aw.z) aw.x *= keep;
        else if (aw.y <= aw.x && aw.y <= aw.z) aw.y *= keep;
        else aw.z *= keep;
    }
    if (qualityCap <= 1) {
        if (aw.x >= aw.y && aw.x >= aw.z) { aw.y = 0.0; aw.z = 0.0; }
        else if (aw.y >= aw.x && aw.y >= aw.z) { aw.x = 0.0; aw.z = 0.0; }
        else { aw.x = 0.0; aw.y = 0.0; }
    }
    aw.x = aw.x < 0.05 ? 0.0 : aw.x;
    aw.y = aw.y < 0.05 ? 0.0 : aw.y;
    aw.z = aw.z < 0.05 ? 0.0 : aw.z;
    float awSum2 = aw.x + aw.y + aw.z;
    aw = awSum2 > 1e-6 ? aw / awSum2 : aw;

    if (uProjectionDebug == 1) {
        // TX-R6/TX-R8: the active projection count alone, as a flat color in
        // the emissive target, with albedo black so lighting (ambient, sun,
        // tonemapping) contributes nothing a test would need to see through.
        int activeCount = (aw.x > 0.0 ? 1 : 0) + (aw.y > 0.0 ? 1 : 0) + (aw.z > 0.0 ? 1 : 0);
        vec3 debugColor = activeCount <= 1 ? vec3(1.0, 0.0, 0.0)
                                            : (activeCount == 2 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
        gAlbedo = vec4(0.0, 0.0, 0.0, 1.0);
        gNormal = encodeNormal(normalize(vViewNormal));
        gMaterial = vec4(0.0, 1.0, 0.0, 1.0);
        gEmissive = vec4(debugColor, 1.0);
        return;
    }

    bool heightBlendOn = uTerrainQuality != 0;
    bool antiTiling = uAntiTilingOverride >= 0 ? (uAntiTilingOverride == 1) : (uTerrainQuality != 0);
    // High keeps full normal-map strength at every distance (its extra
    // projection already carries the distance falloff above); Low/Medium
    // fade normal strength to 0 across the same band distT measures, in
    // place of the old "off beyond uNodeLevel 2" step.
    float normalStrength = uTerrainQuality == 2 ? 1.0 : (1.0 - distT);
    // 0 at or inside uDetailFade0 (no bias, as a near level-0 node always
    // sampled), ramping to 1 (the old nodeLevel >= 2 bias) by uDetailFade1.
    float mipBias = distT;

    // Computed once, unconditionally (every pixel in a quad executes this
    // the same way), so sampleMaterialTriplanar's textureGrad calls -- made
    // from inside per-pixel data-dependent branches below -- always get a
    // correct LOD. See sampleMaterialTriplanar's own comment.
    vec3 ddxLocal = dFdx(vLocalPosition);
    vec3 ddyLocal = dFdy(vLocalPosition);

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
        sampleMaterialTriplanar(look.layer, look.scale, n, vLocalPosition, ddxLocal, ddyLocal, aw, mipBias, antiTiling,
                                normalStrength, sColor, sHeight, sNormal, sRough, sMetal);
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
    // At grazing view angles a normal-mapped normal can tip away from the
    // camera, which lighting and reflections were never meant to see (the
    // faint bands seen looking along the ground). Tip it back just enough to
    // face the camera.
    vec3 toCamera = normalize(-vViewPosition);
    float facing = dot(Nview, toCamera);
    const float kMinFacing = 0.05;
    if (facing < kMinFacing) {
        Nview = normalize(Nview + (kMinFacing - facing) * toCamera);
    }

    gAlbedo = vec4(toLinear(albedo), 1.0);
    gNormal = encodeNormal(Nview);
    gMaterial = vec4(metalOut, max(0.05, roughOut), reflOut, 1.0);
    gEmissive = vec4(0.0, 0.0, 0.0, 1.0);
}
