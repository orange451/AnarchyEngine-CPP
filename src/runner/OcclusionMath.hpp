#pragma once

// Ambient occlusion's settings and formulas, worked out with no GL context so
// they can be tested. gtao.frag, ao_blur.frag, occlusion.glsl, and
// image_lighting.glsl copy them line for line.
namespace runner {

constexpr int kOcclusionStepsPerSide = 6;
// No sample reaches farther than this part of the buffer's height.
constexpr float kOcclusionMaxRadiusFraction = 0.25f;
// Samples fade out over this last part of the radius.
constexpr float kOcclusionFalloffRange = 0.6f;

struct OcclusionQuality {
    // The buffers are the pane's size divided by this: 2 is half, 1 is full.
    int scale = 2;
    int slices = 3;
    int blurRadius = 4;
};
// Enum.EffectQuality's value: Low 0, Medium 1, High 2. Anything else is Medium.
OcclusionQuality QualitySettings(int quality);
// How many full-size pixels Radius covers at a view depth, capped at
// kOcclusionMaxRadiusFraction of bufferHeight. projectionScale is pixels per
// stud at depth 1.
float PixelRadius(float radius, float viewDepth, float projectionScale, float bufferHeight);
// How much a sample this far away counts: 1 out to 40% of radius, falling
// linearly to 0 at radius.
float Falloff(float distance, float radius);
// GTAO's closed-form, cosine-weighted visible arc of one slice between the
// horizon angles h0 and h1 (radians from the view direction), for a normal
// at angle n in the slice whose projection has length projectedLength.
float ArcVisibility(float n, float h0, float h1, float projectedLength);
// Jimenez et al. 2016's fit for light bouncing between occluders, for one
// channel of albedo: never below visibility, and exactly 1 when open.
float MultiBounce(float visibility, float albedo);
// Lagarde and de Rousiers 2014: how much of a reflection the occlusion
// keeps; 1 when open, falling faster for smooth surfaces.
float SpecularOcclusion(float visibility, float NdotV, float roughness);

}  // namespace runner
