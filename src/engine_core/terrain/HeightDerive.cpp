#include "terrain/HeightDerive.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine_core::terrain {

namespace {

// --- sRGB -> linear, for height_from_luminance -----------------------------

float srgb_to_linear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// --- 1D circular box blur, used separably for the 2D high-pass blur -------

// Averages in[0..n) (stride apart) over a wrapped window of 2*radius+1
// samples centered on each element, writing to out (same stride). radius is
// clamped so the window never exceeds n (so it never double-counts a
// sample unless the window legitimately covers the whole axis).
void box_blur_1d_wrap(const float* in, float* out, int n, int stride, int radius) {
    if (n <= 1) {
        if (n == 1) out[0] = in[0];
        return;
    }
    radius = std::max(0, std::min(radius, (n - 1) / 2));
    const int window = 2 * radius + 1;
    std::vector<float> ext(size_t(n) + 2 * size_t(radius));
    for (int i = 0; i < static_cast<int>(ext.size()); ++i) {
        const int pos = ((i - radius) % n + n) % n;
        ext[size_t(i)] = in[size_t(pos) * size_t(stride)];
    }
    std::vector<float> prefix(ext.size() + 1, 0.f);
    for (size_t i = 0; i < ext.size(); ++i) prefix[i + 1] = prefix[i] + ext[i];
    const float inv = 1.f / float(window);
    for (int x = 0; x < n; ++x) {
        const float sum = prefix[size_t(x) + size_t(window)] - prefix[size_t(x)];
        out[size_t(x) * size_t(stride)] = sum * inv;
    }
}

// Separable wrapped box blur: horizontal radius rx, vertical radius ry.
std::vector<float> box_blur_wrap(const std::vector<float>& field, int w, int h, int rx, int ry) {
    std::vector<float> tmp(field.size());
    for (int y = 0; y < h; ++y) box_blur_1d_wrap(&field[size_t(y) * size_t(w)], &tmp[size_t(y) * size_t(w)], w, 1, rx);
    std::vector<float> out(field.size());
    for (int x = 0; x < w; ++x) box_blur_1d_wrap(&tmp[size_t(x)], &out[size_t(x)], h, w, ry);
    return out;
}

// --- Normalize to 0..1, guarding NaN and constant fields -------------------

std::vector<float> normalize_0_1(std::vector<float> v) {
    bool has_nan = false;
    float mn = std::numeric_limits<float>::infinity();
    float mx = -std::numeric_limits<float>::infinity();
    for (float f : v) {
        if (std::isnan(f)) {
            has_nan = true;
            break;
        }
        mn = std::min(mn, f);
        mx = std::max(mx, f);
    }
    // A field that is mathematically constant can still end up with a tiny
    // nonzero span here: the box blur sums and subtracts large running
    // totals, and that cancellation leaves floating-point noise on the
    // order of a few ULPs even when every input sample was identical.
    // Treat anything within that noise floor as constant too, or it would
    // get stretched across the entire 0..1 range by the divide below.
    const float epsilon = 1e-5f * std::max(1.f, std::max(std::fabs(mn), std::fabs(mx)));
    if (has_nan || !(mx - mn > epsilon)) {
        std::fill(v.begin(), v.end(), 0.5f);
        return v;
    }
    const float inv_range = 1.f / (mx - mn);
    for (float& f : v) f = (f - mn) * inv_range;
    return v;
}

// Box-blur high-pass (subtract a wrapped box blur of 1/8 the image's width
// and height, i.e. radius = width/16 and height/16, each floored and
// clamped to at least 1 pixel) then normalize to 0..1. Shared by both
// derivations; this is the one place "high-pass" is defined, per the task
// brief's own wording ("1/8-width box blur").
std::vector<float> high_pass_normalize(const std::vector<float>& field, int w, int h) {
    const int rx = std::max(1, w / 16);
    const int ry = std::max(1, h / 16);
    const std::vector<float> blurred = box_blur_wrap(field, w, h, rx, ry);
    std::vector<float> hp(field.size());
    for (size_t i = 0; i < field.size(); ++i) hp[i] = field[i] - blurred[i];
    return normalize_0_1(std::move(hp));
}

// --- Gradient -> height: periodic Gauss-Seidel Poisson solve on a pyramid --

// div(x,y) = d(gx)/dx + d(gy)/dy via central differences, wrapped.
std::vector<float> divergence(const std::vector<float>& gx, const std::vector<float>& gy, int w, int h) {
    std::vector<float> div(gx.size());
    for (int y = 0; y < h; ++y) {
        const int ym = (y - 1 + h) % h;
        const int yp = (y + 1) % h;
        for (int x = 0; x < w; ++x) {
            const int xm = (x - 1 + w) % w;
            const int xp = (x + 1) % w;
            const float ddx = 0.5f * (gx[size_t(y) * size_t(w) + size_t(xp)] - gx[size_t(y) * size_t(w) + size_t(xm)]);
            const float ddy = 0.5f * (gy[size_t(yp) * size_t(w) + size_t(x)] - gy[size_t(ym) * size_t(w) + size_t(x)]);
            div[size_t(y) * size_t(w) + size_t(x)] = ddx + ddy;
        }
    }
    return div;
}

// In-place periodic 5-point Gauss-Seidel relaxation of the Poisson equation
// (laplacian height == div), wrapped at every edge (the inputs are
// tileable).
void gauss_seidel_relax(std::vector<float>& height, const std::vector<float>& div, int w, int h, int iterations) {
    if (w <= 1 && h <= 1) return;
    // Wrap indices depend only on w/h, not on the iteration or the pixel's
    // other coordinate, so compute each axis's neighbor table once instead
    // of taking two modulo divisions per pixel per sweep (the dominant
    // cost at 1024x1024 before this: tens of millions of divisions).
    // (Sizing via a named size_t variable, not `vector<int> v(size_t(w))`
    // directly: with a single parenthesized argument that is itself a
    // function-style cast to a real type name, that line parses as a
    // function declaration -- the "most vexing parse" -- not a vector.)
    const size_t w_count = size_t(w);
    const size_t h_count = size_t(h);
    std::vector<int> wrap_xm(w_count);
    std::vector<int> wrap_xp(w_count);
    for (int x = 0; x < w; ++x) {
        wrap_xm[size_t(x)] = (x - 1 + w) % w;
        wrap_xp[size_t(x)] = (x + 1) % w;
    }
    std::vector<int> wrap_ym(h_count);
    std::vector<int> wrap_yp(h_count);
    for (int y = 0; y < h; ++y) {
        wrap_ym[size_t(y)] = (y - 1 + h) % h;
        wrap_yp[size_t(y)] = (y + 1) % h;
    }
    for (int it = 0; it < iterations; ++it) {
        for (int y = 0; y < h; ++y) {
            const int row = y * w;
            const int row_up = wrap_ym[size_t(y)] * w;
            const int row_down = wrap_yp[size_t(y)] * w;
            for (int x = 0; x < w; ++x) {
                const float sum = height[size_t(row + wrap_xp[size_t(x)])] + height[size_t(row + wrap_xm[size_t(x)])] +
                                   height[size_t(row_down + x)] + height[size_t(row_up + x)];
                height[size_t(row + x)] = (sum - div[size_t(row + x)]) * 0.25f;
            }
        }
    }
}

// Box-averages a w x h field down to (roughly) half resolution, wrapped.
std::vector<float> downsample_half(const std::vector<float>& src, int w, int h, int dw, int dh) {
    std::vector<float> dst(size_t(dw) * size_t(dh));
    for (int dy = 0; dy < dh; ++dy) {
        for (int dx = 0; dx < dw; ++dx) {
            float sum = 0.f;
            for (int oy = 0; oy < 2; ++oy) {
                const int sy = (dy * 2 + oy) % h;
                for (int ox = 0; ox < 2; ++ox) {
                    const int sx = (dx * 2 + ox) % w;
                    sum += src[size_t(sy) * size_t(w) + size_t(sx)];
                }
            }
            dst[size_t(dy) * size_t(dw) + size_t(dx)] = sum * 0.25f;
        }
    }
    return dst;
}

// Bilinear upsample with wrapped neighbors, used to carry a coarse level's
// solved height up as the next finer level's initial guess.
std::vector<float> upsample_bilinear(const std::vector<float>& src, int sw, int sh, int dw, int dh) {
    std::vector<float> dst(size_t(dw) * size_t(dh));
    for (int y = 0; y < dh; ++y) {
        const float fy = (float(y) + 0.5f) * float(sh) / float(dh) - 0.5f;
        const int y0 = int(std::floor(fy));
        const float ty = fy - float(y0);
        const int y0w = ((y0 % sh) + sh) % sh;
        const int y1w = ((y0 + 1) % sh + sh) % sh;
        for (int x = 0; x < dw; ++x) {
            const float fx = (float(x) + 0.5f) * float(sw) / float(dw) - 0.5f;
            const int x0 = int(std::floor(fx));
            const float tx = fx - float(x0);
            const int x0w = ((x0 % sw) + sw) % sw;
            const int x1w = ((x0 + 1) % sw + sw) % sw;
            const float v00 = src[size_t(y0w) * size_t(sw) + size_t(x0w)];
            const float v10 = src[size_t(y0w) * size_t(sw) + size_t(x1w)];
            const float v01 = src[size_t(y1w) * size_t(sw) + size_t(x0w)];
            const float v11 = src[size_t(y1w) * size_t(sw) + size_t(x1w)];
            const float v0 = v00 * (1.f - tx) + v10 * tx;
            const float v1 = v01 * (1.f - tx) + v11 * tx;
            dst[size_t(y) * size_t(dw) + size_t(x)] = v0 * (1.f - ty) + v1 * ty;
        }
    }
    return dst;
}

struct GradientLevel {
    int w = 0;
    int h = 0;
    std::vector<float> gx;
    std::vector<float> gy;
};

// Builds a pyramid of gx/gy from full resolution (levels.front()) down to a
// small coarsest level (levels.back()), halving each axis (rounding up)
// every step.
std::vector<GradientLevel> build_pyramid(std::vector<float> gx, std::vector<float> gy, int w, int h) {
    std::vector<GradientLevel> levels;
    levels.push_back({w, h, std::move(gx), std::move(gy)});
    while ((levels.back().w > 4 || levels.back().h > 4) && levels.size() < 16) {
        const GradientLevel& prev = levels.back();
        const int nw = std::max(1, (prev.w + 1) / 2);
        const int nh = std::max(1, (prev.h + 1) / 2);
        if (nw == prev.w && nh == prev.h) break;
        GradientLevel next;
        next.w = nw;
        next.h = nh;
        next.gx = downsample_half(prev.gx, prev.w, prev.h, nw, nh);
        next.gy = downsample_half(prev.gy, prev.w, prev.h, nw, nh);
        levels.push_back(std::move(next));
    }
    return levels;
}

// Full-multigrid-style solve: start at the coarsest level (cheap, so it can
// afford many relaxation sweeps to fully settle), then use each solved
// level, upsampled, as the initial guess for the next finer one. Because
// the initial guess is already close, each finer level needs far fewer
// sweeps than solving it cold would.
std::vector<float> solve_height_from_gradient(std::vector<float> gx, std::vector<float> gy, int w, int h) {
    std::vector<GradientLevel> levels = build_pyramid(std::move(gx), std::move(gy), w, h);
    const GradientLevel& coarsest = levels.back();
    std::vector<float> height(size_t(coarsest.w) * size_t(coarsest.h), 0.f);
    for (int li = static_cast<int>(levels.size()) - 1; li >= 0; --li) {
        const GradientLevel& level = levels[size_t(li)];
        const std::vector<float> div = divergence(level.gx, level.gy, level.w, level.h);
        const int iterations = (li == static_cast<int>(levels.size()) - 1) ? 120 : 16;
        gauss_seidel_relax(height, div, level.w, level.h, iterations);
        if (li > 0) {
            const GradientLevel& finer = levels[size_t(li - 1)];
            height = upsample_bilinear(height, level.w, level.h, finer.w, finer.h);
        }
    }
    return height;
}

}  // namespace

std::vector<float> height_from_normals(const std::uint8_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return {};
    if (w == 1 && h == 1) return {0.5f};
    const size_t n = size_t(w) * size_t(h);
    std::vector<float> gx(n), gy(n);
    for (size_t i = 0; i < n; ++i) {
        const float x = float(rgba[4 * i + 0]) / 255.f * 2.f - 1.f;
        const float y = float(rgba[4 * i + 1]) / 255.f * 2.f - 1.f;
        float z = float(rgba[4 * i + 2]) / 255.f * 2.f - 1.f;
        if (z < 0.2f) z = 0.2f;
        gx[i] = -x / z;
        gy[i] = -y / z;
    }
    std::vector<float> height = solve_height_from_gradient(std::move(gx), std::move(gy), w, h);
    return high_pass_normalize(height, w, h);
}

std::vector<float> height_from_luminance(const std::uint8_t* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return {};
    if (w == 1 && h == 1) return {0.5f};
    const size_t n = size_t(w) * size_t(h);
    std::vector<float> luminance(n);
    for (size_t i = 0; i < n; ++i) {
        const float r = srgb_to_linear(float(rgba[4 * i + 0]) / 255.f);
        const float g = srgb_to_linear(float(rgba[4 * i + 1]) / 255.f);
        const float b = srgb_to_linear(float(rgba[4 * i + 2]) / 255.f);
        luminance[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }
    return high_pass_normalize(luminance, w, h);
}

}  // namespace engine_core::terrain
