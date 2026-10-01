#version 330 core
// An AMESH vertex (amesh.hpp's GpuMesh locations) at its GameObject's Transform.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 4) in vec4 aColor;

uniform mat4 uModel;
uniform mat4 uViewProjection;

out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;

void main() {
    // The inverse transpose keeps normals square to the surface under a non-uniform scale.
    vNormal = transpose(inverse(mat3(uModel))) * aNormal;
    vUv = aUv;
    vColor = aColor;
    gl_Position = uViewProjection * (uModel * vec4(aPosition, 1.0));
}
