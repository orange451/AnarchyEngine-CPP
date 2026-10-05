#pragma once

// Bloom's chain, worked out with no GL context so it can be tested. Renderer
// draws the chain PlanBloom gives; bloom.glsl's brightWeight is BrightWeight.
namespace runner {

constexpr int kBloomMaxLevels = 8;
// No level is smaller than this many pixels on either side.
constexpr int kBloomMinPixels = 2;
// BloomEffect.Size is in pixels at a view this tall.
constexpr float kBloomReferenceHeight = 1080.f;
// The upsample tent's offset, in texels of the level it reads, stays in this range.
constexpr float kBloomMinRadius = 0.5f;
constexpr float kBloomMaxRadius = 4.f;

// How many levels a width by height pane has room for, 0 to
// kBloomMaxLevels. Level k is the pane halved k times, rounding down, and
// each must be at least kBloomMinPixels on both sides.
int BloomLevelsAvailable(int width, int height);

struct BloomPlan {
    // Levels drawn, 1 to kBloomMaxLevels. 0 draws no bloom.
    int levels = 0;
    // The upsample tent's offset, in texels of the level it reads.
    float radius = 1.f;
};

// The chain for a BloomEffect's Size on a width by height pane. Size is
// scaled from kBloomReferenceHeight to height. The chain goes down to the
// first level whose texel is at least that many pixels, and radius makes up
// the rest, so the spread follows Size smoothly. Size 0, or a pane with no
// room for a level, plans none.
BloomPlan PlanBloom(float size, int width, int height);

// How much of a color whose brightest channel is brightness goes into the
// bloom, 0 to 1. Nothing below threshold minus a knee of half the threshold,
// a quadratic fade across the knee, and above it exactly the light over the
// threshold: brightness - threshold. Threshold 0 takes all of any light.
float BrightWeight(float brightness, float threshold);

}  // namespace runner
