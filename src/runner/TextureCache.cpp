#include "TextureCache.hpp"

#include "gl.hpp"
#include "texture/Atex.hpp"
#include "texture/TexturePool.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

// Private to this file, so it cannot clash with the copy JadeFX links.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

namespace runner {

namespace {

// How often a file already seen is looked at again.
constexpr std::chrono::seconds kRecheck{1};
// Larger files are refused before they are read.
constexpr std::uintmax_t kMaxFileSize = 256u * 1024u * 1024u;

// Under half float's largest value, 65504.
constexpr float kMaxHalfFloat = 65000.f;

// stb_image does not read OpenEXR, and says only that it does not know the
// file, so an .exr gets its own message.
bool IsOpenExr(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = path.substr(dot + 1);
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension == "exr";
}

// Reverses the order of height rows of row values each, in place.
template <class T>
void FlipRows(std::vector<T>& values, std::size_t row, int height) {
    for (int top = 0, bottom = height - 1; top < bottom; ++top, --bottom) {
        std::swap_ranges(values.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(top) * row),
                         values.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(top + 1) * row),
                         values.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(bottom) * row));
    }
}

}  // namespace

bool DecodeTexture(const std::uint8_t* bytes, std::size_t size, TexturePixels& out, std::string& error) {
    out = TexturePixels{};
    if (bytes == nullptr || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "the file is empty or too large";
        return false;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        error = reason != nullptr ? reason : "it is not an image";
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    // stb_image decodes the top row first; OpenGL takes the bottom row first.
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    for (int line = 0; line < height; ++line) {
        std::copy_n(pixels + static_cast<std::size_t>(line) * row, row,
                    out.rgba.data() + static_cast<std::size_t>(height - 1 - line) * row);
    }
    stbi_image_free(pixels);
    return true;
}

bool DecodeLinearTexture(const std::uint8_t* bytes, std::size_t size, LinearPixels& out, std::string& error,
                         int maxWidth) {
    out = LinearPixels{};
    if (bytes == nullptr || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "the file is empty or too large";
        return false;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    // An HDR file comes as it is. Any other image comes through stb_image's
    // gamma of 2.2, as surface.glsl makes a Material's colors linear.
    float* pixels = stbi_loadf_from_memory(bytes, static_cast<int>(size), &width, &height, &channels, 3);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        error = reason != nullptr ? reason : "it is not an image";
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    // Top row first in, bottom row first out, as DecodeTexture does. Kept
    // within half float, which the GPU holds it in, and a NaN is black.
    const std::size_t row = static_cast<std::size_t>(width) * 3;
    for (int line = 0; line < height; ++line) {
        const float* from = pixels + static_cast<std::size_t>(line) * row;
        float* to = out.rgb.data() + static_cast<std::size_t>(height - 1 - line) * row;
        for (std::size_t i = 0; i < row; ++i) {
            const float value = from[i];
            to[i] = value > 0.f ? std::min(value, kMaxHalfFloat) : 0.f;
        }
    }
    stbi_image_free(pixels);

    while (maxWidth > 0 && out.width > maxWidth) {
        const int halfWidth = std::max(out.width / 2, 1);
        const int halfHeight = std::max(out.height / 2, 1);
        std::vector<float> half(static_cast<std::size_t>(halfWidth) * static_cast<std::size_t>(halfHeight) * 3);
        const auto at = [&](int x, int y, int channel) {
            x = std::min(x, out.width - 1);
            y = std::min(y, out.height - 1);
            return out.rgb[(static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
                            static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(channel)];
        };
        for (int y = 0; y < halfHeight; ++y) {
            for (int x = 0; x < halfWidth; ++x) {
                for (int channel = 0; channel < 3; ++channel) {
                    half[(static_cast<std::size_t>(y) * static_cast<std::size_t>(halfWidth) +
                          static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(channel)] =
                        0.25f * (at(2 * x, 2 * y, channel) + at(2 * x + 1, 2 * y, channel) +
                                 at(2 * x, 2 * y + 1, channel) + at(2 * x + 1, 2 * y + 1, channel));
                }
            }
        }
        out.rgb = std::move(half);
        out.width = halfWidth;
        out.height = halfHeight;
    }
    return true;
}

namespace {

namespace texture = engine_core::texture;

// The GL internal format a baked level uploads as; 0 for RGBA8.
GLenum CompressedFormat(texture::PixelFormat format) {
    switch (format) {
        case texture::PixelFormat::BC1: return RT_GL_COMPRESSED_RGBA_S3TC_DXT1;
        case texture::PixelFormat::BC3: return RT_GL_COMPRESSED_RGBA_S3TC_DXT5;
        case texture::PixelFormat::BC4: return RT_GL_COMPRESSED_RED_RGTC1;
        case texture::PixelFormat::BC5: return RT_GL_COMPRESSED_RG_RGTC2;
        case texture::PixelFormat::RGBA8: break;
    }
    return 0;
}

int LevelSide(int side, int level) { return std::max(1, side >> level); }

}  // namespace

// One finished load step: a level of the texture streamed_[name], from load key.
struct TextureLevel {
    std::string name;
    std::string key;
    int width = 0;
    int height = 0;
    int levels = 0;
    texture::PixelFormat format = texture::PixelFormat::RGBA8;
    int level = 0;
    std::vector<std::uint8_t> bytes;
};

struct TextureCache::Loads {
    std::mutex mutex;
    bool stopping = false;
    std::vector<TextureLevel> levels;
    // {name, key, why} for loads that could not decode their file.
    std::vector<std::array<std::string, 3>> failures;

    bool stopped() {
        std::lock_guard<std::mutex> lock(mutex);
        return stopping;
    }
    void post(TextureLevel level) {
        std::lock_guard<std::mutex> lock(mutex);
        levels.push_back(std::move(level));
    }
};

namespace {

struct MeshLoad {
    std::string name;
    std::string key;
    std::filesystem::path source;
    std::filesystem::path root;       // the resources folder the cache sits beside
    std::string settings;             // what besides the file's bytes names its cache file
    std::filesystem::path cache;      // set by FirstLook, from the file's bytes
    texture::Usage usage = texture::Usage::Color;
    bool flipY = false;
};

using LoadsPtr = std::shared_ptr<TextureCache::Loads>;

TextureLevel LevelOf(const MeshLoad& job, const texture::AtexHeader& header, int level,
                     std::vector<std::uint8_t> bytes) {
    TextureLevel out;
    out.name = job.name;
    out.key = job.key;
    out.width = header.width;
    out.height = header.height;
    out.levels = header.levels;
    out.format = header.formats[0];
    out.level = level;
    out.bytes = std::move(bytes);
    return out;
}

// Reads batches[next] of the cache file, then queues the batch after it.
void ReadBatches(texture::TexturePool& pool, const LoadsPtr& loads, const MeshLoad& job,
                 const texture::AtexHeader& header, std::vector<std::vector<int>> batches, std::size_t next);

void Bake(texture::TexturePool& pool, const LoadsPtr& loads, const MeshLoad& job) {
    if (loads->stopped()) return;
    texture::BakeResult result = texture::bake_texture(job.source, job.usage, job.flipY, job.cache);
    if (!result.baked) {
        std::lock_guard<std::mutex> lock(loads->mutex);
        loads->failures.push_back({job.name, job.key, result.warning});
        return;
    }
    // Baked whole, in memory: every level goes up at once, smallest first;
    // pump spreads the uploads over frames.
    texture::AtexHeader header;
    header.width = result.baked->width;
    header.height = result.baked->height;
    header.levels = result.baked->level_count();
    header.formats = result.baked->formats;
    for (int level = header.levels - 1; level >= 0; --level) {
        loads->post(LevelOf(job, header, level, std::move(result.baked->planes[0][std::size_t(level)])));
    }
    (void)pool;
}

// A texture with no cache file, at the small-levels priority: decodes it,
// builds its mips, and posts the levels up to 64 a side at once, so it shows
// (blurry) without waiting behind every terrain bake. Compressing the larger
// levels, and writing the cache file, follow at the bake priority.
void Preview(texture::TexturePool& pool, const LoadsPtr& loads, const MeshLoad& job) {
    std::string why;
    std::optional<texture::DecodedImage> image = texture::decode_image_file(job.source, why);
    if (!image) {
        std::lock_guard<std::mutex> lock(loads->mutex);
        loads->failures.push_back({job.name, job.key, job.source.u8string() + ": " + why});
        return;
    }
    if (job.flipY) {
        FlipRows(image->rgba, std::size_t(image->width) * 4, image->height);
    }
    bool hasAlpha = false;
    for (std::size_t i = 3; i < image->rgba.size() && !hasAlpha; i += 4) hasAlpha = image->rgba[i] != 255;
    auto baked = std::make_shared<texture::BakedTexture>();
    baked->width = image->width;
    baked->height = image->height;
    baked->formats = {texture::format_for(job.usage, hasAlpha)};
    auto mips = std::make_shared<std::vector<std::vector<std::uint8_t>>>(
        texture::build_mips(job.usage, std::move(image->rgba), image->width, image->height));
    baked->planes.assign(1, std::vector<std::vector<std::uint8_t>>(mips->size()));
    texture::AtexHeader header;
    header.width = baked->width;
    header.height = baked->height;
    header.levels = static_cast<int>(mips->size());
    header.formats = baked->formats;
    const auto encode = [baked, mips](int level) {
        const int w = std::max(1, baked->width >> level), h = std::max(1, baked->height >> level);
        baked->planes[0][std::size_t(level)] =
            texture::encode_level(baked->formats[0], (*mips)[std::size_t(level)].data(), w, h);
    };
    const std::vector<std::vector<int>> batches = texture::streaming_batches(header.width, header.height, header.levels);
    const bool small = !batches.empty() &&
                       std::max(header.width >> batches[0].back(), header.height >> batches[0].back()) <= 64;
    if (small) {
        for (int level : batches[0]) {
            encode(level);
            loads->post(LevelOf(job, header, level, baked->planes[0][std::size_t(level)]));
        }
    }
    pool.submit(texture::JobPriority::MeshBake, [loads, job, baked, header, batches, small, encode] {
        if (loads->stopped()) return;
        for (std::size_t b = small ? 1 : 0; b < batches.size(); ++b) {
            for (int level : batches[b]) {
                encode(level);
                loads->post(LevelOf(job, header, level, baked->planes[0][std::size_t(level)]));
            }
        }
        std::string error;
        texture::write_atex(job.cache, *baked, error);   // next time reads it; a failure costs only that
    });
}

void ReadBatches(texture::TexturePool& pool, const LoadsPtr& loads, const MeshLoad& job,
                 const texture::AtexHeader& header, std::vector<std::vector<int>> batches, std::size_t next) {
    if (loads->stopped() || next >= batches.size()) return;
    for (int level : batches[next]) {
        auto data = texture::read_atex_level(job.cache, header, level);
        if (!data) {
            // The file changed under this load: bake it again.
            pool.submit(texture::JobPriority::MeshBake, [&pool, loads, job] { Bake(pool, loads, job); });
            return;
        }
        loads->post(LevelOf(job, header, level, std::move((*data)[0])));
    }
    if (next + 1 < batches.size()) {
        pool.submit(texture::JobPriority::MeshLargeLevels, [&pool, loads, job, header, batches, next] {
            ReadBatches(pool, loads, job, header, batches, next + 1);
        });
    }
}

// The first step: the cache file's small levels, or, with none, a preview
// (its small levels decoded now, the rest baked later).
void FirstLook(texture::TexturePool& pool, const LoadsPtr& loads, MeshLoad job) {
    if (loads->stopped()) return;
    // Named by the file's bytes, read here off the GL thread.
    job.cache = texture::cache_path(job.root, texture::content_key({job.source}, job.settings));
    const std::optional<texture::AtexHeader> header = texture::read_atex_header(job.cache);
    if (header && header->formats.size() == 1) {
        ReadBatches(pool, loads, job, *header, texture::streaming_batches(header->width, header->height, header->levels),
                    0);
        return;
    }
    if (header || std::filesystem::exists(job.cache)) {
        std::error_code error;
        std::filesystem::remove(job.cache, error);   // stale or broken: baked again
    }
    Preview(pool, loads, job);
}

}  // namespace

TextureCache::TextureCache(Report report) : report_(std::move(report)), loads_(std::make_shared<Loads>()) {}

TextureCache::~TextureCache() {
    // The context may be gone, and its textures with it.
    std::lock_guard<std::mutex> lock(loads_->mutex);
    loads_->stopping = true;
}

void TextureCache::setRoot(const std::filesystem::path& root) {
    if (root == root_) {
        return;
    }
    clear();
    root_ = root;
}

void TextureCache::clear() {
    for (auto* entries : {&environments_[0], &environments_[1]}) {
        for (auto& [path, entry] : *entries) {
            if (entry.texture != 0) {
                glDeleteTextures(1, &entry.texture);
            }
        }
        entries->clear();
    }
    for (auto& [name, streamed] : streamed_) {
        for (unsigned* texture : {&streamed.texture, &streamed.old}) {
            if (*texture != 0) glDeleteTextures(1, texture);
        }
    }
    streamed_.clear();
    if (grey_ != 0) {
        glDeleteTextures(1, &grey_);
        grey_ = 0;
    }
    // Loads still running post into the old Loads, which nothing reads now.
    {
        std::lock_guard<std::mutex> lock(loads_->mutex);
        loads_->stopping = true;
    }
    loads_ = std::make_shared<Loads>();
}

TextureCache::Entry& TextureCache::find(std::unordered_map<std::string, Entry>& entries, const std::string& path,
                                        bool linear, bool flipY) {
    Entry& entry = entries[path];
    const auto now = std::chrono::steady_clock::now();
    if (!entry.tried || now - entry.checked >= kRecheck) {
        entry.checked = now;
        load(path, entry, linear, flipY);
    }
    return entry;
}

unsigned TextureCache::get(const std::string& path, texture::Usage usage, bool flipY, bool alwaysLoaded) {
    if (root_.empty() || path.empty()) {
        return 0;
    }
    const std::string name = path + '|' + std::to_string(int(usage)) + (flipY ? "|1" : "|0");
    Streamed& streamed = streamed_[name];
    if (streamed.path.empty()) {
        streamed.path = path;
        streamed.usage = usage;
        streamed.flipY = flipY;
    }
    streamed.alwaysLoaded = alwaysLoaded;
    const auto now = std::chrono::steady_clock::now();
    if (!streamed.tried || now - streamed.checked >= kRecheck) {
        streamed.checked = now;
        check(streamed);
    }
    if (streamed.visible) return streamed.texture;
    if (streamed.old != 0) return streamed.old;
    // Still loading: a Color texture draws mid grey meanwhile, as terrain
    // does, so a surface reads as loading rather than as a white material.
    // Other usages, and a file that is missing or failed, draw the
    // renderer's own default (0).
    if (usage != texture::Usage::Color || streamed.key.empty() || streamed.failed) return 0;
    if (grey_ == 0) {
        const std::uint8_t pixel[4] = {128, 128, 128, 255};
        glGenTextures(1, &grey_);
        glBindTexture(GL_TEXTURE_2D, grey_);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return grey_;
}

void TextureCache::check(Streamed& streamed) {
    // Texture Paths use '/', which every platform's path splits on.
    const std::filesystem::path file = root_ / std::filesystem::u8path(streamed.path);
    std::error_code error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(file, error);
    if (error) {
        const bool was_there = !streamed.tried || streamed.texture != 0 || streamed.old != 0 ||
                               streamed.stamp != std::filesystem::file_time_type{};
        streamed.tried = true;
        streamed.stamp = {};
        streamed.key.clear();
        for (unsigned* texture : {&streamed.texture, &streamed.old}) {
            if (*texture != 0) glDeleteTextures(1, texture);
        }
        streamed.visible = false;
        streamed.queue.clear();
        if (was_there && report_) {
            report_("Texture " + streamed.path + " was not found in the resources folder");
        }
        return;
    }
    if (streamed.tried && stamp == streamed.stamp) {
        return;
    }
    streamed.tried = true;
    streamed.stamp = stamp;
    start(streamed);
}

void TextureCache::start(Streamed& streamed) {
    const std::filesystem::path file = root_ / std::filesystem::u8path(streamed.path);
    const std::string settings = "mesh|v1|usage=" + std::to_string(int(streamed.usage)) +
                                 "|flip=" + (streamed.flipY ? "1" : "0") +
                                 "|s3tc=" + (texture::s3tc_available() ? "1" : "0");
    streamed.key = texture::cache_key({file}, {streamed.stamp}, settings);
    // What shows now keeps showing until the new version does.
    if (streamed.visible) {
        if (streamed.old != 0) glDeleteTextures(1, &streamed.old);
        streamed.old = streamed.texture;
    } else if (streamed.texture != 0) {
        glDeleteTextures(1, &streamed.texture);
    }
    streamed.texture = 0;
    streamed.visible = false;
    streamed.failed = false;
    streamed.queue.clear();

    MeshLoad job;
    job.name = streamed.path + '|' + std::to_string(int(streamed.usage)) + (streamed.flipY ? "|1" : "|0");
    job.key = streamed.key;
    job.source = file;
    job.root = root_;
    job.settings = settings;
    job.usage = streamed.usage;
    job.flipY = streamed.flipY;
    LoadsPtr loads = loads_;
    texture::TexturePool* pool = &texture::TexturePool::shared();
    pool->submit(texture::JobPriority::MeshSmallLevels, [pool, loads, job] { FirstLook(*pool, loads, job); });
}

void TextureCache::show(Streamed& streamed) {
    if (streamed.visible || streamed.texture == 0) return;
    const bool ready = streamed.alwaysLoaded ? streamed.validFirst == 0 : streamed.validFirst < streamed.levels;
    if (!ready) return;
    streamed.visible = true;
    if (streamed.old != 0) glDeleteTextures(1, &streamed.old);
}

void TextureCache::pump(std::size_t budgetBytes) {
    std::vector<TextureLevel> landed;
    std::vector<std::array<std::string, 3>> failures;
    {
        std::lock_guard<std::mutex> lock(loads_->mutex);
        landed.swap(loads_->levels);
        failures.swap(loads_->failures);
    }
    for (const auto& [name, key, why] : failures) {
        const auto found = streamed_.find(name);
        if (found == streamed_.end() || found->second.key != key) continue;
        Streamed& streamed = found->second;
        streamed.failed = true;
        // A texture that stops reading stops drawing, rather than showing the old version.
        for (unsigned* texture : {&streamed.texture, &streamed.old}) {
            if (*texture != 0) glDeleteTextures(1, texture);
        }
        streamed.visible = false;
        if (report_) {
            report_(IsOpenExr(streamed.path)
                        ? "Texture " + streamed.path + " is OpenEXR, which the renderer cannot read; use a .hdr file"
                        : "Texture " + streamed.path + " is not an image it can draw: " + why);
        }
    }

    GLint alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    for (TextureLevel& level : landed) {
        const auto found = streamed_.find(level.name);
        if (found == streamed_.end() || found->second.key != level.key) continue;   // superseded: dropped
        Streamed& streamed = found->second;
        if (streamed.texture == 0) {
            // Every level allocated now, so uploads never reallocate.
            streamed.width = level.width;
            streamed.height = level.height;
            streamed.levels = level.levels;
            streamed.format = level.format;
            streamed.validFirst = level.levels;
            glGenTextures(1, &streamed.texture);
            glBindTexture(GL_TEXTURE_2D, streamed.texture);
            const GLenum compressed = CompressedFormat(level.format);
            for (int l = 0; l < level.levels; ++l) {
                const int w = LevelSide(level.width, l), h = LevelSide(level.height, l);
                if (compressed != 0) {
                    glCompressedTexImage2D(GL_TEXTURE_2D, l, compressed, w, h, 0,
                                           static_cast<GLsizei>(texture::level_bytes(level.format, w, h)), nullptr);
                } else {
                    glTexImage2D(GL_TEXTURE_2D, l, static_cast<GLint>(GL_RGBA8), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 nullptr);
                }
            }
            glTexParameteri(GL_TEXTURE_2D, RT_GL_TEXTURE_BASE_LEVEL, level.levels - 1);
            glTexParameteri(GL_TEXTURE_2D, RT_GL_TEXTURE_MAX_LEVEL, level.levels - 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_REPEAT));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_REPEAT));
        }
        streamed.queue.emplace_back(level.level, std::move(level.bytes));
    }

    std::size_t spent = 0;
    for (auto& [name, streamed] : streamed_) {
        bool uploaded = false;
        while (!streamed.queue.empty() && spent < budgetBytes) {
            auto [level, bytes] = std::move(streamed.queue.front());
            streamed.queue.pop_front();
            const int w = LevelSide(streamed.width, level), h = LevelSide(streamed.height, level);
            if (bytes.size() != texture::level_bytes(streamed.format, w, h)) continue;
            glBindTexture(GL_TEXTURE_2D, streamed.texture);
            const GLenum compressed = CompressedFormat(streamed.format);
            if (compressed != 0) {
                glCompressedTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, compressed,
                                          static_cast<GLsizei>(bytes.size()), bytes.data());
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
            }
            spent += bytes.size();
            if (level == streamed.validFirst - 1) streamed.validFirst = level;
            uploaded = true;
        }
        if (uploaded) {
            glBindTexture(GL_TEXTURE_2D, streamed.texture);
            glTexParameteri(GL_TEXTURE_2D, RT_GL_TEXTURE_BASE_LEVEL, std::min(streamed.validFirst, streamed.levels - 1));
            show(streamed);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
}

bool TextureCache::loading() const {
    for (const auto& [name, streamed] : streamed_) {
        if (!streamed.key.empty() && !streamed.failed && (streamed.texture == 0 || streamed.validFirst > 0)) {
            return true;
        }
    }
    return false;
}

EnvironmentTexture TextureCache::getEnvironment(const std::string& path, bool flipY) {
    if (root_.empty() || path.empty()) {
        return {};
    }
    const Entry& entry = find(environments_[flipY ? 1 : 0], path, true, flipY);
    return entry.texture != 0 ? EnvironmentTexture{entry.texture, entry.revision} : EnvironmentTexture{};
}

void TextureCache::fail(Entry& entry, const std::string& message) {
    // A texture that stops reading stops drawing, rather than showing the old version.
    if (entry.texture != 0) {
        glDeleteTextures(1, &entry.texture);
        entry.texture = 0;
    }
    if (report_) {
        report_(message);
    }
}

void TextureCache::load(const std::string& path, Entry& entry, bool linear, bool flipY) {
    // Texture Paths use '/', which every platform's path splits on.
    const std::filesystem::path file = root_ / std::filesystem::u8path(path);
    std::error_code error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(file, error);
    if (error) {
        const bool was_there = !entry.tried || entry.texture != 0 || entry.stamp != std::filesystem::file_time_type{};
        entry.tried = true;
        entry.stamp = {};
        if (was_there) {
            fail(entry, "Texture " + path + " was not found in the resources folder");
        }
        return;
    }
    if (entry.tried && stamp == entry.stamp) {
        return;
    }
    entry.tried = true;
    entry.stamp = stamp;

    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error || size > kMaxFileSize) {
        fail(entry, "Texture " + path + " could not be read, or is larger than 256 MiB");
        return;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        fail(entry, "Texture " + path + " could not be read");
        return;
    }
    TexturePixels pixels;
    LinearPixels linearPixels;
    std::string why;
    if (!(linear ? DecodeLinearTexture(bytes.data(), bytes.size(), linearPixels, why)
                 : DecodeTexture(bytes.data(), bytes.size(), pixels, why))) {
        if (IsOpenExr(path)) {
            fail(entry, "Texture " + path + " is OpenEXR, which the renderer cannot read; use a .hdr file");
        } else {
            fail(entry, "Texture " + path + " is not an image it can draw: " + why);
        }
        return;
    }
    if (flipY && linear) {
        FlipRows(linearPixels.rgb, static_cast<std::size_t>(linearPixels.width) * 3, linearPixels.height);
    } else if (flipY) {
        FlipRows(pixels.rgba, static_cast<std::size_t>(pixels.width) * 4, pixels.height);
    }

    if (entry.texture == 0) {
        glGenTextures(1, &entry.texture);
    }
    static std::uint64_t nextRevision = 0;
    entry.revision = ++nextRevision;
    GLint alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, entry.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (linear) {
        // RGBA16F, not RGB16F, which a driver need not render into, nor make mipmaps of.
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(RT_GL_RGBA16F), linearPixels.width, linearPixels.height, 0,
                     RT_GL_RGB, GL_FLOAT, linearPixels.rgb.data());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), pixels.width, pixels.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels.rgba.data());
    }
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_REPEAT));
    // A sky wraps around, but not over the poles.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    static_cast<GLint>(linear ? RT_GL_CLAMP_TO_EDGE : GL_REPEAT));
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace runner
