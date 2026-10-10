#pragma once

#include "texture/TextureBake.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
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
// kept apart from the same path's upright upload. A file that is missing or
// does not decode has no texture; report hears why, once per version of the
// file. Every call but the destructor needs the GL context the textures were
// uploaded in.
//
// get's textures stream: each file is baked once into the project's texture
// cache (compressed, with its mips) on the shared TexturePool, and its
// levels upload smallest first, a frame's worth at a time in pump, so a
// texture appears blurry almost at once and sharpens. A changed file keeps
// drawing its old version until the new one appears. Skies
// (getEnvironment) load whole, as before.
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
    // The GL texture for path, relative to the root with '/' between names,
    // drawn as usage, or 0 while it has nothing to show (the renderer then
    // draws its own white, or flat normal, in its place). Mipmapped and
    // repeating. flipY puts the image's top row at v 0. alwaysLoaded (a
    // Texture whose Streaming is AlwaysLoaded) shows it only once every
    // level is uploaded, never blurry.
    unsigned get(const std::string& path, engine_core::texture::Usage usage, bool flipY = false,
                 bool alwaysLoaded = false);
    // Uploads what finished loading, up to budgetBytes this call, smallest
    // levels first. Once a frame, before drawing.
    void pump(std::size_t budgetBytes = std::size_t(16) << 20);
    // Whether any texture get was asked for still has levels to load (and
    // has not failed): a caller drawing once, not every frame, draws again
    // later.
    bool loading() const;
    // The same file decoded by DecodeLinearTexture, for a Skybox: RGBA16F,
    // mipmapped, repeating across and clamped at the poles. Kept apart from
    // get's upload of the same path.
    EnvironmentTexture getEnvironment(const std::string& path, bool flipY = false);
    // What loading jobs share with this cache; jobs keep it alive. Internal:
    // public only so the job functions in TextureCache.cpp can name it.
    struct Loads;
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

    // One texture get streams: the texture filling now, the one it replaces
    // (drawn until this one shows), and which load (cache key) it is.
    struct Streamed {
        std::string path;
        engine_core::texture::Usage usage = engine_core::texture::Usage::Color;
        bool flipY = false;
        bool alwaysLoaded = false;
        std::filesystem::file_time_type stamp{};
        bool tried = false;
        std::chrono::steady_clock::time_point checked{};
        std::string key;
        bool failed = false;
        unsigned texture = 0;
        unsigned old = 0;
        int width = 0;
        int height = 0;
        int levels = 0;
        engine_core::texture::PixelFormat format = engine_core::texture::PixelFormat::RGBA8;
        // The finest level from which every coarser level is uploaded (levels: none).
        int validFirst = 0;
        bool visible = false;
        // Levels waiting to upload, in order: {level, bytes}.
        std::deque<std::pair<int, std::vector<std::uint8_t>>> queue;
    };


    Entry& find(std::unordered_map<std::string, Entry>& entries, const std::string& path, bool linear, bool flipY);
    void load(const std::string& path, Entry& entry, bool linear, bool flipY);
    void fail(Entry& entry, const std::string& message);
    void check(Streamed& streamed);
    void start(Streamed& streamed);
    void show(Streamed& streamed);

    Report report_;
    std::filesystem::path root_;
    // get's textures, by path, usage, and flip.
    std::unordered_map<std::string, Streamed> streamed_;
    std::shared_ptr<Loads> loads_;
    // Each by flipY: [0] upright, [1] flipped.
    std::unordered_map<std::string, Entry> environments_[2];
};

}  // namespace runner
