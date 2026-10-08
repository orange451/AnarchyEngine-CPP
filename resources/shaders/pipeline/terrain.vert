#version 330 core
// A terrain chunk or LOD node: positions in the Terrain's own (local) space,
// aModel its Transform. Up to 4 material Ids ride in aColor (one byte each,
// identical on all three corners of every triangle thanks to the border
// split -- TX-R3), their weights in aTangent (smoothly interpolated). The
// locations are amesh.hpp's GpuMesh ones, and the instance slots
// runner::InstanceBuffer's.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 3) in vec4 aTangent;
layout (location = 4) in vec4 aColor;
layout (location = 7) in mat4 aModel;
layout (location = 11) in mat3 aNormalMatrix;
uniform mat4 uView;
uniform mat4 uProjection;
// Terrain-local position and normal, for triplanar projections that stick to
// the Terrain's own mesh rather than drifting in world space when the
// Terrain's Transform moves or turns.
out vec3 vLocalPosition;
out vec3 vLocalNormal;
out vec3 vViewPosition;
out vec3 vViewNormal;
flat out uvec4 vIds;
out vec4 vWeights;
// mat3(uView) * aNormalMatrix: the same for every vertex of one instanced
// draw (both factors are per-instance), carried flat so terrain.frag can
// turn a per-pixel LOCAL-space (triplanar-perturbed) normal into view space.
flat out mat3 vNormalToView;
void main() {
    vLocalPosition = aPosition;
    vLocalNormal = aNormal;
    vec4 viewPosition = uView * (aModel * vec4(aPosition, 1.0));
    vNormalToView = mat3(uView) * aNormalMatrix;
    vViewNormal = vNormalToView * aNormal;
    vViewPosition = viewPosition.xyz;
    vIds = uvec4(aColor * 255.0 + 0.5);
    vWeights = aTangent;
    gl_Position = uProjection * viewPosition;
}
