#version 330 core
// Puts the passes together, still linear (the legacy merge.frag): the light
// each opaque surface took, its own glow, and the see-through surfaces over
// them. Where nothing opaque was drawn, only the see-through surfaces are
// left, premultiplied, so the pane's color shows behind them.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uDepth;
uniform sampler2D uEmissive;
uniform sampler2D uAccumulation;
// Premultiplied: rgb already carries its alpha.
uniform sampler2D uTransparency;

void main() {
    vec4 seeThrough = texture(uTransparency, vUv);
    if (texture(uDepth, vUv).r >= 1.0) {
        outColor = seeThrough;
        return;
    }
    vec3 color = max(texture(uAccumulation, vUv).rgb, 0.0) + texture(uEmissive, vUv).rgb;
    outColor = vec4(color * (1.0 - seeThrough.a) + seeThrough.rgb, 1.0);
}
