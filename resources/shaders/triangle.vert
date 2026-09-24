#version 330 core
// JadeFX opens GL 3.3 on Windows and Linux, and 4.1 on macOS. 330 compiles on both.
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec3 aColor;

uniform float uAngle;
uniform vec3 uPosition;

out vec3 vColor;

void main() {
    // Turn about the vertical axis, then place the triangle at uPosition.
    // The mesh is scaled down so several of them fit in the view. A short
    // perspective keeps the face-on shape and lets the near edge grow as
    // the triangle swings past. Positive z comes toward the camera.
    float c = cos(uAngle);
    float s = sin(uAngle);
    const float scale = 0.38;
    float x = aPos.x * c * scale + uPosition.x;
    float y = aPos.y * scale + uPosition.y;
    float z = aPos.x * s * scale + uPosition.z;
    const float dist = 3.0;
    gl_Position = vec4(x * dist, y * dist, z, dist - z);
    vColor = aColor;
}
