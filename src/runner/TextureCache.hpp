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

// An image decoded to linear light, RGB floats, bottom row first.
struct LinearPixels {
    int width = 0;
    int height = 0;
    std::vector<float> rgb;
};

// The widest a sky is kept. Each halving averages 2 by 2 pixels.
constexpr int kMaxEnvironmentWidth = 4096;

// Decodes what DecodeTexture does, for a Skybox. A Radiance HDR file keeps the
// light it holds, brighter than white; any other image is taken as sRGB and
// made linear. Each value is kept between 0 and 65000, which half float
// holds. Halved until it is no wider than maxWidth. False, with error set,
// when it cannot.
bool DecodeLinearTexture(const std::uint8_t* bytes, std::size_t size, LinearPixels& out, std::string& error,
                         int maxWidth = kMaxEnvironmentWidth);

// An uploaded sky image and which upload it is: revision changes each time the
// file is read again, and is never reused, so a renderer rebuilds what it made
// from the image only when it changes. 0 for no image.
struct EnvironmentTexture {
    unsigned texture = 0;
    std::uint64_t revision = 0;
};

// A Scene View's uploaded textures, by Texture Path, as MeshCache keeps its
// meshes: a path loads the first time a frame draws it, from the project's
// resources folder, and its file is looked at again at most once a second and
// reloaded when it changed. A Texture's FlipY uploads the image upside down,
// kept apart from the same path's upright upload. A file that is missing or does not decode has no
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
    // 0 when there is none to draw. Mipmapped, repeating, and RGBA8. flipY
    // puts the image's top row at v 0.
    unsigned get(const std::string& path, bool flipY = false);
    // The same file decoded by DecodeLinearTexture, for a Skybox: RGBA16F,
    // mipmapped, repeating across and clamped at the poles. Kept apart from
    // get's upload of the same path.
    EnvironmentTexture getEnvironment(const std::string& path, bool flipY = false);
    // Deletes every texture.
    void clear();

private:
    struct Entry {
        unsigned texture = 0;
        // The file's time when it was last read; unset before the first try.
        std::filesystem::file_time_type stamp{};
        bool tried = false;
        std::chrono::steady_clock::time_point checked{};
        std::uint64_t revision = 0;
    };

    Entry& find(std::unordered_map<std::string, Entry>& entries, const std::string& path, bool linear, bool flipY);
    void load(const std::string& path, Entry& entry, bool linear, bool flipY);
    void fail(Entry& entry, const std::string& message);

    Report report_;
    std::filesystem::path root_;
    // Each by flipY: [0] upright, [1] flipped.
    std::unordered_map<std::string, Entry> entries_[2];
    std::unordered_map<std::string, Entry> environments_[2];
};

}  // namespace runner
