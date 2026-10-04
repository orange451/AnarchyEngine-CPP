#include "GridBands.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace runner {

namespace {

// What grid.frag uses, which these bands must agree with.
constexpr double kMinCellPixels = 5.0;
// Pixels from a rule's center line to the band's edge. A rule covers pixels
// within one pixel of its line; the rest is room for the shader's derivatives.
constexpr double kRuleHalfWidth = 2.0;
// The axes are 1.5 pixels wide.
constexpr double kAxisHalfWidth = 2.75;
// How far past the samples' estimate a rule spacing is kept, for what falls between them.
constexpr double kNeedMargin = 1.3;
// The faintest alpha worth a band: under 4/255, a gray line changes the scene by
// less than 2/255, which no one can see.
constexpr double kVisibleAlpha = 4.0 / 255.0;
// grid.frag's weights: the finest rule fades from minor to nothing as its cells
// shrink, and the strongest anything is drawn is an axis, at 1.
constexpr double kMinorAlpha = 0.3;
constexpr int kSamplesX = 48;
constexpr int kSamplesY = 28;
// Rule spacings 10^0 up to 10^kMaxPower; finer is never drawn, since lod is at least 0.
constexpr int kMaxPower = 5;
// More lines of one spacing than this is a camera the bands would not save anything for.
constexpr double kMaxLinesPerAxis = 3000;

struct Vec4 {
    double x = 0;
    double y = 0;
    double z = 0;
    double w = 0;
};

Vec4 Mul(const engine_core::Matrix4& matrix, const Vec4& v) {
    const float* a = matrix.m;
    return {a[0] * v.x + a[4] * v.y + a[8] * v.z + a[12] * v.w, a[1] * v.x + a[5] * v.y + a[9] * v.z + a[13] * v.w,
            a[2] * v.x + a[6] * v.y + a[10] * v.z + a[14] * v.w, a[3] * v.x + a[7] * v.y + a[11] * v.z + a[15] * v.w};
}

struct FloorPoint {
    bool hit = false;
    double x = 0;
    double z = 0;
};

class Camera {
public:
    explicit Camera(const GridView& view)
        : view_(view.view),
          projection_(view.projection),
          inverseProjection_(engine_core::matrix4_inverse(view.projection)),
          inverseView_(engine_core::matrix4_inverse(view.view)),
          width_(view.width),
          height_(view.height) {
        const Vec4 eye = Mul(inverseView_, {0, 0, 0, 1});
        eyeX_ = eye.x;
        eyeY_ = eye.y;
        eyeZ_ = eye.z;
        // The near plane's view-space depth, from the projection.
        const Vec4 nearPoint = Mul(inverseProjection_, {0, 0, -1, 1});
        near_ = -nearPoint.z / nearPoint.w;
    }

    double eyeX() const { return eyeX_; }
    double eyeY() const { return eyeY_; }
    double eyeZ() const { return eyeZ_; }

    // Where the ray through pixel position (px, py), from the bottom left, meets
    // the floor between the near and far planes, as grid.frag finds it.
    FloorPoint floorAt(double px, double py) const {
        const double nx = px / width_ * 2 - 1;
        const double ny = py / height_ * 2 - 1;
        const Vec4 nearH = Mul(inverseProjection_, {nx, ny, -1, 1});
        const Vec4 farH = Mul(inverseProjection_, {nx, ny, 1, 1});
        const Vec4 nearW = Mul(inverseView_, {nearH.x / nearH.w, nearH.y / nearH.w, nearH.z / nearH.w, 1});
        const Vec4 farW = Mul(inverseView_, {farH.x / farH.w, farH.y / farH.w, farH.z / farH.w, 1});
        const double rise = farW.y - nearW.y;
        FloorPoint out;
        if (std::fabs(rise) <= 1e-9) {
            return out;
        }
        const double t = -nearW.y / rise;
        out.hit = t > 0 && t <= 1;
        out.x = nearW.x + (farW.x - nearW.x) * t;
        out.z = nearW.z + (farW.z - nearW.z) * t;
        return out;
    }

    // A floor segment as band corners in normalized device coordinates, appended
    // to out as two triangles. Clipped to the near plane; nothing when wholly behind it.
    void band(double ax, double az, double bx, double bz, double halfWidth, std::vector<float>& out) const {
        Vec4 a = Mul(view_, {ax, 0, az, 1});
        Vec4 b = Mul(view_, {bx, 0, bz, 1});
        const double limit = -near_ * 1.001;
        if (a.z > limit && b.z > limit) {
            return;
        }
        auto clip = [&](Vec4& behind, const Vec4& front) {
            const double t = (limit - front.z) / (behind.z - front.z);
            behind = {front.x + (behind.x - front.x) * t, front.y + (behind.y - front.y) * t, limit, 1};
        };
        if (a.z > limit) {
            clip(a, b);
        } else if (b.z > limit) {
            clip(b, a);
        }
        auto screen = [&](const Vec4& point, double& sx, double& sy) {
            const Vec4 c = Mul(projection_, point);
            sx = (c.x / c.w * 0.5 + 0.5) * width_;
            sy = (c.y / c.w * 0.5 + 0.5) * height_;
        };
        double sax = 0;
        double say = 0;
        double sbx = 0;
        double sby = 0;
        screen(a, sax, say);
        screen(b, sbx, sby);
        double dx = sbx - sax;
        double dy = sby - say;
        const double length = std::hypot(dx, dy);
        if (length < 1e-6) {
            dx = 1;
            dy = 0;
        } else {
            dx /= length;
            dy /= length;
        }
        // Across the line, and past each end, by halfWidth pixels.
        const double nx = -dy * halfWidth;
        const double ny = dx * halfWidth;
        const double ex = dx * halfWidth;
        const double ey = dy * halfWidth;
        const std::array<double, 8> corners = {sax - ex + nx, say - ey + ny, sax - ex - nx, say - ey - ny,
                                               sbx + ex - nx, sby + ey - ny, sbx + ex + nx, sby + ey + ny};
        auto emit = [&](int corner) {
            out.push_back(static_cast<float>(corners[corner * 2] / width_ * 2 - 1));
            out.push_back(static_cast<float>(corners[corner * 2 + 1] / height_ * 2 - 1));
        };
        for (int corner : {0, 1, 2, 0, 2, 3}) {
            emit(corner);
        }
    }

private:
    engine_core::Matrix4 view_;
    engine_core::Matrix4 projection_;
    engine_core::Matrix4 inverseProjection_;
    engine_core::Matrix4 inverseView_;
    double width_;
    double height_;
    double eyeX_ = 0;
    double eyeY_ = 0;
    double eyeZ_ = 0;
    double near_ = 0.1;
};

}  // namespace

void build_grid_bands(const GridView& view, std::vector<float>& out) {
    out.clear();
    if (view.width <= 0 || view.height <= 0) {
        return;
    }
    const Camera camera(view);
    // grid.frag's fade: alpha falls off by smoothstep from a quarter of reach to
    // reach, along the floor from the camera. Past where it has fallen below
    // what can be seen even on an axis, nothing needs a band.
    const double reach = std::clamp(std::fabs(camera.eyeY()) * 60.0, 60.0, 600.0);
    double visibleReach = reach;
    for (double t = 0; t <= 1.0; t += 0.005) {
        const double smooth = t * t * (3 - 2 * t);
        if (1.0 - smooth < kVisibleAlpha) {
            visibleReach = reach * (0.25 + 0.75 * t);
            break;
        }
    }

    // How much floor a pixel covers, sampled over the pane: lod picks the rule
    // spacings, so this says how far out each spacing can show.
    std::array<bool, kMaxPower + 1> needed{};
    std::array<double, kMaxPower + 1> farthest{};
    bool anyFloor = false;
    for (int j = 0; j <= kSamplesY; ++j) {
        for (int i = 0; i <= kSamplesX; ++i) {
            const double px = static_cast<double>(i) / kSamplesX * view.width;
            const double py = static_cast<double>(j) / kSamplesY * view.height;
            const FloorPoint here = camera.floorAt(px, py);
            if (!here.hit) {
                continue;
            }
            const double distance = std::hypot(here.x - camera.eyeX(), here.z - camera.eyeZ());
            if (distance > visibleReach) {
                continue;
            }
            anyFloor = true;
            const FloorPoint right = camera.floorAt(px + 1, py);
            const FloorPoint up = camera.floorAt(px, py + 1);
            // A neighbor past the horizon: the footprint is as large as it gets.
            double footprint = 1e9;
            if (right.hit && up.hit) {
                const double perX = std::hypot(right.x - here.x, up.x - here.x);
                const double perZ = std::hypot(right.z - here.z, up.z - here.z);
                footprint = std::hypot(perX, perZ) * kMinCellPixels;
            }
            for (int power = 0; power <= kMaxPower; ++power) {
                const double spacing = std::pow(10.0, power);
                // Spacing 10^k is cell0, cell1, or cell2 where floor(lod) is k, k-1, or k-2.
                // As cell0 it fades out toward floor(lod) = k + 1, where the
                // footprint reaches the spacing: once its weight, 0.3 (1 - fade),
                // is below what can be seen, it shows no more.
                const double fadedFootprint = spacing * std::pow(10.0, -kVisibleAlpha / kMinorAlpha);
                const bool fineEnough = footprint < fadedFootprint * kNeedMargin;
                const bool coarseEnough = power <= 2 || footprint * kNeedMargin >= spacing / 1000.0;
                if (fineEnough && coarseEnough) {
                    needed[power] = true;
                    farthest[power] = std::max(farthest[power], distance);
                }
            }
        }
    }
    if (!anyFloor) {
        return;
    }
    // The coarse spacings: past the last sample that needs them, they still
    // show until the fade, so they reach as far as the grid does.
    std::array<double, kMaxPower + 1> radius{};
    for (int power = 0; power <= kMaxPower; ++power) {
        const double spacing = std::pow(10.0, power);
        radius[power] = std::min(visibleReach, farthest[power] * 1.1 + spacing * 2.0);
    }
    for (int power = 0; power <= kMaxPower; ++power) {
        if (!needed[power]) {
            continue;
        }
        const double spacing = std::pow(10.0, power);
        const double r = std::min(radius[power], spacing * kMaxLinesPerAxis * 0.5);
        // A line the next spacing also draws, as far out, is its band already.
        const bool coarserCovers = power < kMaxPower && needed[power + 1] && radius[power + 1] >= r;
        const long first = static_cast<long>(std::ceil((camera.eyeX() - r) / spacing));
        const long last = static_cast<long>(std::floor((camera.eyeX() + r) / spacing));
        for (long n = first; n <= last; ++n) {
            if (coarserCovers && n % 10 == 0) {
                continue;
            }
            const double x = n * spacing;
            const double half = std::sqrt(std::max(0.0, r * r - (x - camera.eyeX()) * (x - camera.eyeX())));
            camera.band(x, camera.eyeZ() - half, x, camera.eyeZ() + half, kRuleHalfWidth, out);
        }
        const long firstZ = static_cast<long>(std::ceil((camera.eyeZ() - r) / spacing));
        const long lastZ = static_cast<long>(std::floor((camera.eyeZ() + r) / spacing));
        for (long n = firstZ; n <= lastZ; ++n) {
            if (coarserCovers && n % 10 == 0) {
                continue;
            }
            const double z = n * spacing;
            const double half = std::sqrt(std::max(0.0, r * r - (z - camera.eyeZ()) * (z - camera.eyeZ())));
            camera.band(camera.eyeX() - half, z, camera.eyeX() + half, z, kRuleHalfWidth, out);
        }
    }
    // The axes, wider than a rule, out to the fade.
    if (std::fabs(camera.eyeZ()) < visibleReach) {
        const double half = std::sqrt(visibleReach * visibleReach - camera.eyeZ() * camera.eyeZ());
        camera.band(camera.eyeX() - half, 0, camera.eyeX() + half, 0, kAxisHalfWidth, out);
    }
    if (std::fabs(camera.eyeX()) < visibleReach) {
        const double half = std::sqrt(visibleReach * visibleReach - camera.eyeX() * camera.eyeX());
        camera.band(0, camera.eyeZ() - half, 0, camera.eyeZ() + half, kAxisHalfWidth, out);
    }
}

}  // namespace runner
