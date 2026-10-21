#version 330 core
// A caster in a shadow map: a SpotLight's tile, a PointLight's cube-face
// tile, or a DirectionalLight's cascade. Only depth is drawn. Each caster's
// world matrix and bone base come from runner::InstanceBuffer, slots 7 to 10
// and 15; a skinned caster is posed as geometry.vert poses it. Built with
// CUTOUT, it passes on the UVs shadow.frag tests alpha at.
// Built with VERTEX_SHADER, a Material's Shader's vertex stage
// (vertex_shader.glsl) moves each vertex as the G-buffer pass moves it.
layout (location = 0) in vec3 aPosition;
#ifdef VERTEX_SHADER
// The vertex stage reads NORMAL, as in the G-buffer pass.
layout (location = 1) in vec3 aNormal;
#endif
#if defined(CUTOUT) || defined(SURFACE_SHADER) || defined(VERTEX_SHADER)
layout (location = 2) in vec2 aUv;
#endif
#if defined(CUTOUT) || defined(SURFACE_SHADER)
out vec2 vUv;
#endif
#if defined(SURFACE_SHADER) || defined(VERTEX_SHADER)
// A Material's Shader reads the vertex color.
layout (location = 4) in vec4 aColor;
#endif
#ifdef SURFACE_SHADER
// A Material's Shader in shadow.frag reads the vertex color and where it is.
out vec4 vColor;
out vec3 vWorldPosition;
#endif
layout (location = 5) in uvec4 aBones;
layout (location = 6) in vec4 aWeights;
layout (location = 7) in mat4 aModel;
layout (location = 15) in float aBoneBase;

uniform mat4 uViewProjection;
uniform sampler2D uBones;

vec4 boneRow(int texel) {
    return texelFetch(uBones, ivec2(texel % 1024, texel / 1024), 0);
}

mat4 boneMatrix(uint bone) {
    int texel = int(aBoneBase) + 3 * int(bone);
    return transpose(mat4(boneRow(texel), boneRow(texel + 1), boneRow(texel + 2), vec4(0.0, 0.0, 0.0, 1.0)));
}

mat4 skin() {
    if (aBoneBase < 0.0) {
        return mat4(1.0);
    }
    mat4 blended = mat4(0.0);
    float total = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (aWeights[k] > 0.0) {
            blended += aWeights[k] * boneMatrix(aBones[k]);
            total += aWeights[k];
        }
    }
    return total > 0.0 ? blended / total : mat4(1.0);
}

void main() {
#if defined(CUTOUT) || defined(SURFACE_SHADER)
    vUv = aUv;
#endif
    mat4 skinning = skin();
    vec4 world = aModel * (skinning * vec4(aPosition, 1.0));
#ifdef VERTEX_SHADER
    vec3 moved = world.xyz;
    // The normal matrix geometry.vert is given, worked out here: casters carry
    // none. NORMAL is unit length whatever the caster's scale.
    vec3 facing = normalize(transpose(inverse(mat3(aModel))) * (mat3(skinning) * aNormal));
    runVertexShader(moved, facing, aUv, aColor);
    world = vec4(moved, 1.0);
#endif
#ifdef SURFACE_SHADER
    vColor = aColor;
    vWorldPosition = world.xyz;
#endif
    gl_Position = uViewProjection * world;
}
