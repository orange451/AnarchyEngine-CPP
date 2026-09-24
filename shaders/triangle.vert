#version 330 core
// JadeFX opens GL 3.3 on Windows and Linux, and 4.1 on macOS. 330 compiles on both.
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec3 aColor;

uniform float uAngle;

out vec3 vColor;

void main() {
    // Turn about the vertical axis. A short perspective keeps the face-on
    // shape and lets the near edge grow as the triangle swings past.
    float c = cos(uAngle);
    float s = sin(uAngle);
    float x = aPos.x * c;
    float y = aPos.y;
    float z = aPos.x * s;
    const float dist = 3.0;
    gl_Position = vec4(x * dist, y * dist, z, dist - z);
    vColor = aColor;
}
