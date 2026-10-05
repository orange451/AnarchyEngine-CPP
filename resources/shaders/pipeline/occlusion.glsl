// Ambient occlusion as the passes that light surfaces read it (ibl.frag,
// merge.frag): gtao.frag's visibility, blurred, upsampled to this pixel and
// raised to the AmbientOcclusionEffect's Intensity. 1 with none. No
// #version: Renderer puts it in after lighting.glsl, whose viewPositionAt
// it uses.

// Full-size depth, which the upsample and the passes that include this read.
uniform sampler2D uDepth;
uniform sampler2D uOcclusion;
// 1 when this frame shaded occlusion; else uOcclusion is a white texel.
uniform float uOcclusionEnabled;
uniform float uOcclusionIntensity;
// The occlusion buffer is the full-size buffers divided by this: 1 or 2.
uniform float uOcclusionScale;

float occlusionAt(vec2 uv, float depth) {
    if (uOcclusionEnabled < 0.5) {
        return 1.0;
    }
    float visibility;
    if (uOcclusionScale < 1.5) {
        visibility = texture(uOcclusion, uv).r;
    } else {
        // The four half-size texels around this pixel, weighted bilinearly and
        // by how near each one's surface is to this pixel's, so shade does not
        // bleed across a silhouette.
        vec2 size = vec2(textureSize(uOcclusion, 0));
        vec2 fullSize = vec2(textureSize(uDepth, 0));
        vec2 position = uv * size - 0.5;
        vec2 base = floor(position);
        vec2 f = position - base;
        // Most of the screen is open: four open texels skip the depth work.
        float taps[4];
        float lowest = 1.0;
        for (int i = 0; i < 4; ++i) {
            ivec2 texel = clamp(ivec2(base) + ivec2(i & 1, i >> 1), ivec2(0), ivec2(size) - 1);
            taps[i] = texelFetch(uOcclusion, texel, 0).r;
            lowest = min(lowest, taps[i]);
        }
        if (lowest >= 1.0) {
            return 1.0;
        }
        float center = -viewPositionAt(uv, depth).z;
        float sum = 0.0;
        float total = 0.0;
        for (int i = 0; i < 4; ++i) {
            ivec2 offset = ivec2(i & 1, i >> 1);
            ivec2 texel = clamp(ivec2(base) + offset, ivec2(0), ivec2(size) - 1);
            ivec2 full = texel * 2;
            float tapDepth = -viewPositionAt((vec2(full) + 0.5) / fullSize, texelFetch(uDepth, full, 0).r).z;
            float bilinear = (offset.x == 1 ? f.x : 1.0 - f.x) * (offset.y == 1 ? f.y : 1.0 - f.y);
            float w = bilinear / (1e-3 + abs(tapDepth - center) / center);
            sum += taps[i] * w;
            total += w;
        }
        visibility = total > 0.0 ? sum / total : 1.0;
    }
    return pow(clamp(visibility, 0.0, 1.0), uOcclusionIntensity);
}
