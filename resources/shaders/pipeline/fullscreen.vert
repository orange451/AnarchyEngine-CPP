#version 330 core
// One triangle over the whole viewport, from gl_VertexID alone: draw 3
// vertices with an empty vertex array. vUv runs 0 to 1 across the viewport.
out vec2 vUv;

void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
