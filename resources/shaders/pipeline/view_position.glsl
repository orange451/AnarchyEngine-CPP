// The view-space point a G-buffer depth sample came from, for passes that
// take no lighting (post.frag). lighting.glsl has the same for the passes
// that do. No #version.
uniform mat4 uInverseProjection;

vec3 viewPositionAt(vec2 uv, float depth) {
    vec4 clip = vec4(vec3(uv, depth) * 2.0 - 1.0, 1.0);
    vec4 eye = uInverseProjection * clip;
    return eye.xyz / eye.w;
}
