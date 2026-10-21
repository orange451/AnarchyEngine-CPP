// A Shader asset's post(), run once per pixel of the stage's image. Spliced
// into post.frag by runner::AssemblePost after gbuffer.glsl and the position
// helper, and before the user's glsl. No #version.

// What post() may change: the pixel on entry, the result on exit. Premultiplied:
// in the LDR stage rgb is 0 to 1 display color, in the Hdr stage linear light.
vec4 COLOR;

// What it reads.
uniform sampler2D uScreen;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform sampler2D uAlbedo;
uniform sampler2D uMaterial;
uniform vec2 uScreenSize;
uniform float uTime;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uInverseView;

#define SCREEN uScreen
#define DEPTH uDepth
#define NORMALS uNormal
#define ALBEDO uAlbedo
#define MATERIAL uMaterial
#define SCREEN_SIZE uScreenSize
#define TEXEL (1.0 / uScreenSize)
#define TIME uTime
#define VIEW uView
#define PROJECTION uProjection
#define INV_VIEW uInverseView
#define INV_PROJECTION uInverseProjection

vec2 SCREEN_UV;
vec3 CAMERA_POSITION;

// The view-space point a pixel's surface is at; the sky is far along its ray.
vec3 viewPositionAt(vec2 uv) {
    return viewPositionAt(uv, texture(uDepth, uv).r);
}

// How far a pixel's surface is from the camera, in units; 1e9 at the sky.
float depthAt(vec2 uv) {
    float raw = texture(uDepth, uv).r;
    return raw >= 1.0 ? 1.0e9 : -viewPositionAt(uv, raw).z;
}

vec3 worldPositionAt(vec2 uv) {
    return (uInverseView * vec4(viewPositionAt(uv), 1.0)).xyz;
}

// The view-space unit normal of a pixel's surface.
vec3 normalAt(vec2 uv) {
    return decodeNormal(texture(uNormal, uv));
}

// The user's, after this file.
void post();

vec4 runPostShader(vec2 uv) {
    SCREEN_UV = uv;
    CAMERA_POSITION = uInverseView[3].xyz;
    COLOR = texture(uScreen, uv);
    post();
    // Nothing below black, and a coverage the blend can take: a NaN here would
    // spread through bloom and the tone map.
    vec3 rgb = max(COLOR.rgb, vec3(0.0));
    float a = clamp(COLOR.a, 0.0, 1.0);
    return vec4(rgb == rgb ? rgb : vec3(0.0), a == a ? a : 1.0);
}
