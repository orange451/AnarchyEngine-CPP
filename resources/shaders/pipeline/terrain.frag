#version 330 core
// The G-buffer for a terrain chunk: each material Id's look from the
// Terrain's 256 x 2 table (row 0 color, row 1 metalness, roughness,
// reflectivity). Writes the same four targets, packed the same way, as
// deferred.frag.
in vec3 vViewPosition;
in vec3 vViewNormal;
flat in int vMaterial;
uniform sampler2D uTerrainLook;
layout (location = 0) out vec4 gAlbedo;
layout (location = 1) out vec4 gNormal;
// Metalness, roughness, reflectivity.
layout (location = 2) out vec4 gMaterial;
layout (location = 3) out vec4 gEmissive;
vec3 toLinear(vec3 srgb) { return pow(max(srgb, vec3(0.0)), vec3(2.2)); }
void main() {
    vec4 color = texelFetch(uTerrainLook, ivec2(vMaterial, 0), 0);
    vec4 surface = texelFetch(uTerrainLook, ivec2(vMaterial, 1), 0);
    vec3 N = normalize(vViewNormal);
    if (!gl_FrontFacing) N = -N;
    gAlbedo = vec4(toLinear(color.rgb), 1.0);
    gNormal = vec4(N, 1.0);
    gMaterial = vec4(surface.r, max(0.05, surface.g), surface.b, 1.0);
    gEmissive = vec4(0.0, 0.0, 0.0, 1.0);
}
