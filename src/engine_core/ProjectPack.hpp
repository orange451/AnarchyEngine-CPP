#pragma once

// The load pack: every file of a project's tree, with the size and write time
// it had, in one file under .cache. Opening a hundred thousand small files is
// most of a large project's load when the disk cache is cold; one file is not.
// It is only a cache. A load takes a file's bytes from it only when the file
// on disk still has the size and write time the pack recorded, and reads every
// other file from disk.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine_core::project_pack {

// Where a project's pack lives.
std::filesystem::path path_for(const std::filesystem::path& root);

// A pack as read. Each entry's bytes point into buffer.
struct Pack {
    struct Entry {
        std::uintmax_t size = 0;
        std::int64_t written = 0;
        std::string_view bytes;
    };
    std::string buffer;
    // By path relative to the project root, with '/'.
    std::unordered_map<std::string_view, Entry> files;
};

// The pack at file. Empty when there is none, or it is damaged or from
// another version.
std::unique_ptr<Pack> read(const std::filesystem::path& file);

// One file for write. bytes is shared, so a write on another thread reads it
// while the project goes on.
struct Source {
    std::string path;
    std::uintmax_t size = 0;
    std::int64_t written = 0;
    std::shared_ptr<const std::string> bytes;
};

// Replaces the pack at file: a temporary file, then a rename, so a reader
// never sees half of one. False when it could not.
bool write(const std::filesystem::path& file, const std::vector<Source>& sources);

// write on a thread of its own, which nothing waits for.
void write_in_background(std::filesystem::path file, std::vector<Source> sources);

}  // namespace engine_core::project_pack
