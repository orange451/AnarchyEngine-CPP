#version 330 core
// uScene onto the target as it is: the tone-mapped image, after the LDR
// ShaderEffects, onto the pane when FXAA is off (Renderer::blitPass).
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;

void main() {
    fragColor = texture(uScene, vUv);
}
