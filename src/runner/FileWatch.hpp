#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

namespace runner {

// Watches asset files for changes on a thread of its own, so frame code
// never waits on the disk to learn whether a file it drew from changed.
// Every watched file is looked at about once a second.
class FileWatch {
public:
    // Raised (true) once the file changes; whoever watches lowers it.
    using Flag = std::shared_ptr<std::atomic<bool>>;

    // Watches file from how it is now: present with stamp, or missing. The
    // flag goes up when it is written, made, or deleted after that. Dropping
    // every copy of the flag stops the watch.
    static Flag watch(const std::filesystem::path& file, bool present, std::filesystem::file_time_type stamp);

    // Whether flag is up, lowering it. Cheap enough to ask on every draw.
    static bool take(const Flag& flag) {
        return flag != nullptr && flag->load(std::memory_order_relaxed) &&
               flag->exchange(false, std::memory_order_relaxed);
    }
};

}  // namespace runner
