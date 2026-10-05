#version 330 core
// An AMESH vertex (amesh.hpp's GpuMesh locations) at its instance's world
// matrix, passed on in view space. The G-buffer pass and the transparency
// pass both draw with it (the legacy deferred.vert and forward.vert). Each
// instance's matrices and tint come from runner::InstanceBuffer, slots 7 to 14.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 4) in vec4 aColor;
layout (location = 7) in mat4 aModel;
// The inverse transpose of aModel's 3 by 3, made once per instance on the CPU,
// so normals stay square to the surface under a non-uniform scale.
layout (location = 11) in mat3 aNormalMatrix;
// The GameObject's Color, already linear, so it multiplies like a vertex color.
layout (location = 14) in vec3 aTint;

uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vViewPosition;
out vec3 vViewNormal;
out vec2 vUv;
out vec4 vColor;

void main() {
    vec4 viewPosition = uView * (aModel * vec4(aPosition, 1.0));
    // The view has no scale, so its rotation carries normals on.
    vViewNormal = mat3(uView) * (aNormalMatrix * aNormal);
    vViewPosition = viewPosition.xyz;
    vUv = aUv;
    vColor = aColor * vec4(aTint, 1.0);
    gl_Position = uProjection * viewPosition;
}
