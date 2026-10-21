#version 330 core
// A ShaderEffect: a Shader asset's post() over the stage's image (uScreen),
// one full-screen triangle. Built only through runner::AssemblePost, which
// puts gbuffer.glsl, the view-space position helper, post_shader.glsl, and
// the user's glsl after the #version line.
in vec2 vUv;
out vec4 fragColor;

void main() {
    fragColor = runPostShader(vUv);
}
