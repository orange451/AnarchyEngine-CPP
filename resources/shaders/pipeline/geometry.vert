#version 330 core
// An AMESH vertex (amesh.hpp's GpuMesh locations) at its GameObject's
// Transform, passed on in view space. The G-buffer pass and the transparency
// pass both draw with it (the legacy deferred.vert and forward.vert).
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 4) in vec4 aColor;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vViewPosition;
out vec3 vViewNormal;
out vec2 vUv;
out vec4 vColor;

void main() {
    vec4 viewPosition = uView * (uModel * vec4(aPosition, 1.0));
    // The inverse transpose keeps normals square to the surface under a
    // non-uniform scale. The view has no scale, so its rotation carries them on.
    vViewNormal = mat3(uView) * (transpose(inverse(mat3(uModel))) * aNormal);
    vViewPosition = viewPosition.xyz;
    vUv = aUv;
    vColor = aColor;
    gl_Position = uProjection * viewPosition;
}
