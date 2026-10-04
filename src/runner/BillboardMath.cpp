#include "BillboardMath.hpp"

#include "RenderMath.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

BillboardPlacement PlaceBillboard(const engine_core::Matrix4& view, float fovYDegrees, float paneWidth,
                                  float paneHeight, engine_core::Vec3 anchor) {
    BillboardPlacement out;
    if (!(fovYDegrees > 0.f && fovYDegrees < 180.f) || !(paneWidth > 0.f) || !(paneHeight > 0.f)) {
        return out;
    }
    const engine_core::Vec3 eye = engine_core::matrix4_point(view, anchor);
    const float distance = -eye.z;
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !(distance > kSceneNear)) {
        return out;
    }
    const engine_core::Matrix4 projection =
        Perspective(fovYDegrees, paneWidth / paneHeight, kSceneNear, kSceneFar);
    const engine_core::Vec3 ndc = engine_core::matrix4_point(projection, eye);
    const float halfTan = std::tan(fovYDegrees * 0.5f * kDegree);
    out.visible = true;
    out.distance = distance;
    out.x = (ndc.x * 0.5f + 0.5f) * paneWidth;
    out.y = (0.5f - ndc.y * 0.5f) * paneHeight;
    out.pixelsPerUnit = paneHeight / (2.f * distance * halfTan);
    out.depth = std::min(1.f, ndc.z * 0.5f + 0.5f);
    return out;
}

}  // namespace runner
