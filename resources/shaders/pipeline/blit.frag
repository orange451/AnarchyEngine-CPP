#version 330 core
// uScene onto the target as it is: the tone-mapped image, after the LDR
// ShaderEffects, onto the pane when FXAA is off (Renderer::blitPass).
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;
// 1 for a transparent pane, whose alpha is its coverage; 0 draws opaque.
uniform float uKeepAlpha;

void main() {
    vec4 color = texture(uScene, vUv);
    fragColor = vec4(color.rgb, uKeepAlpha > 0.5 ? color.a : 1.0);
}
