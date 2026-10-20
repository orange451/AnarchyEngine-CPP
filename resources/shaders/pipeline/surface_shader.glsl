// A Material's surface shader (a Shader asset's surface()), run on the Surface
// readSurface read, in the G-buffer, see-through, and shadow passes. Spliced in
// by runner::AssembleSurface after surface.glsl and before the user's glsl, and
// only when SURFACE_SHADER is defined. No #version.

// What surface() may change, set from the Surface before it runs.
vec3 ALBEDO;
float ALPHA;
vec3 NORMAL;
float METALNESS;
float ROUGHNESS;
float REFLECTIVITY;
vec3 EMISSIVE;

// What it reads.
vec2 UV;
vec4 COLOR;
vec3 VIEW_POSITION;
vec3 WORLD_POSITION;
vec3 VIEW_NORMAL;
vec3 WORLD_NORMAL;
vec2 SCREEN_UV;
bool FRONT_FACING;
vec3 CAMERA_POSITION;

uniform float uTime;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uInverseView;
uniform vec2 uScreenSize;

#define TIME uTime
#define VIEW uView
#define PROJECTION uProjection
#define INV_VIEW uInverseView
#define TEXTURE_DIFFUSE uDiffuse
#define TEXTURE_NORMAL uNormalMap
#define TEXTURE_ROUGHNESS uRoughnessMap
#define TEXTURE_METALNESS uMetalnessMap
#define TEXTURE_EMISSIVE uEmissiveMap

// The user's, after this file.
void surface();

void surfaceIn(Surface s) {
    ALBEDO = s.albedo;
    ALPHA = s.alpha;
    NORMAL = s.normal;
    METALNESS = s.metalness;
    ROUGHNESS = s.roughness;
    REFLECTIVITY = s.reflectivity;
    EMISSIVE = s.emissive;
}

void surfaceOut(inout Surface s) {
    s.albedo = ALBEDO;
    s.alpha = ALPHA;
    s.normal = normalize(NORMAL);
    s.metalness = clamp(METALNESS, 0.0, 1.0);
    s.roughness = clamp(ROUGHNESS, 0.05, 1.0);
    s.reflectivity = clamp(REFLECTIVITY, 0.0, 1.0);
    s.emissive = max(EMISSIVE, vec3(0.0));
}

// The G-buffer and see-through passes: everything is known.
void runSurfaceShader(inout Surface s, vec3 viewPosition, vec3 viewNormal, vec2 uv, vec4 color, bool frontFacing) {
    UV = uv;
    COLOR = color;
    VIEW_POSITION = viewPosition;
    VIEW_NORMAL = normalize(viewNormal);
    WORLD_POSITION = (uInverseView * vec4(viewPosition, 1.0)).xyz;
    WORLD_NORMAL = normalize(mat3(uInverseView) * viewNormal);
    SCREEN_UV = gl_FragCoord.xy / max(uScreenSize, vec2(1.0));
    FRONT_FACING = frontFacing;
    CAMERA_POSITION = uInverseView[3].xyz;
    surfaceIn(s);
    surface();
    surfaceOut(s);
}

// The shadow pass: the light is the camera, so what depends on the view is
// neutral; only the shader's holes matter.
void runShadowSurfaceShader(inout Surface s, vec3 worldPosition, vec2 uv, vec4 color) {
    UV = uv;
    COLOR = color;
    VIEW_POSITION = vec3(0.0);
    VIEW_NORMAL = vec3(0.0, 0.0, 1.0);
    WORLD_POSITION = worldPosition;
    WORLD_NORMAL = vec3(0.0, 1.0, 0.0);
    SCREEN_UV = vec2(0.0);
    FRONT_FACING = true;
    CAMERA_POSITION = vec3(0.0);
    surfaceIn(s);
    surface();
    surfaceOut(s);
}
