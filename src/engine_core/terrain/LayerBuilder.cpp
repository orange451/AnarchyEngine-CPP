// LayerBuilder: see LayerBuilder.hpp. The only stb_image includer in
// engine_core (CMakeLists.txt gives it that one SYSTEM include dir).

#include "terrain/LayerBuilder.hpp"

#include "terrain/HeightDerive.hpp"
#include "texture/TextureBake.hpp"

// Private to this file, so it cannot clash with the copies TextureCache.cpp
// (studio) and the JadeFX library each compile and define on their own.
// pragma-silenced like engine_core's other vendored single-header
// implementations (ConvexDecomposition.cpp's VHACD.h, PhysicsWorld.cpp's
// box3d.h): SYSTEM on its include dir does not fully suppress the
// warnings this header's unused-function paths trigger under /W4.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#pragma warning(push, 0)
#include "stb_image.h"
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <utility>

namespace engine_core::terrain {

namespace {

// --- Loading -----------------------------------------------------------

// Reads the whole file into memory. Using stbi_load_from_memory (rather
// than stbi_load on the path) sidesteps std::filesystem::path's narrow
// encoding on Windows entirely: ifstream opens the native path, and only
// raw bytes cross into stb_image.
std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamoff length = in.tellg();
    if (length < 0) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<size_t>(length));
    in.seekg(0);
    if (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), length)) return std::nullopt;
    return bytes;
}

// Decodes `path` to RGBA8 and resizes it to size x size, or returns
// nullopt (path empty, or the file could not be opened/decoded -- in the
// latter case *warning is set to the first such failure seen).
std::optional<std::vector<std::uint8_t>> load_and_resize(const std::filesystem::path& path, int size,
                                                           std::string& warning);

std::vector<std::uint8_t> resize_rgba(const std::vector<std::uint8_t>& src, int sw, int sh, int dw, int dh);

void set_warning_once(std::string& warning, const std::filesystem::path& path, const char* reason) {
    if (!warning.empty()) return;
    warning = path.string() + ": " + (reason != nullptr ? reason : "could not be read");
}

std::optional<std::vector<std::uint8_t>> load_and_resize(const std::filesystem::path& path, int size,
                                                           std::string& warning) {
    if (path.empty()) return std::nullopt;
    const std::optional<std::vector<std::uint8_t>> bytes = read_file(path);
    if (!bytes) {
        set_warning_once(warning, path, "could not be opened");
        return std::nullopt;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        set_warning_once(warning, path, stbi_failure_reason());
        return std::nullopt;
    }
    std::vector<std::uint8_t> rgba(pixels, pixels + size_t(w) * size_t(h) * 4);
    stbi_image_free(pixels);
    return resize_rgba(rgba, w, h, size, size);
}

// --- Resize: box filter when shrinking by an exact integer factor on an
// axis, bilinear otherwise (including any upsize); never sharpens. -------

// Resamples a single channel's n samples to m samples.
void resample_1d(const float* src, int n, float* dst, int m) {
    if (n == m) {
        std::copy(src, src + n, dst);
        return;
    }
    if (m > 0 && n % m == 0 && n > m) {
        const int factor = n / m;
        for (int i = 0; i < m; ++i) {
            float sum = 0.f;
            for (int k = 0; k < factor; ++k) sum += src[i * factor + k];
            dst[i] = sum / float(factor);
        }
        return;
    }
    for (int i = 0; i < m; ++i) {
        const float fx = (float(i) + 0.5f) * float(n) / float(m) - 0.5f;
        const int x0 = int(std::floor(fx));
        const float t = fx - float(x0);
        const int x0c = std::max(0, std::min(n - 1, x0));
        const int x1c = std::max(0, std::min(n - 1, x0 + 1));
        dst[i] = src[x0c] * (1.f - t) + src[x1c] * t;
    }
}

std::vector<std::uint8_t> resize_rgba(const std::vector<std::uint8_t>& src, int sw, int sh, int dw, int dh) {
    if (sw == dw && sh == dh) return src;

    // Horizontal pass: sw -> dw, height stays sh.
    std::vector<float> horizontal(size_t(dw) * size_t(sh) * 4);
    {
        // Named size_t variables, not vector<float> row_in(size_t(sw))
        // directly: a single parenthesized argument that is itself a
        // function-style cast to a real type name parses as a function
        // declaration (the "most vexing parse"), not a vector.
        const size_t sw_count = size_t(sw);
        const size_t dw_count = size_t(dw);
        std::vector<float> row_in(sw_count);
        std::vector<float> row_out(dw_count);
        for (int y = 0; y < sh; ++y) {
            for (int c = 0; c < 4; ++c) {
                for (int x = 0; x < sw; ++x) row_in[size_t(x)] = float(src[(size_t(y) * sw + x) * 4 + c]);
                resample_1d(row_in.data(), sw, row_out.data(), dw);
                for (int x = 0; x < dw; ++x) horizontal[(size_t(y) * dw + x) * 4 + c] = row_out[size_t(x)];
            }
        }
    }

    // Vertical pass: sh -> dh, width stays dw.
    std::vector<std::uint8_t> out(size_t(dw) * size_t(dh) * 4);
    {
        const size_t sh_count = size_t(sh);
        const size_t dh_count = size_t(dh);
        std::vector<float> col_in(sh_count);
        std::vector<float> col_out(dh_count);
        for (int x = 0; x < dw; ++x) {
            for (int c = 0; c < 4; ++c) {
                for (int y = 0; y < sh; ++y) col_in[size_t(y)] = horizontal[(size_t(y) * dw + x) * 4 + c];
                resample_1d(col_in.data(), sh, col_out.data(), dh);
                for (int y = 0; y < dh; ++y) {
                    const float v = std::max(0.f, std::min(255.f, col_out[size_t(y)]));
                    out[(size_t(y) * dw + x) * 4 + c] = std::uint8_t(std::lround(v));
                }
            }
        }
    }
    return out;
}

// --- Mip chains ----------------------------------------------------------

// sRGB-ish decode/encode used only for mip-averaging color in linear
// space, per the task brief ("decode pow 2.2 ... re-encode"); not the
// piecewise sRGB curve HeightDerive uses elsewhere, since the brief asks
// for the simple power curve here.
float decode_color(std::uint8_t c) { return std::pow(float(c) / 255.f, 2.2f); }
std::uint8_t encode_color(float linear) {
    const float encoded = std::pow(std::max(0.f, linear), 1.f / 2.2f) * 255.f;
    return std::uint8_t(std::lround(std::max(0.f, std::min(255.f, encoded))));
}

std::uint8_t average_u8(float sum, int count) {
    return std::uint8_t(std::lround(std::max(0.f, std::min(255.f, sum / float(count)))));
}

// Builds the full mip chain for the A array (RGB color, decoded/averaged/
// re-encoded in linear space per the brief; alpha = height, plain
// average), starting from level0 (which becomes level 0 of the result).
std::vector<std::vector<std::uint8_t>> build_color_height_mips(std::vector<std::uint8_t> level0, int w, int h) {
    std::vector<std::vector<std::uint8_t>> mips;
    mips.push_back(std::move(level0));
    int cw = w, ch = h;
    while (cw > 1 || ch > 1) {
        const std::vector<std::uint8_t>& prev = mips.back();
        const int nw = engine_core::texture::mip_size(w, static_cast<int>(mips.size()));
        const int nh = engine_core::texture::mip_size(h, static_cast<int>(mips.size()));
        std::vector<std::uint8_t> next(size_t(nw) * size_t(nh) * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                float linear[3] = {0.f, 0.f, 0.f};
                float alpha_sum = 0.f;
                int count = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    const int sy = std::min(ch - 1, y * 2 + dy);
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(cw - 1, x * 2 + dx);
                        const size_t si = (size_t(sy) * size_t(cw) + size_t(sx)) * 4;
                        for (int c = 0; c < 3; ++c) linear[c] += decode_color(prev[si + size_t(c)]);
                        alpha_sum += float(prev[si + 3]);
                        ++count;
                    }
                }
                const size_t di = (size_t(y) * size_t(nw) + size_t(x)) * 4;
                for (int c = 0; c < 3; ++c) next[di + size_t(c)] = encode_color(linear[c] / float(count));
                next[di + 3] = average_u8(alpha_sum, count);
            }
        }
        mips.push_back(std::move(next));
        cw = nw;
        ch = nh;
    }
    return mips;
}

// Builds the full mip chain for the B array (RG = tangent-space normal
// XY, renormalized as a 3D vector with Z reconstructed, re-encoded as XY
// only; B = roughness, A = metalness, both plain averages, except
// roughness is widened by Toksvig's normal-variance-to-roughness fold --
// see this function's body).
std::vector<std::vector<std::uint8_t>> build_normal_rough_metal_mips(std::vector<std::uint8_t> level0, int w, int h) {
    std::vector<std::vector<std::uint8_t>> mips;
    mips.push_back(std::move(level0));
    int cw = w, ch = h;
    while (cw > 1 || ch > 1) {
        const std::vector<std::uint8_t>& prev = mips.back();
        const int nw = engine_core::texture::mip_size(w, static_cast<int>(mips.size()));
        const int nh = engine_core::texture::mip_size(h, static_cast<int>(mips.size()));
        std::vector<std::uint8_t> next(size_t(nw) * size_t(nh) * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                float vx = 0.f, vy = 0.f, vz = 0.f, rough_sum = 0.f, metal_sum = 0.f;
                int count = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    const int sy = std::min(ch - 1, y * 2 + dy);
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(cw - 1, x * 2 + dx);
                        const size_t si = (size_t(sy) * size_t(cw) + size_t(sx)) * 4;
                        const float nx = float(prev[si + 0]) / 255.f * 2.f - 1.f;
                        const float ny = float(prev[si + 1]) / 255.f * 2.f - 1.f;
                        const float nz = std::sqrt(std::max(0.f, 1.f - nx * nx - ny * ny));
                        vx += nx;
                        vy += ny;
                        vz += nz;
                        rough_sum += float(prev[si + 2]);
                        metal_sum += float(prev[si + 3]);
                        ++count;
                    }
                }
                const float len = std::sqrt(vx * vx + vy * vy + vz * vz);
                // The averaged normal's own length before it is thrown away
                // by renormalizing below: 1.0 when every sample in this 2x2
                // box points the same way (a flat or low-frequency patch),
                // shrinking toward 0 as the box mixes diverging normals (a
                // bump field, a crease). That shrinkage is exactly the
                // information Toksvig's normal-mapping antialiasing turns
                // into roughness: a mip that has blurred away fine normal
                // detail must get rougher, or the lost microfacet spread
                // reappears as unearned, too-sharp specular highlights (the
                // "glossier mips" bug terrain.frag's far falloffs exposed --
                // normals went flatter-looking at distance while staying as
                // shiny as the base texture).
                //
                // ns = |average unit normal| (len/count, in [0, 1]) is the
                // same quantity Toksvig's paper and later "specular
                // antialiasing" writeups (e.g. Valve's "normal mapping
                // without tangent space", the common `roughness =
                // sqrt(roughness^2 + (1 - ns^2))` games use for normal-map
                // mip chains) use as the variance estimate: 1 - ns^2 grows
                // from 0 (coherent) toward 1 (fully incoherent) as the box's
                // normals disagree, and is folded into the roughness in
                // GGX-alpha space (roughness^2) so it combines additively
                // with the texture's own alpha, matching how normal-map and
                // material variance compose physically.
                const float ns = count > 0 ? std::min(1.f, len / float(count)) : 1.f;
                const float variance = std::max(0.f, 1.f - ns * ns);
                const float baseRough = rough_sum / (255.f * float(count));
                const float alpha = baseRough * baseRough + variance;
                const float toksvigRough = std::sqrt(std::min(1.f, alpha));

                float ex = 0.f, ey = 0.f;
                if (len > 1e-6f) {
                    ex = vx / len;
                    ey = vy / len;
                }
                const size_t di = (size_t(y) * size_t(nw) + size_t(x)) * 4;
                next[di + 0] = std::uint8_t(std::lround(std::max(0.f, std::min(255.f, (ex * 0.5f + 0.5f) * 255.f))));
                next[di + 1] = std::uint8_t(std::lround(std::max(0.f, std::min(255.f, (ey * 0.5f + 0.5f) * 255.f))));
                next[di + 2] =
                    std::uint8_t(std::lround(std::max(0.f, std::min(255.f, toksvigRough * 255.f))));
                next[di + 3] = average_u8(metal_sum, count);
            }
        }
        mips.push_back(std::move(next));
        cw = nw;
        ch = nh;
    }
    return mips;
}

}  // namespace

LayerPixels build_layer(const LayerSources& sources, int size) {
    LayerPixels result;
    result.size = size;
    if (size <= 0) return result;

    const std::optional<std::vector<std::uint8_t>> diffuse = load_and_resize(sources.diffuse, size, result.warning);
    const std::optional<std::vector<std::uint8_t>> normal = load_and_resize(sources.normal, size, result.warning);
    const std::optional<std::vector<std::uint8_t>> roughness =
        load_and_resize(sources.roughness, size, result.warning);
    const std::optional<std::vector<std::uint8_t>> metalness =
        load_and_resize(sources.metalness, size, result.warning);
    const std::optional<std::vector<std::uint8_t>> height_map = load_and_resize(sources.height, size, result.warning);

    const size_t n = size_t(size) * size_t(size);

    // Height chain (Task 3): HeightTexture's R channel, resized; else
    // integrated from the (already-resized) normal map; else the
    // (already-resized) diffuse map's luminance; else flat 0.5. Deriving
    // after resizing to `size` (rather than at each source's own
    // resolution) is cheaper -- height_from_normals/height_from_luminance
    // cost grows with pixel count, and this layer is only ever sampled at
    // `size` -- and both functions work at any resolution, so the result
    // is just as valid.
    std::vector<float> height01;
    if (height_map) {
        height01.resize(n);
        for (size_t i = 0; i < n; ++i) height01[i] = float((*height_map)[i * 4 + 0]) / 255.f;
    } else if (normal) {
        height01 = height_from_normals(normal->data(), size, size);
    } else if (diffuse) {
        height01 = height_from_luminance(diffuse->data(), size, size);
    } else {
        height01.assign(n, 0.5f);
    }

    // Level 0, A: color RGB (white if no diffuse) + height in alpha.
    std::vector<std::uint8_t> a0(n * 4);
    for (size_t i = 0; i < n; ++i) {
        if (diffuse) {
            a0[i * 4 + 0] = (*diffuse)[i * 4 + 0];
            a0[i * 4 + 1] = (*diffuse)[i * 4 + 1];
            a0[i * 4 + 2] = (*diffuse)[i * 4 + 2];
        } else {
            a0[i * 4 + 0] = a0[i * 4 + 1] = a0[i * 4 + 2] = 255;
        }
        const float h01 = std::max(0.f, std::min(1.f, height01[i]));
        a0[i * 4 + 3] = std::uint8_t(std::lround(h01 * 255.f));
    }

    // Level 0, B: normal XY (flat if no normal map) + roughness.R +
    // metalness.R (both 255 i.e. roughness/metalness 1 if missing).
    std::vector<std::uint8_t> b0(n * 4);
    for (size_t i = 0; i < n; ++i) {
        if (normal) {
            b0[i * 4 + 0] = (*normal)[i * 4 + 0];
            b0[i * 4 + 1] = (*normal)[i * 4 + 1];
        } else {
            b0[i * 4 + 0] = 128;
            b0[i * 4 + 1] = 128;
        }
        b0[i * 4 + 2] = roughness ? (*roughness)[i * 4 + 0] : 255;
        b0[i * 4 + 3] = metalness ? (*metalness)[i * 4 + 0] : 255;
    }

    result.a_mips = build_color_height_mips(std::move(a0), size, size);
    result.b_mips = build_normal_rough_metal_mips(std::move(b0), size, size);
    return result;
}

namespace {

// The side of each level of a size chain, level 0 first, down to 1.
std::vector<int> chain_sides(int size) {
    std::vector<int> sides;
    if (size <= 0) return sides;
    for (int level = 0;; ++level) {
        const int w = engine_core::texture::mip_size(size, level);
        sides.push_back(w);
        if (w == 1) break;
    }
    return sides;
}

// Channels (first, second) of each pixel of rgba as R and G, B 0, A 255.
std::vector<std::uint8_t> two_channels(const std::vector<std::uint8_t>& rgba, int first, int second) {
    std::vector<std::uint8_t> out(rgba.size());
    for (size_t i = 0; i < rgba.size(); i += 4) {
        out[i + 0] = rgba[i + size_t(first)];
        out[i + 1] = rgba[i + size_t(second)];
        out[i + 2] = 0;
        out[i + 3] = 255;
    }
    return out;
}

}  // namespace

std::array<texture::PixelFormat, 3> layer_formats() {
    using texture::PixelFormat;
    if (!texture::s3tc_available()) return {PixelFormat::RGBA8, PixelFormat::RGBA8, PixelFormat::RGBA8};
    return {PixelFormat::BC3, PixelFormat::BC5, PixelFormat::BC5};
}

LayerBytes compress_layer(const LayerPixels& pixels) {
    const std::array<texture::PixelFormat, 3> formats = layer_formats();
    LayerBytes out;
    out.size = pixels.size;
    out.warning = pixels.warning;
    const std::vector<int> sides = chain_sides(pixels.size);
    for (size_t level = 0; level < pixels.a_mips.size() && level < sides.size(); ++level) {
        const int w = sides[level];
        out.planes[0].push_back(texture::encode_level(formats[0], pixels.a_mips[level].data(), w, w));
        const std::vector<std::uint8_t>& b = pixels.b_mips[level];
        if (formats[1] == texture::PixelFormat::RGBA8) {
            out.planes[1].push_back(two_channels(b, 0, 1));
            out.planes[2].push_back(two_channels(b, 2, 3));
        } else {
            // BC5 reads R and G: B as is, and C's R and G from B's B and A.
            out.planes[1].push_back(texture::encode_level(formats[1], b.data(), w, w));
            const std::vector<std::uint8_t> c = two_channels(b, 2, 3);
            out.planes[2].push_back(texture::encode_level(formats[2], c.data(), w, w));
        }
    }
    return out;
}

LayerBytes preview_layer(const LayerSources& sources, int size) {
    LayerSources diffuse_only;
    diffuse_only.diffuse = sources.diffuse;
    const int preview_size = std::min(64, size);
    LayerBytes small = compress_layer(build_layer(diffuse_only, preview_size));
    const std::vector<int> sides = chain_sides(size);
    const int first = int(std::find(sides.begin(), sides.end(), preview_size) - sides.begin());
    LayerBytes out;
    out.size = size;
    out.first_level = first;
    out.warning = small.warning;
    for (size_t p = 0; p < 3; ++p) {
        out.planes[p].resize(sides.size());
        for (size_t level = 0; level < small.planes[p].size() && first + int(level) < int(sides.size()); ++level) {
            out.planes[p][size_t(first) + level] = std::move(small.planes[p][level]);
        }
    }
    return out;
}

namespace {

// Every level no larger than `largest` a side of a size chain, one color a
// plane; the larger levels left empty, first_level set past them.
LayerBytes constant_layer(int size, const std::array<std::array<std::uint8_t, 4>, 3>& colors, int largest) {
    const std::array<texture::PixelFormat, 3> formats = layer_formats();
    LayerBytes out;
    out.size = size;
    const std::vector<int> sides = chain_sides(size);
    out.first_level = int(sides.size()) - 1;
    for (size_t level = 0; level < sides.size(); ++level) {
        const int w = sides[level];
        const bool kept = w <= largest;
        if (kept) out.first_level = std::min(out.first_level, int(level));
        for (size_t p = 0; p < 3; ++p) {
            out.planes[p].push_back(kept ? texture::constant_level(formats[p], colors[p], w, w)
                                         : std::vector<std::uint8_t>{});
        }
    }
    return out;
}

}  // namespace

LayerBytes placeholder_layer(int size) {
    // Only the small levels: grey is grey at any resolution, and they upload
    // in a few kilobytes where the whole chain at 2048 would take megabytes.
    return constant_layer(size,
                          {{
                              {128, 128, 128, 128},   // mid grey, height 0.5
                              {128, 128, 0, 255},     // flat normal
                              {255, 255, 0, 255},     // roughness 1, metalness 1
                          }},
                          64);
}

LayerBytes untextured_layer(int size) {
    return constant_layer(size, {{{255, 255, 255, 128}, {128, 128, 0, 255}, {255, 255, 0, 255}}}, size);
}

std::size_t layer_bytes(int size) {
    const std::array<texture::PixelFormat, 3> formats = layer_formats();
    std::size_t total = 0;
    for (int w : chain_sides(size)) {
        for (texture::PixelFormat format : formats) total += texture::level_bytes(format, w, w);
    }
    return total;
}

}  // namespace engine_core::terrain

// texture::decode_image_file lives here, beside the one stb_image this
// package compiles.
namespace engine_core::texture {

std::optional<DecodedImage> decode_image_file(const std::filesystem::path& path, std::string& why) {
    const std::optional<std::vector<std::uint8_t>> bytes = terrain::read_file(path);
    if (!bytes) {
        why = "could not be opened";
        return std::nullopt;
    }
    if (bytes->empty() || bytes->size() > (std::size_t(256) << 20)) {
        why = "is empty or larger than 256 MiB";
        return std::nullopt;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        why = reason != nullptr ? reason : "is not an image";
        return std::nullopt;
    }
    DecodedImage image;
    image.width = w;
    image.height = h;
    image.rgba.resize(std::size_t(w) * std::size_t(h) * 4);
    // stb_image decodes the top row first; OpenGL takes the bottom row first.
    const std::size_t row = std::size_t(w) * 4;
    for (int line = 0; line < h; ++line) {
        std::copy_n(pixels + std::size_t(line) * row, row, image.rgba.data() + std::size_t(h - 1 - line) * row);
    }
    stbi_image_free(pixels);
    return image;
}

}  // namespace engine_core::texture
