#include "ShadowMath.hpp"

#include "RenderMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace runner {

using engine_core::Matrix4;
using engine_core::Vec3;

namespace {

float PlaneDistance(const float plane[4], engine_core::Vec3 p) {
    return plane[0] * p.x + plane[1] * p.y + plane[2] * p.z + plane[3];
}

}  // namespace

// Gribb and Hartmann: each plane is the last row of viewProjection plus or less another.
Frustum MakeFrustum(const Matrix4& viewProjection) {
    const float* m = viewProjection.m;
    // Row i of the matrix, which is stored by column.
    const auto row = [m](int i, int k) { return m[k * 4 + i]; };
    Frustum out;
    for (int k = 0; k < 4; ++k) {
        out.planes[0][k] = row(3, k) + row(0, k);
        out.planes[1][k] = row(3, k) - row(0, k);
        out.planes[2][k] = row(3, k) + row(1, k);
        out.planes[3][k] = row(3, k) - row(1, k);
        out.planes[4][k] = row(3, k) - row(2, k);
        out.planes[5][k] = row(3, k) + row(2, k);
    }
    for (int p = 0; p < 6; ++p) {
        const float* plane = out.planes[p];
        out.lengths[p] = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    }
    return out;
}

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

float ShadowNear(float radius) { return std::max(radius * kShadowNearPart, 1e-4f); }

SpotShadow SpotShadowFor(Vec3 position, Vec3 direction, float outerFovDegrees, float radius, int size) {
    const float fov = std::clamp(outerFovDegrees + 2.f, 1.f, kMaxSpotShadowFov);
    const Matrix4 view = LookAtView(position, Add(position, direction), {0.f, 1.f, 0.f});
    const Matrix4 projection = Perspective(fov, 1.f, ShadowNear(radius), radius);
    SpotShadow out;
    out.viewProjection = engine_core::matrix4_multiply(projection, view);
    out.texelPerDistance = 2.f * std::tan(fov * 0.5f * kDegree) / static_cast<float>(std::max(size, 1));
    return out;
}

std::array<Matrix4, 6> CubeFaceViewProjections(Vec3 position, float radius, float faceScale) {
    static const Vec3 kLook[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const Vec3 kUp[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    // Perspective's 1 / tan(fov / 2) is faceScale.
    const float fov = 2.f * std::atan(1.f / faceScale) / kDegree;
    const Matrix4 projection = Perspective(fov, 1.f, ShadowNear(radius), radius);
    std::array<Matrix4, 6> faces;
    for (int face = 0; face < 6; ++face) {
        faces[face] =
            engine_core::matrix4_multiply(projection, LookAtView(position, Add(position, kLook[face]), kUp[face]));
    }
    return faces;
}

CubeTexel CubeFaceUv(Vec3 v, float faceScale) {
    const float ax = std::fabs(v.x);
    const float ay = std::fabs(v.y);
    const float az = std::fabs(v.z);
    // GL's cube-face table: the major axis picks the face, and sc and tc
    // are where on it, as the face matrices above draw them.
    CubeTexel out;
    float sc = 0.f;
    float tc = 0.f;
    float major = 1.f;
    if (ax >= ay && ax >= az) {
        major = ax;
        out.face = v.x > 0.f ? 0 : 1;
        sc = v.x > 0.f ? -v.z : v.z;
        tc = -v.y;
    } else if (ay >= az) {
        major = ay;
        out.face = v.y > 0.f ? 2 : 3;
        sc = v.x;
        tc = v.y > 0.f ? v.z : -v.z;
    } else {
        major = az;
        out.face = v.z > 0.f ? 4 : 5;
        sc = v.z > 0.f ? v.x : -v.x;
        tc = -v.y;
    }
    out.u = sc / major * faceScale * 0.5f + 0.5f;
    out.v = tc / major * faceScale * 0.5f + 0.5f;
    return out;
}

float CubeDepth(Vec3 fromLight, float radius) {
    const float n = ShadowNear(radius);
    const float f = radius;
    const float z = std::max({std::fabs(fromLight.x), std::fabs(fromLight.y), std::fabs(fromLight.z)});
    return ((f + n) / (f - n) - 2.f * f * n / ((f - n) * z)) * 0.5f + 0.5f;
}

bool CubeFaceVisible(const Matrix4& cameraViewProjection, Vec3 position, float radius, int face) {
    return CubeFaceVisible(MakeFrustum(cameraViewProjection), position, radius, face);
}

bool CubeFaceVisible(const Frustum& camera, Vec3 position, float radius, int face) {
    static const Vec3 kAxis[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const Vec3 kSideA[6] = {{0, 1, 0}, {0, 1, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
    static const Vec3 kSideB[6] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    // The pyramid's apex and the corners of its square at radius.
    Vec3 points[5] = {position};
    int count = 1;
    for (const float a : {-1.f, 1.f}) {
        for (const float b : {-1.f, 1.f}) {
            const Vec3 corner = Add(kAxis[face], Add(Scale(kSideA[face], a), Scale(kSideB[face], b)));
            points[count++] = Add(position, Scale(corner, radius));
        }
    }
    // Hidden when every point is outside one plane.
    for (const auto& plane : camera.planes) {
        bool outside = true;
        for (const Vec3& point : points) {
            if (PlaneDistance(plane, point) >= 0.f) {
                outside = false;
                break;
            }
        }
        if (outside) {
            return false;
        }
    }
    return true;
}

float ProjectedReach(Vec3 lightPosition, float radius, Vec3 cameraPosition, float fovYDegrees) {
    const float distance = Length(Sub(lightPosition, cameraPosition));
    if (distance <= radius) {
        return std::numeric_limits<float>::infinity();
    }
    const float sine = radius / distance;
    return sine / std::sqrt(1.f - sine * sine) / std::tan(fovYDegrees * 0.5f * kDegree);
}

Sphere WorldBounds(const Matrix4& model, const float boxMin[3], const float boxMax[3]) {
    const Vec3 center{(boxMin[0] + boxMax[0]) * 0.5f, (boxMin[1] + boxMax[1]) * 0.5f, (boxMin[2] + boxMax[2]) * 0.5f};
    const Vec3 extent{(boxMax[0] - boxMin[0]) * 0.5f, (boxMax[1] - boxMin[1]) * 0.5f, (boxMax[2] - boxMin[2]) * 0.5f};
    float scale = 0.f;
    for (int column = 0; column < 3; ++column) {
        scale = std::max(scale, Length({model.m[column * 4], model.m[column * 4 + 1], model.m[column * 4 + 2]}));
    }
    return {engine_core::matrix4_point(model, center), Length(extent) * scale};
}

bool SphereInFrustum(const Frustum& frustum, const Sphere& sphere, bool ignoreNear) {
    for (int p = 0; p < (ignoreNear ? 5 : 6); ++p) {
        if (PlaneDistance(frustum.planes[p], sphere.center) / frustum.lengths[p] < -sphere.radius) {
            return false;
        }
    }
    return true;
}

bool SphereInFrustum(const Matrix4& viewProjection, const Sphere& sphere, bool ignoreNear) {
    return SphereInFrustum(MakeFrustum(viewProjection), sphere, ignoreNear);
}

bool SpheresTouch(const Sphere& a, const Sphere& b) { return Length(Sub(a.center, b.center)) <= a.radius + b.radius; }

}  // namespace runner
