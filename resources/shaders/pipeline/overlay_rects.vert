#version 330 core
// Flat rectangles in framebuffer pixels from the top left, as the UI draws
// (runner::ProfilerOverlay::RectCanvas): many in one draw.
layout (location = 0) in vec2 aPixel;
layout (location = 1) in vec4 aColor;

uniform vec2 uViewport;

out vec4 vColor;

void main() {
    vColor = aColor;
    gl_Position = vec4(aPixel.x / uViewport.x * 2.0 - 1.0, 1.0 - aPixel.y / uViewport.y * 2.0, 0.0, 1.0);
}
