#pragma once

#include "Matrix4.hpp"

// Where a BillboardGui's anchor lands in a Scene View, by the same camera and
// projection the renderer draws with (RenderMath), so the billboard sits
// exactly on what it floats over. Needs no GL context.
namespace runner {

struct BillboardPlacement {
    // False at or behind the near plane, or for a view, angle, or pane that
    // cannot be drawn: then nothing else is set.
    bool visible = false;
    // The anchor on screen, in pane points from the top left.
    float x = 0.f;
    float y = 0.f;
    // The points one world unit covers at the anchor's distance: what a
    // percentage of 100 on the billboard is.
    float pixelsPerUnit = 0.f;
    // Along the camera's forward axis, in world units.
    float distance = 0.f;
    // As the depth texture holds it: 0 at the near plane, 1 at the far.
    float depth = 0.f;
};

BillboardPlacement PlaceBillboard(const engine_core::Matrix4& view, float fovYDegrees, float paneWidth,
                                  float paneHeight, engine_core::Vec3 anchor);

}  // namespace runner
