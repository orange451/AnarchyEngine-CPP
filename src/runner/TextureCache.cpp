#include "TextureCache.hpp"

#include "gl.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
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

TextureCache::TextureCache(Report report) : report_(std::move(report)) {}

TextureCache::~TextureCache() {
    // The context may be gone, and its textures with it.
    entries_.clear();
}

void TextureCache::setRoot(const std::filesystem::path& root) {
    if (root == root_) {
        return;
    }
    clear();
    root_ = root;
}

void TextureCache::clear() {
    for (auto* entries : {&entries_, &environments_}) {
        for (auto& [path, entry] : *entries) {
            if (entry.texture != 0) {
                glDeleteTextures(1, &entry.texture);
            }
        }
        entries->clear();
    }
}

TextureCache::Entry& TextureCache::find(std::unordered_map<std::string, Entry>& entries, const std::string& path,
                                        bool linear) {
    Entry& entry = entries[path];
    const auto now = std::chrono::steady_clock::now();
    if (!entry.tried || now - entry.checked >= kRecheck) {
        entry.checked = now;
        load(path, entry, linear);
    }
    return entry;
}

unsigned TextureCache::get(const std::string& path) {
    if (root_.empty() || path.empty()) {
        return 0;
    }
    return find(entries_, path, false).texture;
}

EnvironmentTexture TextureCache::getEnvironment(const std::string& path) {
    if (root_.empty() || path.empty()) {
        return {};
    }
    const Entry& entry = find(environments_, path, true);
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

void TextureCache::load(const std::string& path, Entry& entry, bool linear) {
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
