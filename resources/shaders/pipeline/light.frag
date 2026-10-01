#version 330 core
// One light, added into the accumulation buffer: a PointLight or SpotLight on each
// pixel its volume covers (the legacy pointlightDeferred.frag), a DirectionalLight
// on every pixel, drawn with fullscreen.vert. Renderer puts
// lighting.glsl in after the #version line.
out vec4 outColor;

uniform sampler2D uDepth;
uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform sampler2D uMaterial;
// 1 over the buffers' size in pixels.
uniform vec2 uTexel;

// View space.
uniform vec3 uLightPosition;
uniform vec3 uLightDirection;
// Outer and inner cosines; x below -1.5 for a PointLight, below -3 for a DirectionalLight.
uniform vec2 uLightCone;
uniform vec3 uLightColor;
uniform float uLightRadius;
uniform float uLightIntensity;

void main() {
    vec2 uv = gl_FragCoord.xy * uTexel;
    float depth = texture(uDepth, uv).r;
    if (depth >= 1.0) {
        discard;
    }
    vec3 P = viewPositionAt(uv, depth);
    vec3 N = texture(uNormal, uv).rgb;
    vec3 material = texture(uMaterial, uv).rgb;
    vec3 albedo = texture(uAlbedo, uv).rgb;
    outColor = vec4(shadeLight(N, P, albedo, material.x, material.y, uLightPosition, uLightDirection, uLightCone,
                               uLightColor, uLightRadius, uLightIntensity),
                    1.0);
}
