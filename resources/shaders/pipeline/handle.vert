#version 330 core
// A Dragger handle's corner in world space with its straight RGBA, as
// Renderer::setHandles takes them.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec4 aColor;

uniform mat4 uView;
uniform mat4 uProjection;

out vec4 vColor;

void main() {
    vColor = aColor;
    gl_Position = uProjection * uView * vec4(aPosition, 1.0);
}
