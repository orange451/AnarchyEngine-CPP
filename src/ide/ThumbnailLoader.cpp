#include "ThumbnailLoader.hpp"

#include "IdeResources.hpp"
#include "runner/TextureCache.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace ide {
namespace {

// How often a file already loaded is looked at again while it is asked for.
constexpr std::chrono::seconds kRecheck{1};
// Larger files are refused before they are read, as the Scene View's are.
constexpr std::uintmax_t kMaxFileSize = 256u * 1024u * 1024u;

}  // namespace

std::vector<std::uint8_t> shrink_pixels(const std::vector<std::uint8_t>& rgba, int& width, int& height, int max_size) {
    const int from_width = width;
    const int from_height = height;
    const double scale = std::min(1.0, static_cast<double>(max_size) / std::max(from_width, from_height));
    const int to_width = std::max(1, static_cast<int>(from_width * scale + 0.5));
    const int to_height = std::max(1, static_cast<int>(from_height * scale + 0.5));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(to_width) * static_cast<std::size_t>(to_height) * 4);
    for (int y = 0; y < to_height; ++y) {
        // The source rows this one covers, counted from the top.
        const int top = static_cast<int>(static_cast<long long>(y) * from_height / to_height);
        const int bottom = std::max(top + 1, static_cast<int>(static_cast<long long>(y + 1) * from_height / to_height));
        for (int x = 0; x < to_width; ++x) {
            const int left = static_cast<int>(static_cast<long long>(x) * from_width / to_width);
            const int right = std::max(left + 1, static_cast<int>(static_cast<long long>(x + 1) * from_width / to_width));
            // Color weighted by alpha, so clear pixels do not darken the edges of what shows.
            double color[3] = {0, 0, 0};
            double alpha = 0;
            for (int row = top; row < bottom; ++row) {
                // The source is bottom row first.
                const std::uint8_t* line =
                    rgba.data() + static_cast<std::size_t>(from_height - 1 - row) * static_cast<std::size_t>(from_width) * 4;
                for (int column = left; column < right; ++column) {
                    const std::uint8_t* pixel = line + static_cast<std::size_t>(column) * 4;
                    const double weight = pixel[3];
                    color[0] += pixel[0] * weight;
                    color[1] += pixel[1] * weight;
                    color[2] += pixel[2] * weight;
                    alpha += weight;
                }
            }
            const double count = static_cast<double>(bottom - top) * (right - left);
            std::uint8_t* to = out.data() + (static_cast<std::size_t>(y) * to_width + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                to[channel] = static_cast<std::uint8_t>(alpha > 0 ? color[channel] / alpha + 0.5 : 0);
            }
            to[3] = static_cast<std::uint8_t>(alpha / count + 0.5);
        }
    }
    width = to_width;
    height = to_height;
    return out;
}

ThumbnailLoader::ThumbnailLoader(int max_size, std::function<void()> ready)
    : max_size_(max_size), ready_(std::move(ready)), thread_([this] { run(); }) {}

ThumbnailLoader::~ThumbnailLoader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

std::shared_ptr<jadefx::Image> ThumbnailLoader::get(const std::filesystem::path& file) {
    if (file.empty()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[utf8_path(file)];
    entry.wanted = true;
    const auto now = std::chrono::steady_clock::now();
    if (!entry.queued && (!entry.loaded || now - entry.checked >= kRecheck)) {
        entry.queued = true;
        entry.checked = now;
        queue_.push_back(file);
        wake_.notify_one();
    }
    return entry.image;
}

void ThumbnailLoader::retain(const std::vector<std::filesystem::path>& in_use) {
    std::unordered_set<std::string> keep;
    for (const std::filesystem::path& file : in_use) {
        keep.insert(utf8_path(file));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (keep.count(it->first) == 0) {
            // A queued load of it finds no entry and is skipped.
            it = entries_.erase(it);
            continue;
        }
        Entry& entry = it->second;
        if (entry.queued && !entry.wanted) {
            // Its place in the queue is skipped; the next get queues it again.
            entry.queued = false;
        }
        entry.wanted = false;
        ++it;
    }
}

bool ThumbnailLoader::idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty() && !busy_;
}

void ThumbnailLoader::run() {
    for (;;) {
        std::filesystem::path file;
        std::filesystem::file_time_type stamp{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) {
                return;
            }
            file = std::move(queue_.front());
            queue_.pop_front();
            const auto found = entries_.find(utf8_path(file));
            // Forgotten, or dropped as no longer wanted, since it was queued.
            if (found == entries_.end() || !found->second.queued) {
                continue;
            }
            stamp = found->second.loaded ? found->second.stamp : std::filesystem::file_time_type::min();
            busy_ = true;
        }
        std::shared_ptr<jadefx::Image> image;
        const bool changed = load(file, stamp, image);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            const auto found = entries_.find(utf8_path(file));
            if (found == entries_.end()) {
                continue;
            }
            Entry& entry = found->second;
            entry.queued = false;
            entry.loaded = true;
            // Checked again a second after this load ends, not after it was asked for.
            entry.checked = std::chrono::steady_clock::now();
            if (!changed) {
                continue;
            }
            entry.stamp = stamp;
            entry.image = std::move(image);
        }
        if (ready_) {
            ready_();
        }
    }
}

bool ThumbnailLoader::load(const std::filesystem::path& file, std::filesystem::file_time_type& stamp,
                           std::shared_ptr<jadefx::Image>& image) const {
    std::error_code error;
    const std::filesystem::file_time_type now = std::filesystem::last_write_time(file, error);
    if (error) {
        // Missing: new only when it was there before.
        const bool was_there = stamp != std::filesystem::file_time_type{};
        stamp = {};
        image = nullptr;
        return was_there;
    }
    if (now == stamp) {
        return false;
    }
    stamp = now;
    image = nullptr;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error || size == 0 || size > kMaxFileSize) {
        return true;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return true;
    }
    runner::TexturePixels pixels;
    std::string reason;
    if (!runner::DecodeTexture(bytes.data(), bytes.size(), pixels, reason)) {
        return true;
    }
    bytes = {};
    int width = pixels.width;
    int height = pixels.height;
    std::vector<std::uint8_t> small = shrink_pixels(pixels.rgba, width, height, max_size_);
    image = jadefx::Image::fromRgba(width, height, std::move(small));
    return true;
}

}  // namespace ide
