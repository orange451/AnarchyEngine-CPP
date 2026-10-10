#version 330 core
// Writes an opaque surface into the G-buffer (the legacy deferred.frag and
// write.frag). Everything is in view space. Renderer puts surface.glsl in
// after the #version line.
in vec3 vViewPosition;
in vec3 vViewNormal;
in vec2 vUv;
in vec4 vColor;

layout (location = 0) out vec4 gAlbedo;
layout (location = 1) out vec4 gNormal;
// Metalness, roughness, reflectivity.
layout (location = 2) out vec4 gMaterial;
layout (location = 3) out vec4 gEmissive;

void main() {
    Surface s = readSurface(vViewPosition, vViewNormal, vUv, vColor, gl_FrontFacing);
    // Alpha test: a cut-out texture's holes leave the G-buffer alone.
    if (s.alpha < 0.25) {
        discard;
    }
    gAlbedo = vec4(s.albedo, 1.0);
    gNormal = encodeNormal(s.normal);
    gMaterial = vec4(s.metalness, s.roughness, s.reflectivity, 1.0);
    gEmissive = vec4(s.emissive, 1.0);
}
