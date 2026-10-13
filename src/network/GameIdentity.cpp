#include "GameIdentity.hpp"

#include "common/xxhash.h"   // zstd's copy; its names are prefixed ZSTD_ through XXH_NAMESPACE

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <utility>
#include <vector>

namespace network {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

}  // namespace

bool parse_game_id(std::string_view hex32, std::array<std::uint8_t, 16>& out) {
    if (hex32.size() != 32) return false;
    std::array<std::uint8_t, 16> parsed{};
    for (int i = 0; i < 16; ++i) {
        const int high = hex_value(hex32[i * 2]);
        const int low = hex_value(hex32[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        parsed[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    out = parsed;
    return true;
}

std::string format_game_id(const std::array<std::uint8_t, 16>& id) {
    std::string text;
    char buffer[3];
    for (std::uint8_t byte : id) {
        std::snprintf(buffer, sizeof(buffer), "%02x", byte);
        text += buffer;
    }
    return text;
}

std::uint64_t hash_resources(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(folder, error)) return 0;
    std::vector<std::pair<std::string, fs::path>> files;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        files.emplace_back(fs::relative(it->path(), folder, error).generic_string(), it->path());
    }
    std::sort(files.begin(), files.end());
    XXH64_state_t* state = XXH64_createState();
    XXH64_reset(state, 0);
    std::vector<char> chunk(64 * 1024);
    for (const auto& [relative, path] : files) {
        XXH64_update(state, relative.data(), relative.size());
        const char zero = 0;
        XXH64_update(state, &zero, 1);
        std::ifstream in(path, std::ios::binary);
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = in.gcount();
            if (got > 0) XXH64_update(state, chunk.data(), static_cast<std::size_t>(got));
        }
    }
    const std::uint64_t hash = XXH64_digest(state);
    XXH64_freeState(state);
    return hash;
}

void write_identity(BitWriter& writer, const GameIdentity& identity) {
    writer.bytes(identity.game_id.data(), identity.game_id.size());
    const std::size_t length = std::min<std::size_t>(identity.version.size(), 255);
    writer.bits(static_cast<std::uint32_t>(length), 8);
    writer.bytes(reinterpret_cast<const std::uint8_t*>(identity.version.data()), length);
    writer.bits(static_cast<std::uint32_t>(identity.resources_hash & 0xFFFFFFFFu), 32);
    writer.bits(static_cast<std::uint32_t>(identity.resources_hash >> 32), 32);
}

bool read_identity(BitReader& reader, GameIdentity& identity) {
    GameIdentity read;
    reader.bytes(read.game_id.data(), read.game_id.size());
    const std::uint32_t length = reader.bits(8);
    if (reader.failed() || reader.bytes_left() < length) return false;
    read.version.resize(length);
    reader.bytes(reinterpret_cast<std::uint8_t*>(read.version.data()), length);
    const std::uint64_t low = reader.bits(32);
    const std::uint64_t high = reader.bits(32);
    if (reader.failed()) return false;
    read.resources_hash = low | (high << 32);
    identity = std::move(read);
    return true;
}

}  // namespace network
