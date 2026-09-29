#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

// Whole-file reads and safe writes for the engine and the studio.
namespace engine_core {

// A path as UTF-8, the encoding JadeFX and the engine take.
inline std::string utf8_path(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// The whole file. False, with error set, when it cannot be read.
inline bool read_file(const std::filesystem::path& path, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + utf8_path(path);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "cannot read " + utf8_path(path);
        return false;
    }
    return true;
}

// Writes beside the target, then renames over it, so a crash leaves the old
// file whole. Creates the folder. False, with error set, when it fails. A
// temporary file this wrote is not left behind: one left by a full disk would
// keep a new folder from being removed. Whatever else is in the way is kept.
inline bool write_file(const std::filesystem::path& path, const std::string& bytes, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code failure;
    fs::create_directories(path.parent_path(), failure);
    if (failure) {
        error = "cannot create " + utf8_path(path.parent_path()) + ": " + failure.message();
        return false;
    }
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + utf8_path(temp);
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            error = "cannot write " + utf8_path(temp);
            out.close();
            fs::remove(temp, failure);
            return false;
        }
    }
    fs::rename(temp, path, failure);
    if (failure) {
        error = "cannot replace " + utf8_path(path) + ": " + failure.message();
        fs::remove(temp, failure);
        return false;
    }
    return true;
}

}  // namespace engine_core
