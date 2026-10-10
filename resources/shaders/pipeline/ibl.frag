#version 330 core
// The first light in the accumulation buffer: what the ambient and the sky
// give every opaque surface (the legacy ibl.frag), shaded by ambient
// occlusion. Renderer puts lighting.glsl, environment.glsl, occlusion.glsl
// (which declares uDepth), and image_lighting.glsl in after the #version line.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform sampler2D uMaterial;
uniform vec3 uAmbient;
// With no Skybox, the legacy pipeline's stand-in sky, a flat dark gray.
uniform vec3 uSkyRadiance;

void main() {
    float depth = texture(uDepth, vUv).r;
    if (depth >= 1.0) {
        discard;
    }
    vec3 viewDirection = normalize(viewPositionAt(vUv, depth));
    vec3 N = decodeNormal(texture(uNormal, vUv));
    vec3 material = texture(uMaterial, vUv).rgb;
    vec3 albedo = texture(uAlbedo, vUv).rgb;
    float occlusion = occlusionAt(vUv, depth);
    outColor = vec4(skyLight(viewDirection, N, albedo, material.x, material.y, material.z, uAmbient, uSkyRadiance,
                             occlusion),
                    1.0);
}
