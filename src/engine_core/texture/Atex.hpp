#pragma once

// The .atex file: a texture baked once into the project's texture cache so
// later loads skip decoding, resizing, mip building, and compression. Its
// levels are stored smallest first, and the header says where each level is,
// so any one level is one seek and one read: the small levels can be on
// screen before the large ones are even read.
//
// Layout, little-endian: "ATEX", u32 version, u32 width, u32 height, u32
// levels, u32 planes, one u8 PixelFormat per plane padded to 4 bytes, then
// levels x planes entries of {u64 offset, u64 length} indexed [level][plane],
// then the data, level levels-1 first and level 0 last, each level's planes
// together.

#include "texture/BlockCompress.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine_core::texture {

// Bumped whenever the layout or what the bytes mean changes; a file of any
// other version is rejected and baked again.
constexpr std::uint32_t kAtexVersion = 1;

// A baked texture: one or more planes (a terrain layer has three, any other
// texture one), each a full mip chain in its own format, level 0 the largest.
// Level L is max(1, width >> L) by max(1, height >> L).
struct BakedTexture {
    int width = 0;
    int height = 0;
    std::vector<PixelFormat> formats;                            // one per plane
    std::vector<std::vector<std::vector<std::uint8_t>>> planes;  // [plane][level]

    int level_count() const { return planes.empty() ? 0 : static_cast<int>(planes[0].size()); }
};

// Writes baked to path, creating its folder: to a temporary file first,
// renamed into place, so a crash never leaves half a file under the final
// name. False, with error set, when it cannot.
bool write_atex(const std::filesystem::path& path, const BakedTexture& baked, std::string& error);

struct AtexHeader {
    int width = 0;
    int height = 0;
    int levels = 0;
    std::vector<PixelFormat> formats;
    // [level][plane]: {offset, length} in the file.
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges;
};

// The header, checked: "ATEX", kAtexVersion, a known format for every plane,
// each length what level_bytes says, each range inside the file. Nullopt for
// a missing, short, or foreign file.
std::optional<AtexHeader> read_atex_header(const std::filesystem::path& path);

// Level `level` of every plane, [plane]. Nullopt when the file is now short.
std::optional<std::vector<std::vector<std::uint8_t>>> read_atex_level(const std::filesystem::path& path,
                                                                       const AtexHeader& header, int level);

}  // namespace engine_core::texture
