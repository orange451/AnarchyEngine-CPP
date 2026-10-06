#pragma once

// How much a traced reflection counts, and which level of the lit image it
// reads, worked out with no GL context so it can be tested. ssr.glsl copies
// these line for line, as bloom.glsl copies BloomMath.
namespace runner {

// The trace's step budget: strided steps, then bisections once a step hits.
constexpr int kReflectionMaxSteps = 32;
constexpr int kReflectionRefineSteps = 4;
// The lit image's blur, in taps either side of the middle: each level of its
// mip chain is the level above, halved, then blurred by a Gaussian one of its
// own texels wide, so a rough reflection's cone reads a smooth level.
constexpr int kReflectionBlurRadius = 2;

// 1 inside the middle 80% of the screen on both axes, falling smoothly to 0
// at its edges, so reflections never stop at a seam. u and v run 0 to 1.
float EdgeFade(float u, float v);
// 1 up to three quarters of maxDistance, then smoothly to 0 at it. 0 when
// maxDistance is not above 0.
float DistanceFade(float distance, float maxDistance);
// 1 up to four fifths of maxRoughness, then smoothly to 0 at it. 0 when
// maxRoughness is not above 0.
float RoughnessFade(float roughness, float maxRoughness);
// For a reflected ray's view-space z (the camera looks down -Z): 1 for rays
// leaving the camera, smoothly to 0 as they turn back toward it, 0 from z 0.5.
float FacingFade(float reflectedZ);
// The level of the lit image's mip chain, 0 to levels - 1, whose texels a
// reflection cone of this roughness covers at the hit: the cone's radius is
// roughness times the hit's distance, and pixelsPerUnit is how many of
// level 0's pixels one stud spans there.
float ConeLevel(float roughness, float hitDistance, float pixelsPerUnit, int levels);
// Whether one step of the trace hit: the ray's distance from the camera ran
// from depthA to depthB across the step, and the depth buffer's surface there
// is sceneDepth away and thickness deep. The whole span is tested, as McGuire
// and Mara's trace does, so a long stride that passes through the surface
// still hits; a ray already behind it by more than its thickness passes
// behind a thin object.
bool StepHits(float depthA, float depthB, float sceneDepth, float thickness);

// One channel of a trace texel: the reflected light it carries, and how much it counts.
struct ReflectionTexel {
    float light = 0.f;
    float confidence = 0.f;
};
// What the trace writes for a hit of this color at this confidence,
// premultiplied; 0, 0 for a miss.
ReflectionTexel TraceTexel(float hitColor, float confidence);
// One channel of the merge's resolve: the surface's color with the sky's
// reflection, skyLight, swapped for the traced light at the same weight.
float ResolveReflection(float color, float intensity, ReflectionTexel traced, float weight, float skyLight);
// Whether a hit holds after bisection: the ray ended rayDepth from the
// camera, at most thickness behind the surface sceneDepth away.
bool BisectedHitHolds(float rayDepth, float sceneDepth, float thickness);
// How much one tap of the lit image's blur counts, offset taps from the
// middle (within kReflectionBlurRadius), before the blur divides by the sum:
// a Gaussian whose weights add to 1. With firefly, also divided by 1 plus
// the tap's luma (a Karis average), so a glint one texel wide cannot become a
// bright smear across a rough reflection; only the first level down uses it.
float PyramidTapWeight(int offset, float luma, bool firefly);

}  // namespace runner
