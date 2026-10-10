#version 330 core
// An AMESH vertex (amesh.hpp's GpuMesh locations) at its instance's world
// matrix, passed on in view space. The G-buffer pass and the transparency
// pass both draw with it (the legacy deferred.vert and forward.vert). Each
// instance's matrices and tint come from runner::InstanceBuffer, slots 7 to 15.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 4) in vec4 aColor;
// Up to four bones and their weights; an unused one has weight 0.
layout (location = 5) in uvec4 aBones;
layout (location = 6) in vec4 aWeights;
layout (location = 7) in mat4 aModel;
// The inverse transpose of aModel's 3 by 3, made once per instance on the CPU,
// so normals stay square to the surface under a non-uniform scale.
layout (location = 11) in mat3 aNormalMatrix;
// The GameObject's Color, already linear, so it multiplies like a vertex color.
layout (location = 14) in vec3 aTint;
// The first texel of the instance's skinning matrices in uBones, or -1 when
// it is not skinned.
layout (location = 15) in float aBoneBase;

uniform mat4 uView;
uniform mat4 uProjection;
// Three RGBA32F texels a bone, the first three rows of its skinning matrix,
// 1024 texels to a row (runner::BoneTexture).
uniform sampler2D uBones;

out vec3 vViewPosition;
out vec3 vViewNormal;
out vec2 vUv;
out vec4 vColor;

vec4 boneRow(int texel) {
    return texelFetch(uBones, ivec2(texel % 1024, texel / 1024), 0);
}

mat4 boneMatrix(uint bone) {
    int texel = int(aBoneBase) + 3 * int(bone);
    return transpose(mat4(boneRow(texel), boneRow(texel + 1), boneRow(texel + 2), vec4(0.0, 0.0, 0.0, 1.0)));
}

// The vertex's blend of its bones' matrices; the identity when it is not
// skinned, or has no weight (a static part of a skinned mesh).
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
    mat4 skinning = skin();
    vec4 viewPosition = uView * (aModel * (skinning * vec4(aPosition, 1.0)));
    // The view has no scale, so its rotation carries normals on.
    vViewNormal = mat3(uView) * (aNormalMatrix * (mat3(skinning) * aNormal));
    vViewPosition = viewPosition.xyz;
    vUv = aUv;
    vColor = aColor * vec4(aTint, 1.0);
    gl_Position = uProjection * viewPosition;
}
