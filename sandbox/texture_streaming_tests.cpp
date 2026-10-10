// Texture streaming: Texture.Streaming, block compression, the .atex baked
// file, baking into the project's texture cache, and the worker pool that
// runs it all.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "Enum.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine_core::Game;
using engine_core::Texture;
using engine_core::TextureStreaming;

TEST_CASE("TS1 Texture.Streaming defaults to Automatic and takes AlwaysLoaded", "[texture]") {
    SimRole role;
    Game game;
    Texture& texture = game.create<Texture>();
    CHECK(texture.streaming() == TextureStreaming::Automatic);
    REQUIRE_FALSE(texture.set_streaming(static_cast<int>(TextureStreaming::AlwaysLoaded)));
    CHECK(texture.streaming() == TextureStreaming::AlwaysLoaded);
    CHECK(texture.set_streaming(7));
    CHECK(std::string(engine_core::texture_streaming_enum().name) == "TextureStreaming");
}

// ---- Block compression ----

#include "texture/BlockCompress.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

using engine_core::texture::PixelFormat;

namespace {

std::vector<std::uint8_t> gradient(int w, int h) {
    std::vector<std::uint8_t> p(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* q = &p[(size_t(y) * size_t(w) + size_t(x)) * 4];
            // Color runs along one line in RGB (as one BC1 block can hold);
            // alpha, kept apart in BC3, runs the other way.
            const int t = (x + y) * 255 / std::max(1, w + h - 2);
            q[0] = std::uint8_t(t);
            q[1] = std::uint8_t(255 - t / 2);
            q[2] = 128;
            q[3] = std::uint8_t(255 - y * 255 / std::max(1, h - 1));
        }
    }
    return p;
}

double mean_abs_error(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int first, int count) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        for (int c = first; c < first + count; ++c) {
            sum += std::abs(int(a[i + c]) - int(b[i + c]));
            ++n;
        }
    }
    return sum / double(n);
}

}  // namespace

TEST_CASE("TS2 level_bytes rounds up to whole 4x4 blocks", "[texture]") {
    using engine_core::texture::level_bytes;
    CHECK(level_bytes(PixelFormat::BC1, 1, 1) == 8);
    CHECK(level_bytes(PixelFormat::BC3, 5, 4) == 32);
    CHECK(level_bytes(PixelFormat::BC4, 4, 4) == 8);
    CHECK(level_bytes(PixelFormat::BC5, 300, 17) == size_t(75) * 5 * 16);
    CHECK(level_bytes(PixelFormat::RGBA8, 3, 3) == 36);
}

TEST_CASE("TS3 each format round-trips a gradient closely, at odd and tiny sizes", "[texture]") {
    using namespace engine_core::texture;
    // {format, first channel checked, channels checked}
    const std::array<std::array<int, 3>, 5> cases = {{{int(PixelFormat::BC1), 0, 3},
                                                      {int(PixelFormat::BC3), 0, 4},
                                                      {int(PixelFormat::BC4), 0, 1},
                                                      {int(PixelFormat::BC5), 0, 2},
                                                      {int(PixelFormat::RGBA8), 0, 4}}};
    for (const auto& c : cases) {
        const PixelFormat format = PixelFormat(c[0]);
        for (const auto& size : std::array<std::pair<int, int>, 4>{{{64, 64}, {300, 17}, {1, 1}, {2, 2}}}) {
            const int w = size.first, h = size.second;
            const std::vector<std::uint8_t> src = gradient(w, h);
            const std::vector<std::uint8_t> enc = encode_level(format, src.data(), w, h);
            REQUIRE(enc.size() == level_bytes(format, w, h));
            const std::vector<std::uint8_t> dec = decode_level(format, enc.data(), w, h);
            REQUIRE(dec.size() == src.size());
            INFO("format " << c[0] << " size " << w << "x" << h);
            // BC3's color half has no 3-color mode: a 2x2 of 0, 127, 127, 255
            // is as near as 4 colors on one line get.
            const double limit = format == PixelFormat::BC3 && w == 2 ? 8.0 : 6.0;
            CHECK(mean_abs_error(src, dec, c[1], c[2]) < limit);
        }
    }
}

TEST_CASE("TS4 constant_level decodes to its color", "[texture]") {
    using namespace engine_core::texture;
    for (PixelFormat format : {PixelFormat::BC1, PixelFormat::BC3, PixelFormat::BC5, PixelFormat::RGBA8}) {
        const std::vector<std::uint8_t> enc = constant_level(format, {128, 128, 128, 255}, 8, 8);
        REQUIRE(enc.size() == level_bytes(format, 8, 8));
        const std::vector<std::uint8_t> dec = decode_level(format, enc.data(), 8, 8);
        for (size_t i = 0; i < dec.size(); i += 4) {
            CHECK(std::abs(int(dec[i]) - 128) <= 2);
            CHECK(std::abs(int(dec[i + 1]) - 128) <= 2);
        }
    }
}

// ---- The .atex file ----

#include "texture/Atex.hpp"

#include <filesystem>
#include <fstream>

namespace {

engine_core::texture::BakedTexture sample_baked() {
    using namespace engine_core::texture;
    BakedTexture baked;
    baked.width = 16;
    baked.height = 8;
    baked.formats = {PixelFormat::BC1, PixelFormat::BC5};
    baked.planes.resize(2);
    int w = 16, h = 8;
    for (int level = 0; level < 5; ++level) {
        for (std::size_t p = 0; p < 2; ++p) {
            baked.planes[p].push_back(
                std::vector<std::uint8_t>(level_bytes(baked.formats[p], w, h), std::uint8_t(level * 10 + int(p))));
        }
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    return baked;
}

}  // namespace

TEST_CASE("TS5 atex round-trips every level of every plane", "[texture]") {
    using namespace engine_core::texture;
    TempDir dir;
    const std::filesystem::path path = dir.path / "roundtrip.atex";
    const BakedTexture baked = sample_baked();
    std::string error;
    REQUIRE(write_atex(path, baked, error));
    const std::optional<AtexHeader> header = read_atex_header(path);
    REQUIRE(header);
    CHECK(header->width == 16);
    CHECK(header->height == 8);
    CHECK(header->levels == 5);
    REQUIRE(header->formats == baked.formats);
    for (int level = 0; level < 5; ++level) {
        const auto data = read_atex_level(path, *header, level);
        REQUIRE(data);
        CHECK((*data)[0] == baked.planes[0][std::size_t(level)]);
        CHECK((*data)[1] == baked.planes[1][std::size_t(level)]);
    }
}

TEST_CASE("TS6 atex stores the smallest levels first", "[texture]") {
    using namespace engine_core::texture;
    TempDir dir;
    const std::filesystem::path path = dir.path / "order.atex";
    std::string error;
    REQUIRE(write_atex(path, sample_baked(), error));
    const std::optional<AtexHeader> header = read_atex_header(path);
    REQUIRE(header);
    CHECK(header->ranges[4][0].first < header->ranges[0][0].first);
    CHECK(header->ranges[4][1].first < header->ranges[3][0].first);
}

TEST_CASE("TS7 truncated, garbage, and other-version atex files are rejected", "[texture]") {
    using namespace engine_core::texture;
    TempDir dir;
    const std::filesystem::path path = dir.path / "bad.atex";
    std::string error;
    REQUIRE(write_atex(path, sample_baked(), error));
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3);
    CHECK_FALSE(read_atex_header(path));

    { std::ofstream(path, std::ios::binary) << "not an atex file at all"; }
    CHECK_FALSE(read_atex_header(path));

    REQUIRE(write_atex(path, sample_baked(), error));
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(4);
        const std::uint32_t version = kAtexVersion + 1;
        file.write(reinterpret_cast<const char*>(&version), 4);
    }
    CHECK_FALSE(read_atex_header(path));
    CHECK_FALSE(read_atex_header(dir.path / "missing.atex"));
}

// ---- Baking into the project's texture cache ----

#include "texture/TextureBake.hpp"

#include <chrono>

namespace {

// A binary PPM (P6) of the RGB of rgba, which stb_image decodes.
std::filesystem::path write_rgb_ppm(const std::filesystem::path& path, const std::vector<std::uint8_t>& rgba, int w,
                                    int h) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    for (std::size_t i = 0; i < rgba.size(); i += 4) out.write(reinterpret_cast<const char*>(&rgba[i]), 3);
    REQUIRE(bool(out));
    return path;
}

}  // namespace

TEST_CASE("TS8 build_mips halves odd sizes down to 1x1", "[texture]") {
    using namespace engine_core::texture;
    const auto mips = build_mips(Usage::Color, gradient(300, 17), 300, 17);
    REQUIRE(mips.size() == 9);   // 300 150 75 37 18 9 4 2 1
    CHECK(mips[1].size() == std::size_t(150) * 8 * 4);
    CHECK(mips.back().size() == 4);
}

TEST_CASE("TS9 Normal mips stay unit length", "[texture]") {
    using namespace engine_core::texture;
    // Two normals tilted opposite ways average to one pointing straight out.
    std::vector<std::uint8_t> rgba(2 * 1 * 4);
    const std::uint8_t a[4] = {218, 128, 218, 255}, b[4] = {38, 128, 218, 255};
    std::memcpy(&rgba[0], a, 4);
    std::memcpy(&rgba[4], b, 4);
    const auto mips = build_mips(Usage::Normal, rgba, 2, 1);
    REQUIRE(mips.size() == 2);
    CHECK(std::abs(int(mips[1][0]) - 128) <= 2);
    CHECK(int(mips[1][2]) >= 250);
}

TEST_CASE("TS10 format_for picks by usage and alpha, RGBA8 without s3tc", "[texture]") {
    using namespace engine_core::texture;
    set_s3tc_available(true);
    CHECK(format_for(Usage::Color, false) == PixelFormat::BC1);
    CHECK(format_for(Usage::Color, true) == PixelFormat::BC3);
    CHECK(format_for(Usage::Normal, false) == PixelFormat::BC5);
    CHECK(format_for(Usage::Mask, false) == PixelFormat::BC4);
    set_s3tc_available(false);
    CHECK(format_for(Usage::Color, false) == PixelFormat::RGBA8);
    CHECK(format_for(Usage::Normal, false) == PixelFormat::RGBA8);
    set_s3tc_available(true);
}

TEST_CASE("TS11 cache_key changes with the file's time and with the settings", "[texture]") {
    using namespace engine_core::texture;
    TempDir dir;
    const std::filesystem::path file = write_rgb_ppm(dir.path / "key.ppm", gradient(4, 4), 4, 4);
    const std::string a = cache_key({file}, "color|flip0");
    CHECK(a == cache_key({file}, "color|flip0"));
    CHECK(a != cache_key({file}, "color|flip1"));
    CHECK(a != cache_key({file}, "normal|flip0"));
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(5));
    CHECK(a != cache_key({file}, "color|flip0"));
    CHECK(cache_path(dir.path / "resources", a) == dir.path / ".cache" / "textures" / (a + ".atex"));
}

TEST_CASE("TS12 bake_texture writes a readable atex in the usage's format", "[texture]") {
    using namespace engine_core::texture;
    set_s3tc_available(true);
    TempDir dir;
    const std::filesystem::path source = write_rgb_ppm(dir.path / "resources" / "grass.ppm", gradient(64, 32), 64, 32);
    const std::filesystem::path cache = cache_path(dir.path / "resources", "test");
    const BakeResult result = bake_texture(source, Usage::Color, false, cache);
    REQUIRE(result.baked);
    CHECK(result.warning.empty());
    CHECK(result.baked->level_count() == 7);
    const std::optional<AtexHeader> header = read_atex_header(cache);
    REQUIRE(header);
    CHECK(header->width == 64);
    CHECK(header->height == 32);
    CHECK(header->levels == 7);
    CHECK(header->formats == std::vector<PixelFormat>{PixelFormat::BC1});
}

TEST_CASE("TS13 bake_texture: FlipY puts the top row at v 0", "[texture]") {
    using namespace engine_core::texture;
    set_s3tc_available(false);   // RGBA8, so pixels compare exactly
    TempDir dir;
    std::vector<std::uint8_t> rgba(4 * 4 * 4, 0);
    for (int x = 0; x < 4; ++x) rgba[std::size_t(x) * 4] = 255;   // the file's top row is red
    for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    const std::filesystem::path source = write_rgb_ppm(dir.path / "r" / "top.ppm", rgba, 4, 4);
    const BakeResult upright = bake_texture(source, Usage::Color, false, dir.path / "a.atex");
    const BakeResult flipped = bake_texture(source, Usage::Color, true, dir.path / "b.atex");
    REQUIRE(upright.baked);
    REQUIRE(flipped.baked);
    // Bottom row first, as OpenGL takes it: upright has red last, flipped first.
    CHECK(upright.baked->planes[0][0][std::size_t(3 * 4 * 4)] == 255);
    CHECK(flipped.baked->planes[0][0][0] == 255);
    set_s3tc_available(true);
}

TEST_CASE("TS14 bake_texture reports a file it cannot decode", "[texture]") {
    using namespace engine_core::texture;
    TempDir dir;
    std::filesystem::create_directories(dir.path);
    const std::filesystem::path bad = dir.path / "broken.png";
    { std::ofstream(bad) << "not a png"; }
    const BakeResult result = bake_texture(bad, Usage::Color, false, dir.path / "broken.atex");
    CHECK_FALSE(result.baked);
    CHECK_FALSE(result.warning.empty());
    CHECK_FALSE(std::filesystem::exists(dir.path / "broken.atex"));
}

// ---- The texture worker pool ----

#include "texture/TexturePool.hpp"

#include <atomic>
#include <thread>
#include <future>
#include <mutex>

TEST_CASE("TS15 TexturePool runs the higher priority first, and FIFO within one", "[texture]") {
    using namespace engine_core::texture;
    TexturePool pool(1);
    std::mutex mutex;
    std::vector<int> order;
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();
    pool.submit(JobPriority::MeshLargeLevels, [opened] { opened.wait(); });   // holds the one thread
    pool.submit(JobPriority::MeshBake, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(3);
    });
    pool.submit(JobPriority::TerrainPreview, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(1);
    });
    pool.submit(JobPriority::TerrainPreview, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(2);
    });
    gate.set_value();
    pool.wait_idle();
    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("TS16 TexturePool tells on_submit each job's priority, and runs jobs on many threads", "[texture]") {
    using namespace engine_core::texture;
    TexturePool pool(4);
    std::vector<JobPriority> seen;
    pool.set_on_submit([&](JobPriority p) { seen.push_back(p); });
    std::atomic<int> ran{0};
    for (int i = 0; i < 20; ++i) pool.submit(JobPriority::TerrainBake, [&] { ++ran; });
    pool.wait_idle();
    CHECK(ran == 20);
    CHECK(seen.size() == 20);
    CHECK(pool.thread_count() == 4);
    CHECK(TexturePool(0).thread_count() >= 1);
}

// ---- Compressed terrain layers, previews, and placeholders ----

#include "terrain/LayerBuilder.hpp"

TEST_CASE("TS17 compress_layer keeps every level and splits B into normal (B) and roughness+metalness (C)",
          "[texture]") {
    using namespace engine_core::terrain;
    using namespace engine_core::texture;
    set_s3tc_available(true);
    TempDir dir;
    LayerSources sources;
    sources.diffuse = write_rgb_ppm(dir.path / "d.ppm", gradient(256, 256), 256, 256);
    const LayerPixels pixels = build_layer(sources, 256);
    const LayerBytes layer = compress_layer(pixels);
    CHECK(layer.size == 256);
    CHECK(layer.first_level == 0);
    for (const auto& plane : layer.planes) REQUIRE(plane.size() == 9);
    CHECK(layer.planes[0][0].size() == level_bytes(PixelFormat::BC3, 256, 256));
    CHECK(layer.planes[1][0].size() == level_bytes(PixelFormat::BC5, 256, 256));
    CHECK(layer.planes[2][0].size() == level_bytes(PixelFormat::BC5, 256, 256));
    // C's level 0 holds B's roughness (B) and metalness (A) as its R and G.
    const auto c = decode_level(PixelFormat::BC5, layer.planes[2][0].data(), 256, 256);
    CHECK(std::abs(int(c[0]) - int(pixels.b_mips[0][2])) <= 2);
    CHECK(std::abs(int(c[1]) - int(pixels.b_mips[0][3])) <= 2);
    std::size_t total = 0;
    for (const auto& plane : layer.planes) {
        for (const auto& level : plane) total += level.size();
    }
    CHECK(layer_bytes(256) == total);
}

TEST_CASE("TS18 preview_layer fills only the levels from 64 down, from the diffuse alone", "[texture]") {
    using namespace engine_core::terrain;
    TempDir dir;
    LayerSources sources;
    sources.diffuse = write_rgb_ppm(dir.path / "d.ppm", gradient(512, 512), 512, 512);
    sources.normal = dir.path / "never-read.ppm";   // a preview reads nothing but the diffuse
    const LayerBytes preview = preview_layer(sources, 1024);
    CHECK(preview.size == 1024);
    CHECK(preview.first_level == 4);   // 1024 -> 64 is four halvings
    CHECK(preview.warning.empty());
    for (const auto& plane : preview.planes) {
        REQUIRE(plane.size() == 11);   // 1024 .. 1
        for (int level = 0; level < 4; ++level) CHECK(plane[std::size_t(level)].empty());
        for (int level = 4; level < 11; ++level) CHECK_FALSE(plane[std::size_t(level)].empty());
    }
    // A size already 64 or smaller previews complete.
    CHECK(preview_layer(sources, 64).first_level == 0);
}

TEST_CASE("TS19 placeholder_layer holds only the levels from 64 down, mid grey, flat, and half height", "[texture]") {
    using namespace engine_core::terrain;
    using namespace engine_core::texture;
    set_s3tc_available(true);
    const LayerBytes layer = placeholder_layer(256);
    CHECK(layer.first_level == 2);   // 256 -> 64: a few KB to upload, at any size
    for (const auto& plane : layer.planes) {
        REQUIRE(plane.size() == 9);
        CHECK(plane[0].empty());
        CHECK(plane[1].empty());
    }
    const auto a = decode_level(PixelFormat::BC3, layer.planes[0][2].data(), 64, 64);
    CHECK(std::abs(int(a[0]) - 128) <= 2);
    CHECK(std::abs(int(a[3]) - 128) <= 2);
    const auto b = decode_level(PixelFormat::BC5, layer.planes[1][2].data(), 64, 64);
    CHECK(std::abs(int(b[0]) - 128) <= 2);
    CHECK(std::abs(int(b[1]) - 128) <= 2);
    CHECK(placeholder_layer(32).first_level == 0);
}

TEST_CASE("TS20 streaming_batches: every level up to 64 a side together, then each larger one alone", "[texture]") {
    using engine_core::texture::streaming_batches;
    const auto b = streaming_batches(1024, 512, 11);
    REQUIRE(b.size() == 5);
    CHECK(b[0] == std::vector<int>{10, 9, 8, 7, 6, 5, 4});   // 1x1 .. 64x32
    CHECK(b[1] == std::vector<int>{3});                      // 128x64
    CHECK(b[4] == std::vector<int>{0});
    const auto small = streaming_batches(32, 32, 6);
    REQUIRE(small.size() == 1);
    CHECK(small[0] == std::vector<int>{5, 4, 3, 2, 1, 0});
}

TEST_CASE("TS21 BC1 keeps a red and blue block exactly (colors whose axis is square to grey)", "[texture]") {
    using namespace engine_core::texture;
    std::vector<std::uint8_t> tile(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i) {
        tile[std::size_t(i) * 4 + (i < 8 ? 2 : 0)] = 255;   // bottom half blue, top half red
        tile[std::size_t(i) * 4 + 3] = 255;
    }
    for (PixelFormat format : {PixelFormat::BC1, PixelFormat::BC3}) {
        const auto dec = decode_level(format, encode_level(format, tile.data(), 4, 4).data(), 4, 4);
        CHECK(mean_abs_error(tile, dec, 0, 3) < 1.0);
    }
}

TEST_CASE("TS22 atex files written at once to one path never mix their bytes", "[texture]") {
    using namespace engine_core::texture;
    TempDir dir;
    const std::filesystem::path path = dir.path / "shared.atex";
    std::vector<std::thread> writers;
    for (int t = 0; t < 8; ++t) {
        writers.emplace_back([&path, t] {
            BakedTexture baked = sample_baked();
            for (auto& plane : baked.planes)
                for (auto& level : plane) std::fill(level.begin(), level.end(), std::uint8_t(t + 1));
            std::string error;
            for (int i = 0; i < 40; ++i) write_atex(path, baked, error);
        });
    }
    for (std::thread& writer : writers) writer.join();
    const std::optional<AtexHeader> header = read_atex_header(path);
    REQUIRE(header);
    std::uint8_t seen = 0;
    bool mixed = false;
    for (int level = 0; level < header->levels; ++level) {
        const auto data = read_atex_level(path, *header, level);
        REQUIRE(data);
        for (const auto& plane : *data)
            for (std::uint8_t b : plane) {
                if (seen == 0) seen = b;
                mixed = mixed || b != seen;
            }
    }
    CHECK_FALSE(mixed);
}

// ---- Loaded: whether an asset's file is showing (or can play) yet ----

#include "AssetLoads.hpp"

TEST_CASE("TS23 Texture and Mesh Loaded follow what the renderer reports for their Path", "[texture]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    engine_core::clear_asset_loads();
    Texture& texture = game.create<Texture>();
    REQUIRE_FALSE(texture.set_path("textures/a.png"));
    engine_core::Mesh& mesh = game.create<engine_core::Mesh>();
    REQUIRE_FALSE(mesh.set_path("meshes/a.amesh"));
    CHECK_FALSE(texture.loaded(dir.path));
    CHECK_FALSE(mesh.loaded(dir.path));
    engine_core::set_asset_loaded(engine_core::AssetKind::Texture, dir.path, "textures/a.png", true);
    CHECK(texture.loaded(dir.path));
    CHECK_FALSE(mesh.loaded(dir.path));   // kinds are apart
    engine_core::set_asset_loaded(engine_core::AssetKind::Mesh, dir.path, "meshes/a.amesh", true);
    CHECK(mesh.loaded(dir.path));
    engine_core::set_asset_loaded(engine_core::AssetKind::Texture, dir.path, "textures/a.png", false);
    CHECK_FALSE(texture.loaded(dir.path));
    CHECK_FALSE(texture.loaded(dir.path / "elsewhere"));   // another project's root is another file
}

TEST_CASE("TS24 Sound Loaded is whether its file decodes", "[texture]") {
    SimRole role;
    Game game;
    TempDir dir;
    game.set_resources_root(dir.path);
    engine_core::Sound& sound = game.create<engine_core::Sound>();
    REQUIRE_FALSE(sound.set_path("audio/beep.wav"));
    CHECK_FALSE(sound.loaded(dir.path));
    // A tenth of a second of 8 kHz mono 16-bit silence.
    std::filesystem::create_directories(dir.path / "audio");
    std::ofstream out(dir.path / "audio" / "beep.wav", std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t samples = 800;
    out.write("RIFF", 4); u32(36 + samples * 2); out.write("WAVEfmt ", 8); u32(16); u16(1); u16(1); u32(8000);
    u32(16000); u16(2); u16(16); out.write("data", 4); u32(samples * 2);
    for (std::uint32_t i = 0; i < samples; ++i) u16(0);
    out.close();
    CHECK(sound.loaded(dir.path));
}

TEST_CASE("TS25 mip_size floors like GL and reaches 1 for any base", "[texture]") {
    using engine_core::texture::mip_size;
    REQUIRE(mip_size(256, 0) == 256);
    REQUIRE(mip_size(256, 8) == 1);
    REQUIRE(mip_size(256, 9) == 1);
    // Odd sizes floor, as glTexImage*D expects: 96 -> 48, 24, 12, 6, 3, 1.
    REQUIRE(mip_size(96, 5) == 3);
    REQUIRE(mip_size(96, 6) == 1);
    REQUIRE(mip_size(1, 3) == 1);
}
