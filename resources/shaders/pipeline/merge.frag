#version 330 core
// Puts the passes together, still linear (the legacy merge.frag): the light
// each opaque surface took, its own glow, any screen-space reflection in
// place of its sky reflection, and the see-through surfaces over them. Where
// nothing opaque was drawn there is the Skybox, which the sky pass left in
// the accumulation buffer; with no Skybox only the see-through surfaces are
// left there, premultiplied, so the pane's color shows behind them. Renderer
// puts lighting.glsl, environment.glsl, occlusion.glsl, and
// image_lighting.glsl in after the #version line; occlusion.glsl declares
// uDepth and image_lighting.glsl uSkyEnabled: whether a surface lights and
// reflects from the sky's cubes, which stays 0 for a DynamicSky whose
// lighting cube is not ready. uSkyDrawn, below, is separate: whether the sky
// pass drew into uAccumulation this frame, which for a DynamicSky is true
// even then, since it draws straight from its shader.
in vec2 vUv;
out vec4 outColor;

uniform float uSkyDrawn;
uniform sampler2D uEmissive;
uniform sampler2D uAccumulation;
// Premultiplied: rgb already carries its alpha.
uniform sampler2D uTransparency;
uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform sampler2D uMaterial;
// The half-size trace: the reflected light, premultiplied by how much it
// counts, and how much it counts.
uniform sampler2D uReflections;
// 1 when this frame traced reflections.
uniform float uReflectionsEnabled;
uniform float uReflectionsIntensity;
uniform vec3 uAmbient;
uniform vec3 uSkyRadiance;

// The trace at this pixel: its four half-size texels, weighted bilinearly
// and by how near each one's surface is to this pixel's, so a reflection
// does not bleed across a silhouette.
vec4 upsampleReflections(float depth) {
    vec2 halfSize = vec2(textureSize(uReflections, 0));
    vec2 position = vUv * halfSize - 0.5;
    vec2 base = floor(position);
    vec2 f = position - base;
    // Most pixels reflect nothing; they skip the depth work.
    vec4 taps[4];
    float anyHit = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 texel = clamp(ivec2(base) + ivec2(i & 1, i >> 1), ivec2(0), ivec2(halfSize) - 1);
        taps[i] = texelFetch(uReflections, texel, 0);
        anyHit += taps[i].a;
    }
    if (anyHit <= 0.0) {
        return vec4(0.0);
    }
    float center = -viewPositionAt(vUv, depth).z;
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 offset = ivec2(i & 1, i >> 1);
        ivec2 texel = clamp(ivec2(base) + offset, ivec2(0), ivec2(halfSize) - 1);
        vec2 tapUv = (vec2(texel) * 2.0 + 1.0) / vec2(textureSize(uDepth, 0));
        float tapDepth = -viewPositionAt(tapUv, texelFetch(uDepth, texel * 2, 0).r).z;
        float bilinear = (offset.x == 1 ? f.x : 1.0 - f.x) * (offset.y == 1 ? f.y : 1.0 - f.y);
        float w = bilinear / (1e-3 + abs(tapDepth - center) / center);
        sum += taps[i] * w;
        total += w;
    }
    return total > 0.0 ? sum / total : vec4(0.0);
}

void main() {
    vec4 seeThrough = texture(uTransparency, vUv);
    float depth = texture(uDepth, vUv).r;
    if (depth >= 1.0) {
        if (uSkyDrawn < 0.5) {
            outColor = seeThrough;
            return;
        }
        vec3 sky = max(texture(uAccumulation, vUv).rgb, 0.0);
        outColor = vec4(sky * (1.0 - seeThrough.a) + seeThrough.rgb, 1.0);
        return;
    }
    vec3 color = max(texture(uAccumulation, vUv).rgb, 0.0) + texture(uEmissive, vUv).rgb;
    if (uReflectionsEnabled > 0.5) {
        vec4 traced = upsampleReflections(depth);
        if (traced.a > 0.0) {
            vec3 viewDirection = normalize(viewPositionAt(vUv, depth));
            vec3 material = texture(uMaterial, vUv).rgb;
            SkyReflection sky = skyReflection(viewDirection, decodeNormal(texture(uNormal, vUv)), texture(uAlbedo, vUv).rgb,
                                              material.x, material.y, material.z, uAmbient, uSkyRadiance,
                                              occlusionAt(vUv, depth));
            // traced.rgb is premultiplied by traced.a.
            color = max(color + uReflectionsIntensity * (sky.weight * traced.rgb - traced.a * sky.light), 0.0);
        }
    }
    outColor = vec4(color * (1.0 - seeThrough.a) + seeThrough.rgb, 1.0);
}
