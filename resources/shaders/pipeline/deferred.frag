#version 330 core
// Writes an opaque surface into the G-buffer (the legacy deferred.frag and
// write.frag). Everything is in view space. Renderer puts surface.glsl in
// after the #version line.
in vec3 vViewPosition;
in vec3 vViewNormal;
in vec2 vUv;
in vec4 vColor;
flat in float vLodFade;

// An LOD fading in or out (geometry.vert's vLodFade, runner::StepLodFade)
// keeps only some pixels, on a 4 by 4 Bayer pattern fixed to the screen. One
// fading in by f keeps the pixels one fading out by 1 - f drops, so the two
// never both draw a pixel and never leave a hole.
bool lodDithered() {
    if (vLodFade >= 1.0) {
        return false;
    }
    const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0,
                                       13.0, 5.0);
    ivec2 cell = ivec2(gl_FragCoord.xy) & 3;
    float threshold = (kBayer[cell.y * 4 + cell.x] + 0.5) / 16.0;
    // Fading in keeps the top of the pattern, fading out the bottom.
    return vLodFade > 0.0 ? threshold < 1.0 - vLodFade : !(threshold < -vLodFade);
}

layout (location = 0) out vec4 gAlbedo;
layout (location = 1) out vec4 gNormal;
// Metalness, roughness, reflectivity.
layout (location = 2) out vec4 gMaterial;
layout (location = 3) out vec4 gEmissive;

void main() {
    if (lodDithered()) {
        discard;
    }
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
