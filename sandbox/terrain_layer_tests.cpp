// LayerBuilder: packing one terrain Material's texture layer (two RGBA8
// arrays with full mip chains) from its resolved source file paths. Task 4
// of the terrain textures sub-project. A pure unit: no GL, no Instance
// state; Task 5's cache consumes it.

#include "terrain/LayerBuilder.hpp"
#include "texture/TextureBake.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using engine_core::terrain::build_layer;
using engine_core::terrain::layer_bytes;
using engine_core::terrain::LayerPixels;
using engine_core::terrain::LayerSources;

namespace {

// A scratch directory for this file's test images, wiped at the end.
struct ImageDir {
    std::filesystem::path path;
    ImageDir() {
        path = std::filesystem::temp_directory_path() / "anarchy-terrain-layer-test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ImageDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// Writes a binary PPM (P6): the simplest format stb_image can decode, so
// these tests need no PNG/BMP encoder of their own. `rgb` is w*h*3 bytes.
std::filesystem::path write_ppm(const std::filesystem::path& dir, const char* name, int w, int h,
                                 const std::vector<std::uint8_t>& rgb) {
    REQUIRE(rgb.size() == size_t(w) * size_t(h) * 3);
    const std::filesystem::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
    REQUIRE(bool(out));
    return path;
}

// Every pixel the same (r, g, b).
std::vector<std::uint8_t> flat_rgb(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::uint8_t> out(size_t(w) * size_t(h) * 3);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        out[i * 3 + 0] = r;
        out[i * 3 + 1] = g;
        out[i * 3 + 2] = b;
    }
    return out;
}

// A black/white checkerboard, one color per cell of a 1-pixel grid.
std::vector<std::uint8_t> checker_rgb(int w, int h) {
    std::vector<std::uint8_t> out(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t level = ((x + y) % 2 == 0) ? 0 : 255;
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            out[i * 3 + 0] = out[i * 3 + 1] = out[i * 3 + 2] = level;
        }
    }
    return out;
}

// A checkerboard of two caller-given colors, one per cell of a 1-pixel
// grid (LBR7: two opposed normal-map tilts, so every 2x2 mip box mixes
// diverging normals).
std::vector<std::uint8_t> checker2_rgb(int w, int h, std::uint8_t r0, std::uint8_t g0, std::uint8_t b0,
                                         std::uint8_t r1, std::uint8_t g1, std::uint8_t b1) {
    std::vector<std::uint8_t> out(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            if ((x + y) % 2 == 0) {
                out[i * 3 + 0] = r0;
                out[i * 3 + 1] = g0;
                out[i * 3 + 2] = b0;
            } else {
                out[i * 3 + 0] = r1;
                out[i * 3 + 1] = g1;
                out[i * 3 + 2] = b1;
            }
        }
    }
    return out;
}

// Levels a mip chain should have for a power-of-two size: size, size/2, ...
// down to and including 1 (so log2(size)+1 levels).
int expected_mip_count(int size) {
    int count = 1;
    while (size > 1) {
        size /= 2;
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("LBR1 A 300x200 diffuse resizes to size^2 with every mip present and the right sizes",
          "[terrain][textures]") {
    ImageDir dir;
    const std::filesystem::path diffuse = write_ppm(dir.path, "diffuse.ppm", 300, 200, checker_rgb(300, 200));

    for (const int size : {256, 512}) {
        LayerSources sources;
        sources.diffuse = diffuse;
        const LayerPixels layer = build_layer(sources, size);

        REQUIRE(layer.warning.empty());
        REQUIRE(layer.size == size);
        const int expected_levels = expected_mip_count(size);
        REQUIRE(int(layer.a_mips.size()) == expected_levels);
        REQUIRE(int(layer.b_mips.size()) == expected_levels);

        int level_size = size;
        for (int level = 0; level < expected_levels; ++level) {
            const size_t expected_bytes = size_t(level_size) * size_t(level_size) * 4;
            REQUIRE(layer.a_mips[size_t(level)].size() == expected_bytes);
            REQUIRE(layer.b_mips[size_t(level)].size() == expected_bytes);
            level_size = std::max(1, level_size / 2);
        }
    }
}

TEST_CASE("LBR2 Packing round-trips each channel within 1/255 at level 0", "[terrain][textures]") {
    ImageDir dir;
    const int size = 4;  // same size in and out: build_layer resizes nothing, so this is a direct round trip.

    LayerSources sources;
    sources.diffuse = write_ppm(dir.path, "diffuse.ppm", size, size, flat_rgb(size, size, 10, 20, 30));
    sources.normal = write_ppm(dir.path, "normal.ppm", size, size, flat_rgb(size, size, 200, 50, 255));
    sources.roughness = write_ppm(dir.path, "roughness.ppm", size, size, flat_rgb(size, size, 77, 77, 77));
    sources.metalness = write_ppm(dir.path, "metalness.ppm", size, size, flat_rgb(size, size, 190, 190, 190));
    sources.height = write_ppm(dir.path, "height.ppm", size, size, flat_rgb(size, size, 240, 240, 240));

    const LayerPixels layer = build_layer(sources, size);
    REQUIRE(layer.warning.empty());
    REQUIRE(layer.a_mips.front().size() == size_t(size) * size_t(size) * 4);

    const auto within = [](int a, int b) { return std::abs(a - b) <= 1; };
    for (size_t i = 0; i < size_t(size) * size_t(size); ++i) {
        const std::uint8_t* a = &layer.a_mips.front()[i * 4];
        REQUIRE(within(a[0], 10));
        REQUIRE(within(a[1], 20));
        REQUIRE(within(a[2], 30));
        REQUIRE(within(a[3], 240));  // height, from height.ppm's R channel

        const std::uint8_t* b = &layer.b_mips.front()[i * 4];
        REQUIRE(within(b[0], 200));  // normal X
        REQUIRE(within(b[1], 50));   // normal Y
        REQUIRE(within(b[2], 77));   // roughness
        REQUIRE(within(b[3], 190));  // metalness
    }
}

TEST_CASE("LBR3 A missing normal map falls back to flat + luminance height; a missing file warns and falls back",
          "[terrain][textures]") {
    ImageDir dir;
    const int size = 32;

    // No normal, no height: diffuse (non-flat, so its luminance gives a
    // non-trivial height) is the only present map. No path was given for
    // normal/height/roughness/metalness, so none of those is a read
    // failure -- no warning.
    {
        LayerSources sources;
        sources.diffuse = write_ppm(dir.path, "diffuse.ppm", size, size, checker_rgb(size, size));

        const LayerPixels layer = build_layer(sources, size);
        REQUIRE(layer.warning.empty());

        const std::vector<std::uint8_t>& b0 = layer.b_mips.front();
        for (size_t i = 0; i < size_t(size) * size_t(size); ++i) {
            REQUIRE(b0[i * 4 + 0] == 128);  // flat normal X
            REQUIRE(b0[i * 4 + 1] == 128);  // flat normal Y
            REQUIRE(b0[i * 4 + 2] == 255);  // roughness 1 (no roughness map)
            REQUIRE(b0[i * 4 + 3] == 255);  // metalness 1 (no metalness map)
        }

        // The checkerboard diffuse gives height_from_luminance something
        // to work with, so the derived height is not the flat-fallback
        // 0.5 (127 or 128) everywhere.
        const std::vector<std::uint8_t>& a0 = layer.a_mips.front();
        bool any_non_flat_height = false;
        for (size_t i = 0; i < size_t(size) * size_t(size); ++i) {
            const int h = a0[i * 4 + 3];
            if (h < 120 || h > 135) any_non_flat_height = true;
        }
        REQUIRE(any_non_flat_height);
    }

    // A roughness path given but the file does not exist: warning is set
    // and that map falls back to its default (255, i.e. roughness 1).
    {
        LayerSources sources;
        sources.diffuse = write_ppm(dir.path, "diffuse2.ppm", size, size, flat_rgb(size, size, 1, 2, 3));
        sources.roughness = dir.path / "does-not-exist.ppm";

        const LayerPixels layer = build_layer(sources, size);
        REQUIRE_FALSE(layer.warning.empty());
        REQUIRE(layer.warning.find("does-not-exist.ppm") != std::string::npos);

        const std::vector<std::uint8_t>& b0 = layer.b_mips.front();
        for (size_t i = 0; i < size_t(size) * size_t(size); ++i) REQUIRE(b0[i * 4 + 2] == 255);
    }
}

TEST_CASE("LBR4 Mip color averages in linear space: a black/white checker's 1x1 mip is ~186, not 128",
          "[terrain][textures]") {
    ImageDir dir;
    LayerSources sources;
    sources.diffuse = write_ppm(dir.path, "checker.ppm", 2, 2, checker_rgb(2, 2));

    const LayerPixels layer = build_layer(sources, 2);
    REQUIRE(layer.a_mips.size() == 2);  // 2x2, then 1x1
    const std::vector<std::uint8_t>& mip1 = layer.a_mips.back();
    REQUIRE(mip1.size() == 4);

    // Plain (gamma-space) averaging of 0 and 255 would give 128 here; the
    // brief requires decoding to linear (pow 2.2), averaging, and
    // re-encoding, which gives ~186 instead.
    for (int c = 0; c < 3; ++c) {
        INFO("channel " << c << " = " << int(mip1[size_t(c)]));
        REQUIRE(mip1[size_t(c)] >= 180);
        REQUIRE(mip1[size_t(c)] <= 192);
    }
}

TEST_CASE("LBR5 layer_bytes(1024) matches 1024^2 * 3 * 4/3 (BC3 + BC5 + BC5, a byte a texel each) within the "
          "small levels' block rounding",
          "[terrain][textures]") {
    engine_core::texture::set_s3tc_available(true);
    const std::size_t actual = layer_bytes(1024);
    const double ideal = 1024.0 * 1024.0 * 3.0 * 4.0 / 3.0;
    // Levels 2x2 and 1x1 each still take a whole 4x4 block in each plane.
    const double tolerance = 3.0 * 16.0 * 3.0;
    INFO("actual=" << actual << " ideal=" << ideal);
    REQUIRE(std::abs(double(actual) - ideal) <= tolerance);
}

TEST_CASE("LBR6 build_layer at 1024 stays fast enough to run off the render/simulation threads",
          "[terrain][textures]") {
    ImageDir dir;
    LayerSources sources;
    sources.diffuse = write_ppm(dir.path, "diffuse.ppm", 1024, 1024, checker_rgb(1024, 1024));

    const auto started = std::chrono::steady_clock::now();
    const LayerPixels layer = build_layer(sources, 1024);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    INFO("build_layer(1024) took " << ms << " ms");

    REQUIRE(layer.size == 1024);
    REQUIRE(layer.a_mips.front().size() == 1024u * 1024u * 4u);
}

TEST_CASE("LBR7 Toksvig: a mip box of diverging normals gets rougher; one of coherent normals does not",
          "[terrain][textures]") {
    ImageDir dir;
    const int size = 2;  // exactly one 2x2 box -> one 1x1 mip, so the fold is hand-checkable.

    // Two normals tilted +/-0.8 along X (encoded bytes 230/128 and 25/128:
    // X = round((nx*0.5+0.5)*255), Y flat at 128), alternating in a
    // checkerboard so the single 2x2 mip box averages opposed directions --
    // their X components cancel (vx ~ 0) while Z stays positive, so the
    // averaged vector's own length ns = len/count is well under 1: a
    // textbook case for Toksvig's variance-to-roughness fold.
    {
        LayerSources sources;
        sources.diffuse = write_ppm(dir.path, "diffuse_divergent.ppm", size, size, flat_rgb(size, size, 128, 128, 128));
        sources.normal = write_ppm(dir.path, "normal_divergent.ppm", size, size,
                                    checker2_rgb(size, size, 230, 128, 0, 25, 128, 0));
        sources.roughness = write_ppm(dir.path, "roughness_divergent.ppm", size, size, flat_rgb(size, size, 128, 128, 128));

        const LayerPixels layer = build_layer(sources, size);
        REQUIRE(layer.warning.empty());
        REQUIRE(layer.b_mips.size() == 2);  // 2x2, then 1x1
        const std::vector<std::uint8_t>& mip1 = layer.b_mips.back();
        REQUIRE(mip1.size() == 4);

        const int baseRoughByte = 128;
        const int toksvigRoughByte = mip1[2];
        INFO("base roughness byte = " << baseRoughByte << ", mip roughness byte = " << toksvigRoughByte);
        // Hand-computed: ns = 0.6, variance = 1 - ns^2 = 0.64, alpha =
        // (128/255)^2 + 0.64 ~= 0.892, roughness' = sqrt(alpha) ~= 0.944 ->
        // ~241/255. A plain average (the pre-fix behavior) would leave this
        // at 128, unchanged: assert it moved well past that.
        REQUIRE(toksvigRoughByte > 200);
    }

    // The same roughness and size, but every texel's normal points the
    // same way: ns = 1, variance = 0, so the fold leaves roughness
    // unchanged -- proving LBR7's first case is about variance, not merely
    // "roughness always goes up after a mip."
    {
        LayerSources sources;
        sources.diffuse = write_ppm(dir.path, "diffuse_coherent.ppm", size, size, flat_rgb(size, size, 128, 128, 128));
        sources.normal = write_ppm(dir.path, "normal_coherent.ppm", size, size, flat_rgb(size, size, 230, 128, 0));
        sources.roughness = write_ppm(dir.path, "roughness_coherent.ppm", size, size, flat_rgb(size, size, 128, 128, 128));

        const LayerPixels layer = build_layer(sources, size);
        REQUIRE(layer.warning.empty());
        const std::vector<std::uint8_t>& mip1 = layer.b_mips.back();
        REQUIRE(mip1.size() == 4);
        INFO("coherent mip roughness byte = " << int(mip1[2]));
        REQUIRE(std::abs(int(mip1[2]) - 128) <= 2);
    }
}
