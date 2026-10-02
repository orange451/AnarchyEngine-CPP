#include "ShadowMath.hpp"

#include "RenderMath.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

using engine_core::Matrix4;
using engine_core::Vec3;

void CascadeSplits(float nearZ, float shadowDistance, int count, float lambda, float* out) {
    count = std::clamp(count, 1, kMaxCascades);
    const float farZ = std::max(shadowDistance, nearZ);
    out[0] = nearZ;
    for (int i = 1; i <= count; ++i) {
        const float part = static_cast<float>(i) / static_cast<float>(count);
        const float even = nearZ + (farZ - nearZ) * part;
        const float logarithmic = nearZ > 0.f ? nearZ * std::pow(farZ / nearZ, part) : even;
        out[i] = lambda * logarithmic + (1.f - lambda) * even;
    }
}

Sphere FrustumSliceSphere(const Matrix4& cameraWorld, float fovYDegrees, float aspect, float nearZ, float farZ) {
    const float t = std::tan(fovYDegrees * 0.5f * kDegree);
    // How far the frustum's corners lean off its axis, per unit of depth, squared.
    const float k2 = t * t * (1.f + aspect * aspect);
    // On the axis where the near and far corners are equally far, but no
    // farther than the far plane: there its corners alone set the size.
    const float centerDepth = std::min(0.5f * (farZ + nearZ) * (1.f + k2), farZ);
    const float radius = std::sqrt((farZ - centerDepth) * (farZ - centerDepth) + farZ * farZ * k2);
    const Vec3 eye = engine_core::matrix4_position(cameraWorld);
    const Vec3 forward = Normalize({-cameraWorld.m[8], -cameraWorld.m[9], -cameraWorld.m[10]});
    return {Add(eye, Scale(forward, centerDepth)), radius};
}

CascadeFit FitCascade(const Sphere& sphere, Vec3 shineDirection, int size, float pullNear) {
    size = std::max(size, 4);
    // Two texels of slack, so the sphere still fits once its center is floored.
    const float texel = 2.f * sphere.radius / static_cast<float>(size - 2);
    const float half = texel * static_cast<float>(size) * 0.5f;
    // Turned toward the light about the world's origin and never moved, so a
    // whole texel in this view is a whole texel in the world.
    const Matrix4 view = LookAtView({0.f, 0.f, 0.f}, shineDirection, {0.f, 1.f, 0.f});
    const Vec3 center = engine_core::matrix4_point(view, sphere.center);
    const float x = std::floor(center.x / texel) * texel;
    const float y = std::floor(center.y / texel) * texel;
    // It looks down -Z, so the sphere runs from depth -z - r to -z + r.
    const float nearDepth = -center.z - sphere.radius;
    const Matrix4 projection = Orthographic(x - half, x + half, y - half, y + half, nearDepth - std::max(pullNear, 0.f),
                                            -center.z + sphere.radius);
    CascadeFit fit;
    fit.viewProjection = engine_core::matrix4_multiply(projection, view);
    fit.lightView = view;
    fit.texelWorld = texel;
    fit.nearDepth = nearDepth;
    return fit;
}

}  // namespace runner
