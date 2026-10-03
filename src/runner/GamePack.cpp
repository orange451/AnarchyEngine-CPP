#include "GamePack.hpp"

#include "FileBytes.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>

namespace runner {
namespace {

namespace fs = std::filesystem;
using engine_core::utf8_path;

constexpr char kArchiveMagic[4] = {'A', 'E', 'P', 'K'};
constexpr std::uint32_t kVersion = 1;
constexpr char kTrailerMagic[8] = {'A', 'E', 'G', 'A', 'M', 'E', '0', '1'};
constexpr std::size_t kTrailerSize = 8 + 8 + sizeof(kTrailerMagic);
// Marks a folder extracted whole. Written last, and named so no packed file can be it.
constexpr const char* kCompleteMark = ".complete";
// More than any path a file system takes, so a damaged length is caught before it allocates.
constexpr std::uint32_t kMaxPathBytes = 4096;
constexpr std::size_t kChunk = 1 << 20;

// FNV-1a, 64-bit, over every archive byte: the extracted folder's name.
class Hash {
public:
    void add(const char* bytes, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            value_ ^= static_cast<unsigned char>(bytes[i]);
            value_ *= 1099511628211ull;
        }
    }
    std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_ = 14695981039346656037ull;
};

template <std::size_t N>
std::array<char, N> LittleEndian(std::uint64_t value) {
    std::array<char, N> bytes{};
    for (std::size_t i = 0; i < N; ++i) {
        bytes[i] = static_cast<char>((value >> (8 * i)) & 0xffu);
    }
    return bytes;
}

std::uint64_t ReadLittleEndian(const char* bytes, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
    }
    return value;
}

// Writes to out and adds to the hash.
class ArchiveWriter {
public:
    explicit ArchiveWriter(std::ofstream& out) : out_(out) {}

    void write(const char* bytes, std::size_t size) {
        out_.write(bytes, static_cast<std::streamsize>(size));
        hash_.add(bytes, size);
    }
    template <std::size_t N>
    void number(std::uint64_t value) {
        const std::array<char, N> bytes = LittleEndian<N>(value);
        write(bytes.data(), N);
    }
    std::uint64_t hash() const { return hash_.value(); }

private:
    std::ofstream& out_;
    Hash hash_;
};

// Copies size bytes from in to write, a chunk at a time.
template <typename Write>
bool CopyBytes(std::istream& in, std::uint64_t size, std::vector<char>& buffer, Write&& write) {
    while (size > 0) {
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer.size()));
        in.read(buffer.data(), static_cast<std::streamsize>(take));
        if (static_cast<std::size_t>(in.gcount()) != take) {
            return false;
        }
        write(buffer.data(), take);
        size -= take;
    }
    return true;
}

// A path an archive may hold: relative, with '/' between names, and no name
// that is empty, "." or "..", or that a drive letter or backslash could turn
// into somewhere outside the folder.
bool SafeArchivePath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos || path == kCompleteMark) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

std::string Hex(std::uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = kDigits[value & 0xfu];
        value >>= 4;
    }
    return text;
}

}  // namespace

bool add_pack_folder(const fs::path& folder, const std::string& prefix, std::vector<PackFile>& files,
                     std::string& error) {
    std::error_code failure;
    if (!fs::is_directory(folder, failure)) {
        error = utf8_path(folder) + " is not a folder";
        return false;
    }
    std::vector<PackFile> found;
    fs::recursive_directory_iterator it(folder, failure);
    const fs::recursive_directory_iterator end;
    for (; !failure && it != end; it.increment(failure)) {
        const std::string name = utf8_path(it->path().filename());
        if (!name.empty() && name.front() == '.') {
            if (it->is_directory(failure)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(failure)) {
            continue;
        }
        const fs::path relative = it->path().lexically_relative(folder);
        found.push_back({prefix + relative.generic_u8string(), it->path()});
    }
    if (failure) {
        error = "cannot read " + utf8_path(folder) + ": " + failure.message();
        return false;
    }
    // The same folder packs to the same bytes, and so to the same hash.
    std::sort(found.begin(), found.end(), [](const PackFile& a, const PackFile& b) { return a.path < b.path; });
    files.insert(files.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    return true;
}

bool write_game_pack(const fs::path& program, const std::vector<PackFile>& files, const fs::path& output,
                     std::string& error) {
    std::error_code failure;
    fs::path temp = output;
    temp += ".part";
    std::vector<char> buffer(kChunk);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + utf8_path(temp);
            return false;
        }
        auto fail = [&](std::string why) {
            error = std::move(why);
            out.close();
            fs::remove(temp, failure);
            return false;
        };
        std::uint64_t offset = 0;
        if (!program.empty()) {
            std::ifstream in(program, std::ios::binary);
            const std::uint64_t size = fs::file_size(program, failure);
            if (!in || failure) {
                return fail("cannot read the player program " + utf8_path(program));
            }
            if (!CopyBytes(in, size, buffer, [&](const char* bytes, std::size_t n) {
                    out.write(bytes, static_cast<std::streamsize>(n));
                })) {
                return fail("cannot read the player program " + utf8_path(program));
            }
            offset = size;
        }
        ArchiveWriter archive(out);
        archive.write(kArchiveMagic, sizeof(kArchiveMagic));
        archive.number<4>(kVersion);
        archive.number<4>(files.size());
        for (const PackFile& file : files) {
            if (!SafeArchivePath(file.path) || file.path.size() > kMaxPathBytes) {
                return fail("cannot pack \"" + file.path + "\": not a relative path");
            }
            std::ifstream in(file.source, std::ios::binary);
            const std::uint64_t size = fs::file_size(file.source, failure);
            if (!in || failure) {
                return fail("cannot read " + utf8_path(file.source));
            }
            archive.number<4>(file.path.size());
            archive.write(file.path.data(), file.path.size());
            archive.number<8>(size);
            if (!CopyBytes(in, size, buffer, [&](const char* bytes, std::size_t n) { archive.write(bytes, n); })) {
                return fail("cannot read " + utf8_path(file.source) + ": it changed while it was packed");
            }
        }
        const std::array<char, 8> start = LittleEndian<8>(offset);
        const std::array<char, 8> hash = LittleEndian<8>(archive.hash());
        out.write(start.data(), start.size());
        out.write(hash.data(), hash.size());
        out.write(kTrailerMagic, sizeof(kTrailerMagic));
        out.flush();
        if (!out) {
            return fail("cannot write " + utf8_path(temp) + ". Is the disk full?");
        }
    }
    if (!program.empty()) {
        const fs::perms mode = fs::status(program, failure).permissions();
        if (!failure) {
            fs::permissions(temp, mode, failure);
        }
    }
    fs::rename(temp, output, failure);
    if (failure) {
        error = "cannot replace " + utf8_path(output) + ": " + failure.message();
        fs::remove(temp, failure);
        return false;
    }
    return true;
}

std::optional<PackLocation> find_game_pack(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::error_code failure;
    const std::uint64_t size = fs::file_size(file, failure);
    if (!in || failure || size < kTrailerSize) {
        return std::nullopt;
    }
    char trailer[kTrailerSize];
    in.seekg(static_cast<std::streamoff>(size - kTrailerSize));
    in.read(trailer, static_cast<std::streamsize>(kTrailerSize));
    if (!in || std::memcmp(trailer + 16, kTrailerMagic, sizeof(kTrailerMagic)) != 0) {
        return std::nullopt;
    }
    PackLocation where;
    where.offset = ReadLittleEndian(trailer, 8);
    where.hash = ReadLittleEndian(trailer + 8, 8);
    if (where.offset >= size - kTrailerSize) {
        return std::nullopt;
    }
    return where;
}

bool extract_game_pack(const fs::path& file, const PackLocation& where, const fs::path& folder, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    std::error_code failure;
    const std::uint64_t size = fs::file_size(file, failure);
    if (!in || failure) {
        error = "cannot read " + utf8_path(file);
        return false;
    }
    const std::uint64_t end = size - kTrailerSize;
    in.seekg(static_cast<std::streamoff>(where.offset));
    const std::string damaged = utf8_path(file) + " holds a damaged game";
    char head[12];
    in.read(head, sizeof(head));
    if (!in || std::memcmp(head, kArchiveMagic, sizeof(kArchiveMagic)) != 0) {
        error = damaged;
        return false;
    }
    if (ReadLittleEndian(head + 4, 4) != kVersion) {
        error = utf8_path(file) + " was packed by another version of the engine";
        return false;
    }
    const std::uint64_t count = ReadLittleEndian(head + 8, 4);
    std::vector<char> buffer(kChunk);
    for (std::uint64_t i = 0; i < count; ++i) {
        char length[4];
        in.read(length, sizeof(length));
        const std::uint64_t pathBytes = ReadLittleEndian(length, 4);
        if (!in || pathBytes == 0 || pathBytes > kMaxPathBytes) {
            error = damaged;
            return false;
        }
        std::string path(static_cast<std::size_t>(pathBytes), '\0');
        in.read(path.data(), static_cast<std::streamsize>(path.size()));
        char sizeBytes[8];
        in.read(sizeBytes, sizeof(sizeBytes));
        const std::uint64_t fileSize = ReadLittleEndian(sizeBytes, 8);
        const std::uint64_t at = static_cast<std::uint64_t>(in.tellg());
        if (!in || !SafeArchivePath(path) || fileSize > end - std::min(end, at)) {
            error = damaged;
            return false;
        }
        const fs::path target = folder / fs::u8path(path);
        fs::create_directories(target.parent_path(), failure);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (failure || !out) {
            error = "cannot write " + utf8_path(target);
            return false;
        }
        if (!CopyBytes(in, fileSize, buffer, [&](const char* bytes, std::size_t n) {
                out.write(bytes, static_cast<std::streamsize>(n));
            })) {
            error = damaged;
            return false;
        }
        if (!out.flush()) {
            error = "cannot write " + utf8_path(target) + ". Is the disk full?";
            return false;
        }
    }
    return true;
}

bool unpack_game(const fs::path& file, const PackLocation& where, const fs::path& cache, fs::path& folder,
                 std::string& error) {
    std::error_code failure;
    const std::string name = Hex(where.hash);
    folder = cache / name;
    if (fs::is_regular_file(folder / kCompleteMark, failure)) {
        return true;
    }
    // A folder of its own, so two copies of the game started together do not
    // write over each other; the first to finish is the one kept.
    std::random_device random;
    const fs::path temp = cache / (name + ".part-" + std::to_string(random()));
    fs::remove_all(temp, failure);
    if (!extract_game_pack(file, where, temp, error)) {
        fs::remove_all(temp, failure);
        return false;
    }
    std::string ignored;
    if (!engine_core::write_file(temp / kCompleteMark, std::string(), ignored)) {
        error = "cannot write to " + utf8_path(cache);
        fs::remove_all(temp, failure);
        return false;
    }
    // A folder without the mark was left half written. One with it was just
    // finished by another copy of the game, and is kept.
    if (fs::exists(folder, failure) && !fs::is_regular_file(folder / kCompleteMark, failure)) {
        fs::remove_all(folder, failure);
    }
    fs::rename(temp, folder, failure);
    if (failure) {
        fs::remove_all(temp, failure);
        if (!fs::is_regular_file(folder / kCompleteMark, failure)) {
            error = "cannot extract the game into " + utf8_path(folder);
            return false;
        }
    }
    return true;
}

}  // namespace runner
