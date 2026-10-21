// A Material's Shader's vertex stage (a Shader asset's vertex()), run on each
// vertex in world space before the view, in the G-buffer, see-through, and
// shadow passes. Spliced in by runner::AssembleVertex after the #version line
// and before the user's glsl, only when VERTEX_SHADER is defined. No #version.

// What vertex() may change: where the vertex is and which way it faces, in
// world space, after skinning and the instance's world matrix.
vec3 VERTEX;
vec3 NORMAL;

// What it reads.
vec2 UV;
vec4 COLOR;

uniform float uTime;

#define TIME uTime

// The user's, after this file.
void vertex();

void runVertexShader(inout vec3 world, inout vec3 worldNormal, vec2 uv, vec4 color) {
    VERTEX = world;
    NORMAL = worldNormal;
    UV = uv;
    COLOR = color;
    vertex();
    world = VERTEX;
    float facing = length(NORMAL);
    worldNormal = facing > 0.0 ? NORMAL / facing : worldNormal;
}
