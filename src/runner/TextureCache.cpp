#include "TextureCache.hpp"

#include "gl.hpp"

#include <algorithm>
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
    for (auto& [path, entry] : entries_) {
        if (entry.texture != 0) {
            glDeleteTextures(1, &entry.texture);
        }
    }
    entries_.clear();
}

unsigned TextureCache::get(const std::string& path) {
    if (root_.empty() || path.empty()) {
        return 0;
    }
    Entry& entry = entries_[path];
    const auto now = std::chrono::steady_clock::now();
    if (!entry.tried || now - entry.checked >= kRecheck) {
        entry.checked = now;
        load(path, entry);
    }
    return entry.texture;
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

void TextureCache::load(const std::string& path, Entry& entry) {
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
    std::string why;
    if (!DecodeTexture(bytes.data(), bytes.size(), pixels, why)) {
        fail(entry, "Texture " + path + " is not an image it can draw: " + why);
        return;
    }

    if (entry.texture == 0) {
        glGenTextures(1, &entry.texture);
    }
    GLint alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, entry.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), pixels.width, pixels.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_REPEAT));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_REPEAT));
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace runner
