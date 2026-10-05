#version 330 core
// A caster in a shadow map: a SpotLight's tile, a PointLight's cube-face
// tile, or a DirectionalLight's cascade. Only depth is drawn. Each caster's
// world matrix comes from runner::InstanceBuffer, slots 7 to 10.
layout (location = 0) in vec3 aPosition;
layout (location = 7) in mat4 aModel;

uniform mat4 uViewProjection;

void main() {
    gl_Position = uViewProjection * (aModel * vec4(aPosition, 1.0));
}
