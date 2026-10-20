#version 330 core
// Shadow maps have no color: depth is all they keep. Built with CUTOUT for a
// caster whose texture keeps alpha (runner::MeshDraw::cutout): its holes cast
// nothing, by the G-buffer's own alpha test (deferred.frag). Without it, no
// texture is read and the GPU's early depth test stays on.
// Built with SURFACE_SHADER, a Material's Shader that discards runs here too
// (surface_shader.glsl, after surface.glsl, which declares uDiffuse), so its
// holes let light through as the G-buffer's do.
#if defined(CUTOUT) && !defined(SURFACE_SHADER)
in vec2 vUv;
uniform sampler2D uDiffuse;
#endif
#ifdef SURFACE_SHADER
in vec2 vUv;
in vec4 vColor;
in vec3 vWorldPosition;
#endif

void main() {
#ifdef SURFACE_SHADER
    Surface s;
    vec4 diffuse = texture(uDiffuse, vUv);
    s.albedo = diffuse.rgb * vColor.rgb;
    s.alpha = diffuse.a * vColor.a;
    s.normal = vec3(0.0, 0.0, 1.0);
    s.metalness = 0.0;
    s.roughness = 1.0;
    s.reflectivity = 0.0;
    s.emissive = vec3(0.0);
    runShadowSurfaceShader(s, vWorldPosition, vUv, vColor);
    if (s.alpha < 0.25) {
        discard;
    }
#elif defined(CUTOUT)
    if (texture(uDiffuse, vUv).a < 0.25) {
        discard;
    }
#endif
}
