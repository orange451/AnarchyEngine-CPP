# Texture Streaming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Textures appear immediately at low resolution and sharpen to full; terrain never sits white while loading.

**Architecture:** CPU-side work (decode, mips, BC compression, `.atex` cache, worker pool) lives in a new `engine_core/texture/` module with no GL, used by both `TerrainTextures` (engine_core) and `TextureCache` (runner). The runner uploads finished levels on the GL thread under a per-frame byte budget and lowers `GL_TEXTURE_BASE_LEVEL` as finer levels land. Terrain gets priority at every stage and a cold-cache 64² preview per layer.

**Tech Stack:** C++17, OpenGL 3.3 core + `GL_EXT_texture_compression_s3tc`, stb_image, `rgbcx.h` (bc7enc project, MIT), Catch2.

**Spec:** `docs/superpowers/specs/2026-10-09-texture-streaming-design.md`

## Global Constraints

- OpenGL 3.3 core only. Compressed formats: BC1/BC3 via `GL_EXT_texture_compression_s3tc` (checked at startup), BC4/BC5 via core RGTC. No BC7, no GL 4.x calls.
- Without s3tc, every baked texture is RGBA8 (the pixel format is part of the cache key).
- `.atex` stores levels smallest first; any level is one seek + one read.
- Cache: `<project>/.cache/textures/<hash>.atex`, outside `resources/`, gitignored, copied by packaging.
- Worker pool: `max(1, hardware_concurrency - 2)` threads. Workers never touch GL.
- GL upload budget: ~16 MB per frame.
- Terrain jobs outrank mesh-texture jobs at every stage.
- Pending terrain layers draw mid grey (128,128,128) with flat normal; layer 0 stays white.
- Engine distances are "units", never "studs".
- Comments match surrounding style: plain prose explaining what and why.

## Review Focus

1. **Texture file edited while its bake is in flight** — the old texture keeps drawing, the stale bake result is discarded (generation check), the new one replaces it. Test in Task 7 and Task 9.
2. **Non-power-of-two and tiny sources (e.g. 300×17, 1×1)** — mip chain halves with `max(1, n/2)`, BC blocks pad partial 4×4 tiles, nothing crashes. Test in Tasks 2 and 4.
3. **Corrupt / truncated / older-version `.atex`** — detected, deleted, re-baked; never uploaded. Test in Task 3.
4. **Two Textures sharing a path with different FlipY or usage** — distinct cache keys and GL textures. Test in Task 4.
5. **Terrain layer count grows mid-stream (Material added)** — arrays reallocate once, already-uploaded layers are re-uploaded, no white flash (old arrays draw until new ones hold every displayed layer's current levels). Test in Task 8 (GL check).

---

## File Structure

| File | Responsibility |
|---|---|
| `third_party/rgbcx/rgbcx.h` | Vendored BC1–BC5 encoder/decoder (MIT). |
| `src/engine_core/texture/BlockCompress.{hpp,cpp}` | Pixel formats, block sizes, encode one RGBA8 level to BC1/3/4/5/RGBA8, constant-colour level. |
| `src/engine_core/texture/Atex.{hpp,cpp}` | `.atex` header, write file, read header, read one level. |
| `src/engine_core/texture/TextureBake.{hpp,cpp}` | Mip chains per usage, cache key, cache path, bake-or-load. |
| `src/engine_core/texture/TexturePool.{hpp,cpp}` | Priority worker pool shared by terrain and mesh textures. |
| `src/engine_core/terrain/LayerBuilder.{hpp,cpp}` | Keeps `build_layer` (RGBA8) and adds `compress_layer`, `preview_layer`, `placeholder_layer`. |
| `src/engine_core/TerrainTextures.{hpp,cpp}` | Per-layer publish, pool, preview first, `.atex` cache. |
| `src/runner/Renderer.cpp`, `resources/shaders/pipeline/terrain.frag` | Three compressed arrays, in-place layer upload, BASE_LEVEL. |
| `src/runner/TextureCache.{hpp,cpp}` | Usage argument, streaming uploads, placeholder, AlwaysLoaded. |
| `resources/shaders/pipeline/surface.glsl` | Normal map Z rebuilt from BC5 XY. |
| `src/engine_datatypes/Enum.{hpp,cpp}`, `src/engine_instances/AssetInstances.{hpp,cpp}`, `src/engine_core/SnapshotPump.{hpp,cpp}`, `src/engine_core/LuaApi.cpp` | `Enum.TextureStreaming`, `Texture.Streaming`, snapshot plumbing. |
| `src/engine_core/Project.cpp`, GamePack source | `.gitignore` entry, cache copied into packages. |
| `tests/TextureStreamingTest.cpp` | All new unit tests (added to `engine-tests`). |

Build/test commands (Git Bash, from repo root; see memory: `MSYS_NO_PATHCONV=1` for cmake):

```bash
cmake --build build --config Debug --target engine-tests && ./build/Debug/engine-tests "[texture]"
```

---

### Task 1: Enum.TextureStreaming and Texture.Streaming

**Files:**
- Modify: `src/engine_datatypes/Enum.hpp`, `src/engine_datatypes/Enum.cpp:237-245,328`
- Modify: `src/engine_instances/AssetInstances.hpp:58-77`, `src/engine_instances/AssetInstances.cpp:88-102,646-661,795`
- Modify: `src/engine_core/LuaApi.cpp` (property docs, next to `Terrain.TextureSize` at ~1108)
- Modify: `src/engine_core/SnapshotPump.hpp:96-125,202-207`, `src/engine_core/SnapshotPump.cpp:365-379,477-531`
- Test: `tests/TextureStreamingTest.cpp` (create; add to `engine-tests` in `CMakeLists.txt:833`)

**Interfaces:**
- Produces: `enum class TextureStreaming { Automatic = 0, AlwaysLoaded = 1 };`, `const EnumType& texture_streaming_enum();`, `Texture::streaming()`, `Texture::set_streaming(TextureStreaming)`, saved property `"Streaming"` default `Automatic`. Snapshot fields next to every `*_flip_y`: `bool diffuse_always_loaded` … `emissive_always_loaded`, `image_always_loaded`, `sun_always_loaded`, `moon_always_loaded` (true when the Texture's Streaming is AlwaysLoaded).

- [ ] **Step 1: Write the failing test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "engine_instances/AssetInstances.hpp"
#include "engine_datatypes/Enum.hpp"

TEST_CASE("Texture.Streaming defaults to Automatic and round-trips", "[texture]") {
    // Build a DataModel and Texture the way tests/PropertiesTest.cpp does.
    auto game = make_test_game();
    auto* texture = game->create<Texture>("Texture");
    CHECK(texture->streaming() == TextureStreaming::Automatic);
    texture->set_streaming(TextureStreaming::AlwaysLoaded);
    CHECK(texture->streaming() == TextureStreaming::AlwaysLoaded);
    CHECK(texture_streaming_enum().name == std::string("TextureStreaming"));
}
```

(Use whatever helper `PropertiesTest.cpp` uses to make a DataModel; copy it into this file if it is local to that test.)

- [ ] **Step 2: Run it, expect a compile failure** (`streaming` undeclared).

- [ ] **Step 3: Implement**, mirroring `Terrain.TextureSize` exactly:
  - `Enum.hpp`: comment + `enum class TextureStreaming { Automatic = 0, AlwaysLoaded = 1 };` + `const EnumType& texture_streaming_enum();`
  - `Enum.cpp`: `const EnumEntry kTextureStreamings[] = {{"Automatic", 0}, {"AlwaysLoaded", 1}};`, its `EnumType`, add to the all-types list, accessor.
  - `Texture`: member `TextureStreaming streaming_ = TextureStreaming::Automatic;`, reset in `on_reuse`, setter calls `note_property_change("Streaming", …)` with enum slots like `texture_size_slot`, read/write functions and `lua_saved_property("Streaming", "Enum.TextureStreaming", read_streaming, write_streaming, "Enum.TextureStreaming.Automatic")` next to FlipY.
  - Comment on `streaming()`: "Automatic shows the smallest mips first and sharpens to full resolution; AlwaysLoaded keeps the Texture's placeholder until every mip is uploaded."
  - `LuaApi.cpp`: property doc string in the same form as TextureSize.
  - `SnapshotPump`: extend each `texture_path` lambda to also write the `*_always_loaded` flag.

- [ ] **Step 4: Run** `engine-tests "[texture]"` → PASS; also run `properties-tests` → PASS.

- [ ] **Step 5: Commit** `git commit -m "Texture gets a Streaming property (Automatic, AlwaysLoaded)"`

---

### Task 2: Block compression

**Files:**
- Create: `third_party/rgbcx/rgbcx.h` (+ `LICENSE`), `src/engine_core/texture/BlockCompress.hpp`, `src/engine_core/texture/BlockCompress.cpp`
- Modify: `CMakeLists.txt` (add the .cpp to the engine_core source list near line 655; `third_party/rgbcx` as a SYSTEM include)
- Test: `tests/TextureStreamingTest.cpp`

Before vendoring, read the license header of `rgbcx.h` from https://github.com/richgel999/bc7enc_rdo and confirm it is MIT or public domain. If it is not, stop and ask the user.

**Interfaces:**
- Produces:

```cpp
namespace engine_core::texture {

enum class PixelFormat : std::uint8_t { RGBA8 = 0, BC1 = 1, BC3 = 2, BC4 = 3, BC5 = 4 };

// Bytes one level of width x height takes in format: whole 4x4 blocks for BC
// (8 bytes BC1/BC4, 16 bytes BC3/BC5), 4 a pixel for RGBA8.
std::size_t level_bytes(PixelFormat format, int width, int height);

// One level of RGBA8 pixels (bottom row first, as TexturePixels) in format.
// Partial 4x4 tiles at the edges repeat the last row/column. BC4 takes R,
// BC5 takes R and G. Any thread.
std::vector<std::uint8_t> encode_level(PixelFormat format, const std::uint8_t* rgba, int width, int height);

// The same, decoded back to RGBA8 (tests and the RGBA8 fallback path only).
std::vector<std::uint8_t> decode_level(PixelFormat format, const std::uint8_t* data, int width, int height);

// A level of width x height filled with one RGBA8 color, already encoded.
std::vector<std::uint8_t> constant_level(PixelFormat format, std::array<std::uint8_t, 4> rgba, int width, int height);

// rgbcx::init must run once before encode_level; this does it, thread-safely.
void ensure_encoder_ready();

}
```

- [ ] **Step 1: Write failing tests**

```cpp
#include "engine_core/texture/BlockCompress.hpp"
using namespace engine_core::texture;

static std::vector<std::uint8_t> gradient(int w, int h) {
    std::vector<std::uint8_t> p(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        auto* q = &p[(size_t(y) * w + x) * 4];
        q[0] = std::uint8_t(x * 255 / std::max(1, w - 1));
        q[1] = std::uint8_t(y * 255 / std::max(1, h - 1));
        q[2] = 128; q[3] = 255;
    }
    return p;
}

static double mean_abs_error(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int channels) {
    double sum = 0; size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) for (int c = 0; c < channels; ++c) { sum += std::abs(int(a[i + c]) - int(b[i + c])); ++n; }
    return sum / double(n);
}

TEST_CASE("level_bytes rounds up to whole blocks", "[texture]") {
    CHECK(level_bytes(PixelFormat::BC1, 1, 1) == 8);
    CHECK(level_bytes(PixelFormat::BC3, 5, 4) == 32);
    CHECK(level_bytes(PixelFormat::BC5, 300, 17) == size_t(75) * 5 * 16);
    CHECK(level_bytes(PixelFormat::RGBA8, 3, 3) == 36);
}

TEST_CASE("each format round-trips a gradient closely", "[texture]") {
    for (auto [format, channels] : {std::pair{PixelFormat::BC1, 3}, {PixelFormat::BC3, 4},
                                    {PixelFormat::BC4, 1}, {PixelFormat::BC5, 2}}) {
        for (auto [w, h] : {std::pair{64, 64}, {300, 17}, {1, 1}, {2, 2}}) {
            const auto src = gradient(w, h);
            const auto enc = encode_level(format, src.data(), w, h);
            REQUIRE(enc.size() == level_bytes(format, w, h));
            const auto dec = decode_level(format, enc.data(), w, h);
            CHECK(mean_abs_error(src, dec, channels) < 6.0);
        }
    }
}

TEST_CASE("constant_level decodes to its color", "[texture]") {
    const auto enc = constant_level(PixelFormat::BC3, {128, 128, 128, 255}, 8, 8);
    const auto dec = decode_level(PixelFormat::BC3, enc.data(), 8, 8);
    for (size_t i = 0; i < dec.size(); i += 4) { CHECK(std::abs(dec[i] - 128) <= 2); CHECK(dec[i + 3] == 255); }
}
```

- [ ] **Step 2: Run, expect compile failure.**

- [ ] **Step 3: Implement.** In `BlockCompress.cpp` define `RGBCX_IMPLEMENTATION` before including `rgbcx.h`. `ensure_encoder_ready` wraps `rgbcx::init()` in `std::call_once`. `encode_level` walks 4×4 tiles, gathers a 16-pixel RGBA block (clamping coordinates to the edge), and calls `rgbcx::encode_bc1(level, out, block, true, false)`, `encode_bc3`, `encode_bc4(out, block, 4)` (stride 4 reads R), `encode_bc5(out, block, 0, 1, 4)`. Use quality level 10 for BC1/BC3. RGBA8 is a copy. `decode_level` uses `rgbcx::unpack_bc1/3/4/5` and writes back only in-bounds pixels; BC4 decode fills G=B=0, A=255; BC5 fills B=0, A=255. `constant_level` encodes one block and repeats it.

- [ ] **Step 4: Run** → PASS.

- [ ] **Step 5: Commit** `git commit -m "BC1/BC3/BC4/BC5 level encoding for baked textures"`

---

### Task 3: The .atex file

**Files:**
- Create: `src/engine_core/texture/Atex.hpp`, `src/engine_core/texture/Atex.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/TextureStreamingTest.cpp`

**Interfaces:**
- Consumes: `PixelFormat`, `level_bytes` (Task 2).
- Produces:

```cpp
namespace engine_core::texture {

constexpr std::uint32_t kAtexVersion = 1;

// A baked texture: one or more planes (terrain layers use 3, A/B/C; other
// textures 1), each a full mip chain in its own format, level 0 the largest.
struct BakedTexture {
    int width = 0, height = 0;
    std::vector<PixelFormat> formats;                          // one per plane
    std::vector<std::vector<std::vector<std::uint8_t>>> planes; // [plane][level]
    int level_count() const;   // planes[0].size()
};

// Writes baked to path (via a temporary file renamed into place, so a crash
// never leaves a half file under the final name). Levels are stored smallest
// first, every plane's level L together. False, with error set, on failure.
bool write_atex(const std::filesystem::path& path, const BakedTexture& baked, std::string& error);

struct AtexHeader {
    int width = 0, height = 0, levels = 0;
    std::vector<PixelFormat> formats;
    // [level][plane] -> byte range in the file
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges;
};

// Reads and validates the header: magic "ATEX", kAtexVersion, sizes that
// match level_bytes, ranges inside the file. Nullopt for anything else.
std::optional<AtexHeader> read_atex_header(const std::filesystem::path& path);

// Reads level `level` of every plane. Nullopt if the file is short or changed.
std::optional<std::vector<std::vector<std::uint8_t>>> read_atex_level(const std::filesystem::path& path,
                                                                       const AtexHeader& header, int level);
}
```

- [ ] **Step 1: Write failing tests**

```cpp
#include "engine_core/texture/Atex.hpp"

static BakedTexture sample_baked() {
    BakedTexture b; b.width = 16; b.height = 8; b.formats = {PixelFormat::BC1, PixelFormat::BC5};
    b.planes.resize(2);
    int w = 16, h = 8;
    for (int level = 0; level < 5; ++level) {
        for (int p = 0; p < 2; ++p)
            b.planes[p].push_back(std::vector<std::uint8_t>(level_bytes(b.formats[p], w, h), std::uint8_t(level * 10 + p)));
        w = std::max(1, w / 2); h = std::max(1, h / 2);
    }
    return b;
}

static std::filesystem::path temp_file(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / "anarchy-atex-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

TEST_CASE("atex round-trips every level of every plane", "[texture]") {
    const auto path = temp_file("roundtrip.atex");
    const auto baked = sample_baked();
    std::string error;
    REQUIRE(write_atex(path, baked, error));
    const auto header = read_atex_header(path);
    REQUIRE(header);
    CHECK(header->levels == 5);
    for (int level = 0; level < 5; ++level) {
        const auto data = read_atex_level(path, *header, level);
        REQUIRE(data);
        CHECK((*data)[0] == baked.planes[0][level]);
        CHECK((*data)[1] == baked.planes[1][level]);
    }
}

TEST_CASE("atex stores smallest levels first", "[texture]") {
    const auto path = temp_file("order.atex");
    std::string error;
    REQUIRE(write_atex(path, sample_baked(), error));
    const auto header = read_atex_header(path);
    REQUIRE(header);
    CHECK(header->ranges[4][0].first < header->ranges[0][0].first);
}

TEST_CASE("truncated, garbage, and old-version atex are rejected", "[texture]") {
    const auto path = temp_file("bad.atex");
    std::string error;
    REQUIRE(write_atex(path, sample_baked(), error));
    std::filesystem::resize_file(path, std::filesystem::file_size(path) / 2);
    CHECK_FALSE(read_atex_header(path));
    { std::ofstream(path, std::ios::binary) << "not an atex file at all"; }
    CHECK_FALSE(read_atex_header(path));
    REQUIRE(write_atex(path, sample_baked(), error));
    { std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out); f.seekp(4); std::uint32_t v = 0; f.write(reinterpret_cast<char*>(&v), 4); }
    CHECK_FALSE(read_atex_header(path));
}
```

- [ ] **Step 2: Run, expect compile failure.**

- [ ] **Step 3: Implement.** Layout, little-endian: `"ATEX"`, `u32 version`, `u32 width`, `u32 height`, `u32 levels`, `u32 planeCount`, `u8 formats[planeCount]` padded to 4, then `levels × planeCount` entries of `u64 offset, u64 length` indexed `[level][plane]`, then data written for level `levels-1` down to `0`. Validate `length == level_bytes(format, max(1,w>>L), max(1,h>>L))` and `offset+length <= file_size`. Open with `std::ifstream(path, binary)` (path overload, not narrow string; see the comment at `LayerBuilder.cpp:34`).

- [ ] **Step 4: Run** → PASS.

- [ ] **Step 5: Commit** `git commit -m "The .atex baked texture file"`

---

### Task 4: Baking and the cache

**Files:**
- Create: `src/engine_core/texture/TextureBake.hpp`, `src/engine_core/texture/TextureBake.cpp`
- Modify: `CMakeLists.txt` (this file decodes with stb_image; follow the pattern at `CMakeLists.txt:688-709` for which TU owns `STB_IMAGE_IMPLEMENTATION` — reuse the decode in `LayerBuilder.cpp` or `runner::DecodeTexture` rather than a third copy; if engine_core cannot see runner, move `DecodeTexture` into `engine_core/texture/` and have runner include it from there)
- Test: `tests/TextureStreamingTest.cpp`

**Interfaces:**
- Consumes: Tasks 2–3.
- Produces:

```cpp
namespace engine_core::texture {

enum class Usage : std::uint8_t { Color = 0, Normal = 1, Mask = 2 };

// Set once at startup by the runner (false when s3tc is missing). Any thread.
void set_s3tc_available(bool available);
bool s3tc_available();

// Color: BC1 if every alpha is 255, else BC3. Normal: BC5. Mask: BC4.
// RGBA8 for all when !s3tc_available() (BC4/BC5 are core, but one fallback
// keeps one code path; revisit only if needed).
PixelFormat format_for(Usage usage, bool has_alpha);

// Full mip chain, level 0 = rgba (width x height), each next level a 2x2
// box average, max(1, n/2). Normal usage renormalizes XY per texel.
std::vector<std::vector<std::uint8_t>> build_mips(Usage usage, std::vector<std::uint8_t> rgba, int width, int height);

// The cache key: hex of a 64-bit FNV-1a over kAtexVersion, every source
// path (generic_u8string), its last_write_time count, and `settings` (usage,
// flip, size, format, layer-packing version — whatever changes the bytes).
std::string cache_key(const std::vector<std::filesystem::path>& sources, const std::string& settings);

// <project>/.cache/textures/<key>.atex, where project is resources_root's parent.
std::filesystem::path cache_path(const std::filesystem::path& resources_root, const std::string& key);

struct BakeResult {
    std::optional<BakedTexture> baked;   // nullopt when the source failed to decode
    std::string warning;                 // decode or cache-write failure, else empty
};

// Decodes source, flips if asked, builds mips, encodes every level, writes
// the .atex at cache (warning on write failure, result still returned).
BakeResult bake_texture(const std::filesystem::path& source, Usage usage, bool flip_y,
                        const std::filesystem::path& cache);
}
```

- [ ] **Step 1: Write failing tests**

```cpp
#include "engine_core/texture/TextureBake.hpp"

TEST_CASE("build_mips halves non-power-of-two sizes down to 1x1", "[texture]") {
    const auto mips = build_mips(Usage::Color, gradient(300, 17), 300, 17);
    REQUIRE(mips.size() == 9);              // 300,150,75,37,18,9,4,2,1
    CHECK(mips.back().size() == 4);
}

TEST_CASE("format_for picks by usage and alpha", "[texture]") {
    set_s3tc_available(true);
    CHECK(format_for(Usage::Color, false) == PixelFormat::BC1);
    CHECK(format_for(Usage::Color, true) == PixelFormat::BC3);
    CHECK(format_for(Usage::Normal, false) == PixelFormat::BC5);
    CHECK(format_for(Usage::Mask, false) == PixelFormat::BC4);
    set_s3tc_available(false);
    CHECK(format_for(Usage::Color, false) == PixelFormat::RGBA8);
    set_s3tc_available(true);
}

TEST_CASE("cache_key changes with file time and settings", "[texture]") {
    const auto file = temp_file("key-source.png");
    { std::ofstream(file) << "x"; }
    const auto a = cache_key({file}, "color|flip0");
    CHECK(a == cache_key({file}, "color|flip0"));
    CHECK(a != cache_key({file}, "color|flip1"));   // Review Focus 4
    CHECK(a != cache_key({file}, "normal|flip0"));
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(5));
    CHECK(a != cache_key({file}, "color|flip0"));
}

TEST_CASE("bake_texture writes a readable atex", "[texture]") {
    // Write a 64x64 PNG with stbi_write_png (already vendored with stb) into a temp resources folder.
    const auto root = temp_file("bake-project") / "resources";
    std::filesystem::create_directories(root);
    const auto png = root / "grass.png";
    write_test_png(png, gradient(64, 64), 64, 64);
    const auto cache = cache_path(root, "test");
    const auto result = bake_texture(png, Usage::Color, false, cache);
    REQUIRE(result.baked);
    CHECK(result.warning.empty());
    const auto header = read_atex_header(cache);
    REQUIRE(header);
    CHECK(header->levels == 7);
    CHECK(header->formats[0] == PixelFormat::BC1);
}

TEST_CASE("bake_texture reports an undecodable source", "[texture]") {
    const auto bad = temp_file("broken.png");
    { std::ofstream(bad) << "not a png"; }
    const auto result = bake_texture(bad, Usage::Color, false, temp_file("broken.atex"));
    CHECK_FALSE(result.baked);
    CHECK_FALSE(result.warning.empty());
}
```

(`write_test_png`: a small helper in the test file around `stbi_write_png`; if stb_image_write is not in the repo, write a 24-bit BMP by hand instead, which stb_image decodes.)

- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement** per the interface comments. `cache_path` creates nothing; `bake_texture` does `create_directories(cache.parent_path())`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `git commit -m "Texture baking into the project's .cache"`

---

### Task 5: TexturePool

**Files:**
- Create: `src/engine_core/texture/TexturePool.hpp`, `src/engine_core/texture/TexturePool.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/TextureStreamingTest.cpp`

**Interfaces:**
- Produces:

```cpp
namespace engine_core::texture {

// Lower runs first. Terrain stages all come before mesh stages.
enum class JobPriority : int {
    TerrainPreview = 0, TerrainSmallLevels = 1, MeshSmallLevels = 2,
    TerrainBake = 3, TerrainLargeLevels = 4, MeshBake = 5, MeshLargeLevels = 6,
};

// A process-wide pool of max(1, cores - 2) threads running jobs by priority,
// FIFO within a priority. Jobs must not touch GL. Destruction finishes the
// running jobs and drops queued ones.
class TexturePool {
public:
    explicit TexturePool(int threads = 0);   // 0: max(1, hardware_concurrency - 2)
    ~TexturePool();
    void submit(JobPriority priority, std::function<void()> job);
    void wait_idle();                         // tests
    static TexturePool& shared();             // the process's pool
};
}
```

- [ ] **Step 1: Write failing tests**

```cpp
#include "engine_core/texture/TexturePool.hpp"

TEST_CASE("TexturePool runs higher priority first, FIFO within one", "[texture]") {
    TexturePool pool(1);
    std::mutex m; std::vector<int> order;
    std::promise<void> gate; auto opened = gate.get_future().share();
    pool.submit(JobPriority::MeshLargeLevels, [opened] { opened.wait(); });   // holds the one thread
    pool.submit(JobPriority::MeshBake, [&] { std::lock_guard l(m); order.push_back(3); });
    pool.submit(JobPriority::TerrainPreview, [&] { std::lock_guard l(m); order.push_back(1); });
    pool.submit(JobPriority::TerrainPreview, [&] { std::lock_guard l(m); order.push_back(2); });
    gate.set_value();
    pool.wait_idle();
    CHECK(order == std::vector<int>{1, 2, 3});
}
```

- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement** with a `std::priority_queue` of `{priority, sequence, job}`, one mutex, one condition variable, a running counter for `wait_idle`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `git commit -m "A shared priority worker pool for texture work"`

---

### Task 6: Compressed terrain layers, previews, placeholders

**Files:**
- Modify: `src/engine_core/terrain/LayerBuilder.hpp:30-43`, `src/engine_core/terrain/LayerBuilder.cpp`
- Test: `tests/TextureStreamingTest.cpp`

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces (replaces `LayerBytes`; `build_layer` keeps returning RGBA8 under a new name):

```cpp
namespace engine_core::terrain {

// build_layer's existing output, unchanged: RGBA8 A (color + height) and B
// (normal XY, roughness, metalness) mip chains.
struct LayerPixels { int size = 0; std::vector<std::vector<std::uint8_t>> a_mips, b_mips; std::string warning; };
LayerPixels build_layer(const LayerSources& sources, int size);

// A layer as the renderer uploads it: three planes (A: BC3 color+height,
// B: BC5 normal XY, C: BC5 roughness+metalness; RGBA8 each without s3tc),
// indexed by level of the Terrain's full chain at `size`. Levels below
// first_level are absent (empty vectors) — a preview or a partly read cache
// file. first_level 0 is complete.
struct LayerBytes {
    int size = 0;
    int first_level = 0;
    std::array<std::vector<std::vector<std::uint8_t>>, 3> planes;
    std::string warning;
};

// Splits B into B (R,G) and C (B,A) and encodes each level.
LayerBytes compress_layer(const LayerPixels& pixels);
// Diffuse only, built at 64 (or size if smaller) and placed at the levels a
// size-sized chain has from 64 down: first_level = log2(size / 64).
LayerBytes preview_layer(const LayerSources& sources, int size);
// Every level mid grey (128,128,128), height 0.5, flat normal, roughness 1,
// metalness 0 — a layer with nothing to show yet.
LayerBytes placeholder_layer(int size);
// Bytes of a complete layer at size (all three planes, all levels).
std::size_t layer_bytes(int size);
}
```

- [ ] **Step 1: Write failing tests**

```cpp
#include "engine_core/terrain/LayerBuilder.hpp"
using namespace engine_core::terrain;

TEST_CASE("preview_layer fills only the levels from 64 down", "[texture]") {
    LayerSources sources; sources.diffuse = write_test_png_file("preview-diffuse.png", gradient(512, 512), 512, 512);
    const auto preview = preview_layer(sources, 1024);
    CHECK(preview.first_level == 4);                 // 1024 -> 64 is 4 halvings
    for (int level = 0; level < 4; ++level) CHECK(preview.planes[0][level].empty());
    CHECK_FALSE(preview.planes[0][4].empty());
    CHECK(preview.planes[0].size() == 11);           // 1024..1
}

TEST_CASE("placeholder_layer is complete and grey", "[texture]") {
    const auto layer = placeholder_layer(256);
    CHECK(layer.first_level == 0);
    const auto dec = decode_level(engine_core::texture::format_for(engine_core::texture::Usage::Color, true),
                                  layer.planes[0][0].data(), 256, 256);
    CHECK(std::abs(dec[0] - 128) <= 2);
}

TEST_CASE("compress_layer keeps level count and splits B into B and C", "[texture]") {
    LayerSources sources; sources.diffuse = write_test_png_file("cl-diffuse.png", gradient(256, 256), 256, 256);
    const auto layer = compress_layer(build_layer(sources, 256));
    CHECK(layer.planes[0].size() == 9);
    CHECK(layer.planes[1].size() == 9);
    CHECK(layer.planes[2].size() == 9);
}
```

- [ ] **Step 2: Run, expect failures.**
- [ ] **Step 3: Implement.** Rename `LayerBytes` users to `LayerPixels` where they need RGBA8. A plane A level uses `PixelFormat::BC3` (or RGBA8), B and C use `BC5` (or RGBA8). `preview_layer`: `LayerSources only_diffuse; only_diffuse.diffuse = sources.diffuse;` then `compress_layer(build_layer(only_diffuse, std::min(64, size)))` and shift its levels into a vector sized for `size`'s chain. `placeholder_layer` uses `constant_level` per level. `layer_bytes` sums `level_bytes` over the three planes.
- [ ] **Step 4: Run** `engine-tests "[texture]"` and the existing terrain tests (`terrain-editor-tests`) → PASS.
- [ ] **Step 5: Commit** `git commit -m "Terrain layers are compressed, with grey placeholders and 64-pixel previews"`

---

### Task 7: TerrainTextures — per-layer publish, previews first, cache

**Files:**
- Modify: `src/engine_core/TerrainTextures.hpp`, `src/engine_core/TerrainTextures.cpp`
- Test: `tests/TextureStreamingTest.cpp` (and update existing TerrainTextures tests that assumed all-at-once publish)

**Behavior (replaces `worker_loop` and the single `worker_`):**
1. When `update()` queues a layer that needs building, it immediately publishes a set where that layer's slot is `placeholder_layer(size)` (unless the slot already holds bytes at this size, which keep drawing). New `TerrainTextureSet` gains `std::vector<std::uint64_t> layer_revisions` (one per layer, bumped whenever that layer's pointer changes) so the renderer re-uploads only changed layers.
2. Per layer, at generation `g`:
   - If `cache_path(root, key)` exists and its header validates: submit `TerrainSmallLevels` reading levels ≤64², then one `TerrainLargeLevels` job per larger level, smallest first. Each finished read publishes the layer with a lower `first_level`.
   - Else: submit `TerrainPreview` (`preview_layer`), then `TerrainBake` (`compress_layer(build_layer(...))` → `write_atex` → publish complete layer).
   - Every job carries `(terrain, layer index, generation)`; results whose generation is no longer current are dropped (Review Focus 1).
3. Results land in `results_` under `mutex_`; `update()` applies them and publishes one new set per Terrain per tick if anything changed.
4. The in-memory `cache_` of shared layers stays, keyed as today plus the cache key.
5. `memory_bytes` uses the new `layer_bytes`.

The cache key's `settings` string: `"terrain|v1|size=<size>|s3tc=<0|1>"` with the five source paths as `sources`.

- [ ] **Step 1: Write failing tests**

```cpp
#include "engine_core/TerrainTextures.hpp"

TEST_CASE("a terrain layer publishes before the other layers finish", "[texture]") {
    // Two Materials on one Terrain, sources 1024x1024 PNGs; use the scene
    // setup from the existing TerrainTextures tests.
    auto fixture = make_terrain_fixture({"a.png", "b.png"}, Enum::TextureSize::Large);
    fixture.textures.update(fixture.game);
    auto set = fixture.textures.published(fixture.terrain);
    REQUIRE(set);                              // placeholders publish at once
    CHECK(set->layers.size() == 3);
    fixture.pump_until([&] {                   // update() each tick until...
        auto s = fixture.textures.published(fixture.terrain);
        return s && s->layers[1]->first_level < 10 && s->layers[2]->first_level < 10;
    });
    // ...both have at least a preview, and at that moment no bake has
    // finished yet is not asserted (timing); what is: previews exist.
    auto s = fixture.textures.published(fixture.terrain);
    CHECK(s->layers[1]->first_level <= 4);
}

TEST_CASE("cold cache: every layer previews before any full bake starts", "[texture]") {
    // Use a TexturePool with 1 thread injected into TerrainTextures (add a
    // constructor taking TexturePool&) and record job priorities in order.
    auto fixture = make_terrain_fixture({"a.png", "b.png", "c.png"}, Enum::TextureSize::Medium, /*pool threads*/ 1);
    fixture.textures.update(fixture.game);
    fixture.textures.wait_idle();
    const auto& kinds = fixture.recorded_priorities();
    const auto first_bake = std::find(kinds.begin(), kinds.end(), JobPriority::TerrainBake);
    CHECK(std::count(kinds.begin(), first_bake, JobPriority::TerrainPreview) == 3);
}

TEST_CASE("warm cache reads levels, no bake", "[texture]") {
    auto fixture = make_terrain_fixture({"a.png"}, Enum::TextureSize::Medium, 1);
    fixture.textures.update(fixture.game); fixture.textures.wait_idle(); fixture.textures.update(fixture.game);
    auto second = make_terrain_fixture_same_project(fixture);  // new TerrainTextures, same files and cache
    second.textures.update(second.game); second.textures.wait_idle();
    const auto& kinds = second.recorded_priorities();
    CHECK(std::count(kinds.begin(), kinds.end(), JobPriority::TerrainBake) == 0);
    CHECK(std::count(kinds.begin(), kinds.end(), JobPriority::TerrainSmallLevels) == 1);
}

TEST_CASE("a source edited mid-bake drops the stale result", "[texture]") {
    auto fixture = make_terrain_fixture({"a.png"}, Enum::TextureSize::Medium, 1);
    fixture.textures.update(fixture.game);
    fixture.touch_source("a.png", gradient_red(512, 512));   // new content + newer mtime
    fixture.advance_clock_past_stamp_throttle();
    fixture.textures.update(fixture.game);
    fixture.textures.wait_idle(); fixture.textures.update(fixture.game);
    CHECK(fixture.published_layer_is_red(1));
}
```

`make_terrain_fixture` and helpers: build them in the test file from whatever the current TerrainTextures tests use (find them with `grep -rn "TerrainTextures" tests`). Recording priorities: add an optional `std::function<void(JobPriority)> on_submit` to TexturePool for tests.

- [ ] **Step 2: Run, expect failures.**
- [ ] **Step 3: Implement** per Behavior above. Keep `wait_idle()` meaning "pool idle and no results pending application".
- [ ] **Step 4: Run** `engine-tests "[texture]"` and all terrain test targets → PASS.
- [ ] **Step 5: Commit** `git commit -m "Terrain layers publish one at a time, preview first, from the texture cache"`

---

### Task 8: Renderer — three compressed arrays, in-place uploads, BASE_LEVEL

**Files:**
- Modify: `src/runner/Renderer.cpp:1511-1700`, `src/runner/Renderer.hpp:93-101`, `src/runner/gl.hpp`, `src/runner/gl.cpp` (load `glCompressedTexImage3D`, `glCompressedTexSubImage3D`, `glCompressedTexImage2D`, `glCompressedTexSubImage2D`; add the BC enum constants), `src/runner/TerrainDraws.cpp`, `resources/shaders/pipeline/terrain.frag`
- Test: extend `tests/SceneRenderCheck.cpp` (GL check target `scene-render-check`)

**GL constants** (put in `gl.hpp` with the other `RT_GL_*`):

```cpp
constexpr GLenum RT_GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1;  // BC1
constexpr GLenum RT_GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3;  // BC3
constexpr GLenum RT_GL_COMPRESSED_RED_RGTC1 = 0x8DBB;           // BC4
constexpr GLenum RT_GL_COMPRESSED_RG_RGTC2 = 0x8DBD;            // BC5
constexpr GLenum RT_GL_TEXTURE_BASE_LEVEL = 0x813C;
constexpr GLenum RT_GL_TEXTURE_MAX_LEVEL = 0x813D;
```

Use DXT1 **RGBA** (0x83F1) so `rgbcx` 3-color blocks decode consistently. At context creation, check the extension list (`glGetStringi(GL_EXTENSIONS, i)`) for `GL_EXT_texture_compression_s3tc` and call `engine_core::texture::set_s3tc_available`.

**Behavior:**
- `MeshDraw` gets `terrainSurfaceC`.
- `TerrainArrayEntry` holds `surface[3]`, `size`, `layerCount`, per-layer `uploadedRevision` and `uploadedFirstLevel`, plus a pending queue of `(layer, level)` uploads.
- Arrays are created with `glCompressedTexImage3D(..., nullptr)` for every level (or `glTexImage3D` RGBA8 without s3tc) and `GL_TEXTURE_MAX_LEVEL = levels-1`.
- When a set arrives with the same size and layer count: for each layer whose revision changed, queue its present levels (smallest first). Upload from the queue under the shared ~16 MB/frame budget with `glCompressedTexSubImage3D`.
- When size or layer count changes: create new arrays, queue every layer, keep drawing the old arrays until every displayed layer has been uploaded at its current `first_level`, then swap (Review Focus 5).
- After uploads, `GL_TEXTURE_BASE_LEVEL` for all three arrays = max over layers of `uploadedFirstLevel` (the finest level every layer has).
- `terrain.frag`: add `uniform sampler2DArray uSurfaceC;`. Where it reads B's `.ba` for roughness/metalness, read C's `.rg` instead; B now supplies only `.rg` (normal XY). A supplies color in `.rgb` and height in `.a` as before. Bind C on the next free unit in `TerrainDraws.cpp`/`Renderer.cpp` wherever A and B are bound.

- [ ] **Step 1: Add the GL check** to `SceneRenderCheck.cpp`: a scene with a 2-material Terrain on a cold cache; render frames until the first frame where `terrainLayerCount > 0`; assert the center pixel is not (255,255,255) and is within 30 of the diffuse's average color within 1 s of wall time. Then add a third Material and assert no frame between adding it and its upload shows a white terrain pixel at the sampled points.
- [ ] **Step 2: Run** `scene-render-check` → FAIL (white).
- [ ] **Step 3: Implement** per Behavior.
- [ ] **Step 4: Run** `scene-render-check` → PASS; `studio-tests` → PASS (except the known flakes in memory).
- [ ] **Step 5: Commit** `git commit -m "Terrain draws from three compressed arrays that sharpen as levels arrive"`

---

### Task 9: TextureCache streams mesh textures

**Files:**
- Modify: `src/runner/TextureCache.hpp`, `src/runner/TextureCache.cpp`, `src/runner/GameView.cpp:418-422,539-543,606-607`, `src/ide/MaterialBall.cpp:83-87`, `resources/shaders/pipeline/surface.glsl:63`
- Test: `tests/TextureStreamingTest.cpp` (pure logic), `scene-render-check` (GL)

**Interfaces:**
- Consumes: Tasks 2–5, Task 8's GL loaders and constants.
- Produces:

```cpp
// The GL texture for path, or a 1x1 placeholder (white for Color and Mask,
// flat (128,128,255) for Normal) until its first levels are uploaded; 0 only
// when path is empty. alwaysLoaded keeps the placeholder until every level is
// uploaded.
unsigned get(const std::string& path, engine_core::texture::Usage usage, bool flipY = false, bool alwaysLoaded = false);
// Uploads finished levels within budgetBytes and lowers BASE_LEVEL. Once a
// frame on the GL thread, before drawing (GameView calls it).
void pump(std::size_t budgetBytes = 16u << 20);
```

`getEnvironment` (Skybox, RGBA16F) is unchanged.

**Behavior:**
- Entry key: path + usage + flipY. First `get`: compute the cache key (`settings = "mesh|v1|usage=<n>|flip=<0|1>|s3tc=<0|1>"`); if the `.atex` validates, submit `MeshSmallLevels` (levels ≤64²) then `MeshLargeLevels` per larger level; else submit `MeshBake` and afterwards stream the result's levels from memory, small first. Results go to a locked queue; `pump` uploads them.
- The real GL texture is allocated with all levels when the header (or bake) is known; `get` returns it once its first upload landed (Automatic) or once level 0 landed (AlwaysLoaded); before that, the placeholder.
- Hot reload: the existing once-a-second stamp check; a changed stamp starts a new generation; the old texture keeps being returned until the new one is visible, then is deleted. Stale-generation results are dropped (Review Focus 1).
- Undecodable source: today's `report` message once per file version; placeholder stays.
- Callers: diffuse, emissive, sun, moon → `Usage::Color`; normal → `Usage::Normal`; roughness, metalness → `Usage::Mask`. Pass the snapshot's `*_always_loaded`.
- `surface.glsl:63`: `vec2 xy = texture(uNormalMap, uv).rg * 2.0 - 1.0; vec3 map = vec3(xy, sqrt(max(0.0, 1.0 - dot(xy, xy))));`. The flat placeholder (128,128,255) still decodes to (0,0,1).
- Remove `glGenerateMipmap` from this path.

- [ ] **Step 1: Write a failing logic test** for the level order helper, which you extract as a pure function:

```cpp
// runner/TextureCache.hpp (or a small header beside it)
// Levels to read, in order: every level whose larger side is <= 64 in one
// batch first, then each larger level alone, smallest first.
std::vector<std::vector<int>> streaming_batches(int width, int height, int levels);

TEST_CASE("streaming_batches: small levels together, then one level at a time", "[texture]") {
    const auto b = runner::streaming_batches(1024, 512, 11);
    REQUIRE(b.size() == 5);
    CHECK(b[0] == std::vector<int>{10, 9, 8, 7, 6, 5, 4});   // 1x1 .. 64x32
    CHECK(b[1] == std::vector<int>{3});                     // 128x64
    CHECK(b[4] == std::vector<int>{0});
}
```

- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** the helper, then the Behavior.
- [ ] **Step 4: Add a GL check** to `scene-render-check`: a Part with a 2048² diffuse, cold cache. Assert that `get()` returns the placeholder on frame 1, a texture with `BASE_LEVEL > 0` within 0.5 s, and `BASE_LEVEL == 0` within 5 s. Same with `alwaysLoaded = true`: assert it never returns a texture whose `BASE_LEVEL > 0`.
- [ ] **Step 5: Run** `engine-tests "[texture]"`, `scene-render-check`, `studio-tests` → PASS.
- [ ] **Step 6: Commit** `git commit -m "Mesh textures stream from the texture cache, smallest levels first"`

---

### Task 10: Packaging and .gitignore

**Files:**
- Modify: `src/engine_core/Project.cpp` (where it writes `.gitignore`), the GamePack source that copies `resources/` (find with `grep -rn "resources" src/ide/*Pack* src/engine_core/*Pack*`)
- Test: `tests/GamePackTest.cpp`

- [ ] **Step 1: Write failing tests:** a new project's `.gitignore` contains a `.cache/` line; packaging a project whose `.cache/textures/x.atex` exists produces a package containing `.cache/textures/x.atex`, and Player's resolved cache root for a package points at it.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.** Existing projects: when opening a project whose `.gitignore` lacks `.cache/`, append it once.
- [ ] **Step 4: Run** `engine-tests` → PASS.
- [ ] **Step 5: Commit** `git commit -m "Projects ignore and packages carry the texture cache"`

---

### Task 11: Live check on the H1Z1 demo

Release builds, warmed up (see memory: rebuild means `build/Release/AnarchyStudio.exe`; profile warmed-up).

- [ ] **Step 1:** Rebuild Release. Delete `<demo project>/.cache`. Launch the studio with the MCP port/token (memory: drive studio over MCP), open the H1Z1 terrain demo, and screenshot at 0.25 s, 0.5 s, 1 s, 3 s after the place opens. Expected: no white terrain at 0.5 s; blurry color first, sharp later.
- [ ] **Step 2:** Close and reopen. Expected: textured within a few frames, sharp in under 1 s.
- [ ] **Step 3:** Record the terrain's texture memory (`TerrainTextures::memory_bytes`, shown in the IDE) before (on `main`) and after. Expected ~2.7x lower.
- [ ] **Step 4:** Package the demo, run it with AnarchyPlayer.exe, confirm no bake jobs run (log or profiler).
- [ ] **Step 5:** Report the screenshots and numbers to the user. Do not claim success without them.
