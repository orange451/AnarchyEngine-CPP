#version 330 core
// A line segment's end in world space, as Renderer::setOutlines takes it,
// passed on with how far it is from the camera and where it lands on the pane.
layout (location = 0) in vec3 aPosition;

uniform mat4 uView;
uniform mat4 uProjection;

out float vDistance;
out vec4 vClip;

void main() {
    vec4 viewPosition = uView * vec4(aPosition, 1.0);
    vDistance = -viewPosition.z;
    vClip = uProjection * viewPosition;
    gl_Position = vClip;
}
