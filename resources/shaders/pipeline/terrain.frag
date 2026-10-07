#version 330 core
// The G-buffer for a terrain LOD node (level 0: a chunk): each material Id's look from the
// Terrain's 256 x 2 table (row 0 color, row 1 metalness, roughness,
// reflectivity). Writes the same four targets, packed the same way, as
// deferred.frag.
in vec3 vViewPosition;
in vec3 vViewNormal;
flat in int vMaterial;
uniform sampler2D uTerrainLook;
// A LOD node cross-fading with another level: a 4 x 4 ordered dither, the
// one fading in and the one fading out testing the same threshold the
// opposite ways, so together they cover every pixel once.
uniform float uFade;   // 1 = fully drawn
uniform int uFadeIn;   // 1: this draw is fading in; 0: fading out
const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
layout (location = 0) out vec4 gAlbedo;
layout (location = 1) out vec4 gNormal;
// Metalness, roughness, reflectivity.
layout (location = 2) out vec4 gMaterial;
layout (location = 3) out vec4 gEmissive;
vec3 toLinear(vec3 srgb) { return pow(max(srgb, vec3(0.0)), vec3(2.2)); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
    if (uFadeIn == 1 ? threshold > uFade : threshold <= 1.0 - uFade) discard;
    vec4 color = texelFetch(uTerrainLook, ivec2(vMaterial, 0), 0);
    vec4 surface = texelFetch(uTerrainLook, ivec2(vMaterial, 1), 0);
    vec3 N = normalize(vViewNormal);
    if (!gl_FrontFacing) N = -N;
    gAlbedo = vec4(toLinear(color.rgb), 1.0);
    gNormal = vec4(N, 1.0);
    gMaterial = vec4(surface.r, max(0.05, surface.g), surface.b, 1.0);
    gEmissive = vec4(0.0, 0.0, 0.0, 1.0);
}
