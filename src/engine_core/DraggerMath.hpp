#pragma once

#include "Matrix4.hpp"
#include "Vector2.hpp"
#include "types.hpp"

#include <optional>
#include <vector>

// The geometry under a Dragger's translate handles: rays from the view,
// which handle a point is over, and how far a drag has moved. No GL and no
// DataModel. It matches runner::Perspective: column-major matrices, right
// handed, Y up, a camera looking down its -Z with a vertical field of view,
// near 0.1 and far 1000. Screen points are in points from the view's top
// left, y down.
namespace engine_core {

// Enum.DraggerHandle's values, and None for no handle.
enum class DraggerHandle { None = -1, X = 0, Y = 1, Z = 2, XY = 3, YZ = 4, XZ = 5 };

// Where the handles sit: the target's position, and the three axes the
// arrows point along (unit length).
struct DraggerFrame {
    Vec3 origin{};
    Vec3 axes[3]{};
};

// The view the handles are seen in: the camera's Transform, its vertical
// field of view, and the view's size in points.
struct DraggerView {
    Matrix4 camera = matrix4_identity();
    float fov_degrees = 70.f;
    Vec2 size{};
};

struct DraggerRay {
    Vec3 origin{};
    Vec3 direction{};
};

// What a drag remembers from its press: the frame then, the handle, the
// point the press hit, and for an arrow how far along the axis it was.
struct DragStart {
    DraggerFrame frame{};
    DraggerHandle handle = DraggerHandle::None;
    Vec3 hit{};
    float along = 0.f;
};

// An arrow is this many pixels long at any distance. A point this close to
// an arrow is over it. Plane squares span these fractions of an arrow along
// both of their axes.
constexpr float kArrowPixels = 100.f;
constexpr float kPickPixels = 8.f;
constexpr float kPlaneNear = 0.25f;
constexpr float kPlaneFar = 0.40f;

// The world ray through a point of the view.
DraggerRay viewport_ray(const DraggerView& view, Vec2 point);
// The view point a world point draws at. False when it is behind the near plane.
bool project_point(const DraggerView& view, Vec3 point, Vec2& out);
// The world length of one pixel at origin's depth.
float handle_scale(const DraggerView& view, Vec3 origin);
// False for an arrow that points nearly at the camera, and for a plane seen
// nearly edge on: a handle that cannot be drawn or grabbed usefully.
bool handle_visible(const DraggerFrame& frame, const DraggerView& view, DraggerHandle handle);
// The handle under point, or None. Plane squares win over arrows. depth,
// when not null, gets the hit's distance from the camera.
DraggerHandle pick_handle(const DraggerFrame& frame, const DraggerView& view, Vec2 point, float* depth);
// Starts a drag of handle from point. False when the ray cannot meet the handle.
bool begin_drag(const DraggerFrame& frame, const DraggerView& view, Vec2 point, DraggerHandle handle,
                DragStart& out);
// The world offset since the drag began, snapped along the frame's axes to a
// multiple of increment when it is above 0. Nullopt when the ray runs
// parallel to the axis or plane, or meets it behind the camera or past the
// far plane: the caller keeps the last offset.
std::optional<Vec3> drag_offset(const DragStart& start, const DraggerView& view, Vec2 point, double increment);
// The frame for a target's Transform: world axes, or with local the
// target's rotation columns, normalized.
DraggerFrame dragger_frame(const Matrix4& target, bool local);

// One corner of a handle triangle: world position, then straight RGBA.
struct HandleVertex {
    float position[3]{};
    float color[4]{};
};

// The handles of frame as seen in view, as colored world-space triangles
// into out (cleared first): plane squares, then arrows, each a shaft and a
// cone. Sized and hidden by the same rules pick_handle uses, so what is drawn
// is what can be grabbed. X is red, Y green, Z blue; a square takes its
// normal's color, see-through. hovered brightens; while active is not None it
// is yellow and the rest fade. Nothing when the frame is behind the camera.
void handle_mesh(const DraggerFrame& frame, const DraggerView& view, DraggerHandle hovered, DraggerHandle active,
                 std::vector<HandleVertex>& out);

}  // namespace engine_core
