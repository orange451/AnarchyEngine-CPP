#pragma once

// How much a traced reflection counts, and which level of the lit image it
// reads, worked out with no GL context so it can be tested. ssr.glsl copies
// these line for line, as bloom.glsl copies BloomMath.
namespace runner {

// The trace's step budget: strided steps, then bisections once a step hits.
constexpr int kReflectionMaxSteps = 32;
constexpr int kReflectionRefineSteps = 4;

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

}  // namespace runner
