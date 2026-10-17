#include "FileWatch.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace runner {

namespace {

// How often each watched file is looked at.
constexpr std::chrono::milliseconds kSweep{1000};

class Watcher {
public:
    ~Watcher() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    FileWatch::Flag add(const std::filesystem::path& file, bool present, std::filesystem::file_time_type stamp) {
        auto flag = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(mutex_);
        added_.push_back(Item{file, present, stamp, flag});
        if (!thread_.joinable()) {
            thread_ = std::thread([this] { run(); });
        }
        return flag;
    }

private:
    struct Item {
        std::filesystem::path file;
        bool present = false;
        std::filesystem::file_time_type stamp{};
        // Weak: a watch nobody holds the flag of is dropped.
        std::weak_ptr<std::atomic<bool>> flag;
    };

    void run() {
        std::vector<Item> items;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (stopping_) {
                    return;
                }
                for (Item& item : added_) {
                    items.push_back(std::move(item));
                }
                added_.clear();
            }
            const auto began = std::chrono::steady_clock::now();
            std::size_t kept = 0;
            for (std::size_t i = 0; i < items.size(); ++i) {
                Item& item = items[i];
                const std::shared_ptr<std::atomic<bool>> flag = item.flag.lock();
                if (!flag) {
                    continue;
                }
                std::error_code error;
                const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(item.file, error);
                const bool present = !error;
                if (present != item.present || (present && stamp != item.stamp)) {
                    item.present = present;
                    item.stamp = present ? stamp : std::filesystem::file_time_type{};
                    flag->store(true, std::memory_order_relaxed);
                }
                if (kept != i) {
                    items[kept] = std::move(item);
                }
                ++kept;
            }
            items.resize(kept);
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_until(lock, began + kSweep, [this] { return stopping_; });
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Item> added_;
    bool stopping_ = false;
    std::thread thread_;
};

Watcher& TheWatcher() {
    static Watcher watcher;
    return watcher;
}

}  // namespace

FileWatch::Flag FileWatch::watch(const std::filesystem::path& file, bool present,
                                 std::filesystem::file_time_type stamp) {
    return TheWatcher().add(file, present, stamp);
}

}  // namespace runner
