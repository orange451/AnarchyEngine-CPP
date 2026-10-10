#include "texture/BlockCompress.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine_core::texture {

namespace {

int blocks_across(int n) { return (std::max(n, 1) + 3) / 4; }

std::size_t block_size(PixelFormat format) {
    return format == PixelFormat::BC1 || format == PixelFormat::BC4 ? 8 : 16;
}

// The 4x4 tile at block (bx, by), RGBA8, edge pixels repeated past the image.
void gather(const std::uint8_t* rgba, int width, int height, int bx, int by, std::uint8_t out[16][4]) {
    for (int y = 0; y < 4; ++y) {
        const int sy = std::min(by * 4 + y, height - 1);
        for (int x = 0; x < 4; ++x) {
            const int sx = std::min(bx * 4 + x, width - 1);
            std::memcpy(out[y * 4 + x], rgba + (std::size_t(sy) * std::size_t(width) + std::size_t(sx)) * 4, 4);
        }
    }
}

void scatter(std::uint8_t* rgba, int width, int height, int bx, int by, const std::uint8_t in[16][4]) {
    for (int y = 0; y < 4; ++y) {
        const int sy = by * 4 + y;
        if (sy >= height) break;
        for (int x = 0; x < 4; ++x) {
            const int sx = bx * 4 + x;
            if (sx >= width) break;
            std::memcpy(rgba + (std::size_t(sy) * std::size_t(width) + std::size_t(sx)) * 4, in[y * 4 + x], 4);
        }
    }
}

// ---- BC4: one channel, two 8-bit endpoints and 3-bit indices ----

void bc4_palette(int a0, int a1, int out[8]) {
    out[0] = a0;
    out[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i) out[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (int i = 1; i < 5; ++i) out[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        out[6] = 0;
        out[7] = 255;
    }
}

// Writes one BC4 block from endpoints a0, a1 (a0 > a1: 8 values; else 6
// values plus exact 0 and 255), each texel at its nearest; the summed error.
int write_bc4(const std::uint8_t tile[16][4], int channel, int a0, int a1, std::uint8_t out[8]) {
    int palette[8];
    bc4_palette(a0, a1, palette);
    std::uint64_t bits = 0;
    int total = 0;
    for (int i = 0; i < 16; ++i) {
        const int v = tile[i][channel];
        int best = 0, best_error = 1 << 30;
        for (int k = 0; k < 8; ++k) {
            const int e = std::abs(palette[k] - v);
            if (e < best_error) {
                best_error = e;
                best = k;
            }
        }
        bits |= std::uint64_t(best) << (3 * i);
        total += best_error;
    }
    out[0] = std::uint8_t(a0);
    out[1] = std::uint8_t(a1);
    for (int i = 0; i < 6; ++i) out[2 + i] = std::uint8_t(bits >> (8 * i));
    return total;
}

// Channel `channel` of the tile into 8 bytes: the 8-value mode spanning the
// tile's range, or the 6-value mode spanning it without its 0s and 255s
// (which that mode holds exactly), whichever is nearer.
void encode_bc4(const std::uint8_t tile[16][4], int channel, std::uint8_t out[8]) {
    int lo = 255, hi = 0, inner_lo = 255, inner_hi = 0;
    for (int i = 0; i < 16; ++i) {
        const int v = tile[i][channel];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        if (v != 0 && v != 255) {
            inner_lo = std::min(inner_lo, v);
            inner_hi = std::max(inner_hi, v);
        }
    }
    if (hi == lo) {
        write_bc4(tile, channel, hi, lo, out);   // every index 0 reads it
        return;
    }
    const int eight = write_bc4(tile, channel, hi, lo, out);
    if (inner_lo > inner_hi) {
        inner_lo = inner_hi = 128;   // only 0s and 255s: any inner pair will do
    }
    std::uint8_t six[8];
    if (write_bc4(tile, channel, inner_lo, inner_hi, six) < eight) std::memcpy(out, six, 8);
}

void decode_bc4(const std::uint8_t in[8], std::uint8_t tile[16][4], int channel) {
    int palette[8];
    bc4_palette(in[0], in[1], palette);
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= std::uint64_t(in[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) tile[i][channel] = std::uint8_t(palette[(bits >> (3 * i)) & 7]);
}

// ---- BC1 color block: two RGB565 endpoints and 2-bit indices ----

std::uint16_t pack565(const float c[3]) {
    const int r = std::clamp(int(std::lround(c[0] * 31.f / 255.f)), 0, 31);
    const int g = std::clamp(int(std::lround(c[1] * 63.f / 255.f)), 0, 63);
    const int b = std::clamp(int(std::lround(c[2] * 31.f / 255.f)), 0, 31);
    return std::uint16_t((r << 11) | (g << 5) | b);
}

void unpack565(std::uint16_t v, int out[3]) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

// The four colors of a block in 4-color mode (BC3's color block is always in
// it; BC1's is whenever c0 > c1).
void color_palette(std::uint16_t c0, std::uint16_t c1, bool four_color, int out[4][3]) {
    unpack565(c0, out[0]);
    unpack565(c1, out[1]);
    for (int k = 0; k < 3; ++k) {
        if (four_color) {
            out[2][k] = (2 * out[0][k] + out[1][k] + 1) / 3;
            out[3][k] = (out[0][k] + 2 * out[1][k] + 1) / 3;
        } else {
            out[2][k] = (out[0][k] + out[1][k]) / 2;
            out[3][k] = 0;
        }
    }
}

int color_distance(const int a[3], const std::uint8_t b[4]) {
    const int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

// Picks each texel's nearest of the first `colors` colors; the summed squared error.
long long pick_indices(const std::uint8_t tile[16][4], const int palette[4][3], int colors, int indices[16]) {
    long long total = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, best_error = 1 << 30;
        for (int k = 0; k < colors; ++k) {
            const int e = color_distance(palette[k], tile[i]);
            if (e < best_error) {
                best_error = e;
                best = k;
            }
        }
        indices[i] = best;
        total += best_error;
    }
    return total;
}

void put_color_block(std::uint16_t c0, std::uint16_t c1, const int indices[16], std::uint8_t out[8]) {
    out[0] = std::uint8_t(c0);
    out[1] = std::uint8_t(c0 >> 8);
    out[2] = std::uint8_t(c1);
    out[3] = std::uint8_t(c1 >> 8);
    std::uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= std::uint32_t(indices[i]) << (2 * i);
    std::memcpy(out + 4, &bits, 4);   // little-endian, as every target is
}

// Writes a block from two float endpoints, in 4-color mode (c0 > c1), or,
// when three_color, in BC1's 3-color mode (c0 <= c1: c0, c1, their midpoint;
// index 3, transparent black, is never used). Returns its squared error.
long long write_color_block(const std::uint8_t tile[16][4], const float e0[3], const float e1[3], bool three_color,
                            std::uint8_t out[8]) {
    std::uint16_t c0 = pack565(e0), c1 = pack565(e1);
    int indices[16] = {};
    long long error = 0;
    if (c0 == c1) {
        // One color: every index 0 reads c0 in either mode.
        int palette[4][3];
        color_palette(c0, c1, true, palette);
        for (int i = 0; i < 16; ++i) error += color_distance(palette[0], tile[i]);
    } else {
        if ((c0 < c1) != three_color) std::swap(c0, c1);
        int palette[4][3];
        color_palette(c0, c1, !three_color, palette);
        error = pick_indices(tile, palette, three_color ? 3 : 4, indices);
    }
    put_color_block(c0, c1, indices, out);
    return error;
}

// For one channel of `bits` bits, the endpoint pair (high, low) whose
// two-thirds point, (2 high + low) / 3 as the decoder rounds it, lands
// nearest each 8-bit value: what the 4-color mode's index 2 reads.
struct SingleColorTable {
    std::uint8_t high[256], low[256];
    explicit SingleColorTable(int bits) {
        const int levels = 1 << bits;
        const auto expand = [bits](int q) { return bits == 5 ? (q << 3) | (q >> 2) : (q << 2) | (q >> 4); };
        for (int v = 0; v < 256; ++v) {
            int best = 1 << 30;
            for (int h = 0; h < levels; ++h) {
                for (int l = 0; l < levels; ++l) {
                    const int e = std::abs((2 * expand(h) + expand(l) + 1) / 3 - v);
                    if (e < best) {
                        best = e;
                        high[v] = std::uint8_t(h);
                        low[v] = std::uint8_t(l);
                    }
                }
            }
        }
    }
};

// A tile of one color, as near it as BC1 gets: every texel reads index 2.
void encode_single_color(const std::uint8_t color[4], std::uint8_t out[8]) {
    static const SingleColorTable five(5), six(6);
    std::uint16_t c0 = std::uint16_t((five.high[color[0]] << 11) | (six.high[color[1]] << 5) | five.high[color[2]]);
    std::uint16_t c1 = std::uint16_t((five.low[color[0]] << 11) | (six.low[color[1]] << 5) | five.low[color[2]]);
    int index = 2;
    if (c0 < c1) {
        std::swap(c0, c1);
        index = 3;   // the same point, counted from the other end
    } else if (c0 == c1) {
        index = 0;
    }
    int indices[16];
    for (int& i : indices) i = index;
    put_color_block(c0, c1, indices, out);
}

// The 2x2 least-squares endpoints for the indices block holds: each texel's
// index puts it at a known share of the way from c0 to c1. False when the
// indices leave the system singular (every texel on one end).
bool refit_endpoints(const std::uint8_t tile[16][4], const std::uint8_t block[8], float f0[3], float f1[3]) {
    const std::uint16_t c0 = std::uint16_t(block[0] | (block[1] << 8));
    const std::uint16_t c1 = std::uint16_t(block[2] | (block[3] << 8));
    if (c0 == c1) return false;
    // Share of c1 each index reads: 4-color mode, or 3-color (index 3 unused).
    static const float kFour[4] = {0.f, 1.f, 1.f / 3.f, 2.f / 3.f};
    static const float kThree[4] = {0.f, 1.f, 0.5f, 0.f};
    const float* weight = c0 > c1 ? kFour : kThree;
    std::uint32_t bits;
    std::memcpy(&bits, block + 4, 4);
    float aa = 0, ab = 0, bb = 0, ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        const float w1 = weight[(bits >> (2 * i)) & 3], w0 = 1.f - w1;
        aa += w0 * w0;
        ab += w0 * w1;
        bb += w1 * w1;
        for (int k = 0; k < 3; ++k) {
            ax[k] += w0 * tile[i][k];
            bx[k] += w1 * tile[i][k];
        }
    }
    const float det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-6f) return false;
    for (int k = 0; k < 3; ++k) {
        f0[k] = std::clamp((ax[k] * bb - bx[k] * ab) / det, 0.f, 255.f);
        f1[k] = std::clamp((bx[k] * aa - ax[k] * ab) / det, 0.f, 255.f);
    }
    return true;
}

// A tile's color block. One color takes the exact single-color fit.
// Otherwise: endpoints along the tile's principal color axis, spanning its
// projection, then a least-squares refit to the indices that picks, in
// 4-color mode and, when three_color (BC1, never BC3), 3-color mode too;
// whichever is nearest.
void encode_color(const std::uint8_t tile[16][4], bool three_color, std::uint8_t out[8]) {
    bool single = true;
    for (int i = 1; i < 16 && single; ++i) single = std::memcmp(tile[i], tile[0], 3) == 0;
    if (single) {
        encode_single_color(tile[0], out);
        return;
    }
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 3; ++k) mean[k] += tile[i][k] / 16.f;
    float cov[6] = {0, 0, 0, 0, 0, 0};   // rr rg rb gg gb bb
    for (int i = 0; i < 16; ++i) {
        const float r = tile[i][0] - mean[0], g = tile[i][1] - mean[1], b = tile[i][2] - mean[2];
        cov[0] += r * r;
        cov[1] += r * g;
        cov[2] += r * b;
        cov[3] += g * g;
        cov[4] += g * b;
        cov[5] += b * b;
    }
    float axis[3] = {1.f, 1.f, 1.f};
    for (int iteration = 0; iteration < 8; ++iteration) {
        const float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
        const float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
        const float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length < 1e-6f) break;
        axis[0] = x / length;
        axis[1] = y / length;
        axis[2] = z / length;
    }
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; ++i) {
        const float t = (tile[i][0] - mean[0]) * axis[0] + (tile[i][1] - mean[1]) * axis[1] +
                        (tile[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    float e0[3], e1[3];
    for (int k = 0; k < 3; ++k) {
        e0[k] = std::clamp(mean[k] + axis[k] * hi, 0.f, 255.f);
        e1[k] = std::clamp(mean[k] + axis[k] * lo, 0.f, 255.f);
    }
    long long best = -1;
    for (int mode = 0; mode < (three_color ? 2 : 1); ++mode) {
        std::uint8_t block[8];
        long long error = write_color_block(tile, e0, e1, mode == 1, block);
        if (best < 0 || error < best) {
            best = error;
            std::memcpy(out, block, 8);
        }
        float f0[3], f1[3];
        if (refit_endpoints(tile, block, f0, f1)) {
            error = write_color_block(tile, f0, f1, mode == 1, block);
            if (error < best) {
                best = error;
                std::memcpy(out, block, 8);
            }
        }
    }
}

void decode_color(const std::uint8_t in[8], bool always_four_color, std::uint8_t tile[16][4]) {
    const std::uint16_t c0 = std::uint16_t(in[0] | (in[1] << 8));
    const std::uint16_t c1 = std::uint16_t(in[2] | (in[3] << 8));
    int palette[4][3];
    color_palette(c0, c1, always_four_color || c0 > c1, palette);
    std::uint32_t bits;
    std::memcpy(&bits, in + 4, 4);
    for (int i = 0; i < 16; ++i) {
        const int k = (bits >> (2 * i)) & 3;
        for (int c = 0; c < 3; ++c) tile[i][c] = std::uint8_t(palette[k][c]);
        tile[i][3] = !always_four_color && c0 <= c1 && k == 3 ? 0 : 255;
    }
}

void encode_block(PixelFormat format, const std::uint8_t tile[16][4], std::uint8_t* out) {
    switch (format) {
        case PixelFormat::BC1: encode_color(tile, true, out); break;
        case PixelFormat::BC3:
            encode_bc4(tile, 3, out);
            encode_color(tile, false, out + 8);
            break;
        case PixelFormat::BC4: encode_bc4(tile, 0, out); break;
        case PixelFormat::BC5:
            encode_bc4(tile, 0, out);
            encode_bc4(tile, 1, out + 8);
            break;
        case PixelFormat::RGBA8: break;
    }
}

void decode_block(PixelFormat format, const std::uint8_t* in, std::uint8_t tile[16][4]) {
    for (int i = 0; i < 16; ++i) {
        tile[i][0] = tile[i][1] = tile[i][2] = 0;
        tile[i][3] = 255;
    }
    switch (format) {
        case PixelFormat::BC1: decode_color(in, false, tile); break;
        case PixelFormat::BC3:
            decode_color(in + 8, true, tile);
            decode_bc4(in, tile, 3);
            break;
        case PixelFormat::BC4: decode_bc4(in, tile, 0); break;
        case PixelFormat::BC5:
            decode_bc4(in, tile, 0);
            decode_bc4(in + 8, tile, 1);
            break;
        case PixelFormat::RGBA8: break;
    }
}

}  // namespace

std::size_t level_bytes(PixelFormat format, int width, int height) {
    if (format == PixelFormat::RGBA8) return std::size_t(std::max(width, 1)) * std::size_t(std::max(height, 1)) * 4;
    return std::size_t(blocks_across(width)) * std::size_t(blocks_across(height)) * block_size(format);
}

std::vector<std::uint8_t> encode_level(PixelFormat format, const std::uint8_t* rgba, int width, int height) {
    if (format == PixelFormat::RGBA8) {
        return std::vector<std::uint8_t>(rgba, rgba + level_bytes(format, width, height));
    }
    std::vector<std::uint8_t> out(level_bytes(format, width, height));
    const int across = blocks_across(width), down = blocks_across(height);
    const std::size_t size = block_size(format);
    std::uint8_t tile[16][4];
    for (int by = 0; by < down; ++by) {
        for (int bx = 0; bx < across; ++bx) {
            gather(rgba, width, height, bx, by, tile);
            encode_block(format, tile, out.data() + (std::size_t(by) * std::size_t(across) + std::size_t(bx)) * size);
        }
    }
    return out;
}

std::vector<std::uint8_t> decode_level(PixelFormat format, const std::uint8_t* data, int width, int height) {
    std::vector<std::uint8_t> out(std::size_t(width) * std::size_t(height) * 4);
    if (format == PixelFormat::RGBA8) {
        std::memcpy(out.data(), data, out.size());
        return out;
    }
    const int across = blocks_across(width), down = blocks_across(height);
    const std::size_t size = block_size(format);
    std::uint8_t tile[16][4];
    for (int by = 0; by < down; ++by) {
        for (int bx = 0; bx < across; ++bx) {
            decode_block(format, data + (std::size_t(by) * std::size_t(across) + std::size_t(bx)) * size, tile);
            scatter(out.data(), width, height, bx, by, tile);
        }
    }
    return out;
}

std::vector<std::uint8_t> constant_level(PixelFormat format, std::array<std::uint8_t, 4> rgba, int width,
                                         int height) {
    if (format == PixelFormat::RGBA8) {
        std::vector<std::uint8_t> out(level_bytes(format, width, height));
        for (std::size_t i = 0; i < out.size(); i += 4) std::memcpy(&out[i], rgba.data(), 4);
        return out;
    }
    std::uint8_t tile[16][4];
    for (int i = 0; i < 16; ++i) std::memcpy(tile[i], rgba.data(), 4);
    std::uint8_t block[16];
    encode_block(format, tile, block);
    const std::size_t size = block_size(format);
    std::vector<std::uint8_t> out(level_bytes(format, width, height));
    for (std::size_t offset = 0; offset < out.size(); offset += size) std::memcpy(&out[offset], block, size);
    return out;
}

}  // namespace engine_core::texture
