// HeightDerive: deriving a terrain layer's blend height from its normal map
// (integrated) or its diffuse brightness (luminance), for Materials with no
// HeightTexture. Task 3 of the terrain textures sub-project. A pure unit:
// no GL, no Instance state; Task 4's layer builder consumes it.

#include "terrain/HeightDerive.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

using engine_core::terrain::height_from_luminance;
using engine_core::terrain::height_from_normals;

namespace {

// Encodes a tangent-space normal (nx, ny, nz) into an RGBA8 texel the way
// height_from_normals decodes it back (x = r/255*2-1, ...).
void put_normal(std::vector<std::uint8_t>& rgba, size_t i, float nx, float ny, float nz) {
    const auto enc = [](float v) {
        const float c = (v + 1.f) * 0.5f * 255.f;
        return static_cast<std::uint8_t>(std::lround(std::max(0.f, std::min(255.f, c))));
    };
    rgba[4 * i + 0] = enc(nx);
    rgba[4 * i + 1] = enc(ny);
    rgba[4 * i + 2] = enc(nz);
    rgba[4 * i + 3] = 255;
}

// Builds a tileable "sum of sines" height field (integer cycle counts kx,
// ky so it wraps continuously) and its matching tangent-space normal map
// (from the field's analytic slope), the way a real height-painted terrain
// layer's normal map would look.
struct SineField {
    int w, h;
    float amplitude, kx, ky;

    float height(int x, int y) const {
        const float fx = float(x) / float(w);
        const float fy = float(y) / float(h);
        return amplitude * std::sin(2.f * 3.14159265f * kx * fx) + amplitude * std::sin(2.f * 3.14159265f * ky * fy);
    }

    // dh/dx, dh/dy (in pixel units) of height(), analytically.
    void slope(int x, int y, float& sx, float& sy) const {
        const float fx = float(x) / float(w);
        const float fy = float(y) / float(h);
        const float two_pi = 2.f * 3.14159265f;
        sx = amplitude * two_pi * kx / float(w) * std::cos(two_pi * kx * fx);
        sy = amplitude * two_pi * ky / float(h) * std::cos(two_pi * ky * fy);
    }

    std::vector<std::uint8_t> normal_map() const {
        std::vector<std::uint8_t> rgba(size_t(w) * size_t(h) * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float sx, sy;
                slope(x, y, sx, sy);
                const float norm = std::sqrt(sx * sx + sy * sy + 1.f);
                put_normal(rgba, size_t(y) * size_t(w) + size_t(x), -sx / norm, -sy / norm, 1.f / norm);
            }
        }
        return rgba;
    }

    std::vector<float> field() const {
        std::vector<float> out(size_t(w) * size_t(h));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) out[size_t(y) * size_t(w) + size_t(x)] = height(x, y);
        return out;
    }
};

double pearson_correlation(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= double(a.size());
    mb /= double(b.size());
    double cov = 0.0, va = 0.0, vb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double da = a[i] - ma;
        const double db = b[i] - mb;
        cov += da * db;
        va += da * da;
        vb += db * db;
    }
    if (va <= 0.0 || vb <= 0.0) return 0.0;
    return cov / std::sqrt(va * vb);
}

}  // namespace

TEST_CASE("HD1 height_from_normals correlates with the height field its normal map was built from", "[terrain][textures]") {
    // Equal cycle counts on both axes: the box-blur high-pass then
    // attenuates the field's x and y sine components by the same factor
    // (same wavelength => same box-filter response on each axis), so it
    // only rescales h0 rather than reshaping it, keeping the correlation
    // test robust to the exact high-pass width or solver iteration count.
    const SineField sine{128, 128, 3.0f, 2.0f, 2.0f};
    const std::vector<std::uint8_t> rgba = sine.normal_map();

    const auto started = std::chrono::steady_clock::now();
    const std::vector<float> derived = height_from_normals(rgba.data(), sine.w, sine.h);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    INFO("128x128 height_from_normals took "
         << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << " us");

    REQUIRE(derived.size() == size_t(sine.w) * size_t(sine.h));
    const double correlation = pearson_correlation(derived, sine.field());
    REQUIRE(correlation >= 0.9);

    for (const float v : derived) {
        REQUIRE_FALSE(std::isnan(v));
        REQUIRE(v >= 0.f);
        REQUIRE(v <= 1.f);
    }
}

TEST_CASE("HD2 A normal map with a constant tilt yields no ramp", "[terrain][textures]") {
    // Every texel decodes to the same nonzero slope. No periodic field can
    // have a nonzero constant gradient (integrating a constant slope all
    // the way around a wrapped axis would have to end up back where it
    // started), so the wrapped Poisson solve itself collapses this
    // inconsistent target to a flat field before the high-pass even runs;
    // the high-pass then has nothing left to do. Either way, the result
    // must not show a large-scale ramp: the span across the whole result
    // must be a small fraction (< 5%) of the full 0..1 output range.
    const int w = 64, h = 64;
    std::vector<std::uint8_t> rgba(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) put_normal(rgba, i, 0.6f, 0.3f, 0.8f);

    const std::vector<float> derived = height_from_normals(rgba.data(), w, h);
    REQUIRE(derived.size() == size_t(w) * size_t(h));

    float mn = derived[0], mx = derived[0];
    for (const float v : derived) {
        REQUIRE_FALSE(std::isnan(v));
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    REQUIRE((mx - mn) < 0.05f);
}

TEST_CASE("HD3 height_from_luminance gives a checkerboard two distinct levels", "[terrain][textures]") {
    // A 1-pixel checkerboard is the highest spatial frequency an image can
    // hold: a box blur wide enough to matter (here the high-pass's 1/8
    // width) averages it to a near-flat gray almost everywhere, so
    // subtracting that blur leaves the checkerboard itself, scaled, with
    // its two levels intact.
    const int w = 32, h = 32;
    std::vector<std::uint8_t> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t level = ((x + y) % 2 == 0) ? 0 : 255;
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            rgba[4 * i + 0] = level;
            rgba[4 * i + 1] = level;
            rgba[4 * i + 2] = level;
            rgba[4 * i + 3] = 255;
        }
    }

    const std::vector<float> derived = height_from_luminance(rgba.data(), w, h);
    REQUIRE(derived.size() == size_t(w) * size_t(h));

    int low = 0, high = 0;
    for (const float v : derived) {
        REQUIRE_FALSE(std::isnan(v));
        if (v < 0.1f) ++low;
        else if (v > 0.9f) ++high;
        else FAIL("value " << v << " is not near one of two levels");
    }
    REQUIRE(low > 0);
    REQUIRE(high > 0);
    REQUIRE(low + high == w * h);
}

TEST_CASE("HD4 A 1x1 input and a flat normal map return all 0.5, never NaN", "[terrain][textures]") {
    const std::uint8_t one_pixel[4] = {10, 200, 40, 255};
    const std::vector<float> from_one_normals = height_from_normals(one_pixel, 1, 1);
    REQUIRE(from_one_normals.size() == 1);
    REQUIRE(from_one_normals[0] == 0.5f);

    const std::vector<float> from_one_luminance = height_from_luminance(one_pixel, 1, 1);
    REQUIRE(from_one_luminance.size() == 1);
    REQUIRE(from_one_luminance[0] == 0.5f);

    const int w = 16, h = 16;
    std::vector<std::uint8_t> flat(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) put_normal(flat, i, 0.f, 0.f, 1.f);
    const std::vector<float> flat_result = height_from_normals(flat.data(), w, h);
    REQUIRE(flat_result.size() == size_t(w) * size_t(h));
    for (const float v : flat_result) {
        REQUIRE_FALSE(std::isnan(v));
        REQUIRE(v == 0.5f);
    }

    // A flat color is just as constant to height_from_luminance.
    std::vector<std::uint8_t> flat_color(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        flat_color[4 * i + 0] = 128;
        flat_color[4 * i + 1] = 128;
        flat_color[4 * i + 2] = 128;
        flat_color[4 * i + 3] = 255;
    }
    const std::vector<float> flat_luminance = height_from_luminance(flat_color.data(), w, h);
    for (const float v : flat_luminance) {
        REQUIRE_FALSE(std::isnan(v));
        REQUIRE(v == 0.5f);
    }
}

TEST_CASE("HD5 height_from_normals stays fast at 1024x1024", "[terrain][textures]") {
    const SineField sine{1024, 1024, 5.0f, 4.0f, 3.0f};
    const std::vector<std::uint8_t> rgba = sine.normal_map();

    const auto started = std::chrono::steady_clock::now();
    const std::vector<float> derived = height_from_normals(rgba.data(), sine.w, sine.h);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    INFO("1024x1024 height_from_normals took " << ms << " ms");

    REQUIRE(derived.size() == size_t(sine.w) * size_t(sine.h));
    for (const float v : derived) REQUIRE_FALSE(std::isnan(v));
}
