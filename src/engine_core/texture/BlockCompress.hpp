#pragma once

// Block compression for baked textures: BC1 (opaque color), BC3 (color and
// alpha), BC4 (one channel), and BC5 (two channels), the formats OpenGL 3.3
// draws from directly (BC4/BC5 in core, BC1/BC3 through
// GL_EXT_texture_compression_s3tc), plus plain RGBA8 for when s3tc is
// missing. Every function is pure and safe on any thread.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine_core::texture {

// How a baked level's bytes are laid out. The values are stored in .atex
// files, so they never change.
enum class PixelFormat : std::uint8_t { RGBA8 = 0, BC1 = 1, BC3 = 2, BC4 = 3, BC5 = 4 };

// Bytes one level of width x height takes in format: whole 4x4 blocks for BC
// (8 bytes a block for BC1 and BC4, 16 for BC3 and BC5), 4 a pixel for RGBA8.
std::size_t level_bytes(PixelFormat format, int width, int height);

// The side of mip `level` of a texture whose level 0 is `base` on that side:
// halved and floored each level, never below 1, as GL sizes mipmaps. Every
// place that allocates, builds, or uploads terrain levels must agree on this.
inline int mip_size(int base, int level) {
    return std::max(base >> level, 1);
}

// One level of RGBA8 pixels, rows in the order given, in format. A partial
// 4x4 tile at the right or top edge repeats the last column or row. BC1 and
// BC3 take RGB (BC3 also A), BC4 takes R, BC5 takes R and G.
std::vector<std::uint8_t> encode_level(PixelFormat format, const std::uint8_t* rgba, int width, int height);

// encode_level's bytes back to RGBA8. Channels a format does not hold come
// back as BC4: G = B = 0, A = 255; BC5: B = 0, A = 255; BC1: A = 255.
std::vector<std::uint8_t> decode_level(PixelFormat format, const std::uint8_t* data, int width, int height);

// A level of width x height filled with one RGBA8 color, already encoded.
std::vector<std::uint8_t> constant_level(PixelFormat format, std::array<std::uint8_t, 4> rgba, int width,
                                         int height);

}  // namespace engine_core::texture
