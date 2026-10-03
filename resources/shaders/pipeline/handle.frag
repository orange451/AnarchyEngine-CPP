#version 330 core
// Dragger handles over the tone mapped pane, unlit, straight alpha: always on
// top, so a handle behind a surface can still be grabbed.
in vec4 vColor;
out vec4 fragColor;

void main() {
    fragColor = vColor;
}
