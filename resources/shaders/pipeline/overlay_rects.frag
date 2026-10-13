#version 330 core
// A rectangle's color, blended over the UI as its own rectangles are.
in vec4 vColor;

out vec4 fragColor;

void main() {
    fragColor = vColor;
}
