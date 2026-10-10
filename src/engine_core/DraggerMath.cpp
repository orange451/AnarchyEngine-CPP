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
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
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

// The Rotation handles, defined below beside the drawing they share.
DraggerHandle pick_ring(const DraggerFrame& frame, const DraggerView& view, Vec2 point, float* depth);
bool begin_ring(const DraggerFrame& frame, const DraggerView& view, Vec2 point, DraggerHandle handle,
                DragStart& out);

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

DraggerHandle pick_handle(const DraggerFrame& frame, const DraggerView& view, Vec2 point, float* depth,
                          DraggerMode mode) {
    if (mode == DraggerMode::Rotation) {
        return pick_ring(frame, view, point, depth);
    }
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
                DragStart& out, DraggerMode mode) {
    if (mode == DraggerMode::Rotation) {
        return begin_ring(frame, view, point, handle, out);
    }
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

namespace {

struct Rgba {
    float r, g, b, a;
};

constexpr Rgba kAxisColors[3] = {{0.90f, 0.20f, 0.20f, 1.f}, {0.30f, 0.85f, 0.30f, 1.f}, {0.25f, 0.45f, 0.95f, 1.f}};
constexpr Rgba kActiveColor = {1.f, 0.85f, 0.2f, 1.f};
constexpr float kPlaneAlpha = 0.4f;
constexpr float kFadedAlpha = 0.35f;
constexpr float kHoverWhite = 0.4f;
constexpr float kShaftEnd = 0.8f;
constexpr float kShaftPixels = 3.f;
constexpr float kConePixels = 6.f;
constexpr int kConeSides = 8;

Rgba handle_color(DraggerHandle handle, DraggerHandle hovered, DraggerHandle active) {
    Rgba color;
    if (is_arrow(handle)) {
        color = kAxisColors[arrow_axis(handle)];
    } else {
        int first = 0;
        int second = 0;
        int normal = 0;
        plane_axes(handle, first, second, normal);
        color = kAxisColors[normal];
        color.a = kPlaneAlpha;
    }
    if (active != DraggerHandle::None) {
        if (handle == active) {
            return Rgba{kActiveColor.r, kActiveColor.g, kActiveColor.b, std::max(color.a, 0.6f)};
        }
        color.a *= kFadedAlpha;
        return color;
    }
    if (handle == hovered) {
        color.r += (1.f - color.r) * kHoverWhite;
        color.g += (1.f - color.g) * kHoverWhite;
        color.b += (1.f - color.b) * kHoverWhite;
    }
    return color;
}

void push(std::vector<HandleVertex>& out, Vec3 point, Rgba color) {
    HandleVertex vertex;
    vertex.position[0] = point.x;
    vertex.position[1] = point.y;
    vertex.position[2] = point.z;
    vertex.color[0] = color.r;
    vertex.color[1] = color.g;
    vertex.color[2] = color.b;
    vertex.color[3] = color.a;
    out.push_back(vertex);
}

void push_triangle(std::vector<HandleVertex>& out, Vec3 a, Vec3 b, Vec3 c, Rgba color) {
    push(out, a, color);
    push(out, b, color);
    push(out, c, color);
}

// Two unit vectors square to axis and to each other.
void perpendiculars(Vec3 axis, Vec3& u, Vec3& v) {
    const Vec3 other = std::abs(axis.x) < 0.9f ? Vec3{1.f, 0.f, 0.f} : Vec3{0.f, 1.f, 0.f};
    u = normalize(cross(axis, other));
    v = cross(axis, u);
}

// The rings. Each is kRingSegments straight pieces about its axis, kRingPixels
// across from the middle; one seen nearly edge on turns by how far the
// pointer moves along it on screen instead of by the angle it sweeps.
constexpr int kRingSegments = 64;
constexpr float kRingWidthPixels = 4.f;
constexpr float kFacing = 0.3f;
constexpr float kPi = 3.14159265f;

DraggerHandle ring_handle(int axis) { return static_cast<DraggerHandle>(axis); }

// The ring about axis: point i of kRingSegments, wrapping.
struct Ring {
    Vec3 origin;
    Vec3 u;
    Vec3 v;
    float radius;
    Vec3 point(int i) const {
        const float angle = 2.f * kPi * static_cast<float>(i % kRingSegments) / kRingSegments;
        return add(origin, add(scale(u, std::cos(angle) * radius), scale(v, std::sin(angle) * radius)));
    }
};

Ring ring_of(const DraggerFrame& frame, const DraggerView& view, int axis) {
    Ring ring;
    ring.origin = frame.origin;
    perpendiculars(frame.axes[axis], ring.u, ring.v);
    ring.radius = kRingPixels * handle_scale(view, frame.origin);
    return ring;
}

// The point of ring nearest point on screen, within kPickPixels, nearest the
// camera when two are: where the press is on it. False when none is that near.
bool ring_hit(const Ring& ring, const DraggerView& view, Vec2 point, Vec3& hit, float& depth) {
    const Vec3 eye = eye_of(view).position;
    bool found = false;
    for (int i = 0; i < kRingSegments; ++i) {
        const Vec3 a = ring.point(i);
        const Vec3 b = ring.point(i + 1);
        Vec2 sa;
        Vec2 sb;
        if (!project_point(view, a, sa) || !project_point(view, b, sb)) {
            continue;
        }
        float along = 0.f;
        if (segment_distance(point, sa, sb, along) > kPickPixels) {
            continue;
        }
        const Vec3 at = add(a, scale(sub(b, a), along));
        const float distance = length(sub(at, eye));
        if (!found || distance < depth) {
            found = true;
            hit = at;
            depth = distance;
        }
    }
    return found;
}

DraggerHandle pick_ring(const DraggerFrame& frame, const DraggerView& view, Vec2 point, float* depth) {
    if (handle_scale(view, frame.origin) <= 0.f) {
        return DraggerHandle::None;
    }
    DraggerHandle best = DraggerHandle::None;
    float best_depth = 0.f;
    for (int axis = 0; axis < 3; ++axis) {
        Vec3 hit;
        float distance = 0.f;
        if (ring_hit(ring_of(frame, view, axis), view, point, hit, distance) &&
            (best == DraggerHandle::None || distance < best_depth)) {
            best = ring_handle(axis);
            best_depth = distance;
        }
    }
    if (best != DraggerHandle::None && depth != nullptr) {
        *depth = best_depth;
    }
    return best;
}

bool begin_ring(const DraggerFrame& frame, const DraggerView& view, Vec2 point, DraggerHandle handle,
                DragStart& out) {
    if (!is_arrow(handle)) {
        return false;
    }
    const int index = arrow_axis(handle);
    const Vec3 axis = frame.axes[index];
    const Ring ring = ring_of(frame, view, index);
    DragStart start;
    start.frame = frame;
    start.handle = handle;
    start.mode = DraggerMode::Rotation;
    start.press = point;
    float depth = 0.f;
    if (!ring_hit(ring, view, point, start.hit, depth)) {
        return false;
    }
    const Vec3 toward = normalize(sub(frame.origin, eye_of(view).position));
    start.facing = std::abs(dot(axis, toward)) >= kFacing;
    Vec3 spoke = sub(start.hit, frame.origin);
    if (start.facing) {
        Vec3 hit;
        float t = 0.f;
        if (ray_plane(viewport_ray(view, point), frame.origin, axis, hit, t)) {
            spoke = sub(hit, frame.origin);
        } else {
            start.facing = false;
        }
    }
    spoke = sub(spoke, scale(axis, dot(spoke, axis)));
    if (length(spoke) <= 0.f) {
        return false;
    }
    start.spoke = normalize(spoke);
    // The way the ring runs at the press, on screen: turning forward moves along it.
    const Vec3 forward = cross(axis, normalize(sub(start.hit, frame.origin)));
    Vec2 a;
    Vec2 b;
    start.tangent = {1.f, 0.f};
    if (project_point(view, start.hit, a) && project_point(view, add(start.hit, scale(forward, ring.radius * 0.1f)), b)) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float n = std::sqrt(dx * dx + dy * dy);
        if (n > 1e-4f) {
            start.tangent = {dx / n, dy / n};
        }
    }
    out = start;
    return true;
}

void ring_mesh(const DraggerFrame& frame, const DraggerView& view, DraggerHandle hovered, DraggerHandle active,
               std::vector<HandleVertex>& out) {
    const float half = kRingWidthPixels * 0.5f * handle_scale(view, frame.origin);
    const Vec3 eye = eye_of(view).position;
    for (int axis = 0; axis < 3; ++axis) {
        const Ring ring = ring_of(frame, view, axis);
        const Rgba color = handle_color(ring_handle(axis), hovered, active);
        for (int i = 0; i < kRingSegments; ++i) {
            const Vec3 a = ring.point(i);
            const Vec3 b = ring.point(i + 1);
            // Each piece faces the camera, so the ring keeps its width seen edge on.
            const Vec3 middle = scale(add(a, b), 0.5f);
            const Vec3 side = scale(normalize(cross(sub(b, a), normalize(sub(eye, middle)))), half);
            push_triangle(out, sub(a, side), add(a, side), add(b, side), color);
            push_triangle(out, sub(a, side), add(b, side), sub(b, side), color);
        }
    }
}

}  // namespace

std::optional<float> drag_angle(DragStart& start, const DraggerView& view, Vec2 point, double increment) {
    if (start.mode != DraggerMode::Rotation || !is_arrow(start.handle)) {
        return std::nullopt;
    }
    const DraggerFrame& frame = start.frame;
    const Vec3 axis = frame.axes[arrow_axis(start.handle)];
    if (start.facing) {
        Vec3 hit;
        float t = 0.f;
        if (!ray_plane(viewport_ray(view, point), frame.origin, axis, hit, t)) {
            return std::nullopt;
        }
        Vec3 spoke = sub(hit, frame.origin);
        spoke = sub(spoke, scale(axis, dot(spoke, axis)));
        if (length(spoke) <= 0.f) {
            return std::nullopt;
        }
        spoke = normalize(spoke);
        const float raw = std::atan2(dot(cross(start.spoke, spoke), axis), dot(start.spoke, spoke));
        float step = raw - start.last;
        while (step > kPi) {
            step -= 2.f * kPi;
        }
        while (step < -kPi) {
            step += 2.f * kPi;
        }
        start.turned += step;
        start.last = raw;
    } else {
        // One ring's worth of pixels along it is one radian.
        const float along = (point.x - start.press.x) * start.tangent.x + (point.y - start.press.y) * start.tangent.y;
        start.turned = along / kRingPixels;
    }
    if (increment > 0.0) {
        const double step = increment * static_cast<double>(kDegree);
        return static_cast<float>(std::round(start.turned / step) * step);
    }
    return start.turned;
}

void handle_mesh(const DraggerFrame& frame, const DraggerView& view, DraggerHandle hovered, DraggerHandle active,
                 std::vector<HandleVertex>& out, DraggerMode mode) {
    out.clear();
    Vec2 ignored;
    if (!project_point(view, frame.origin, ignored)) {
        return;
    }
    if (mode == DraggerMode::Rotation) {
        ring_mesh(frame, view, hovered, active, out);
        return;
    }
    const float pixel = handle_scale(view, frame.origin);
    const float arrow = kArrowPixels * pixel;
    const Vec3 eye = eye_of(view).position;
    for (DraggerHandle handle : {DraggerHandle::XY, DraggerHandle::YZ, DraggerHandle::XZ}) {
        if (!handle_visible(frame, view, handle)) {
            continue;
        }
        int first = 0;
        int second = 0;
        int normal = 0;
        plane_axes(handle, first, second, normal);
        const Rgba color = handle_color(handle, hovered, active);
        const Vec3 a = frame.axes[first];
        const Vec3 b = frame.axes[second];
        const auto corner = [&](float u, float v) {
            return add(frame.origin, add(scale(a, arrow * u), scale(b, arrow * v)));
        };
        push_triangle(out, corner(kPlaneNear, kPlaneNear), corner(kPlaneFar, kPlaneNear), corner(kPlaneFar, kPlaneFar),
                      color);
        push_triangle(out, corner(kPlaneNear, kPlaneNear), corner(kPlaneFar, kPlaneFar), corner(kPlaneNear, kPlaneFar),
                      color);
    }
    for (DraggerHandle handle : {DraggerHandle::X, DraggerHandle::Y, DraggerHandle::Z}) {
        if (!handle_visible(frame, view, handle)) {
            continue;
        }
        const Rgba color = handle_color(handle, hovered, active);
        const Vec3 axis = frame.axes[arrow_axis(handle)];
        const Vec3 base = add(frame.origin, scale(axis, arrow * kShaftEnd));
        const Vec3 tip = add(frame.origin, scale(axis, arrow));
        // The shaft faces the camera, so it keeps its width from any side.
        const Vec3 middle = add(frame.origin, scale(axis, arrow * kShaftEnd * 0.5f));
        const Vec3 side = scale(normalize(cross(axis, normalize(sub(eye, middle)))), kShaftPixels * 0.5f * pixel);
        push_triangle(out, sub(frame.origin, side), add(frame.origin, side), add(base, side), color);
        push_triangle(out, sub(frame.origin, side), add(base, side), sub(base, side), color);
        Vec3 u;
        Vec3 v;
        perpendiculars(axis, u, v);
        const float radius = kConePixels * pixel;
        for (int side_index = 0; side_index < kConeSides; ++side_index) {
            const float from = 6.28318531f * static_cast<float>(side_index) / kConeSides;
            const float to = 6.28318531f * static_cast<float>(side_index + 1) / kConeSides;
            const Vec3 p0 = add(base, add(scale(u, std::cos(from) * radius), scale(v, std::sin(from) * radius)));
            const Vec3 p1 = add(base, add(scale(u, std::cos(to) * radius), scale(v, std::sin(to) * radius)));
            push_triangle(out, tip, p0, p1, color);
            push_triangle(out, base, p1, p0, color);
        }
    }
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
