// The floor grid's bands: thin triangles around the lines, which the grid
// shader runs on instead of the whole view. They must cover every pixel the
// shader visibly draws, which this checks against a copy of grid.frag's math.
// Visibly: alpha of 4/255 or more. Under that, a gray line over the scene
// changes it by less than 2/255, which no one can see.

#include "runner/GridBands.hpp"
#include "runner/RenderMath.hpp"

#include "Matrix4.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

constexpr double kVisibleAlpha = 4.0 / 255.0;

void Expect(bool condition, const std::string& label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label.c_str());
        ++gFailures;
    }
}

struct V4 {
    double x, y, z, w;
};

V4 Mul(const engine_core::Matrix4& m, V4 v) {
    const float* a = m.m;
    return {a[0] * v.x + a[4] * v.y + a[8] * v.z + a[12] * v.w, a[1] * v.x + a[5] * v.y + a[9] * v.z + a[13] * v.w,
            a[2] * v.x + a[6] * v.y + a[10] * v.z + a[14] * v.w, a[3] * v.x + a[7] * v.y + a[11] * v.z + a[15] * v.w};
}

struct Floor {
    bool hit = false;
    double x = 0;
    double z = 0;
    double dirY = 0;
};

// grid.frag's ray from the near plane to the far, at a point in normalized device coordinates.
Floor RayAt(const engine_core::Matrix4& inverseProjection, const engine_core::Matrix4& inverseView, double nx,
            double ny) {
    const V4 nearH = Mul(inverseProjection, {nx, ny, -1, 1});
    const V4 farH = Mul(inverseProjection, {nx, ny, 1, 1});
    const V4 nearW = Mul(inverseView, {nearH.x / nearH.w, nearH.y / nearH.w, nearH.z / nearH.w, 1});
    const V4 farW = Mul(inverseView, {farH.x / farH.w, farH.y / farH.w, farH.z / farH.w, 1});
    const double rise = farW.y - nearW.y;
    const double t = std::fabs(rise) > 1e-6 ? -nearW.y / rise : -1.0;
    Floor out;
    out.hit = t > 0 && t <= 1;
    const double c = std::clamp(t, 0.0, 1.0);
    out.x = nearW.x + (farW.x - nearW.x) * c;
    out.z = nearW.z + (farW.z - nearW.z) * c;
    const double dx = farW.x - nearW.x;
    const double dy = farW.y - nearW.y;
    const double dz = farW.z - nearW.z;
    out.dirY = dy / std::sqrt(dx * dx + dy * dy + dz * dz);
    return out;
}

double Rule(double cx, double cz, double px, double pz, double cell) {
    auto gap = [&](double coord, double pixel) {
        const double f = coord / cell + 0.5;
        return std::fabs(f - std::floor(f) - 0.5) * cell / std::max(pixel, 1e-6);
    };
    return 1.0 - std::clamp(std::min(gap(cx, px), gap(cz, pz)), 0.0, 1.0);
}

double Smoothstep(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// grid.frag's alpha at the pixel centered at (x + 0.5, y + 0.5), counted from
// the bottom left, with no surface in front of the floor. Derivatives are taken
// toward the next pixel, as within a 2x2 quad.
double GridAlpha(const engine_core::Matrix4& inverseProjection, const engine_core::Matrix4& inverseView,
                 engine_core::Vec3 eye, int width, int height, int x, int y) {
    auto ndc = [&](double px, double py) {
        return std::pair<double, double>{(px + 0.5) / width * 2 - 1, (py + 0.5) / height * 2 - 1};
    };
    const auto [nx, ny] = ndc(x, y);
    const Floor here = RayAt(inverseProjection, inverseView, nx, ny);
    if (!here.hit) {
        return 0;
    }
    const auto [rx, ry] = ndc(x + 1, y);
    const auto [ux, uy] = ndc(x, y + 1);
    const Floor right = RayAt(inverseProjection, inverseView, rx, ry);
    const Floor up = RayAt(inverseProjection, inverseView, ux, uy);
    const double px = std::hypot(right.x - here.x, up.x - here.x);
    const double pz = std::hypot(right.z - here.z, up.z - here.z);
    const double lod = std::max(0.0, std::log(std::hypot(px, pz) * 5.0) / std::log(10.0) + 1.0);
    const double fade = lod - std::floor(lod);
    const double cell0 = std::pow(10.0, std::floor(lod));
    double alpha = std::max(Rule(here.x, here.z, px, pz, cell0) * 0.3 * (1 - fade),
                            std::max(Rule(here.x, here.z, px, pz, cell0 * 10) * (0.55 + (0.3 - 0.55) * fade),
                                     Rule(here.x, here.z, px, pz, cell0 * 100) * 0.55));
    const double onX = 1 - std::clamp(std::fabs(here.z) / std::max(pz, 1e-6) - 0.75 + 0.5, 0.0, 1.0);
    const double onZ = 1 - std::clamp(std::fabs(here.x) / std::max(px, 1e-6) - 0.75 + 0.5, 0.0, 1.0);
    alpha = std::max(alpha, std::max(onX, onZ));
    const double reach = std::clamp(std::fabs(eye.y) * 60.0, 60.0, 600.0);
    alpha *= 1 - Smoothstep(0.25 * reach, reach, std::hypot(here.x - eye.x, here.z - eye.z));
    alpha *= Smoothstep(0.0, 0.08, std::fabs(here.dirY));
    return alpha;
}

// Which pixel centers the triangles cover, as a rasterizer would.
std::vector<bool> Coverage(const std::vector<float>& triangles, int width, int height) {
    std::vector<bool> covered(static_cast<std::size_t>(width) * height, false);
    for (std::size_t t = 0; t + 6 <= triangles.size(); t += 6) {
        double xs[3];
        double ys[3];
        for (int v = 0; v < 3; ++v) {
            xs[v] = (triangles[t + v * 2] * 0.5 + 0.5) * width;
            ys[v] = (triangles[t + v * 2 + 1] * 0.5 + 0.5) * height;
        }
        const double area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0]);
        if (std::fabs(area) < 1e-12) {
            continue;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({xs[0], xs[1], xs[2]}))));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({xs[0], xs[1], xs[2]}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ys[0], ys[1], ys[2]}))));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({ys[0], ys[1], ys[2]}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const double cx = x + 0.5;
                const double cy = y + 0.5;
                double w[3];
                for (int e = 0; e < 3; ++e) {
                    const int a = (e + 1) % 3;
                    const int b = (e + 2) % 3;
                    w[e] = ((xs[b] - xs[a]) * (cy - ys[a]) - (cx - xs[a]) * (ys[b] - ys[a])) / area;
                }
                if (w[0] >= 0 && w[1] >= 0 && w[2] >= 0) {
                    covered[static_cast<std::size_t>(y) * width + x] = true;
                }
            }
        }
    }
    return covered;
}

struct Pose {
    const char* name;
    engine_core::Vec3 eye;
    engine_core::Vec3 target;
};

void TestCoversEveryDrawnPixel() {
    constexpr int kWidth = 480;
    constexpr int kHeight = 270;
    const Pose poses[] = {
        {"high", {6.f, 8.f, 12.f}, {0.f, 0.f, 0.f}},     {"low", {0.f, 1.5f, 9.f}, {0.f, 1.f, 0.f}},
        {"grazing", {2.f, 0.3f, 20.f}, {0.f, 0.3f, 0.f}}, {"down", {0.5f, 25.f, 0.5f}, {0.f, 0.f, 0.f}},
        {"axes", {3.f, 2.f, 3.f}, {0.f, 0.f, 0.f}},      {"far", {40.f, 60.f, 90.f}, {0.f, 0.f, 0.f}},
        {"below", {4.f, -3.f, 8.f}, {0.f, 0.f, 0.f}},
    };
    for (const Pose& pose : poses) {
        runner::GridView view;
        view.view = runner::LookAtView(pose.eye, pose.target, {0.f, 1.f, 0.f});
        view.projection = runner::Perspective(60.f, static_cast<float>(kWidth) / kHeight, 0.1f, 1000.f);
        view.width = kWidth;
        view.height = kHeight;
        std::vector<float> triangles;
        runner::build_grid_bands(view, triangles);
        Expect(!triangles.empty() && triangles.size() % 6 == 0, std::string(pose.name) + ": bands are triangles");
        const std::vector<bool> covered = Coverage(triangles, kWidth, kHeight);
        const engine_core::Matrix4 inverseProjection = engine_core::matrix4_inverse(view.projection);
        const engine_core::Matrix4 inverseView = engine_core::matrix4_inverse(view.view);
        int drawn = 0;
        int missed = 0;
        int shaded = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const bool in = covered[static_cast<std::size_t>(y) * kWidth + x];
                shaded += in ? 1 : 0;
                if (GridAlpha(inverseProjection, inverseView, pose.eye, kWidth, kHeight, x, y) >= kVisibleAlpha) {
                    ++drawn;
                    missed += in ? 0 : 1;
                }
            }
        }
        Expect(drawn > 0, std::string(pose.name) + ": the grid draws something");
        Expect(missed == 0, std::string(pose.name) + ": every visibly drawn pixel is in a band (" + std::to_string(missed) +
                                " of " + std::to_string(drawn) + " missed)");
        std::printf("grid bands %-8s shade %5.1f%% of the view for %5.1f%% drawn\n", pose.name,
                    100.0 * shaded / (kWidth * kHeight), 100.0 * drawn / (kWidth * kHeight));
    }
}

void TestNoFloorNoBands() {
    // Looking straight up from above the floor: none of it is in view.
    runner::GridView view;
    view.view = runner::LookAtView({0.f, 5.f, 0.f}, {0.f, 10.f, 0.1f}, {0.f, 1.f, 0.f});
    view.projection = runner::Perspective(60.f, 16.f / 9.f, 0.1f, 1000.f);
    view.width = 160;
    view.height = 90;
    std::vector<float> triangles;
    runner::build_grid_bands(view, triangles);
    const std::vector<bool> covered = Coverage(triangles, 160, 90);
    Expect(std::count(covered.begin(), covered.end(), true) == 0, "with no floor in view, nothing is shaded");
}

}  // namespace

int RunGridBandsTests() {
    gFailures = 0;
    TestCoversEveryDrawnPixel();
    TestNoFloorNoBands();
    return gFailures;
}
