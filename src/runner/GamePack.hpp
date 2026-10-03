#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace runner {

// A game packed for the player: a project's files and the engine resources it
// draws with, in one archive. Export appends it to a copy of the player
// program, so the game is one file to send; in a Mac bundle it is written alone
// as Contents/Resources/game.pak, since bytes after a signed program would
// break its signature. The player finds the archive at the end of its own
// file, extracts it once to a folder named by its hash, and plays from there.
//
//   archive   "AEPK", u32 version, u32 file count, then for each file:
//             u32 path length, the path (UTF-8, '/' separators), u64 size, its bytes
//   trailer   u64 where the archive starts, u64 the archive's hash, "AEGAME01"
//
// Numbers are little-endian. The trailer is the file's last 24 bytes.

// The archive's two folders: the project, and the engine resources the player
// searches before its own.
inline constexpr const char* kPackedProject = "game";
inline constexpr const char* kPackedResources = "engine";
// A Mac player's archive, in its bundle's Contents/Resources.
inline constexpr const char* kBundlePack = "game.pak";

// One file to pack: its path in the archive, and where it is on disk now.
struct PackFile {
    std::string path;
    std::filesystem::path source;
};

// Adds every file under folder to files, at prefix followed by its path below
// folder. Names that start with '.', such as .git and .gitkeep, are left out,
// and so is everything under them. False, with why, when folder cannot be read.
bool add_pack_folder(const std::filesystem::path& folder, const std::string& prefix, std::vector<PackFile>& files,
                     std::string& error);

// Writes program's bytes, then the archive of files and its trailer, to
// output. With an empty program, output is the archive alone, as game.pak is.
// Writes beside output and renames over it, so a failure leaves no half file,
// and keeps program's permissions, so a Linux or Mac program stays runnable.
// False, with why, when a file cannot be read or output cannot be written.
bool write_game_pack(const std::filesystem::path& program, const std::vector<PackFile>& files,
                     const std::filesystem::path& output, std::string& error);

// Where a file's archive starts, and its hash.
struct PackLocation {
    std::uint64_t offset = 0;
    std::uint64_t hash = 0;
};

// The archive at the end of file. Nothing when the file has none, as a player
// that was never exported has none.
std::optional<PackLocation> find_game_pack(const std::filesystem::path& file);

// Writes every file in the archive under folder. False, with why, when the
// archive is damaged, names a path outside folder, or a file cannot be written.
bool extract_game_pack(const std::filesystem::path& file, const PackLocation& where,
                       const std::filesystem::path& folder, std::string& error);

// The folder the archive is extracted to, under cache: named by its hash, so
// an exported game is extracted on its first run and reused after, and a game
// exported again gets a folder of its own. A folder left half written, as by a
// crash, is extracted again. False, with why, when it cannot be extracted.
bool unpack_game(const std::filesystem::path& file, const PackLocation& where, const std::filesystem::path& cache,
                 std::filesystem::path& folder, std::string& error);

}  // namespace runner
