#pragma once

#include "Bits.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace network {

// What a client sends to prove it belongs to this game: the project's
// gameId, its version, and a hash of the resources folder it shipped with.
struct GameIdentity {
    std::array<std::uint8_t, 16> game_id{};
    std::string version = "1.0.0";
    std::uint64_t resources_hash = 0;
};

bool parse_game_id(std::string_view hex32, std::array<std::uint8_t, 16>& out);
std::string format_game_id(const std::array<std::uint8_t, 16>& id);

// XXH64 over every file under folder: its path relative to folder with '/'
// separators (UTF-8), a NUL, its size, then its bytes, in sorted path order.
// Names starting with '.', and everything under them, are skipped, as a packed game skips them.
// 0 for a missing folder or any read error; callers treat 0 as no identity.
std::uint64_t hash_resources(const std::filesystem::path& folder);

void write_identity(BitWriter& writer, const GameIdentity& identity);
bool read_identity(BitReader& reader, GameIdentity& identity);

}  // namespace network
