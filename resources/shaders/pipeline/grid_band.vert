#version 330 core
// The floor grid's bands (GridBands): triangles around its lines, given as
// normalized device coordinates. vUv runs 0 to 1 across the viewport, as the
// fullscreen triangle gives it, so grid.frag works the same on either. Only vUv
// is passed: on these many small triangles, more varyings cost more than the
// per-pixel matrix work they would save.
layout(location = 0) in vec2 aPosition;
out vec2 vUv;

void main() {
    vUv = aPosition * 0.5 + 0.5;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
