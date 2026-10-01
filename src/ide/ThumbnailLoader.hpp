#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace jadefx {
class Image;
}

namespace ide {

// Shrinks width x height RGBA pixels, bottom row first as runner::DecodeTexture
// gives them, to fit max_size on the longer side, averaging the pixels each
// one covers, and turns them top row first. Pixels that fit already are only turned.
// Sets width and height to the result's.
std::vector<std::uint8_t> shrink_pixels(const std::vector<std::uint8_t>& rgba, int& width, int& height, int max_size);

// Image files as small images, read, decoded, and shrunk on a thread of its
// own, so a folder of large textures never holds up a frame. A file streams
// in: get answers null until its thumbnail is ready, and ready runs on the
// loader's thread each time one is, or one changes. A thumbnail stays until
// retain lets it go, so a folder shown again needs no load. A file is looked at
// again at most once a second while it is asked for, and reloads when it changed.
class ThumbnailLoader {
public:
    // max_size: the longer side of a thumbnail, in pixels. ready must be safe
    // to call from another thread, as a ChangeFlag's setter is.
    ThumbnailLoader(int max_size, std::function<void()> ready);
    // Stops the thread, dropping what it has not loaded.
    ~ThumbnailLoader();

    ThumbnailLoader(const ThumbnailLoader&) = delete;
    ThumbnailLoader& operator=(const ThumbnailLoader&) = delete;

    // file's thumbnail as last loaded, or null while it loads or when it is
    // missing or does not decode. Queues it the first time it is asked for.
    std::shared_ptr<jadefx::Image> get(const std::filesystem::path& file);
    // Forgets every file not in in_use, as when the last Texture naming it goes,
    // and keeps the rest's thumbnails however long they go unshown. Drops a
    // load not yet started for a file not asked for since the last call, as
    // when the folder shown changes before its thumbnails load.
    void retain(const std::vector<std::filesystem::path>& in_use);
    // Whether no file waits or loads. For tests.
    bool idle();

private:
    struct Entry {
        std::shared_ptr<jadefx::Image> image;
        // The file's time when it was last read; unset before the first read or when it is missing.
        std::filesystem::file_time_type stamp{};
        bool loaded = false;
        bool queued = false;
        // Asked for since the last retain.
        bool wanted = true;
        std::chrono::steady_clock::time_point checked{};
    };

    void run();
    // Reads file when it changed since stamp. False when there is nothing new to keep.
    bool load(const std::filesystem::path& file, std::filesystem::file_time_type& stamp,
              std::shared_ptr<jadefx::Image>& image) const;

    const int max_size_;
    const std::function<void()> ready_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::filesystem::path> queue_;
    bool busy_ = false;
    bool stop_ = false;
    // Last, so it starts after everything it reads.
    std::thread thread_;
};

}  // namespace ide
