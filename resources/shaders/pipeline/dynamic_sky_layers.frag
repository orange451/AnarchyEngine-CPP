#version 330 core
// The DynamicSky's air and clouds (procedural_sky.glsl's skyLayers) at half
// the view's size, which dynamic_sky.frag reads between texels: both change
// slowly across the view, and they are nearly all of the sky's cost. Renderer
// puts lighting.glsl, environment.glsl, and procedural_sky.glsl in after the
// #version line.
in vec2 vUv;
layout(location = 0) out vec4 outAir;
layout(location = 1) out vec4 outCloud;

uniform sampler2D uDepth;
// From view space to the world's: a DynamicSky does not turn.
uniform mat3 uViewToSky;

void main() {
    // Only where a sky pixel reads this texel. A full-size pixel's read
    // between texels reaches those less than a texel (at most two of its
    // own) away, so the 4 by 4 full-size pixels about this texel's center.
    ivec2 size = textureSize(uDepth, 0);
    ivec2 first = ivec2(ceil(vUv * vec2(size) - 2.5));
    bool sky = false;
    for (int y = 0; y < 4 && !sky; ++y) {
        for (int x = 0; x < 4 && !sky; ++x) {
            sky = texelFetch(uDepth, clamp(first + ivec2(x, y), ivec2(0), size - 1), 0).r >= 1.0;
        }
    }
    if (!sky) {
        discard;
    }
    vec3 direction = normalize(uViewToSky * viewPositionAt(vUv, 1.0));
    vec3 air;
    vec4 cloud;
    skyLayers(direction, air, cloud);
    outAir = vec4(min(air, vec3(kMaxHalf)), 1.0);
    outCloud = min(cloud, vec4(kMaxHalf));
}
