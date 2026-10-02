#pragma once

#include "Matrix4.hpp"

// The shadow maps' cameras, worked out with no GL context so they can be
// tested. Lengths are world units (studs), and every rule about size is a
// ratio, so a place built at any scale gets the same shadows. shadow.glsl
// reads the maps back with these projections.
namespace runner {

constexpr int kMaxCascades = 4;

struct Sphere {
    engine_core::Vec3 center{};
    float radius = 0.f;
};

// Where each of count cascades ends, in view depth: out[0] is nearZ and
// out[count] is shadowDistance. lambda blends an even split (0) with a
// logarithmic one (1). out holds count + 1 values; count is 1 to kMaxCascades.
void CascadeSplits(float nearZ, float shadowDistance, int count, float lambda, float* out);

// The smallest sphere around the camera's frustum from view depth nearZ to
// farZ, in world space. Its radius depends only on the angles and depths, so
// a cascade fitted to it keeps its size however the camera turns.
Sphere FrustumSliceSphere(const engine_core::Matrix4& cameraWorld, float fovYDegrees, float aspect, float nearZ,
                          float farZ);

struct CascadeFit {
    // World to the cascade's clip space.
    engine_core::Matrix4 viewProjection = engine_core::matrix4_identity();
    // World to the light's view: a rotation only, looking down the shine.
    engine_core::Matrix4 lightView = engine_core::matrix4_identity();
    // World units across one texel.
    float texelWorld = 0.f;
    // The near plane's depth in lightView before any pull: the sphere's nearest point to the sun.
    float nearDepth = 0.f;
};

// An orthographic map size texels square around sphere, looking down
// shineDirection. Its center is floored to whole texels in a view that only
// turns, so a camera that moves slides the map by whole texels and shadow
// edges do not crawl. pullNear moves the near plane that far toward the sun,
// so casters outside the sphere that shade it are drawn: GLES has no depth clamp.
CascadeFit FitCascade(const Sphere& sphere, engine_core::Vec3 shineDirection, int size, float pullNear = 0.f);

}  // namespace runner
