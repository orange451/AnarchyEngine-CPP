#pragma once

// Baking a texture file into the project's texture cache: decode it, build
// its mip chain, compress every level, and write an .atex next to the
// project (not in resources/, and not in git). The cache key names the
// source files, their times, and every setting that changes the bytes, so
// editing a file bakes it again and a stale bake is never read. Safe on any
// thread; touches only the given files.

#include "texture/Atex.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine_core::texture {

// What a texture is drawn as, which picks how it is compressed and how its
// mips are averaged.
enum class Usage : std::uint8_t { Color = 0, Normal = 1, Mask = 2 };

// Whether the GL context draws BC1/BC3 (GL_EXT_texture_compression_s3tc).
// The runner sets it once it has a context; true until then. Any thread.
void set_s3tc_available(bool available);
bool s3tc_available();

// Color: BC1 when every alpha is 255, else BC3. Normal: BC5 (X and Y; the
// shader rebuilds Z). Mask: BC4 (R). RGBA8 for all without s3tc, so one
// fallback path covers every texture.
PixelFormat format_for(Usage usage, bool has_alpha);

// The full chain from rgba (width x height, level 0) down to 1x1, each level
// a box average of the one above, max(1, n / 2) a side. Normal usage
// renormalizes each averaged normal.
std::vector<std::vector<std::uint8_t>> build_mips(Usage usage, std::vector<std::uint8_t> rgba, int width, int height);

// 16 hex digits: FNV-1a over kAtexVersion, each source's path and
// last_write_time (a missing file counts as time 0), and settings, which
// names whatever else changes the baked bytes.
std::string cache_key(const std::vector<std::filesystem::path>& sources, const std::string& settings);
// The same, with each source's last_write_time given (default for missing)
// rather than read: for callers that already stat their files.
std::string cache_key(const std::vector<std::filesystem::path>& sources,
                      const std::vector<std::filesystem::file_time_type>& stamps, const std::string& settings);

// <project>/.cache/textures/<key>.atex, the project being resources_root's
// parent folder.
std::filesystem::path cache_path(const std::filesystem::path& resources_root, const std::string& key);

// An image file decoded to RGBA8, bottom row first (as OpenGL takes it).
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// Reads and decodes path (any format stb_image reads). Nullopt, with why
// set, when it cannot.
std::optional<DecodedImage> decode_image_file(const std::filesystem::path& path, std::string& why);

struct BakeResult {
    // Nullopt when the source could not be read or decoded.
    std::optional<BakedTexture> baked;
    // Why it could not be baked, or why the cache file could not be written
    // (the result is still good then); empty when all went well.
    std::string warning;
};

// Decodes source, flips it when flip_y (its top row at v 0), builds its
// mips, compresses every level, and writes cache.
BakeResult bake_texture(const std::filesystem::path& source, Usage usage, bool flip_y,
                        const std::filesystem::path& cache);

}  // namespace engine_core::texture
