#version 330 core
// Puts the passes together, still linear (the legacy merge.frag): the light
// each opaque surface took, its own glow, and the see-through surfaces over
// them. Where nothing opaque was drawn there is the Skybox, which the sky pass
// left in the accumulation buffer; with no Skybox only the see-through
// surfaces are left there, premultiplied, so the pane's color shows behind them.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uDepth;
uniform sampler2D uEmissive;
uniform sampler2D uAccumulation;
// Premultiplied: rgb already carries its alpha.
uniform sampler2D uTransparency;
// 1 with a Skybox.
uniform float uSkyEnabled;

void main() {
    vec4 seeThrough = texture(uTransparency, vUv);
    if (texture(uDepth, vUv).r >= 1.0) {
        if (uSkyEnabled < 0.5) {
            outColor = seeThrough;
            return;
        }
        vec3 sky = max(texture(uAccumulation, vUv).rgb, 0.0);
        outColor = vec4(sky * (1.0 - seeThrough.a) + seeThrough.rgb, 1.0);
        return;
    }
    vec3 color = max(texture(uAccumulation, vUv).rgb, 0.0) + texture(uEmissive, vUv).rgb;
    outColor = vec4(color * (1.0 - seeThrough.a) + seeThrough.rgb, 1.0);
}
