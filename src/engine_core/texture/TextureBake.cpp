#include "texture/TextureBake.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace engine_core::texture {

namespace {

std::atomic<bool> gS3tcAvailable{true};

void fnv(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
}

void fnv(std::uint64_t& hash, const std::string& text) {
    fnv(hash, text.data(), text.size());
    const char end = '\0';   // so "ab"+"c" and "a"+"bc" differ
    fnv(hash, &end, 1);
}

std::uint8_t to_byte(float v) { return std::uint8_t(std::clamp(std::lround(v), 0l, 255l)); }

std::vector<std::uint8_t> next_level(Usage usage, const std::vector<std::uint8_t>& src, int w, int h, int nw,
                                     int nh) {
    std::vector<std::uint8_t> out(std::size_t(nw) * std::size_t(nh) * 4);
    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
            float sum[4] = {0, 0, 0, 0};
            int count = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int sy = std::min(y * 2 + dy, h - 1);
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, w - 1);
                    const std::uint8_t* p = &src[(std::size_t(sy) * std::size_t(w) + std::size_t(sx)) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    ++count;
                }
            }
            std::uint8_t* q = &out[(std::size_t(y) * std::size_t(nw) + std::size_t(x)) * 4];
            if (usage == Usage::Normal) {
                float n[3];
                for (int c = 0; c < 3; ++c) n[c] = sum[c] / float(count) / 127.5f - 1.f;
                const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (length > 1e-4f) {
                    for (float& v : n) v /= length;
                } else {
                    n[0] = n[1] = 0.f;
                    n[2] = 1.f;
                }
                for (int c = 0; c < 3; ++c) q[c] = to_byte((n[c] + 1.f) * 127.5f);
                q[3] = to_byte(sum[3] / float(count));
            } else {
                for (int c = 0; c < 4; ++c) q[c] = to_byte(sum[c] / float(count));
            }
        }
    }
    return out;
}

}  // namespace

void set_s3tc_available(bool available) { gS3tcAvailable.store(available, std::memory_order_relaxed); }

bool s3tc_available() { return gS3tcAvailable.load(std::memory_order_relaxed); }

PixelFormat format_for(Usage usage, bool has_alpha) {
    if (!s3tc_available()) return PixelFormat::RGBA8;
    switch (usage) {
        case Usage::Normal: return PixelFormat::BC5;
        case Usage::Mask: return PixelFormat::BC4;
        case Usage::Color: break;
    }
    return has_alpha ? PixelFormat::BC3 : PixelFormat::BC1;
}

std::vector<std::vector<std::uint8_t>> build_mips(Usage usage, std::vector<std::uint8_t> rgba, int width,
                                                  int height) {
    std::vector<std::vector<std::uint8_t>> mips;
    mips.push_back(std::move(rgba));
    int w = width, h = height;
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        mips.push_back(next_level(usage, mips.back(), w, h, nw, nh));
        w = nw;
        h = nh;
    }
    return mips;
}

std::string cache_key(const std::vector<std::filesystem::path>& sources, const std::string& settings) {
    std::vector<std::filesystem::file_time_type> stamps;
    for (const std::filesystem::path& source : sources) {
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(source, error);
        stamps.push_back(error ? std::filesystem::file_time_type{} : stamp);
    }
    return cache_key(sources, stamps, settings);
}

std::string cache_key(const std::vector<std::filesystem::path>& sources,
                      const std::vector<std::filesystem::file_time_type>& stamps, const std::string& settings) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const std::uint32_t version = kAtexVersion;
    fnv(hash, &version, sizeof(version));
    for (std::size_t i = 0; i < sources.size(); ++i) {
        fnv(hash, sources[i].generic_u8string());
        const long long ticks =
            i < stamps.size() ? static_cast<long long>(stamps[i].time_since_epoch().count()) : 0ll;
        fnv(hash, &ticks, sizeof(ticks));
    }
    fnv(hash, settings);
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

std::filesystem::path cache_path(const std::filesystem::path& resources_root, const std::string& key) {
    return resources_root.parent_path() / ".cache" / "textures" / (key + ".atex");
}

BakeResult bake_texture(const std::filesystem::path& source, Usage usage, bool flip_y,
                        const std::filesystem::path& cache) {
    BakeResult result;
    std::string why;
    std::optional<DecodedImage> image = decode_image_file(source, why);
    if (!image) {
        result.warning = source.u8string() + ": " + why;
        return result;
    }
    if (flip_y) {
        const std::size_t row = std::size_t(image->width) * 4;
        for (int top = 0, bottom = image->height - 1; top < bottom; ++top, --bottom) {
            std::swap_ranges(image->rgba.begin() + std::ptrdiff_t(std::size_t(top) * row),
                             image->rgba.begin() + std::ptrdiff_t(std::size_t(top + 1) * row),
                             image->rgba.begin() + std::ptrdiff_t(std::size_t(bottom) * row));
        }
    }
    bool has_alpha = false;
    for (std::size_t i = 3; i < image->rgba.size() && !has_alpha; i += 4) has_alpha = image->rgba[i] != 255;
    const PixelFormat format = format_for(usage, has_alpha);

    BakedTexture baked;
    baked.width = image->width;
    baked.height = image->height;
    baked.formats = {format};
    baked.planes.resize(1);
    const std::vector<std::vector<std::uint8_t>> mips = build_mips(usage, std::move(image->rgba), baked.width,
                                                                   baked.height);
    for (std::size_t level = 0; level < mips.size(); ++level) {
        const int w = std::max(1, baked.width >> level), h = std::max(1, baked.height >> level);
        baked.planes[0].push_back(encode_level(format, mips[level].data(), w, h));
    }
    std::string error;
    if (!write_atex(cache, baked, error)) result.warning = error;
    result.baked = std::move(baked);
    return result;
}

}  // namespace engine_core::texture
