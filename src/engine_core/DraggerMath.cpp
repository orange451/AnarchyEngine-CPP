#include "DraggerMath.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core {
namespace {

constexpr float kNear = 0.1f;
constexpr float kFar = 1000.f;
constexpr float kParallel = 1e-6f;
constexpr float kDegree = 0.01745329252f;

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 v) { return std::sqrt(dot(v, v)); }
Vec3 normalize(Vec3 v) {
    const float n = length(v);
    return n > 0.f ? scale(v, 1.f / n) : v;
}

// The camera's axes and eye from its Transform.
struct Eye {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    Vec3 position;
    float tan_half;
    float aspect;
};

Eye eye_of(const DraggerView& view) {
    const float* m = view.camera.m;
    Eye eye;
    eye.right = normalize({m[0], m[1], m[2]});
    eye.up = normalize({m[4], m[5], m[6]});
    eye.forward = normalize({-m[8], -m[9], -m[10]});
    eye.position = {m[12], m[13], m[14]};
    eye.tan_half = std::tan(view.fov_degrees * kDegree * 0.5f);
    eye.aspect = view.size.y > 0.f ? view.size.x / view.size.y : 1.f;
    return eye;
}

bool is_arrow(DraggerHandle handle) {
    return handle == DraggerHandle::X || handle == DraggerHandle::Y || handle == DraggerHandle::Z;
}

int arrow_axis(DraggerHandle handle) { return static_cast<int>(handle); }

// A plane handle's two axes and its normal axis.
void plane_axes(DraggerHandle handle, int& first, int& second, int& normal) {
    switch (handle) {
    case DraggerHandle::XY:
        first = 0, second = 1, normal = 2;
        return;
    case DraggerHandle::YZ:
        first = 1, second = 2, normal = 0;
        return;
    default:
        first = 0, second = 2, normal = 1;
        return;
    }
}

// Where ray meets the plane through origin with this normal. False when
// parallel, behind the ray, or past the far plane.
bool ray_plane(const DraggerRay& ray, Vec3 origin, Vec3 normal, Vec3& hit, float& t) {
    const float denom = dot(ray.direction, normal);
    if (std::abs(denom) < kParallel) {
        return false;
    }
    t = dot(sub(origin, ray.origin), normal) / denom;
    if (t <= 0.f || t > kFar) {
        return false;
    }
    hit = add(ray.origin, scale(ray.direction, t));
    return true;
}

// The closest points between ray and the line origin + s * axis: s along the
// axis and t along the ray. False when they run parallel.
bool ray_line(const DraggerRay& ray, Vec3 origin, Vec3 axis, float& s, float& t) {
    const Vec3 w0 = sub(origin, ray.origin);
    const float b = dot(axis, ray.direction);
    const float d = dot(axis, w0);
    const float e = dot(ray.direction, w0);
    const float denom = 1.f - b * b;
    if (std::abs(denom) < kParallel) {
        return false;
    }
    s = (b * e - d) / denom;
    t = (e - b * d) / denom;
    return true;
}

float snapped(float value, double increment) {
    if (increment <= 0.0) {
        return value;
    }
    const double step = increment;
    return static_cast<float>(std::round(value / step) * step);
}

float segment_distance(Vec2 point, Vec2 a, Vec2 b, float& along) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float squared = dx * dx + dy * dy;
    along = squared > 0.f ? std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / squared, 0.f, 1.f) : 0.f;
    const float px = a.x + dx * along - point.x;
    const float py = a.y + dy * along - point.y;
    return std::sqrt(px * px + py * py);
}

}  // namespace

DraggerRay viewport_ray(const DraggerView& view, Vec2 point) {
    const Eye eye = eye_of(view);
    const float nx = view.size.x > 0.f ? 2.f * point.x / view.size.x - 1.f : 0.f;
    const float ny = view.size.y > 0.f ? 1.f - 2.f * point.y / view.size.y : 0.f;
    DraggerRay ray;
    ray.origin = eye.position;
    ray.direction = normalize(add(add(scale(eye.right, nx * eye.tan_half * eye.aspect), scale(eye.up, ny * eye.tan_half)),
                                  eye.forward));
    return ray;
}

bool project_point(const DraggerView& view, Vec3 point, Vec2& out) {
    const Eye eye = eye_of(view);
    const Vec3 v = sub(point, eye.position);
    const float z = dot(v, eye.forward);
    if (z <= kNear || eye.tan_half <= 0.f) {
        return false;
    }
    const float nx = dot(v, eye.right) / (z * eye.tan_half * eye.aspect);
    const float ny = dot(v, eye.up) / (z * eye.tan_half);
    out.x = (nx + 1.f) * 0.5f * view.size.x;
    out.y = (1.f - ny) * 0.5f * view.size.y;
    return true;
}

float handle_scale(const DraggerView& view, Vec3 origin) {
    const Eye eye = eye_of(view);
    const float z = std::max(dot(sub(origin, eye.position), eye.forward), kNear);
    return view.size.y > 0.f ? 2.f * z * eye.tan_half / view.size.y : 0.f;
}

bool handle_visible(const DraggerFrame& frame, const DraggerView& view, DraggerHandle handle) {
    if (handle == DraggerHandle::None) {
        return false;
    }
    const Vec3 toward = normalize(sub(frame.origin, eye_of(view).position));
    if (is_arrow(handle)) {
        return std::abs(dot(frame.axes[arrow_axis(handle)], toward)) <= 0.98f;
    }
    int first = 0;
    int second = 0;
    int normal = 0;
    plane_axes(handle, first, second, normal);
    return std::abs(dot(frame.axes[normal], toward)) >= 0.1f;
}

DraggerHandle pick_handle(const DraggerFrame& frame, const DraggerView& view, Vec2 point, float* depth) {
    const DraggerRay ray = viewport_ray(view, point);
    const float arrow = kArrowPixels * handle_scale(view, frame.origin);
    if (arrow <= 0.f) {
        return DraggerHandle::None;
    }
    // Plane squares first: they are the smaller targets.
    DraggerHandle best = DraggerHandle::None;
    float best_t = 0.f;
    for (DraggerHandle handle : {DraggerHandle::XY, DraggerHandle::YZ, DraggerHandle::XZ}) {
        if (!handle_visible(frame, view, handle)) {
            continue;
        }
        int first = 0;
        int second = 0;
        int normal = 0;
        plane_axes(handle, first, second, normal);
        Vec3 hit;
        float t = 0.f;
        if (!ray_plane(ray, frame.origin, frame.axes[normal], hit, t)) {
            continue;
        }
        const Vec3 local = sub(hit, frame.origin);
        const float u = dot(local, frame.axes[first]) / arrow;
        const float v = dot(local, frame.axes[second]) / arrow;
        const bool inside = u >= kPlaneNear && u <= kPlaneFar && v >= kPlaneNear && v <= kPlaneFar;
        if (inside && (best == DraggerHandle::None || t < best_t)) {
            best = handle;
            best_t = t;
        }
    }
    if (best != DraggerHandle::None) {
        if (depth != nullptr) {
            *depth = best_t;
        }
        return best;
    }
    Vec2 start;
    if (!project_point(view, frame.origin, start)) {
        return DraggerHandle::None;
    }
    const Vec3 eye = eye_of(view).position;
    float best_distance = kPickPixels;
    for (DraggerHandle handle : {DraggerHandle::X, DraggerHandle::Y, DraggerHandle::Z}) {
        if (!handle_visible(frame, view, handle)) {
            continue;
        }
        const Vec3 axis = frame.axes[arrow_axis(handle)];
        Vec2 tip;
        if (!project_point(view, add(frame.origin, scale(axis, arrow)), tip)) {
            continue;
        }
        float along = 0.f;
        const float distance = segment_distance(point, start, tip, along);
        if (distance <= best_distance) {
            best_distance = distance;
            best = handle;
            if (depth != nullptr) {
                *depth = length(sub(add(frame.origin, scale(axis, arrow * along)), eye));
            }
        }
    }
    return best;
}

bool begin_drag(const DraggerFrame& frame, const DraggerView& view, Vec2 point, DraggerHandle handle,
                DragStart& out) {
    if (handle == DraggerHandle::None) {
        return false;
    }
    const DraggerRay ray = viewport_ray(view, point);
    DragStart start;
    start.frame = frame;
    start.handle = handle;
    if (is_arrow(handle)) {
        const Vec3 axis = frame.axes[arrow_axis(handle)];
        float s = 0.f;
        float t = 0.f;
        if (!ray_line(ray, frame.origin, axis, s, t)) {
            return false;
        }
        start.along = s;
        start.hit = add(frame.origin, scale(axis, s));
    } else {
        int first = 0;
        int second = 0;
        int normal = 0;
        plane_axes(handle, first, second, normal);
        float t = 0.f;
        if (!ray_plane(ray, frame.origin, frame.axes[normal], start.hit, t)) {
            return false;
        }
    }
    out = start;
    return true;
}

std::optional<Vec3> drag_offset(const DragStart& start, const DraggerView& view, Vec2 point, double increment) {
    const DraggerRay ray = viewport_ray(view, point);
    const DraggerFrame& frame = start.frame;
    if (is_arrow(start.handle)) {
        const Vec3 axis = frame.axes[arrow_axis(start.handle)];
        float s = 0.f;
        float t = 0.f;
        if (!ray_line(ray, frame.origin, axis, s, t) || t <= 0.f || t > kFar) {
            return std::nullopt;
        }
        return scale(axis, snapped(s - start.along, increment));
    }
    if (start.handle == DraggerHandle::None) {
        return std::nullopt;
    }
    int first = 0;
    int second = 0;
    int normal = 0;
    plane_axes(start.handle, first, second, normal);
    Vec3 hit;
    float t = 0.f;
    if (!ray_plane(ray, frame.origin, frame.axes[normal], hit, t)) {
        return std::nullopt;
    }
    const Vec3 raw = sub(hit, start.hit);
    const float u = snapped(dot(raw, frame.axes[first]), increment);
    const float v = snapped(dot(raw, frame.axes[second]), increment);
    return add(scale(frame.axes[first], u), scale(frame.axes[second], v));
}

DraggerFrame dragger_frame(const Matrix4& target, bool local) {
    DraggerFrame frame;
    frame.origin = {target.m[12], target.m[13], target.m[14]};
    if (local) {
        frame.axes[0] = normalize({target.m[0], target.m[1], target.m[2]});
        frame.axes[1] = normalize({target.m[4], target.m[5], target.m[6]});
        frame.axes[2] = normalize({target.m[8], target.m[9], target.m[10]});
    } else {
        frame.axes[0] = {1.f, 0.f, 0.f};
        frame.axes[1] = {0.f, 1.f, 0.f};
        frame.axes[2] = {0.f, 0.f, 1.f};
    }
    return frame;
}

}  // namespace engine_core
