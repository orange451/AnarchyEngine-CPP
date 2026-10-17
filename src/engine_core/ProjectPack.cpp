#include "ProjectPack.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>
#include <thread>
#include <utility>

namespace engine_core::project_pack {
namespace {

namespace fs = std::filesystem;

// "APAK", the version, then a marker that reads back wrong on a machine of the
// other byte order. Then the entry count and a hash of everything after the
// header. Each entry: path length, path, size, write time, then its bytes,
// whose length is the size.
constexpr char kMagic[4] = {'A', 'P', 'A', 'K'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kOrder = 0x01020304u;
constexpr std::size_t kHeaderSize = 4 + 4 + 4 + 8 + 8;

template <typename T>
void put(std::string& out, T value) {
    char raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.append(raw, sizeof(T));
}

template <typename T>
bool get(const std::string& in, std::size_t& at, T& value) {
    if (in.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

// Eight bytes a step, so 50 MB hash in milliseconds. Not for adversaries: it
// catches a pack damaged on disk.
std::uint64_t hash_of(const char* data, std::size_t size) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull ^ size;
    std::size_t at = 0;
    for (; at + 8 <= size; at += 8) {
        std::uint64_t word;
        std::memcpy(&word, data + at, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    for (; at < size; ++at) {
        hash = (hash ^ static_cast<unsigned char>(data[at])) * 0x100000001b3ull;
    }
    return hash;
}

}  // namespace

fs::path path_for(const fs::path& root) { return root / ".cache" / "place.pack"; }

std::unique_ptr<Pack> read(const fs::path& file) {
    auto pack = std::make_unique<Pack>();
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return nullptr;
    }
    const std::streamoff length = in.tellg();
    if (length < static_cast<std::streamoff>(kHeaderSize)) {
        return nullptr;
    }
    in.seekg(0);
    std::string& buffer = pack->buffer;
    buffer.resize(static_cast<std::size_t>(length));
    if (!in.read(buffer.data(), static_cast<std::streamsize>(length))) {
        return nullptr;
    }
    if (std::memcmp(buffer.data(), kMagic, 4) != 0) {
        return nullptr;
    }
    std::size_t at = 4;
    std::uint32_t version = 0;
    std::uint32_t order = 0;
    std::uint64_t count = 0;
    std::uint64_t hash = 0;
    if (!get(buffer, at, version) || !get(buffer, at, order) || !get(buffer, at, count) || !get(buffer, at, hash) ||
        version != kVersion || order != kOrder) {
        return nullptr;
    }
    if (hash_of(buffer.data() + kHeaderSize, buffer.size() - kHeaderSize) != hash) {
        return nullptr;
    }
    pack->files.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint32_t path_size = 0;
        if (!get(buffer, at, path_size) || buffer.size() - at < path_size) {
            return nullptr;
        }
        const std::string_view path(buffer.data() + at, path_size);
        at += path_size;
        Pack::Entry entry;
        if (!get(buffer, at, entry.size) || !get(buffer, at, entry.written) || buffer.size() - at < entry.size) {
            return nullptr;
        }
        entry.bytes = std::string_view(buffer.data() + at, static_cast<std::size_t>(entry.size));
        at += static_cast<std::size_t>(entry.size);
        pack->files.emplace(path, entry);
    }
    if (at != buffer.size()) {
        return nullptr;
    }
    return pack;
}

bool write(const fs::path& file, const std::vector<Source>& sources) {
    std::string body;
    std::size_t total = 0;
    for (const Source& source : sources) {
        total += 4 + source.path.size() + 16 + (source.bytes ? source.bytes->size() : 0);
    }
    body.reserve(total);
    std::uint64_t count = 0;
    for (const Source& source : sources) {
        // A file's bytes are its size, or the entry is not worth keeping.
        if (!source.bytes || source.bytes->size() != source.size) {
            continue;
        }
        put(body, static_cast<std::uint32_t>(source.path.size()));
        body.append(source.path);
        put(body, static_cast<std::uintmax_t>(source.size));
        put(body, source.written);
        body.append(*source.bytes);
        ++count;
    }
    std::string header(kMagic, 4);
    put(header, kVersion);
    put(header, kOrder);
    put(header, count);
    put(header, hash_of(body.data(), body.size()));

    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    // Two studios on one project each write a temporary file of their own.
    static std::atomic<unsigned> serial{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path temporary = file;
    temporary += "." + std::to_string(stamp) + "." + std::to_string(serial.fetch_add(1)) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.close();
        if (!out) {
            fs::remove(temporary, error);
            return false;
        }
    }
    fs::rename(temporary, file, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

void write_in_background(fs::path file, std::vector<Source> sources) {
    try {
        std::thread([file = std::move(file), sources = std::move(sources)] {
            try {
                write(file, sources);
            } catch (...) {
                // Out of memory or a filesystem error: no pack this time.
            }
        }).detach();
    } catch (const std::system_error&) {
        // No thread: the next load reads the tree from disk again, and tries again.
    }
}

}  // namespace engine_core::project_pack
