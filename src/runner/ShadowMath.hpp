#pragma once

#include "Matrix4.hpp"

#include <array>

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

// A PointLight's or SpotLight's map starts this part of its Radius away, so
// depth precision is the same at any scale.
constexpr float kShadowNearPart = 0.01f;
// A SpotLight's map is never wider than this, in degrees: past it a
// perspective map has almost no texels left in the middle.
constexpr float kMaxSpotShadowFov = 170.f;
// Texels at each edge of an atlas tile that its own lookups never centre on:
// the 3 by 3 filter reaches two texels out.
constexpr int kTileGuard = 2;

float ShadowNear(float radius);

struct SpotShadow {
    engine_core::Matrix4 viewProjection = engine_core::matrix4_identity();
    // World units across a texel, per unit of distance from the light.
    float texelPerDistance = 0.f;
};

// A perspective map down direction, a little wider than the cone so the
// filter taps at its edge stay on the map, from ShadowNear(radius) to radius.
SpotShadow SpotShadowFor(engine_core::Vec3 position, engine_core::Vec3 direction, float outerFovDegrees, float radius,
                         int size);

// GL's cube faces in order, +X, -X, +Y, -Y, +Z, -Z, each with the up GL lays
// that face out with. faceScale below 1 widens each past 90 degrees so its
// 90 degrees end that part of the way to the tile's edge: a filter tap that
// crosses the face's edge still reads true depth, so there is no seam.
std::array<engine_core::Matrix4, 6> CubeFaceViewProjections(engine_core::Vec3 position, float radius,
                                                            float faceScale = 1.f);

struct CubeTexel {
    // GL's order, as CubeFaceViewProjections.
    int face = 0;
    // On that face's tile, 0 to 1.
    float u = 0.5f;
    float v = 0.5f;
};
// Where a direction from a PointLight lands, for faces drawn with faceScale.
// shadow.glsl picks the face and place the same way.
CubeTexel CubeFaceUv(engine_core::Vec3 fromLight, float faceScale);
// The 0 to 1 depth a cube face draws at fromLight (a point less the light):
// perspective depth along whichever axis it is most along. Any faceScale.
float CubeDepth(engine_core::Vec3 fromLight, float radius);
// Whether any of a cube face's pyramid, from the light out to radius, can be
// inside the camera's frustum. A face that cannot is never read this frame.
bool CubeFaceVisible(const engine_core::Matrix4& cameraViewProjection, engine_core::Vec3 position, float radius,
                     int face);

// How big a light's sphere of reach looks: the tangent of its angular
// radius over the tangent of half the view's vertical angle, so 1 fills the
// view top to bottom. Scale-free: a place ten times bigger, seen from ten
// times farther, gets the same. Infinite with the camera inside it.
float ProjectedReach(engine_core::Vec3 lightPosition, float radius, engine_core::Vec3 cameraPosition,
                     float fovYDegrees);

// A mesh's local box as a world sphere under model, scale and all.
Sphere WorldBounds(const engine_core::Matrix4& model, const float boxMin[3], const float boxMax[3]);
// Whether any of sphere is inside the clip volume of viewProjection. With
// ignoreNear, anything on the near plane's far side counts too.
bool SphereInFrustum(const engine_core::Matrix4& viewProjection, const Sphere& sphere, bool ignoreNear = false);
bool SpheresTouch(const Sphere& a, const Sphere& b);

}  // namespace runner
