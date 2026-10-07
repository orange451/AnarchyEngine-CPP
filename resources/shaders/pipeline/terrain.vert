#version 330 core
// A terrain chunk: positions in the Terrain's space, aModel its Transform.
// The vertex's material Ids ride in aColor (one byte each), their weights in
// aTangent. This version draws the first Id alone. The locations are
// amesh.hpp's GpuMesh ones, and the instance slots runner::InstanceBuffer's.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 3) in vec4 aTangent;
layout (location = 4) in vec4 aColor;
layout (location = 7) in mat4 aModel;
layout (location = 11) in mat3 aNormalMatrix;
uniform mat4 uView;
uniform mat4 uProjection;
out vec3 vViewPosition;
out vec3 vViewNormal;
flat out int vMaterial;
void main() {
    vec4 viewPosition = uView * (aModel * vec4(aPosition, 1.0));
    vViewNormal = mat3(uView) * (aNormalMatrix * aNormal);
    vViewPosition = viewPosition.xyz;
    vMaterial = int(aColor.r * 255.0 + 0.5);
    gl_Position = uProjection * viewPosition;
}
