#version 330 core
// The scene's depth under the cursor, copied into a 1 by 1 float target
// (runner::Renderer::copyProbe). A float color target reads back to the CPU
// without waiting for the GPU; the depth attachment itself does not.
uniform sampler2D uDepth;
// The G-buffer texel under the cursor.
uniform vec2 uProbeTexel;

out vec4 fragColor;

void main() {
    fragColor = vec4(texelFetch(uDepth, ivec2(uProbeTexel), 0).r, 0.0, 0.0, 1.0);
}
