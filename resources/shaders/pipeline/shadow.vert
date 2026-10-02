#version 330 core
// A caster in a shadow map: a SpotLight's tile, a PointLight's cube-face
// tile, or a DirectionalLight's cascade. Only depth is drawn.
layout (location = 0) in vec3 aPosition;

uniform mat4 uModel;
uniform mat4 uViewProjection;

void main() {
    gl_Position = uViewProjection * (uModel * vec4(aPosition, 1.0));
}
