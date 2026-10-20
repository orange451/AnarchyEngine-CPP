#version 330 core
// A caster in a shadow map: a SpotLight's tile, a PointLight's cube-face
// tile, or a DirectionalLight's cascade. Only depth is drawn. Each caster's
// world matrix and bone base come from runner::InstanceBuffer, slots 7 to 10
// and 15; a skinned caster is posed as geometry.vert poses it. Built with
// CUTOUT, it passes on the UVs shadow.frag tests alpha at.
layout (location = 0) in vec3 aPosition;
#if defined(CUTOUT) || defined(SURFACE_SHADER)
layout (location = 2) in vec2 aUv;
out vec2 vUv;
#endif
#ifdef SURFACE_SHADER
// A Material's Shader in shadow.frag reads the vertex color and where it is.
layout (location = 4) in vec4 aColor;
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
    vec4 world = aModel * (skin() * vec4(aPosition, 1.0));
#ifdef SURFACE_SHADER
    vColor = aColor;
    vWorldPosition = world.xyz;
#endif
    gl_Position = uViewProjection * world;
}
