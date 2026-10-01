#include "TextureImport.hpp"

#include "AssetInstances.hpp"
#include "IdeResources.hpp"
#include "Project.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace ide {
namespace {

// Past this many names taken, the import gives up rather than search on.
constexpr int kMaxSuffix = 10000;

std::string AsciiLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

std::optional<std::string> resource_path_inside(const std::filesystem::path& root, const std::filesystem::path& file) {
    std::error_code failure;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, failure);
    if (failure) {
        return std::nullopt;
    }
    const std::filesystem::path full = std::filesystem::weakly_canonical(file, failure);
    if (failure) {
        return std::nullopt;
    }
    const std::filesystem::path inside = full.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..") {
        return std::nullopt;
    }
    std::string path = utf8_path(inside);
    std::replace(path.begin(), path.end(), static_cast<char>(std::filesystem::path::preferred_separator), '/');
    if (engine_core::resource_path_error(path)) {
        return std::nullopt;
    }
    return path;
}

bool is_texture_file(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::vector<std::string>& known = texture_file_extensions();
    return std::find(known.begin(), known.end(), AsciiLower(path.substr(dot + 1))) != known.end();
}

const std::vector<std::string>& texture_file_extensions() {
    static const std::vector<std::string> extensions = {"png", "jpg", "jpeg", "tga", "bmp",  "gif", "hdr", "psd",
                                                        "dds", "ktx", "ktx2", "webp", "exr", "tif", "tiff"};
    return extensions;
}

std::optional<std::string> store_resource_file(const std::filesystem::path& resources_root, const std::string& folder,
                                               const std::string& file_name, const std::string& bytes,
                                               std::string& error) {
    const std::string name = engine_core::sanitize_file_name(file_name);
    const std::size_t dot = name.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string extension = dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
    for (int suffix = 1; suffix <= kMaxSuffix; ++suffix) {
        const std::string file = suffix == 1 ? name : stem + "-" + std::to_string(suffix) + extension;
        const std::string path = folder + "/" + file;
        const std::filesystem::path target = resources_root / path_from_utf8(path);
        std::error_code failure;
        if (!std::filesystem::exists(target, failure)) {
            if (!write_file(target, bytes, error)) {
                return std::nullopt;
            }
            return path;
        }
        std::string there;
        std::string unread;
        if (read_file(target, there, unread) && there == bytes) {
            return path;
        }
    }
    error = "every name for " + name + " in " + folder + " is taken";
    return std::nullopt;
}

std::optional<std::string> import_texture_file(const std::filesystem::path& resources_root, const std::string& source,
                                               std::string& error, const std::string& folder) {
    const std::filesystem::path from = path_from_utf8(source);
    if (std::optional<std::string> inside = resource_path_inside(resources_root, from)) {
        return inside;
    }
    std::string bytes;
    if (!read_file(from, bytes, error)) {
        return std::nullopt;
    }
    return store_resource_file(resources_root, folder, utf8_path(from.filename()), bytes, error);
}

}  // namespace ide
