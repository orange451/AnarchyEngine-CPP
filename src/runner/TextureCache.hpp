#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace runner {

// An image decoded to RGBA, 8 bits a channel, bottom row first, as OpenGL
// takes it: AMESH's v 0 is the bottom of the image.
struct TexturePixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// Decodes PNG, JPEG, TGA, BMP, GIF, HDR, PSD, or PNM bytes, the formats
// stb_image reads. False, with error set, when it cannot.
bool DecodeTexture(const std::uint8_t* bytes, std::size_t size, TexturePixels& out, std::string& error);

// A Scene View's uploaded textures, by Texture Path, as MeshCache keeps its
// meshes: a path loads the first time a frame draws it, from the project's
// resources folder, and its file is looked at again at most once a second and
// reloaded when it changed. A file that is missing or does not decode has no
// texture; report hears why, once per version of the file. Every call but the
// destructor needs the GL context the textures were uploaded in.
class TextureCache {
public:
    using Report = std::function<void(const std::string& message)>;

    explicit TextureCache(Report report = {});
    // Needs no GL: a cache still holding textures is one whose context is gone.
    ~TextureCache();

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // The resources folder paths are under. Empty loads nothing. Another root clears the cache.
    void setRoot(const std::filesystem::path& root);
    // The GL texture for path, relative to the root with '/' between names, or
    // 0 when there is none to draw. Mipmapped, repeating, and RGBA8.
    unsigned get(const std::string& path);
    // Deletes every texture.
    void clear();

private:
    struct Entry {
        unsigned texture = 0;
        // The file's time when it was last read; unset before the first try.
        std::filesystem::file_time_type stamp{};
        bool tried = false;
        std::chrono::steady_clock::time_point checked{};
    };

    void load(const std::string& path, Entry& entry);
    void fail(Entry& entry, const std::string& message);

    Report report_;
    std::filesystem::path root_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace runner
